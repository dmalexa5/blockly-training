from __future__ import annotations

import asyncio
import contextlib
import time
from dataclasses import dataclass, field
from typing import Any

from fastapi import FastAPI, HTTPException, WebSocket, WebSocketDisconnect
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import Response
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel

MODULES = {"button", "rebounder"}
DEVICE_FRESH_SECONDS = 5.0
DEFAULT_WAIT_SECONDS = 60.0


class RunRequest(BaseModel):
    code: str


class DeviceEvent(BaseModel):
    event: str
    module: str
    active: bool | None = None
    triggered: bool | None = None
    uptime_ms: int | None = None
    reason: str | None = None


@dataclass
class ModuleState:
    last_poll_at: float | None = None
    pending_command: str | None = None
    waiter: asyncio.Future[DeviceEvent] | None = None


@dataclass
class AppState:
    modules: dict[str, ModuleState] = field(default_factory=lambda: {name: ModuleState() for name in MODULES})
    websockets: set[WebSocket] = field(default_factory=set)
    run_task: asyncio.Task[None] | None = None
    status: str = "idle"
    lock: asyncio.Lock = field(default_factory=asyncio.Lock)


class RbdrRuntime:
    def __init__(self, state: AppState):
        self._state = state

    async def activate_and_wait(self, module: str, timeout: float = DEFAULT_WAIT_SECONDS) -> DeviceEvent:
        if module not in MODULES:
            raise RuntimeError(f"unknown module: {module}")

        await log(self._state, f"activating {module}")
        async with self._state.lock:
            module_state = self._state.modules[module]
            now = time.monotonic()
            if module_state.last_poll_at is None or now - module_state.last_poll_at > DEVICE_FRESH_SECONDS:
                raise RuntimeError(f"{module} module is not connected")
            if module_state.waiter is not None and not module_state.waiter.done():
                raise RuntimeError(f"{module} module already has a pending activation")

            loop = asyncio.get_running_loop()
            module_state.waiter = loop.create_future()
            module_state.pending_command = "activate"
            waiter = module_state.waiter

        try:
            event = await asyncio.wait_for(waiter, timeout=timeout)
            await log(self._state, f"{module} triggered")
            return event
        finally:
            await self._queue_deactivate(module)

    async def _queue_deactivate(self, module: str) -> None:
        async with self._state.lock:
            module_state = self._state.modules[module]
            module_state.pending_command = "deactivate"
            module_state.waiter = None
        await log(self._state, f"deactivating {module}")


async def broadcast(state: AppState, payload: dict[str, Any]) -> None:
    stale: list[WebSocket] = []
    for websocket in state.websockets:
        try:
            await websocket.send_json(payload)
        except RuntimeError:
            stale.append(websocket)
    for websocket in stale:
        state.websockets.discard(websocket)


async def set_status(state: AppState, status: str) -> None:
    state.status = status
    await broadcast(state, {"type": "status", "status": status})


async def log(state: AppState, message: str) -> None:
    await broadcast(state, {"type": "log", "message": message})


def validate_generated_code(code: str) -> list[str]:
    lines = [line.strip() for line in code.splitlines() if line.strip()]
    allowed = {
        'await rbdr.activate_and_wait("button")',
        'await rbdr.activate_and_wait("rebounder")',
    }
    invalid = [line for line in lines if line not in allowed]
    if invalid:
        raise HTTPException(status_code=400, detail="Code contains unsupported statements")
    return lines


async def execute_generated_code(state: AppState, lines: list[str]) -> None:
    rbdr = RbdrRuntime(state)
    try:
        await set_status(state, "running")
        if not lines:
            await log(state, "program is empty")
        for line in lines:
            if line.endswith('"button")'):
                await rbdr.activate_and_wait("button")
            elif line.endswith('"rebounder")'):
                await rbdr.activate_and_wait("rebounder")
        await set_status(state, "idle")
        await log(state, "run complete")
    except asyncio.CancelledError:
        await clear_pending(state)
        await set_status(state, "idle")
        await log(state, "run stopped")
        raise
    except Exception as exc:
        await clear_pending(state)
        await set_status(state, "error")
        await log(state, f"run error: {exc}")


async def clear_pending(state: AppState) -> None:
    async with state.lock:
        for module_state in state.modules.values():
            if module_state.waiter is not None and not module_state.waiter.done():
                module_state.waiter.cancel()
            module_state.waiter = None
            module_state.pending_command = None


def create_app() -> FastAPI:
    app = FastAPI(title="RBDR Blockly")
    state = AppState()
    app.state.rbdr = state

    app.add_middleware(
        CORSMiddleware,
        allow_origins=["*"],
        allow_methods=["*"],
        allow_headers=["*"],
    )

    @app.post("/api/run")
    async def run_program(request: RunRequest) -> dict[str, str]:
        lines = validate_generated_code(request.code)
        async with state.lock:
            if state.run_task is not None and not state.run_task.done():
                raise HTTPException(status_code=409, detail="A program is already running")
            state.run_task = asyncio.create_task(execute_generated_code(state, lines))
        await asyncio.sleep(0)
        return {"status": "started"}

    @app.post("/api/stop")
    async def stop_program() -> dict[str, str]:
        task = state.run_task
        if task is not None and not task.done():
            task.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await task
        else:
            await clear_pending(state)
            await set_status(state, "idle")
        return {"status": "stopped"}

    @app.get("/poll", response_model=None)
    async def poll(module: str):
        if module not in MODULES:
            raise HTTPException(status_code=404, detail="Unknown module")
        async with state.lock:
            module_state = state.modules[module]
            module_state.last_poll_at = time.monotonic()
            command = module_state.pending_command
            module_state.pending_command = None
        if command is None:
            return Response(status_code=204)
        await log(state, f"{module} polled command: {command}")
        return {"cmd": command}

    @app.post("/events")
    async def receive_event(event: DeviceEvent) -> dict[str, str]:
        if event.module not in MODULES:
            raise HTTPException(status_code=404, detail="Unknown module")
        await log(state, f"{event.module} event: {event.event}")
        async with state.lock:
            module_state = state.modules[event.module]
            waiter = module_state.waiter
            if event.event == "triggered" and waiter is not None and not waiter.done():
                waiter.set_result(event)
        return {"status": "accepted"}

    @app.websocket("/api/ws")
    async def websocket_events(websocket: WebSocket) -> None:
        await websocket.accept()
        state.websockets.add(websocket)
        await websocket.send_json({"type": "status", "status": state.status})
        try:
            while True:
                await websocket.receive_text()
        except WebSocketDisconnect:
            state.websockets.discard(websocket)

    return app


app = create_app()

with contextlib.suppress(RuntimeError):
    app.mount("/", StaticFiles(directory="dist", html=True), name="frontend")
