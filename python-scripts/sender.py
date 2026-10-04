#!/usr/bin/env python3
"""TCP client that zips a file, base64-encodes it, and streams it over TCP."""

from __future__ import annotations

import argparse
import base64
import io
import select
import shutil
import signal
import socket
import sys
import time
import zipfile
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

RECONNECT_DELAY = 2.0
DEFAULT_START_DELAY = 1.0
DEFAULT_INTERVAL = 0.0
DEFAULT_BURST = 32
DEFAULT_RATE = 160.0
READY_POLL_INTERVAL = 0.1
READY_STATUS_INTERVAL = 2.0
FINAL_ACK_TIMEOUT = 30.0
FILE_END_CONTROL_PREFIX = b"\x1eEND "
RATE_CONTROL_PREFIX = b"\x1eRATE "
FILE_START_MARKER_PREFIX = "$$$LOAD-"
FILE_START_MARKER_SUFFIX = "%%%"
EXCLUDED_DIR_NAMES = [".git", ".gitignore", ".vscode", "__pycache__"]
EXCLUDED_FILE_NAMES = [".gitignore"]
MIN_INLINE_PROGRESS_WIDTH = 80
ANSI_RED = "\033[31m"
ANSI_RESET = "\033[0m"


@dataclass(frozen=True)
class PayloadInfo:
  payload: bytes
  source_bytes: int
  zip_bytes: int
  base64_bytes: int


@dataclass(frozen=True)
class TransferItem:
  source_path: Path
  archive_relpath: Path


def _red(text: str) -> str:
  return f"{ANSI_RED}{text}{ANSI_RESET}"


def _pause_on_mismatch() -> None:
  try:
    input("[sender] Press Enter to exit...")
  except EOFError:
    pass


def _terminal_width(default: int = 120) -> int:
  try:
    return max(MIN_INLINE_PROGRESS_WIDTH, shutil.get_terminal_size((default, 20)).columns)
  except OSError:
    return default


def _emit_progress(message: str, progress_width: int) -> int:
  inline_limit = max(40, _terminal_width() - 1)
  if len(message) <= inline_limit:
    progress_width = max(progress_width, len(message))
    sys.stdout.write(f"\r{message:<{progress_width}}")
    sys.stdout.flush()
    return progress_width
  if progress_width:
    sys.stdout.write("\r" + (" " * progress_width) + "\r")
  sys.stdout.write(message + "\n")
  sys.stdout.flush()
  return 0


def build_parser() -> argparse.ArgumentParser:
  parser = argparse.ArgumentParser(
      description="Zip a file, base64-encode it, and stream it to a TCP receiver.",
      formatter_class=argparse.RawDescriptionHelpFormatter,
      epilog=(
          "Examples:\n"
          "  python scripts/sender.py scripts/email.txt --host 192.168.1.139\n"
          "  python scripts/sender.py scripts --host 192.168.1.139\n"
          "  python scripts/sender.py firmware.bin image.jpg --host 192.168.1.139 --port 9000\n"
          "  python scripts/sender.py payloads report.pdf --host esp32-airtype.local --rate 80 --burst 16\n"
      ),
  )
  parser.add_argument(
      "inputs",
      nargs="+",
      type=Path,
      help="One or more files or folders to stream.",
  )
  parser.add_argument(
      "--host",
      default="192.168.1.139",
      help="Receiver hostname or IP (default: %(default)s)",
  )
  parser.add_argument(
      "--port",
      type=int,
      default=9000,
      help="Receiver TCP port (default: %(default)s)",
  )
  parser.add_argument(
      "--interval",
      type=float,
      default=DEFAULT_INTERVAL,
      help=(
          "Seconds between bursts (0 to disable, combined with --rate when set, "
          "default: %(default)s)"
      ),
  )
  parser.add_argument(
      "--burst",
      type=int,
      default=DEFAULT_BURST,
      help=(
          "Number of characters to send per interval (default: %(default)s). "
          "Smaller bursts smooth out typing."
      ),
  )
  parser.add_argument(
      "-rate",
      "--rate",
      type=float,
      default=DEFAULT_RATE,
      help=(
          "Keyboard typing rate in characters per second (default: "
          "%(default)s)"
      ),
  )
  parser.add_argument(
      "--connect-timeout",
      type=float,
      default=5.0,
      help="Seconds to wait when opening the socket (default: %(default)s)",
  )
  parser.add_argument(
      "--start-delay",
      type=float,
      default=DEFAULT_START_DELAY,
      help="Seconds to wait after connecting before streaming (default: %(default)s)",
  )
  return parser


