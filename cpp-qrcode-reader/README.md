# QRCode Reader (Windows)

This folder is isolated from the existing Python scripts and ESP32 firmware.

Purpose:
- host a future native C++ rewrite of `scripts/qr_2_file.py`
- target Windows 11
- avoid changing the current Python-based workflow while the C++ version is being developed

Intended scope:
- camera capture
- QR decode
- transfer/session state machine
- optional ESP32 TCP space sender
- file reconstruction and extract

Suggested build toolchain:
- Visual Studio 2022 or Build Tools
- CMake 3.24+
- vcpkg for dependencies such as OpenCV and zlib

Dependencies expected by `CMakeLists.txt`:
- `OpenCV` with `core`, `imgproc`, `highgui`, `objdetect`, `videoio`
- `nlohmann_json`
- Windows system libs:
  - `ws2_32`
  - `bcrypt`

Current layout:
- `CMakeLists.txt`: standalone build entry
- `src/main.cpp`: native Windows receiver app mirroring the QR transfer protocol
- `include/`: headers for the future receiver
- `third_party/`: optional local dependency area if needed later

This folder does not modify or depend on:
- `scripts/`
- `src/main.cpp` for ESP32
- PlatformIO configuration

Example configure/build:
```powershell
$cmake = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
& $cmake -S cpp-qrcode-reader -B cpp-qrcode-reader/build `
  -DCMAKE_TOOLCHAIN_FILE=C:/src/vcpkg/scripts/buildsystems/vcpkg.cmake `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static
& $cmake --build cpp-qrcode-reader/build --config Release
```

Release output:
- `cpp-qrcode-reader\build\Release\X64\qrcode-reader.exe`
- Static release builds use `x64-windows-static` so the exe does not need third-party DLLs beside it.
- A `config.json` is copied beside the exe during build.

Runtime config:
- The app reads `config.json` from the same folder as `qrcode-reader.exe`.
- If `config.json` is missing, the app creates one with defaults.
- Default `camera_index` is `1`.
- Default `incoming_dir` is `qrcode-reader.incoming` next to the exe.
- The receiver does not auto-quit on a frame limit or idle timeout. It keeps running until you close it or it finishes a valid session.
- `connect_timeout_seconds` only controls each ESP32 TCP connect attempt. It is not an app shutdown timer.

Current implementation notes:
- keeps the same framed JSON protocol as `scripts/qr_2_file.py`
- keeps the page-id sync behavior so sender/receiver stay aligned
- prefers a lean OpenCV-only decode path for better steady-state speed
- uses PowerShell `Expand-Archive` on Windows for ZIP extraction

This native receiver is intentionally isolated and can be iterated without touching the Python scripts.
