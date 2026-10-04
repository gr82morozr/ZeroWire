# ZeroWire

ZeroWire is a file-transfer workspace built around an ESP32-S3 USB HID keyboard device.
It supports two main delivery modes:

- direct TCP-to-keyboard streaming, where a host sends text payloads to the ESP32 and the board types them into the target machine
- QR session transfer, where a sender shows framed QR pages and a receiver webcam app reconstructs the files while using the ESP32 keyboard to advance, repeat, or navigate pages

## PlatformIO Workspace

Open `ZeroWire.code-workspace` in VS Code and install the recommended PlatformIO IDE extension. Only `esp32-keyboard/` is a PlatformIO project. The workspace includes it as a separate folder for the LOLIN S3 Mini firmware with the Arduino framework.

Edit the firmware's Wi-Fi settings before deploying. In **Terminal > Run Task**, use:

- **PlatformIO: Build ESP32 Keyboard** (also Ctrl+Shift+B)
- **PlatformIO: Upload ESP32 Keyboard** for USB flashing
- **PlatformIO: Upload ESP32 Keyboard (OTA)** for wireless updates
- **PlatformIO: Monitor ESP32 Keyboard** for USB serial output

From a PlatformIO terminal in `esp32-keyboard/`:

```sh
pio run
pio run -e esp32-s3-com -t upload
pio device monitor -b 115200
pio run -e esp32-s3-ota -t upload
```

USB is the default environment. If automatic port detection picks the wrong device, append `--upload-port COM5` to the USB upload command (replace `COM5` with your device's port). List ports with `pio device list`.

For OTA, first flash over USB and ensure the board connects to Wi-Fi. The OTA target is `esp32-keyboard.local`; if name resolution fails, append `--upload-port <device-ip>` to the OTA command. Close the serial monitor before USB flashing.

Board and environment settings live in `esp32-keyboard/platformio.ini`. Build output stays in `esp32-keyboard/.pio/`. You can also open `esp32-keyboard/` directly in VS Code.

## Repository Layout

### `esp32-keyboard/`

PlatformIO firmware for the ESP32-S3 keyboard bridge.

Main functions:

- exposes a USB HID keyboard to the target machine
- connects to Wi-Fi and supports OTA updates
- accepts TCP input from host tools on port `9000`
- buffers incoming text and types it out at a controlled rate
- reports flow-control status back to the sender
- accepts control frames such as `RATE`, `END`, `KEY SPACE`, `KEY LEFT`, and `KEY RIGHT`
- supports INPUT mode behavior controlled by the device button and status LED

Primary source:

- [esp32-keyboard/src/main.cpp](/d:/projects.git/ZeroWire/esp32-keyboard/src/main.cpp)

### `python-scripts/`

Host-side utilities for generating or receiving transfer payloads.

Main scripts:

- [python-scripts/sender.py](/d:/projects.git/ZeroWire/python-scripts/sender.py)
  Sends one or more files to the ESP32 over TCP. Each file is zipped, base64-encoded, flow-controlled, and acknowledged before the next file starts.
- [python-scripts/f2q.py](/d:/projects.git/ZeroWire/python-scripts/f2q.py)
  Builds a full QR transfer session in memory, caches all QR pages, and displays them in GUI or console mode. Frames include session metadata, file metadata, per-file QR sequence, and session-wide QR sequence.
- [python-scripts/qr_2_file.py](/d:/projects.git/ZeroWire/python-scripts/qr_2_file.py)
  Older Python QR receiver workflow kept for reference and comparison with the native C++ receiver.
- [python-scripts/input_box.py](/d:/projects.git/ZeroWire/python-scripts/input_box.py)
  Receives typed payloads, detects save markers, and reconstructs transferred files.

Supporting items:

- `config.json`: local script/runtime settings
- `received_qr/`: captured or generated QR-related working files

### `cpp-qrcode-reader/`

Native Windows 11 QR receiver application.

Main functions:

- captures frames from a webcam
- detects and decodes QR pages
- accepts only valid framed protocol messages
- starts only on a valid `SESSION_START`
- ignores malformed frames and mismatched session frames until the correct next frame appears
- reconstructs transferred files from framed data chunks
- extracts received ZIP payloads into `qrcode-reader.incoming`
- sends page navigation commands back through the ESP32 keyboard bridge
- emits a per-frame beep and stays running until closed or until the active session completes

Build output:

- [cpp-qrcode-reader/build/Release/X64/qrcode-reader.exe](/d:/projects.git/ZeroWire/cpp-qrcode-reader/build/Release/X64/qrcode-reader.exe)

Primary source:

- [cpp-qrcode-reader/src/main.cpp](/d:/projects.git/ZeroWire/cpp-qrcode-reader/src/main.cpp)

## Typical Workflows

### 1. ESP32 typed file transfer

Use `python-scripts/sender.py` to send files to the ESP32 over TCP. The board types the payload into the target machine as USB HID keyboard input.

### 2. QR session transfer

Use `python-scripts/f2q.py` to display a full QR session. Use `cpp-qrcode-reader/qrcode-reader.exe` on the receiving machine to scan and reconstruct the files. The receiver uses the ESP32 keyboard bridge to send `SPACE`, `LEFT`, and `RIGHT` navigation commands back to the sender flow.

### 3. OTA firmware update

Build and upload the ESP32 firmware with PlatformIO. The project supports both USB flashing and OTA once the board has already been flashed with Wi-Fi-capable firmware.

## OTA Notes

- `esp32-s3-ota` only works after the board has already been flashed once over USB with firmware that reaches Wi-Fi and calls `ArduinoOTA.begin()`.
- The active OTA hostname is `esp32-keyboard.local`.
- If mDNS does not resolve on this machine, connect over USB with `esp32-s3-com` once, open the serial monitor, read the current IP from the boot log, and temporarily set `upload_port` to that IP.

## Entry Points

- Firmware PlatformIO config: [esp32-keyboard/platformio.ini](esp32-keyboard/platformio.ini)
- ESP32 firmware: [esp32-keyboard/src/main.cpp](/d:/projects.git/ZeroWire/esp32-keyboard/src/main.cpp)
- QR sender: [python-scripts/f2q.py](/d:/projects.git/ZeroWire/python-scripts/f2q.py)
- TCP sender: [python-scripts/sender.py](/d:/projects.git/ZeroWire/python-scripts/sender.py)
- Native QR receiver: [cpp-qrcode-reader/src/main.cpp](/d:/projects.git/ZeroWire/cpp-qrcode-reader/src/main.cpp)

Subfolder READMEs contain more implementation-specific details.