def _sleep_with_stop(duration: float, stop_flag: "StopFlag") -> None:
  """Sleep up to duration seconds, bailing out early if stop_flag is set."""
  end = time.monotonic() + max(0.0, duration)
  while not stop_flag.is_set:
    remaining = end - time.monotonic()
    if remaining <= 0:
      break
    time.sleep(min(0.1, remaining))


def _build_payload(item: TransferItem) -> PayloadInfo:
  archive_name = item.archive_relpath.as_posix()
  source_bytes = item.source_path.stat().st_size
  with io.BytesIO() as buffer:
    with zipfile.ZipFile(
        buffer, mode="w", compression=zipfile.ZIP_DEFLATED, compresslevel=9
    ) as zf:
      zf.writestr(archive_name, item.source_path.read_bytes())
    zipped = buffer.getvalue()
  payload = base64.b64encode(zipped)
  print(
      f"[sender] Loaded {item.source_path} as {archive_name} "
      f"({source_bytes} bytes -> zip {len(zipped)} bytes -> "
      f"{len(payload)} base64 bytes)"
  )
  return PayloadInfo(
      payload=payload,
      source_bytes=source_bytes,
      zip_bytes=len(zipped),
      base64_bytes=len(payload),
  )


def _marker_archive_path(item: TransferItem) -> str:
  zip_relpath = item.archive_relpath.parent / f"{item.archive_relpath.name}.zip"
  return zip_relpath.as_posix().replace("/", "\\")


def _encode_marker_path(raw_path: str) -> str:
  return base64.b64encode(raw_path.encode("utf-8")).decode("ascii")


def _encode_marker_int(value: int) -> str:
  return base64.b64encode(str(max(0, value)).encode("ascii")).decode("ascii")


def _build_end_marker(item: TransferItem) -> bytes:
  marker_path = _encode_marker_path(f"D:\\Temp\\{_marker_archive_path(item)}")
  marker = f"@@@SAVE-{marker_path}###"
  return marker.encode("ascii")


def _build_start_marker(item: TransferItem, payload_info: PayloadInfo) -> bytes:
  marker_path = _encode_marker_path(f"D:\\Temp\\{_marker_archive_path(item)}")
  marker_source_bytes = _encode_marker_int(payload_info.source_bytes)
  marker_payload_bytes = _encode_marker_int(payload_info.base64_bytes)
  marker = (
      f"{FILE_START_MARKER_PREFIX}{marker_path}!{marker_source_bytes}!{marker_payload_bytes}"
      f"{FILE_START_MARKER_SUFFIX}"
  )
  return marker.encode("ascii")


def _build_file_end_control(total_chars: int) -> bytes:
  return FILE_END_CONTROL_PREFIX + str(total_chars).encode("ascii") + b"\n"


def _build_rate_control(rate: float) -> bytes:
  rate_cps = max(1, int(round(rate)))
  return RATE_CONTROL_PREFIX + str(rate_cps).encode("ascii") + b"\n"


def _expand_inputs(inputs: list[Path]) -> list[TransferItem]:
  files: list[TransferItem] = []
  for input_path in inputs:
    if not input_path.exists():
      sys.exit(f"[sender] Input path not found: {input_path}")
    if input_path.is_file():
      if input_path.name in EXCLUDED_FILE_NAMES:
        continue
      files.append(TransferItem(source_path=input_path, archive_relpath=Path(input_path.name)))
      continue
    if input_path.is_dir():
      for path in sorted(candidate for candidate in input_path.rglob("*") if candidate.is_file()):
        if any(part in EXCLUDED_DIR_NAMES for part in path.relative_to(input_path).parts[:-1]):
          continue
        if path.name in EXCLUDED_FILE_NAMES:
          continue
        files.append(
            TransferItem(
                source_path=path,
                archive_relpath=Path(input_path.name) / path.relative_to(input_path),
            )
        )
      continue
    sys.exit(f"[sender] Unsupported input path: {input_path}")
  if not files:
    sys.exit("[sender] No files found to send.")
  return files


