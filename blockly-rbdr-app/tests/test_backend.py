from __future__ import annotations

import asyncio
import json

import httpx
import pytest

import backend.main as backend_main
from backend.main import create_app


class DeviceMock:
    def __init__(self):
        self.commands: list[tuple[str, str]] = []
        self.health_status = 200
        self.command_status = 202

    async def __call__(self, request: httpx.Request) -> httpx.Response:
        module = request.url.host.split(".", 1)[0].replace("-", "_")
        if request.url.path == "/health":
            if self.health_status != 200:
                return httpx.Response(self.health_status)
            module_type = "rebounder" if module == "rebounder" else "button"
            return httpx.Response(200, json={"module": module, "type": module_type})
        if request.url.path == "/command":
            cmd = json.loads(request.content)["cmd"]
            self.commands.append((module, cmd))
            return httpx.Response(self.command_status)
        return httpx.Response(404)


@pytest.fixture
async def client(monkeypatch: pytest.MonkeyPatch):
    device = DeviceMock()
    monkeypatch.setattr(backend_main, "DEVICE_TRANSPORT", httpx.MockTransport(device))
    app = create_app()
    transport = httpx.ASGITransport(app=app)
    async with httpx.AsyncClient(transport=transport, base_url="http://test") as async_client:
        async_client.device = device  # type: ignore[attr-defined]
        yield async_client


async def test_backend_checks_health_then_posts_activate_and_deactivate(client: httpx.AsyncClient):
    response = await client.post("/api/run", json={"code": 'await rbdr.activate_and_wait("button_left")\n'})
    assert response.status_code == 200
    await asyncio.sleep(0)

    assert client.device.commands == [("button_left", "activate")]  # type: ignore[attr-defined]

    await client.post("/events", json={"event": "triggered", "module": "button_left"})
    await asyncio.sleep(0)

    assert client.device.commands == [("button_left", "activate"), ("button_left", "deactivate")]  # type: ignore[attr-defined]


async def test_modules_api_returns_configured_modules_without_device_urls(client: httpx.AsyncClient):
    response = await client.get("/api/modules")

    assert response.status_code == 200
    assert response.json() == {
        "modules": [
            {"id": "button_left", "type": "button", "label": "Left button"},
            {"id": "button_right", "type": "button", "label": "Right button"},
            {"id": "rebounder", "type": "rebounder", "label": "Rebounder"},
        ]
    }


async def test_configured_alternate_module_id_runs_independently(monkeypatch: pytest.MonkeyPatch):
    monkeypatch.setattr(
        backend_main,
        "MODULE_CONFIG",
        (
            backend_main.ModuleConfig(
                id="button_left",
                type="button",
                label="Left button",
                base_url="http://button-left.local",
            ),
            backend_main.ModuleConfig(
                id="button_right",
                type="button",
                label="Right button",
                base_url="http://button-right.local",
            ),
        ),
    )
    device = DeviceMock()
    monkeypatch.setattr(backend_main, "DEVICE_TRANSPORT", httpx.MockTransport(device))
    app = create_app()
    transport = httpx.ASGITransport(app=app)
    async with httpx.AsyncClient(transport=transport, base_url="http://test") as client:
        response = await client.post(
            "/api/run",
            json={"code": 'await rbdr.activate_and_wait("button_right")\n'},
        )
        assert response.status_code == 200
        await asyncio.sleep(0)

        assert device.commands == [("button_right", "activate")]

        await client.post("/events", json={"event": "triggered", "module": "button_right"})
        await asyncio.sleep(0)
        assert device.commands == [("button_right", "activate"), ("button_right", "deactivate")]


async def test_failed_health_check_marks_run_error(client: httpx.AsyncClient):
    client.device.health_status = 503  # type: ignore[attr-defined]

    response = await client.post("/api/run", json={"code": 'await rbdr.activate_and_wait("button_left")\n'})
    assert response.status_code == 200

    await asyncio.sleep(0)
    app_state = client._transport.app.state.rbdr  # type: ignore[attr-defined]
    assert app_state.status == "error"


