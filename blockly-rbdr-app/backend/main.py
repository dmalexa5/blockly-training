from __future__ import annotations

import ast
import asyncio
import contextlib
from dataclasses import dataclass, field
from typing import Any

import httpx
from fastapi import FastAPI, HTTPException, WebSocket, WebSocketDisconnect
from fastapi.middleware.cors import CORSMiddleware
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel

DEFAULT_WAIT_SECONDS = 60.0
DEVICE_TIMEOUT_SECONDS = 2.0
MAX_REPEAT_COUNT = 20
MIN_USER_WAIT_SECONDS = 1
MAX_USER_WAIT_SECONDS = 10
DEVICE_TRANSPORT: httpx.AsyncBaseTransport | None = None


@dataclass(frozen=True)
class ModuleConfig:
    id: str
    type: str
    label: str
    base_url: str


MODULE_CONFIG = (
    ModuleConfig(id="button_left", type="button", label="Left button", base_url="http://button-left.local"),
    ModuleConfig(id="button_right", type="button", label="Right button", base_url="http://button-right.local"),
    ModuleConfig(id="rebounder", type="rebounder", label="Rebounder", base_url="http://rebounder.local"),
)


def module_ids() -> set[str]:
    return {module.id for module in MODULE_CONFIG}


def module_config(module_id: str) -> ModuleConfig:
    for module in MODULE_CONFIG:
        if module.id == module_id:
            return module
    raise RuntimeError(f"unknown module: {module_id}")


class RunRequest(BaseModel):
    code: str


class DeviceEvent(BaseModel):
    event: str
    module: str


@dataclass
class ModuleState:
    waiter: asyncio.Future[DeviceEvent] | None = None


@dataclass
class AppState:
    modules: dict[str, ModuleState] = field(default_factory=lambda: {name: ModuleState() for name in module_ids()})
    websockets: set[WebSocket] = field(default_factory=set)
    run_task: asyncio.Task[None] | None = None
    status: str = "idle"
    lock: asyncio.Lock = field(default_factory=asyncio.Lock)


@dataclass(frozen=True)
class ProgramAction:
    kind: str
    value: str | int


class RbdrRuntime:
    def __init__(self, state: AppState):
        self._state = state

    async def wait(self, seconds: int) -> None:
        await log(self._state, f"waiting {seconds} seconds")
        await asyncio.sleep(seconds)

    async def activate_and_wait(self, module: str, timeout: float = DEFAULT_WAIT_SECONDS) -> DeviceEvent:
        if module not in module_ids():
            raise RuntimeError(f"unknown module: {module}")

        await log(self._state, f"activating {module}")
        config = module_config(module)
        async with self._state.lock:
            module_state = self._state.modules[module]
            if module_state.waiter is not None and not module_state.waiter.done():
                raise RuntimeError(f"{module} module already has a pending activation")

            loop = asyncio.get_running_loop()
            module_state.waiter = loop.create_future()
            waiter = module_state.waiter

        try:
            await send_device_command(config, "activate")
        except Exception:
            async with self._state.lock:
                if module_state.waiter is waiter:
                    module_state.waiter = None
            raise

        try:
            event = await asyncio.wait_for(waiter, timeout=timeout)
            await log(self._state, f"{module} triggered")
            return event
        finally:
            await self._queue_deactivate(module)

    async def _queue_deactivate(self, module: str) -> None:
        config = module_config(module)
        await send_device_command(config, "deactivate")
        async with self._state.lock:
            module_state = self._state.modules[module]
            module_state.waiter = None
        await log(self._state, f"deactivating {module}")


async def send_device_command(module: ModuleConfig, command: str) -> None:
    async with httpx.AsyncClient(transport=DEVICE_TRANSPORT, timeout=DEVICE_TIMEOUT_SECONDS) as client:
        try:
            health = await client.get(f"{module.base_url}/health")
        except httpx.HTTPError as exc:
            raise RuntimeError(f"{module.id} module is not connected") from exc

        if health.status_code != 200:
            raise RuntimeError(f"{module.id} module is not connected")
        identity = health.json()
        if identity != {"module": module.id, "type": module.type}:
            raise RuntimeError(f"{module.id} health identity mismatch")

        response = await client.post(f"{module.base_url}/command", json={"cmd": command})
        if response.status_code == 409:
            raise RuntimeError(f"{module.id} module is busy")
        if response.status_code != 202:
            raise RuntimeError(f"{module.id} command failed: HTTP {response.status_code}")


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