def _drain_ready_state(
    sock: socket.socket,
    pending: str,
    ready: bool,
    saw_state: bool,
    accepted_ack: int,
    typed_ack: int,
    buffer_free: int,
    finished_typed: int,
    file_done_typed: int,
    file_done_expected: int,
    file_done_seen: bool,
    rate_ack_cps: int,
    rate_ack_seen: bool,
) -> tuple[str, bool, bool, int, int, int, int, int, int, bool, int, bool]:
  """Read newline-delimited readiness state from the receiver.

  `1` means ready to accept input, `0` means wait.
  `E <n>` means the receiver has accepted n payload characters into RAM.
  `A <n>` means the receiver has typed n payload characters.
  `B <n>` means the receiver has n free bytes in its input buffer.
  `F <n>` means the receiver's input buffer drained after typing n characters.
  `D <typed> <expected>` means the receiver finished this file.
  `R <cps> <interval_us>` means the receiver applied the typing rate.
  """
  while True:
    readable, _, _ = select.select([sock], [], [], 0)
    if not readable:
      break
    data = sock.recv(128)
    if not data:
      raise ConnectionError("socket closed by remote host")
    pending += data.decode("ascii", errors="ignore")
    while "\n" in pending:
      line, pending = pending.split("\n", 1)
      line = line.strip()
      if line == "1":
        if not ready:
          print("[sender] Receiver ready. Resuming transfer.")
        ready = True
        saw_state = True
      elif line == "0":
        if ready:
          print("[sender] Receiver not in input mode. Pausing transfer.")
        else:
          print("[sender] Receiver not in input mode. Waiting...")
        ready = False
        saw_state = True
      elif line.startswith("A "):
        try:
          typed_ack = max(typed_ack, int(line.split(maxsplit=1)[1]))
        except ValueError:
          pass
      elif line.startswith("E "):
        try:
          accepted_ack = max(accepted_ack, int(line.split(maxsplit=1)[1]))
        except ValueError:
          pass
      elif line.startswith("B "):
        try:
          buffer_free = max(0, int(line.split(maxsplit=1)[1]))
        except ValueError:
          pass
      elif line.startswith("F "):
        try:
          finished_typed = max(finished_typed, int(line.split(maxsplit=1)[1]))
        except ValueError:
          pass
      elif line.startswith("D "):
        parts = line.split()
        if len(parts) == 3:
          try:
            file_done_typed = int(parts[1])
            file_done_expected = int(parts[2])
            file_done_seen = True
          except ValueError:
            pass
      elif line.startswith("R "):
        parts = line.split()
        if len(parts) >= 2:
          try:
            rate_ack_cps = int(parts[1])
            rate_ack_seen = True
          except ValueError:
            pass
  return (
      pending,
      ready,
      saw_state,
      accepted_ack,
      typed_ack,
      buffer_free,
      finished_typed,
      file_done_typed,
      file_done_expected,
      file_done_seen,
      rate_ack_cps,
      rate_ack_seen,
  )


