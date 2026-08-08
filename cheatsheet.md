# RBDR Cheatsheet

## Blockly

Serve the app and open it in your browser:

```sh
cd blockly-rbdr-app
npm install
npm run dev -- --open
```

## Module

Build with critical options configured:

```sh
cd rbdr-module
idf.py -DRBDR_WIFI_SSID="your-ssid" \
  -DRBDR_WIFI_PASSWORD="your-password" \
  -DRBDR_SERVER_BASE_URL="http://COMPUTER_WIFI_IP:8000" \
  build
```

Flash and monitor in the same command:

```sh
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

Quit the flash/monitor screen:

```text
Ctrl+]
```
