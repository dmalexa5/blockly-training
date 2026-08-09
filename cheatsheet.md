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
cd module
idf.py -DMODULE_TYPE=button \
  -DMODULE_NAME="button_left" \
  -DWIFI_SSID="your-ssid" \
  -DWIFI_PASSWORD="your-password" \
  -DSERVER_BASE_URL="http://COMPUTER_WIFI_IP:8000" \
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
