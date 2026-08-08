from __future__ import annotations

import asyncio
import time

import httpx
import pytest

from backend.main import DEVICE_FRESH_SECONDS, create_app


@pytest.fixture
async def client():
    app = create_app()
    transport = httpx.ASGITransport(app=app)
    async with httpx.AsyncClient(transport=transport, base_url="http://test") as async_client:
        yield async_client


async def test_poll_returns_queued_commands(client: httpx.AsyncClient):
    await client.get("/poll", params={"module": "button"})
    response = await client.post("/api/run", json={"code": 'await rbdr.activate_and_wait("button")\n'})
    assert response.status_code == 200

    command = await client.get("/poll", params={"module": "button"})
    assert command.status_code == 200
    assert command.json() == {"cmd": "activate"}

    await client.post(
        "/events",
        json={"event": "triggered", "module": "button", "active": True, "triggered": True},
    )
    await asyncio.sleep(0)

    deactivate = await client.get("/poll", params={"module": "button"})
    assert deactivate.status_code == 200
    assert deactivate.json() == {"cmd": "deactivate"}


async def test_missing_module_connection_fails_fast(client: httpx.AsyncClient):
    response = await client.post("/api/run", json={"code": 'await rbdr.activate_and_wait("button")\n'})
    assert response.status_code == 200

    await asyncio.sleep(0)
    app_state = client._transport.app.state.rbdr  # type: ignore[attr-defined]
    assert app_state.status == "error"


async def test_stale_module_connection_fails_fast(client: httpx.AsyncClient):
    await client.get("/poll", params={"module": "button"})
    app_state = client._transport.app.state.rbdr  # type: ignore[attr-defined]
    app_state.modules["button"].last_poll_at = time.monotonic() - DEVICE_FRESH_SECONDS - 1

    response = await client.post("/api/run", json={"code": 'await rbdr.activate_and_wait("button")\n'})
    assert response.status_code == 200

    await asyncio.sleep(0)
    assert app_state.status == "error"


async def test_rejects_second_active_run(client: httpx.AsyncClient):
    await client.get("/poll", params={"module": "button"})
    first = await client.post("/api/run", json={"code": 'await rbdr.activate_and_wait("button")\n'})
    second = await client.post("/api/run", json={"code": 'await rbdr.activate_and_wait("button")\n'})

    assert first.status_code == 200
    assert second.status_code == 409


async def test_wrong_module_event_does_not_complete_wait(client: httpx.AsyncClient):
    await client.get("/poll", params={"module": "button"})
    await client.get("/poll", params={"module": "rebounder"})
    await client.post("/api/run", json={"code": 'await rbdr.activate_and_wait("button")\n'})

    await client.post(
        "/events",
        json={"event": "triggered", "module": "rebounder", "active": True, "triggered": True},
    )
    await asyncio.sleep(0)

    app_state = client._transport.app.state.rbdr  # type: ignore[attr-defined]
    assert app_state.modules["button"].waiter is not None
    assert not app_state.modules["button"].waiter.done()

    await client.post("/api/stop")


async def test_unsupported_generated_code_is_rejected(client: httpx.AsyncClient):
    response = await client.post("/api/run", json={"code": "print('nope')\n"})
    assert response.status_code == 400


async def test_repeat_loop_repeats_button_commands(client: httpx.AsyncClient):
    await client.get("/poll", params={"module": "button"})
    response = await client.post(
        "/api/run",
        json={"code": 'for count in range(2):\n  await rbdr.activate_and_wait("button")\n'},
    )
    assert response.status_code == 200

    first_activate = await client.get("/poll", params={"module": "button"})
    assert first_activate.status_code == 200
    assert first_activate.json() == {"cmd": "activate"}

    await client.post(
        "/events",
        json={"event": "triggered", "module": "button", "active": True, "triggered": True},
    )
    await asyncio.sleep(0)

    first_deactivate = await client.get("/poll", params={"module": "button"})
    assert first_deactivate.status_code == 200
    assert first_deactivate.json() == {"cmd": "deactivate"}

    second_activate = await client.get("/poll", params={"module": "button"})
    assert second_activate.status_code == 200
    assert second_activate.json() == {"cmd": "activate"}

    await client.post(
        "/events",
        json={"event": "triggered", "module": "button", "active": True, "triggered": True},
    )
    await asyncio.sleep(0)

    second_deactivate = await client.get("/poll", params={"module": "button"})
    assert second_deactivate.status_code == 200
    assert second_deactivate.json() == {"cmd": "deactivate"}


async def test_repeat_loop_body_can_contain_both_modules(client: httpx.AsyncClient):
    await client.get("/poll", params={"module": "button"})
    await client.get("/poll", params={"module": "rebounder"})
    response = await client.post(
        "/api/run",
        json={
            "code": (
                'for count in range(2):\n'
                '  await rbdr.activate_and_wait("button")\n'
                '  await rbdr.activate_and_wait("rebounder")\n'
            ),
        },
    )
    assert response.status_code == 200

    button_activate = await client.get("/poll", params={"module": "button"})
    assert button_activate.status_code == 200
    assert button_activate.json() == {"cmd": "activate"}

    await client.post(
        "/events",
        json={"event": "triggered", "module": "button", "active": True, "triggered": True},
    )
    await asyncio.sleep(0)

    button_deactivate = await client.get("/poll", params={"module": "button"})
    rebounder_activate = await client.get("/poll", params={"module": "rebounder"})
    assert button_deactivate.status_code == 200
    assert button_deactivate.json() == {"cmd": "deactivate"}
    assert rebounder_activate.status_code == 200
    assert rebounder_activate.json() == {"cmd": "activate"}

    await client.post(
        "/events",
        json={"event": "triggered", "module": "rebounder", "active": True, "triggered": True},
    )
    await asyncio.sleep(0)

    rebounder_deactivate = await client.get("/poll", params={"module": "rebounder"})
    second_button_activate = await client.get("/poll", params={"module": "button"})
    assert rebounder_deactivate.status_code == 200
    assert rebounder_deactivate.json() == {"cmd": "deactivate"}
    assert second_button_activate.status_code == 200
    assert second_button_activate.json() == {"cmd": "activate"}

    await client.post("/api/stop")


async def test_empty_repeat_loop_is_accepted_and_skipped(client: httpx.AsyncClient):
    response = await client.post("/api/run", json={"code": "for count in range(3):\n  pass\n"})
    assert response.status_code == 200

    await asyncio.sleep(0)
    app_state = client._transport.app.state.rbdr  # type: ignore[attr-defined]
    assert app_state.status == "idle"


@pytest.mark.parametrize(
    "code",
    [
        'for count in range(1):\n  for inner in range(1):\n    await rbdr.activate_and_wait("button")\n',
        'for count in range(0):\n  await rbdr.activate_and_wait("button")\n',
        'for count in range(21):\n  await rbdr.activate_and_wait("button")\n',
        'for count in range(1.5):\n  await rbdr.activate_and_wait("button")\n',
        'for count in range(-1):\n  await rbdr.activate_and_wait("button")\n',
        'for count in range(times):\n  await rbdr.activate_and_wait("button")\n',
        'for count in range(1):\n  print("nope")\n',
    ],
)
async def test_unsupported_repeat_shapes_are_rejected(client: httpx.AsyncClient, code: str):
    response = await client.post("/api/run", json={"code": code})
    assert response.status_code == 400
