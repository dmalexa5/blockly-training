# RBDR Blockly App

Proof-of-concept Blockly app for two ESP32-backed statement blocks: `button` and `rebounder`.

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

- `GET /poll?module=button`
- `GET /poll?module=rebounder`
- `POST /events` with JSON like:

```json
{"event":"triggered","module":"button","active":true,"triggered":true,"uptime_ms":1234}
```

Module names are logical IDs configured in the backend module list. To flash two physical
button modules, build the same firmware with different IDs, for example:

```sh
idf.py -DRBDR_MODULE_NAME=button_left build flash
idf.py -DRBDR_MODULE_NAME=button_right build flash
```
