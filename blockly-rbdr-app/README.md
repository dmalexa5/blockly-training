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

- `GET /poll?module=button_left`
- `GET /poll?module=button_right`
- `GET /poll?module=rebounder`
- `POST /events` with JSON like:

```json
{"event":"triggered","module":"button_left","active":true,"triggered":true,"uptime_ms":1234}
```

Module names are logical IDs configured in the backend module list. To flash the two physical
button modules, build the same firmware with the matching IDs:

```sh
idf.py -DRBDR_MODULE_NAME=button_left build flash
idf.py -DRBDR_MODULE_NAME=button_right build flash
```
