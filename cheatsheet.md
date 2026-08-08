# RBDR Debugging Cheatsheet

## Blockly Side

### Start the backend

From the repo root:

```sh
cd blockly-rbdr-app
python3 -m venv .venv
source .venv/bin/activate
python -m pip install -e ".[dev]"
uvicorn backend.main:app --reload --host 0.0.0.0 --port 8000
```

The backend listens on port `8000`. Binding to `0.0.0.0` lets ESP32 modules reach it from the Wi-Fi network.

### Start the Blockly frontend

In another shell:

```sh
cd blockly-rbdr-app
npm install
npm run dev
```

Open the Vite URL, usually:

```text
http://localhost:5173
```

The Vite dev server proxies app API calls to `http://127.0.0.1:8000`.

### Put the computer and modules on the same Wi-Fi

The firmware runs as a Wi-Fi station and connects to the SSID/password compiled into the module.

Default firmware values are placeholders:

```text
RBDR_WIFI_SSID=example-network-name
RBDR_WIFI_PASSWORD=example-pwd
RBDR_SERVER_BASE_URL=http://192.168.4.1
```

Use a Wi-Fi network that both the computer and ESP32 can join. Common setups:

- Computer joins a normal router Wi-Fi network, and the ESP32 joins the same SSID.
- Computer creates a hotspot, and the ESP32 joins that hotspot.

Find the computer IP address on that Wi-Fi network, then compile the module with:

```sh
cd button-module
idf.py -DRBDR_WIFI_SSID="your-ssid" \
  -DRBDR_WIFI_PASSWORD="your-password" \
  -DRBDR_SERVER_BASE_URL="http://COMPUTER_WIFI_IP:8000" \
  build
```

For an open network, use an empty password:

```sh
idf.py -DRBDR_WIFI_SSID="your-ssid" \
  -DRBDR_WIFI_PASSWORD="" \
  -DRBDR_SERVER_BASE_URL="http://COMPUTER_WIFI_IP:8000" \
  build
```

If the firmware is still using the repo default `http://192.168.4.1`, make sure the backend computer is actually reachable at `192.168.4.1` on the module's Wi-Fi network.

### Manually check backend endpoints

Set this once:

```sh
export RBDR_BACKEND=http://127.0.0.1:8000
```

Poll for a command:

```sh
curl -i "$RBDR_BACKEND/poll?module=button"
curl -i "$RBDR_BACKEND/poll?module=rebounder"
```

Expected result when no command is queued:

```text
HTTP/1.1 204 No Content
```

Queue a button activation:

```sh
curl -i -X POST "$RBDR_BACKEND/api/run" \
  -H "Content-Type: application/json" \
  -d '{"code":"await rbdr.activate_and_wait(\"button\")\n"}'
```

Expected response:

```json
{"status":"started"}
```

Poll again:

```sh
curl -i "$RBDR_BACKEND/poll?module=button"
```

Expected response:

```json
{"cmd":"activate"}
```

Simulate the module reporting a button press:

```sh
curl -i -X POST "$RBDR_BACKEND/events" \
  -H "Content-Type: application/json" \
  -d '{"event":"triggered","module":"button","active":true,"triggered":true,"uptime_ms":1234}'
```

Expected response:

```json
{"status":"accepted"}
```

Poll once more:

```sh
curl -i "$RBDR_BACKEND/poll?module=button"
```

Expected response:

```json
{"cmd":"deactivate"}
```

Stop or reset a run:

```sh
curl -i -X POST "$RBDR_BACKEND/api/stop"
```

Expected response:

```json
{"status":"stopped"}
```

Bad module names return `404`:

```sh
curl -i "$RBDR_BACKEND/poll?module=nope"
```

Unsupported generated code returns `400`:

```sh
curl -i -X POST "$RBDR_BACKEND/api/run" \
  -H "Content-Type: application/json" \
  -d '{"code":"print(\"nope\")\n"}'
```

Starting a second run while one is active returns `409`.

### Watch Blockly-side logs

The frontend connects to:

```text
ws://<vite-host>/api/ws
```

Useful log messages in the app:

- `activating button`
- `button polled command: activate`
- `button event: triggered`
- `button triggered`
- `deactivating button`
- `run complete`
- `run error: button module is not connected`

If a run immediately errors with `module is not connected`, the backend has not seen a recent `/poll?module=...` from that module. The freshness window is `5` seconds.

## Module Side

### ESP-IDF setup

Make sure the ESP-IDF environment is loaded before running `idf.py`. For a standard ESP-IDF install:

```sh
source "$IDF_PATH/export.sh"
```

If `IDF_PATH` is not set, source the `export.sh` from your ESP-IDF checkout.

### Clean

From the module directory:

```sh
cd button-module
idf.py clean
```

For a full CMake/config cleanup:

```sh
idf.py fullclean
```

`fullclean` removes generated build files, so the next build will regenerate them.

### Build

```sh
idf.py build
```

Build with Wi-Fi/server overrides:

```sh
idf.py -DRBDR_WIFI_SSID="your-ssid" \
  -DRBDR_WIFI_PASSWORD="your-password" \
  -DRBDR_SERVER_BASE_URL="http://COMPUTER_WIFI_IP:8000" \
  build
```

Hardware defaults from `button-module/main/CMakeLists.txt`:

```text
RBDR_BUTTON_GPIO=4
RBDR_LED_GPIO=48
RBDR_LED_COUNT=1
RBDR_COMMS_PERIOD_MS=50
RBDR_SENSE_PERIOD_MS=10
RBDR_CONTROL_PERIOD_MS=20
```

Override them the same way if needed:

```sh
idf.py -DRBDR_BUTTON_GPIO=4 -DRBDR_LED_GPIO=48 build
```

### Flash

Auto-detect the serial port:

```sh
idf.py flash
```

Specify the port:

```sh
idf.py -p /dev/cu.usbmodemXXXX flash
```

On Linux, the port is usually something like:

```text
/dev/ttyACM0
/dev/ttyUSB0
```

On macOS, it is usually something like:

```text
/dev/cu.usbmodemXXXX
/dev/cu.usbserial-XXXX
```

### See serial log output

Open the ESP-IDF monitor:

```sh
idf.py monitor
```

Or flash and monitor in one command:

```sh
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

Monitor baud rate is `115200` in `button-module/sdkconfig`.

Exit the monitor with:

```text
Ctrl+]
```

Useful firmware log lines:

```text
I button: wifi connected
W button: wifi disconnected, retrying
W button: poll failed: ...
W button: unexpected poll status: ...
W button: invalid command json: ...
I button: posted event status=200 body=...
W button: post failed: ...
```

### End-to-end smoke test

1. Start the backend on `0.0.0.0:8000`.
2. Start the frontend and open the Vite URL.
3. Build and flash the module with the correct `RBDR_WIFI_SSID`, `RBDR_WIFI_PASSWORD`, and `RBDR_SERVER_BASE_URL`.
4. Open `idf.py monitor` and wait for `wifi connected`.
5. In Blockly, run a program with a `button` block.
6. The module should poll `{"cmd":"activate"}`.
7. Press the button.
8. The module should post a `triggered` event.
9. The backend should queue `{"cmd":"deactivate"}` and finish the run.

If step 5 immediately fails, the module is not polling the backend. Check Wi-Fi credentials, computer IP address, firewall settings, and the compiled `RBDR_SERVER_BASE_URL`.
