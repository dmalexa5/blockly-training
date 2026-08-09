# RBDR Blockly App

Proof-of-concept Blockly app for ESP32-backed statement blocks: left/right `button` modules and `rebounder`.

## Backend

```sh
python3 -m venv .venv
source .venv/bin/activate
python -m pip install -e ".[dev]"
uvicorn backend.main:app --reload --host 0.0.0.0 --port 8000
```

## Frontend

```sh
npm install
npm run dev
```

For local development, open the Vite URL. Vite proxies backend requests to `http://127.0.0.1:8000`.

## ESP32 Contract

- Backend calls each configured device `GET /health` before commands.
- Backend sends `POST /command` to the device with `{"cmd":"activate"}` or `{"cmd":"deactivate"}`.
- Devices send `POST /events` to the backend with JSON like:

```json
{"module":"button_left","event":"triggered"}
```

Module names are logical IDs configured in the backend module list. Build the shared firmware
from `module/` with the physical type and matching logical ID:

```sh
idf.py -DMODULE_TYPE=button -DMODULE_NAME=button_left build flash
idf.py -DMODULE_TYPE=button -DMODULE_NAME=button_right build flash
idf.py -DMODULE_TYPE=rebounder -DMODULE_NAME=rebounder build flash
```