async def test_device_busy_409_is_surfaced_as_error(client: httpx.AsyncClient):
    client.device.command_status = 409  # type: ignore[attr-defined]

    response = await client.post("/api/run", json={"code": 'await rbdr.activate_and_wait("button_left")\n'})
    assert response.status_code == 200

    await asyncio.sleep(0)
    app_state = client._transport.app.state.rbdr  # type: ignore[attr-defined]
    assert app_state.status == "error"


async def test_rejects_second_active_run(client: httpx.AsyncClient):
    first = await client.post("/api/run", json={"code": 'await rbdr.activate_and_wait("button_left")\n'})
    second = await client.post("/api/run", json={"code": 'await rbdr.activate_and_wait("button_left")\n'})

    assert first.status_code == 200
    assert second.status_code == 409

    await client.post("/api/stop")


async def test_wrong_module_event_does_not_complete_wait(client: httpx.AsyncClient):
    await client.post("/api/run", json={"code": 'await rbdr.activate_and_wait("button_left")\n'})
    await asyncio.sleep(0)

    await client.post("/events", json={"event": "triggered", "module": "rebounder"})
    await asyncio.sleep(0)

    app_state = client._transport.app.state.rbdr  # type: ignore[attr-defined]
    assert app_state.modules["button_left"].waiter is not None
    assert not app_state.modules["button_left"].waiter.done()

    await client.post("/api/stop")


async def test_unsupported_generated_code_is_rejected(client: httpx.AsyncClient):
    response = await client.post("/api/run", json={"code": "print('nope')\n"})
    assert response.status_code == 400


async def test_wait_program_is_accepted_and_completes(client: httpx.AsyncClient, monkeypatch: pytest.MonkeyPatch):
    sleep_calls: list[float] = []
    original_sleep = asyncio.sleep

    async def immediate_sleep(seconds: float):
        sleep_calls.append(seconds)
        await original_sleep(0)

    monkeypatch.setattr(backend_main.asyncio, "sleep", immediate_sleep)

    response = await client.post("/api/run", json={"code": "await rbdr.wait(1)\n"})
    assert response.status_code == 200

    await original_sleep(0)
    app_state = client._transport.app.state.rbdr  # type: ignore[attr-defined]
    assert app_state.status == "idle"
    assert 1 in sleep_calls


async def test_wait_before_button_preserves_order(client: httpx.AsyncClient, monkeypatch: pytest.MonkeyPatch):
    sleep_calls: list[float] = []
    original_sleep = asyncio.sleep

    async def immediate_sleep(seconds: float):
        sleep_calls.append(seconds)
        await original_sleep(0)

    monkeypatch.setattr(backend_main.asyncio, "sleep", immediate_sleep)

    response = await client.post(
        "/api/run",
        json={"code": 'await rbdr.wait(1)\nawait rbdr.activate_and_wait("button_left")\n'},
    )
    assert response.status_code == 200

    await original_sleep(0)
    assert 1 in sleep_calls
    assert client.device.commands == [("button_left", "activate")]  # type: ignore[attr-defined]

    await client.post("/api/stop")


async def test_repeat_loop_repeats_button_commands(client: httpx.AsyncClient):
    response = await client.post(
        "/api/run",
        json={"code": 'for count in range(2):\n  await rbdr.activate_and_wait("button_left")\n'},
    )
    assert response.status_code == 200
    await asyncio.sleep(0)

    assert client.device.commands == [("button_left", "activate")]  # type: ignore[attr-defined]

    await client.post("/events", json={"event": "triggered", "module": "button_left"})
    await asyncio.sleep(0)
    await asyncio.sleep(0)

    assert client.device.commands == [  # type: ignore[attr-defined]
        ("button_left", "activate"),
        ("button_left", "deactivate"),
        ("button_left", "activate"),
    ]

    await client.post("/events", json={"event": "triggered", "module": "button_left"})
    await asyncio.sleep(0)

    assert client.device.commands == [  # type: ignore[attr-defined]
        ("button_left", "activate"),
        ("button_left", "deactivate"),
        ("button_left", "activate"),
        ("button_left", "deactivate"),
    ]