def stream_forever(
    host: str,
    port: int,
    interval: float,
    connect_timeout: float,
    payload: bytes,
    label: str,
    batch_label: str,
    burst: int,
    start_delay: float,
    rate: float,
    stop_flag: "StopFlag",
) -> None:
  """Open a TCP connection and stream payload bytes."""
  total_chars = len(payload)
  file_end_control = _build_file_end_control(total_chars)
  rate_control = _build_rate_control(rate)
  requested_rate_cps = max(1, int(round(rate)))
  progress_width = 0

  while not stop_flag.is_set:
    sock: Optional[socket.socket] = None
    index = 0
    accepted_ack = 0
    typed_ack = 0
    try:
      print(f"[sender] Connecting to {host}:{port} ...")
      sock = socket.create_connection((host, port), timeout=connect_timeout)
      sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
      print(f"[sender] Connected to {host}:{port} for {label}")
      if start_delay > 0:
        print(f"[sender] Waiting {start_delay:.1f}s before transmitting...")
        _sleep_with_stop(start_delay, stop_flag)

      next_send = time.monotonic()
      index = 0
      pending_state = ""
      receiver_ready = False
      saw_ready_state = False
      accepted_ack = 0
      typed_ack = 0
      buffer_free = 0
      finished_typed = 0
      file_end_sent = False
      file_done_typed = 0
      file_done_expected = 0
      file_done_seen = False
      rate_control_sent = False
      rate_ack_cps = 0
      rate_ack_seen = False
      file_end_sent_at: Optional[float] = None
      last_waiting_status = 0.0
      last_rate_sample_time = time.monotonic()
      last_rate_sample_typed = 0
      measured_typed_rate = 0.0
      while not stop_flag.is_set:
        (
            pending_state,
            receiver_ready,
            saw_ready_state,
            accepted_ack,
            typed_ack,
            buffer_free,
            finished_typed,
            file_done_typed,
            file_done_expected,
            file_done_seen,
            rate_ack_cps,
            rate_ack_seen,
        ) = _drain_ready_state(
            sock,
            pending_state,
            receiver_ready,
            saw_ready_state,
            accepted_ack,
            typed_ack,
            buffer_free,
            finished_typed,
            file_done_typed,
            file_done_expected,
            file_done_seen,
            rate_ack_cps,
            rate_ack_seen,
        )
        now = time.monotonic()
        if now - last_rate_sample_time >= 1.0:
          typed_delta = typed_ack - last_rate_sample_typed
          time_delta = now - last_rate_sample_time
          if time_delta > 0:
            measured_typed_rate = typed_delta / time_delta
          last_rate_sample_typed = typed_ack
          last_rate_sample_time = now
        if receiver_ready and not rate_control_sent:
          sock.sendall(rate_control)
          rate_control_sent = True
          rate_ack_cps = 0
          rate_ack_seen = False
        if rate_control_sent and not rate_ack_seen:
          _sleep_with_stop(READY_POLL_INTERVAL, stop_flag)
          continue
        if rate_ack_seen and rate_ack_cps != requested_rate_cps:
          print()
          print(_red(
              f"[sender] ESP32 applied typing rate {rate_ack_cps} cps, "
              f"but sender requested {requested_rate_cps} cps."
          ))
          _pause_on_mismatch()
          stop_flag.set()
          return
        if index >= total_chars and not file_end_sent:
          sock.sendall(file_end_control)
          file_end_sent = True
          remaining_chars = max(0, total_chars - typed_ack)
          drain_seconds = remaining_chars / max(1.0, float(rate_ack_cps or requested_rate_cps))
          file_end_sent_at = time.monotonic() + max(FINAL_ACK_TIMEOUT, drain_seconds * 2.0 + 10.0)
        if file_end_sent and finished_typed >= total_chars and file_done_seen:
          clear = " " * max(0, progress_width)
          sys.stdout.write(f"\r{clear}\r")
          sys.stdout.flush()
          sent_ok = index == total_chars
          accepted_ok = accepted_ack == total_chars
          typed_ok = typed_ack == total_chars
          buffer_done_ok = finished_typed == total_chars
          file_done_ok = (
              file_done_typed == total_chars and file_done_expected == total_chars
          )
          ok = sent_ok and accepted_ok and typed_ok and buffer_done_ok and file_done_ok
          status = "OK" if ok else _red("MISMATCH")
          print(
              f"[sender] Result for {label}: {status} "
              f"sent={index}/{total_chars}, "
              f"accepted={accepted_ack}/{total_chars}, "
              f"typed={typed_ack}/{total_chars}, "
              f"buffer_done={finished_typed}/{total_chars}, "
              f"file_done={file_done_typed}/{file_done_expected}"
          )
          if not ok:
            print(_red("[sender] Transfer counts do not match. Leaving console paused."))
            _pause_on_mismatch()
            stop_flag.set()
          print(f"[sender] Finished sending {label}.")
          return
        if file_end_sent_at is not None and time.monotonic() > file_end_sent_at:
          print()
          print(_red(
              f"[sender] Timed out waiting for final ACKs for {label}: "
              f"sent={index}/{total_chars}, accepted={accepted_ack}/{total_chars}, "
              f"typed={typed_ack}/{total_chars}, buffer_done={finished_typed}/{total_chars}, "
              f"file_done={file_done_typed}/{file_done_expected}"
          ))
          _pause_on_mismatch()
          stop_flag.set()
          return
        if index >= total_chars:
          progress = (
              f"[sender] {batch_label} {label} | "
              f"tx {index}/{total_chars} | "
              f"typed {typed_ack}/{total_chars} | "
              f"buf {(accepted_ack / total_chars) * 100.0:5.1f}% | "
              f"type {(typed_ack / total_chars) * 100.0:5.1f}% | "
              f"rate {measured_typed_rate:5.0f}/{rate_ack_cps or requested_rate_cps} cps | "
              f"wait {finished_typed}/{total_chars} {file_done_typed}/{total_chars}"
          )
          progress_width = _emit_progress(progress, progress_width)
          _sleep_with_stop(READY_POLL_INTERVAL, stop_flag)
          continue
        if not receiver_ready:
          now = time.monotonic()
          if not saw_ready_state and now - last_waiting_status >= READY_STATUS_INTERVAL:
            print("[sender] No readiness state from receiver yet. Waiting...")
            last_waiting_status = now
          elif saw_ready_state and now - last_waiting_status >= READY_STATUS_INTERVAL:
            print("[sender] Receiver not in input mode. Waiting...")
            last_waiting_status = now
          _sleep_with_stop(READY_POLL_INTERVAL, stop_flag)
          continue
        now = time.monotonic()
        if now < next_send:
          _sleep_with_stop(min(0.01, next_send - now), stop_flag)
          continue
        receiver_buffered = max(0, index - accepted_ack)
        send_window = max(0, buffer_free - receiver_buffered)
        if send_window <= 0:
          _sleep_with_stop(READY_POLL_INTERVAL, stop_flag)
          continue
        send_count = min(burst, send_window, total_chars - index)
        chunk = payload[index:index + send_count]
        index += send_count
        sock.sendall(chunk)
        percent = (index / total_chars) * 100.0
        progress = (
            f"[sender] {batch_label} {label} | "
            f"tx {index}/{total_chars} ({percent:5.1f}%) | "
            f"typed {typed_ack}/{total_chars} ({(typed_ack / total_chars) * 100.0:5.1f}%) | "
            f"buf {(accepted_ack / total_chars) * 100.0:5.1f}% | "
            f"rate {measured_typed_rate:5.0f}/{rate_ack_cps or requested_rate_cps} cps"
        )
        progress_width = _emit_progress(progress, progress_width)
        delay = interval if interval > 0 else 0.0
        if rate > 0:
          delay = max(delay, send_count / rate)
        if delay > 0:
          next_send = time.monotonic() + delay
    except KeyboardInterrupt:
      stop_flag.set()
    except (ConnectionError, OSError) as exc:
      if stop_flag.is_set:
        break
      if index > 0 or accepted_ack > 0 or typed_ack > 0:
        print()
        print(_red(
            f"[sender] Socket error after transfer started for {label}: {exc!r}. "
            "Not retrying because that could duplicate already-typed characters."
        ))
        print(_red(
            f"[sender] Last counts: sent={index}/{total_chars}, "
            f"accepted={accepted_ack}/{total_chars}, typed={typed_ack}/{total_chars}"
        ))
        _pause_on_mismatch()
        stop_flag.set()
        return
      print(
          f"[sender] Socket error: {exc!r}. Retrying in {RECONNECT_DELAY:.0f} seconds..."
      )
      _sleep_with_stop(RECONNECT_DELAY, stop_flag)
    finally:
      if sock:
        try:
          sock.close()
        except OSError:
          pass


