# Python Client

Utilities for interacting with the ESP32 firmware from a host computer.

## TCP Console Helper (`sender.py`)

`sender.py` uses only Python's standard library. Provide one or more files or
directories; directories are expanded recursively into files.

For each file, the sender:

1. Creates an in-memory ZIP containing that file.
2. Base64-encodes the ZIP.
3. Appends a typed save marker:
   `@@@SAVE-D:\\Temp\\<filename>.zip###`
4. Streams the resulting ASCII payload to the ESP32 over TCP.
5. Sends a TCP-only file-end control frame that the ESP32 does not type.
6. Waits for ESP32 per-file completion ACKs before moving to the next file.

```bash
python scripts/sender.py scripts/email.txt --host 192.168.1.136 --port 9000
python scripts/sender.py firmware.bin image.jpg --host 192.168.1.136
python scripts/sender.py payloads --host esp32-airtype.local -rate 120 --burst 16
```

Useful defaults:

- `--burst 32`
- `--rate 100` / `-rate 100`
- `--start-delay 1.0`
- `--connect-timeout 5.0`

## Flow Control Protocol

The ESP32 accepts input into a fixed 16 KB RAM ring buffer. A separate FreeRTOS
keyboard task drains that buffer at the HID typing rate.

ESP32-to-sender messages:

- `1`: INPUT mode ready.
- `0`: not in INPUT mode. Releasing the hardware button terminates the current
  TCP session and clears queued keyboard data on the ESP32.
- `B <free>`: free bytes remaining in the ESP32 input buffer.
- `E <accepted>`: total payload bytes accepted into the buffer.
- `A <typed>`: total payload bytes actually typed by USB HID keyboard.
- `F <typed>`: input buffer fully drained at this typed count.
- `D <typed> <expected>`: current file finished at this typed count.
- `R <cps> <interval_us>`: typing rate applied by the ESP32.

Sender-to-ESP32 control frame:

- `0x1E END <total_payload_chars>\n`: file-end marker. This frame is consumed
  by firmware and is not typed.
- `0x1E RATE <chars_per_second>\n`: typing-rate command. This frame is consumed
  by firmware and is not typed.

`sender.py` throttles sends using `B` and `E`, displays typed progress using
`A`, and does not continue to the next file until `F` and `D` match the expected
per-file payload length.

`-rate` / `--rate` controls the ESP32 keyboard typing rate. The default is
`100` characters per second. The sender waits for the `R` acknowledgement before
sending payload bytes.

If a mismatch, timeout, or socket failure happens after partial transfer,
`sender.py` prints the counts in red, pauses, and stops the remaining batch to
avoid duplicating already typed characters.

Releasing the ESP32 INPUT button during a transfer is an intentional abort. The
firmware closes the TCP connection and clears its keyboard buffer so the next
INPUT session cannot resume stale typing.

## Base64 Decoder (`decode_base64.py`)

`decode_base64.py` reads a base64-encoded file and writes decoded bytes to a new
file.

```bash
python scripts/decode_base64.py encoded.txt decoded.bin
```

The same workflow is available as a PowerShell script.

```powershell
pwsh -File scripts/decode_base64.ps1 -InputPath encoded.txt -OutputPath decoded.bin
```