async def test_repeat_loop_body_can_contain_both_modules(client: httpx.AsyncClient):
    response = await client.post(
        "/api/run",
        json={
            "code": (
                'for count in range(2):\n'
                '  await rbdr.activate_and_wait("button_left")\n'
                '  await rbdr.activate_and_wait("rebounder")\n'
            ),
        },
    )
    assert response.status_code == 200
    await asyncio.sleep(0)

    assert client.device.commands == [("button_left", "activate")]  # type: ignore[attr-defined]

    await client.post("/events", json={"event": "triggered", "module": "button_left"})
    await asyncio.sleep(0)
    await asyncio.sleep(0)

    assert client.device.commands == [  # type: ignore[attr-defined]
        ("button_left", "activate"),
        ("button_left", "deactivate"),
        ("rebounder", "activate"),
    ]

    await client.post("/events", json={"event": "triggered", "module": "rebounder"})
    await asyncio.sleep(0)
    await asyncio.sleep(0)

    assert client.device.commands[-2:] == [("rebounder", "deactivate"), ("button_left", "activate")]  # type: ignore[attr-defined]

    await client.post("/api/stop")


async def test_empty_repeat_loop_is_accepted_and_skipped(client: httpx.AsyncClient):
    response = await client.post("/api/run", json={"code": "for count in range(3):\n  pass\n"})
    assert response.status_code == 200

    await asyncio.sleep(0)
    app_state = client._transport.app.state.rbdr  # type: ignore[attr-defined]
    assert app_state.status == "idle"


async def test_repeat_loop_repeats_waits(client: httpx.AsyncClient, monkeypatch: pytest.MonkeyPatch):
    sleep_calls: list[float] = []
    original_sleep = asyncio.sleep

    async def immediate_sleep(seconds: float):
        sleep_calls.append(seconds)
        await original_sleep(0)

    monkeypatch.setattr(backend_main.asyncio, "sleep", immediate_sleep)

    response = await client.post("/api/run", json={"code": "for count in range(2):\n  await rbdr.wait(1)\n"})
    assert response.status_code == 200

    await original_sleep(0)
    assert sleep_calls.count(1) == 2


@pytest.mark.parametrize(
    "code",
    [
        'for count in range(1):\n  for inner in range(1):\n    await rbdr.activate_and_wait("button_left")\n',
        'for count in range(0):\n  await rbdr.activate_and_wait("button_left")\n',
        'for count in range(21):\n  await rbdr.activate_and_wait("button_left")\n',
        'for count in range(1.5):\n  await rbdr.activate_and_wait("button_left")\n',
        'for count in range(-1):\n  await rbdr.activate_and_wait("button_left")\n',
        'for count in range(times):\n  await rbdr.activate_and_wait("button_left")\n',
        'for count in range(1):\n  print("nope")\n',
    ],
)
async def test_unsupported_repeat_shapes_are_rejected(client: httpx.AsyncClient, code: str):
    response = await client.post("/api/run", json={"code": code})
    assert response.status_code == 400


@pytest.mark.parametrize(
    "code",
    [
        "await rbdr.wait(0)\n",
        "await rbdr.wait(11)\n",
        "await rbdr.wait(-1)\n",
        "await rbdr.wait(1.5)\n",
        "await rbdr.wait(seconds)\n",
        "await rbdr.wait(seconds=1)\n",
        "await rbdr.wait(1, 2)\n",
        "await rbdr.sleep(1)\n",
    ],
)
async def test_unsupported_wait_shapes_are_rejected(client: httpx.AsyncClient, code: str):
    response = await client.post("/api/run", json={"code": code})
    assert response.status_code == 400