class StopFlag:
  def __init__(self) -> None:
    self.is_set = False

  def set(self) -> None:
    self.is_set = True


def main() -> None:
  parser = build_parser()
  args = parser.parse_args()
  files_to_send = _expand_inputs(args.inputs)

  stop_flag = StopFlag()

  def handle_stop(signum: int, _frame: Optional[object]) -> None:
    print(f"[sender] Received signal {signum}, shutting down...")
    stop_flag.set()

  for sig in (signal.SIGINT, signal.SIGTERM):
    signal.signal(sig, handle_stop)

  try:
    total_files = len(files_to_send)
    print(f"[sender] Total files to send: {total_files}")
    for index, item in enumerate(files_to_send, start=1):
      if stop_flag.is_set:
        break
      label = item.archive_relpath.as_posix()
      batch_label = f"File {index}/{total_files}"
      print(f"[sender] Preparing {label}")
      payload_info = _build_payload(item)
      payload = (
          _build_start_marker(item, payload_info)
          + payload_info.payload
          + _build_end_marker(item)
      )
      stream_forever(
          host=args.host,
          port=args.port,
          interval=args.interval,
          connect_timeout=args.connect_timeout,
          payload=payload,
          label=label,
          batch_label=batch_label,
          burst=args.burst,
          start_delay=args.start_delay,
          rate=args.rate,
          stop_flag=stop_flag,
      )
  except KeyboardInterrupt:
    stop_flag.set()
  finally:
    print("[sender] Stopped.")


if __name__ == "__main__":
  if sys.version_info < (3, 8):
    sys.exit("Python 3.8+ is required.")
  main()