def _validation_error() -> HTTPException:
    return HTTPException(status_code=400, detail="Code contains unsupported statements")


def _action_from_statement(node: ast.stmt) -> ProgramAction:
    if not isinstance(node, ast.Expr):
        raise _validation_error()
    value = node.value
    if not isinstance(value, ast.Await):
        raise _validation_error()
    call = value.value
    if not isinstance(call, ast.Call):
        raise _validation_error()
    if call.keywords or len(call.args) != 1:
        raise _validation_error()
    if not isinstance(call.func, ast.Attribute):
        raise _validation_error()
    if not isinstance(call.func.value, ast.Name) or call.func.value.id != "rbdr":
        raise _validation_error()

    if call.func.attr == "activate_and_wait":
        module = call.args[0]
        if not isinstance(module, ast.Constant) or module.value not in module_ids():
            raise _validation_error()
        return ProgramAction("activate", module.value)

    if call.func.attr == "wait":
        seconds = call.args[0]
        if not isinstance(seconds, ast.Constant) or type(seconds.value) is not int:
            raise _validation_error()
        if seconds.value < MIN_USER_WAIT_SECONDS or seconds.value > MAX_USER_WAIT_SECONDS:
            raise _validation_error()
        return ProgramAction("wait", seconds.value)

    raise _validation_error()


def _actions_from_block(nodes: list[ast.stmt]) -> list[ProgramAction]:
    actions: list[ProgramAction] = []
    for node in nodes:
        if isinstance(node, ast.Pass):
            continue
        if isinstance(node, ast.For):
            raise _validation_error()
        actions.append(_action_from_statement(node))
    return actions


def _module_from_action(action: ProgramAction) -> str:
    if action.kind != "activate" or not isinstance(action.value, str):
        raise _validation_error()
    return action.value


def _repeat_count(node: ast.For) -> int:
    if not isinstance(node.target, ast.Name):
        raise _validation_error()
    if node.orelse:
        raise _validation_error()
    if not isinstance(node.iter, ast.Call):
        raise _validation_error()
    if not isinstance(node.iter.func, ast.Name) or node.iter.func.id != "range":
        raise _validation_error()
    if node.iter.keywords or len(node.iter.args) != 1:
        raise _validation_error()
    count = node.iter.args[0]
    if not isinstance(count, ast.Constant) or type(count.value) is not int:
        raise _validation_error()
    if count.value < 1 or count.value > MAX_REPEAT_COUNT:
        raise _validation_error()
    return count.value


def validate_generated_code(code: str) -> list[ProgramAction]:
    try:
        tree = ast.parse(code)
    except SyntaxError as exc:
        raise _validation_error() from exc

    actions: list[ProgramAction] = []
    for node in tree.body:
        if isinstance(node, ast.For):
            count = _repeat_count(node)
            body = _actions_from_block(node.body)
            actions.extend(body * count)
        else:
            actions.append(_action_from_statement(node))
    return actions


async def execute_generated_code(state: AppState, actions: list[ProgramAction]) -> None:
    rbdr = RbdrRuntime(state)
    try:
        await set_status(state, "running")
        if not actions:
            await log(state, "program is empty")
        for action in actions:
            if action.kind == "wait":
                if not isinstance(action.value, int):
                    raise RuntimeError("invalid wait action")
                await rbdr.wait(action.value)
            else:
                await rbdr.activate_and_wait(_module_from_action(action))
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
        actions = validate_generated_code(request.code)
        async with state.lock:
            if state.run_task is not None and not state.run_task.done():
                raise HTTPException(status_code=409, detail="A program is already running")
            state.run_task = asyncio.create_task(execute_generated_code(state, actions))
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

    @app.get("/api/modules")
    async def list_modules() -> dict[str, list[dict[str, str]]]:
        return {
            "modules": [
                {"id": module.id, "type": module.type, "label": module.label}
                for module in MODULE_CONFIG
            ]
        }

    @app.post("/events")
    async def receive_event(event: DeviceEvent) -> dict[str, str]:
        if event.module not in module_ids():
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
