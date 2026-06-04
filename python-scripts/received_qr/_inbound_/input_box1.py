#!/usr/bin/env python3
"""Receive typed sender.py payloads, detect end markers, and extract files."""

from __future__ import annotations

import base64
import binascii
import io
import multiprocessing as mp
import queue
import re
import zipfile
from pathlib import Path
from tkinter import BOTH, WORD, Button, Label, StringVar, Tk
from tkinter import scrolledtext

START_MARKER_PREFIX = "$$$LOAD-"
START_MARKER_SUFFIX = "%%%"
START_MARKER_RE = re.compile(r"\$\$\$LOAD-(.*?)%%%", re.DOTALL)
END_MARKER_PREFIX = "@@@SAVE-"
END_MARKER_SUFFIX = "###"
END_MARKER_RE = re.compile(r"@@@SAVE-(.*?)###", re.DOTALL)
ZIP_TEMP_DIR = Path(r"D:\Temp\InpueBox")
INBOUND_DIR = Path("Z:/Tools/_inbound_")
POLL_INTERVAL_MS = 60
MAX_STATUS_LINES = 10
WINDOW_WIDTH = 760
WINDOW_HEIGHT = 220
TEXT_WIDTH = 84
TEXT_HEIGHT = 1
STATUS_HEIGHT = 4
PAYLOAD_SANITIZE_RE = re.compile(r"\s+")
BASE64_BODY_RE = re.compile(r"[^A-Za-z0-9+/=]")
VISIBLE_TEXT_LIMIT = 200


def decode_marker_path(encoded_path: str) -> str:
    raw = encoded_path.strip()
    raw += "=" * ((4 - (len(raw) % 4)) % 4)
    try:
        return base64.b64decode(raw.encode("ascii"), validate=False).decode("utf-8")
    except (binascii.Error, UnicodeDecodeError, ValueError):
        # Keep compatibility with older non-base64 senders.
        return encoded_path


def decode_marker_int(encoded_value: str) -> int | None:
    raw = encoded_value.strip()
    if not raw:
        return None
    raw += "=" * ((4 - (len(raw) % 4)) % 4)
    try:
        decoded = base64.b64decode(raw.encode("ascii"), validate=False).decode("ascii")
        return max(0, int(decoded.strip()))
    except (binascii.Error, UnicodeDecodeError, ValueError):
        return None


def parse_start_marker_payload(raw_marker_payload: str) -> tuple[str, int | None, int | None]:
    parts = raw_marker_payload.split("!")
    path_part = parts[0]
    source_bytes = decode_marker_int(parts[1]) if len(parts) >= 2 else None
    payload_bytes = decode_marker_int(parts[2]) if len(parts) >= 3 else None
    return decode_marker_path(path_part), source_bytes, payload_bytes


def sanitize_target_path(raw_path: str) -> Path:
    normalized = raw_path.strip().replace("\\", "/")
    parts = [part for part in Path(normalized).parts if part not in ("", ".", "/")]
    safe_parts: list[str] = []
    for part in parts:
        if part.endswith("\\"):
            continue
        if part in ("..",):
            continue
        drive, tail = part.split(":", 1) if ":" in part else ("", part)
        candidate = tail if drive else part
        candidate = candidate.strip().replace(":", "_")
        if candidate:
            safe_parts.append(candidate)

    # Sender markers use D:\Temp\... as a transport path prefix.
    # Strip that prefix so both single-file and folder sends resolve
    # to the same relative path inside our temp/extract destinations.
    if safe_parts and safe_parts[0].lower() == "temp":
        safe_parts = safe_parts[1:]
    if safe_parts:
        return Path(*safe_parts)
    return Path("received.zip")


def extract_transfer(text: str) -> tuple[str, Path] | None:
    start = text.rfind(END_MARKER_PREFIX)
    if start < 0:
        return None
    end = text.find(END_MARKER_SUFFIX, start)
    if end < 0:
        return None

    marker = text[start : end + len(END_MARKER_SUFFIX)]
    match = END_MARKER_RE.fullmatch(marker)
    if match is None:
        return None

    payload_b64 = PAYLOAD_SANITIZE_RE.sub("", text[:start])
    archive_name = sanitize_target_path(decode_marker_path(match.group(1)))
    return payload_b64, archive_name


def consume_start_marker(text: str) -> tuple[str, Path, int | None, int | None] | None:
    start = text.rfind(START_MARKER_PREFIX)
    if start < 0:
        return None
    end = text.find(START_MARKER_SUFFIX, start)
    if end < 0:
        return None

    marker_end = end + len(START_MARKER_SUFFIX)
    marker = text[start:marker_end]
    match = START_MARKER_RE.fullmatch(marker)
    if match is None:
        return None

    decoded_path, source_bytes, payload_bytes = parse_start_marker_payload(match.group(1))
    archive_name = sanitize_target_path(decoded_path)
    return text[marker_end:], archive_name, source_bytes, payload_bytes


def normalize_base64_payload(payload_b64: str) -> bytes:
    sanitized = PAYLOAD_SANITIZE_RE.sub("", payload_b64)
    sanitized = BASE64_BODY_RE.sub("", sanitized)
    sanitized = sanitized.rstrip("=")
    sanitized += "=" * ((4 - (len(sanitized) % 4)) % 4)
    return sanitized.encode("ascii")


def safe_extract_zip(zip_bytes: bytes, output_dir: Path) -> None:
    output_root = output_dir.resolve()
    with zipfile.ZipFile(io.BytesIO(zip_bytes)) as archive:
        for member in archive.infolist():
            if member.is_dir():
                continue
            destination = (output_root / member.filename).resolve()
            if destination != output_root and output_root not in destination.parents:
                raise ValueError(f"Blocked path outside inbound dir: {member.filename}")
            destination.parent.mkdir(parents=True, exist_ok=True)
            with archive.open(member) as source, destination.open("wb") as target:
                target.write(source.read())


def process_transfer(job_queue: "mp.Queue[tuple[str, str] | None]", status_queue: "mp.Queue[str]") -> None:
    while True:
        job = job_queue.get()
        if job is None:
            return

        payload_b64, archive_name = job
        try:
            zip_bytes = base64.b64decode(normalize_base64_payload(payload_b64), validate=False)
            ZIP_TEMP_DIR.mkdir(parents=True, exist_ok=True)
            INBOUND_DIR.mkdir(parents=True, exist_ok=True)

            archive_path = ZIP_TEMP_DIR / archive_name
            archive_path.parent.mkdir(parents=True, exist_ok=True)
            archive_path.write_bytes(zip_bytes)
            safe_extract_zip(zip_bytes, INBOUND_DIR)

            status_queue.put(
                f"Saved {archive_name} -> {archive_path.parent}; extracted -> {INBOUND_DIR}"
            )
        except (binascii.Error, ValueError) as exc:
            status_queue.put(f"Decode failed for {archive_name}: {exc}")
        except zipfile.BadZipFile as exc:
            status_queue.put(f"ZIP failed for {archive_name}: {exc}")
        except Exception as exc:  # pragma: no cover
            status_queue.put(f"Transfer failed for {archive_name}: {exc}")


class InputBoxApp:
    def __init__(self) -> None:
        self.root = Tk()
        self.root.title("input_box1")
        self.center_window(WINDOW_WIDTH, WINDOW_HEIGHT)
        self.root.resizable(False, False)
        self.root.minsize(WINDOW_WIDTH, WINDOW_HEIGHT)

        self.info_label = Label(
            self.root,
            text="Receiver buffer and status",
            anchor="w",
        )
        self.info_label.pack(padx=10, pady=(10, 0), fill=BOTH, expand=False)
        self.clear_button = Button(
            self.root,
            text="Clear / Fresh Start",
            command=self.clear_all_state,
            anchor="center",
        )
        self.clear_button.pack(padx=10, pady=(6, 0), fill="x", expand=False)
        self.progress_var = StringVar(
            value="RAM buffer: 0 chars | Visible tail: 0 chars"
        )
        self.progress_label = Label(
            self.root,
            textvariable=self.progress_var,
            anchor="w",
        )
        self.progress_label.pack(padx=10, pady=(4, 0), fill=BOTH, expand=False)

        self.text_area = scrolledtext.ScrolledText(
            self.root,
            wrap=WORD,
            width=TEXT_WIDTH,
            height=TEXT_HEIGHT,
            undo=False,
            maxundo=0,
        )
        self.text_area.pack(padx=10, pady=(6, 10), fill="x", expand=False)

        self.status_area = scrolledtext.ScrolledText(
            self.root,
            wrap=WORD,
            width=TEXT_WIDTH,
            height=STATUS_HEIGHT,
            state="disabled",
        )
        self.status_area.pack(padx=10, pady=(0, 10), fill="x", expand=False)

        self.job_queue: "mp.Queue[tuple[str, str] | None]" = mp.Queue()
        self.status_queue: "mp.Queue[str]" = mp.Queue()
        self.worker = self.start_worker()
        self.receive_buffer = ""
        self.last_widget_text = ""
        self.status_lines: list[str] = []
        self.active_archive_name: Path | None = None
        self.active_source_bytes: int | None = None
        self.active_payload_bytes: int | None = None

        self.text_area.bind("<KeyRelease>", self.on_text_input)
        self.root.protocol("WM_DELETE_WINDOW", self.on_close)
        self.text_area.focus_set()
        self.root.after(50, self.ensure_input_focus)
        self.root.after(250, self.ensure_input_focus)
        self.root.after(50, self.fit_window_to_content)
        self.root.after(POLL_INTERVAL_MS, self.poll_status)

    def center_window(self, width: int, height: int) -> None:
        self.root.update_idletasks()
        screen_w = self.root.winfo_screenwidth()
        screen_h = self.root.winfo_screenheight()
        x = max(0, (screen_w - width) // 2)
        y = max(0, (screen_h - height) // 2)
        self.root.geometry(f"{width}x{height}+{x}+{y}")

    def fit_window_to_content(self) -> None:
        self.root.update_idletasks()
        width = self.root.winfo_reqwidth()
        height = self.root.winfo_reqheight()
        self.center_window(width, height)

    def start_worker(self) -> mp.Process:
        worker = mp.Process(
            target=process_transfer,
            args=(self.job_queue, self.status_queue),
            daemon=True,
        )
        worker.start()
        return worker

    def append_status(self, line: str) -> None:
        self.status_lines.append(line)
        self.status_lines = self.status_lines[-MAX_STATUS_LINES:]
        self.status_area.configure(state="normal")
        self.status_area.delete("1.0", "end")
        self.status_area.insert("1.0", "\n".join(self.status_lines))
        self.status_area.configure(state="disabled")

    def update_progress(self) -> None:
        visible = len(self.last_widget_text)
        total = len(self.receive_buffer)
        parts = [f"RAM buffer: {total} chars", f"Visible tail: {visible} chars"]
        if self.active_payload_bytes:
            payload_percent = min(100.0, (total / self.active_payload_bytes) * 100.0)
            parts.append(
                f"Transfer: {total}/{self.active_payload_bytes} chars ({payload_percent:5.1f}%)"
            )
            if self.active_source_bytes:
                estimated_bytes = min(
                    self.active_source_bytes,
                    int((total / self.active_payload_bytes) * self.active_source_bytes),
                )
                parts.append(
                    f"File: {estimated_bytes}/{self.active_source_bytes} bytes"
                )
        elif self.active_source_bytes:
            parts.append(f"File size: {self.active_source_bytes} bytes")
        self.progress_var.set(" | ".join(parts))

    def reset_receive_buffer(self) -> None:
        self.receive_buffer = ""
        self.last_widget_text = ""
        self.active_source_bytes = None
        self.active_payload_bytes = None
        self.text_area.delete("1.0", "end")

    def clear_all_state(self) -> None:
        self.reset_receive_buffer()
        self.active_archive_name = None
        self.status_lines = []
        self.status_area.configure(state="normal")
        self.status_area.delete("1.0", "end")
        self.status_area.configure(state="disabled")
        self.update_progress()
        self.ensure_input_focus()

    def ensure_input_focus(self) -> None:
        try:
            self.root.lift()
            self.root.focus_force()
            self.text_area.focus_set()
            self.text_area.mark_set("insert", "end")
            self.text_area.see("insert")
        except Exception:
            pass

    def refresh_visible_tail(self) -> None:
        visible = self.receive_buffer[-VISIBLE_TEXT_LIMIT:]
        self.text_area.delete("1.0", "end")
        self.text_area.insert("1.0", visible)
        self.text_area.mark_set("insert", "end")
        self.text_area.see("insert")
        self.last_widget_text = visible
        self.update_progress()

    def on_text_input(self, event: object | None = None) -> None:
        if not self.worker.is_alive():
            self.append_status("Worker restarted after unexpected exit")
            self.worker = self.start_worker()

        widget_text = self.text_area.get("1.0", "end-1c")
        previous_tail = self.receive_buffer[-len(self.last_widget_text) :] if self.last_widget_text else ""

        if widget_text.startswith(previous_tail):
            appended = widget_text[len(previous_tail) :]
            if appended:
                self.receive_buffer += appended
        else:
            self.receive_buffer = widget_text

        start_event = consume_start_marker(self.receive_buffer)
        if start_event is not None:
            self.receive_buffer, archive_name, source_bytes, payload_bytes = start_event
            self.active_archive_name = archive_name
            self.active_source_bytes = source_bytes
            self.active_payload_bytes = payload_bytes
            if source_bytes is not None:
                self.append_status(f"Start {archive_name}; size {source_bytes} bytes; cleared stale input")
            else:
                self.append_status(f"Start {archive_name}; cleared stale input")
            self.refresh_visible_tail()

        transfer = extract_transfer(self.receive_buffer)
        if transfer is not None:
            payload_b64, archive_name = transfer
            self.job_queue.put((payload_b64, str(archive_name)))
            self.active_archive_name = None
            self.reset_receive_buffer()
            self.append_status(f"Queued {archive_name}")
            self.update_progress()
            return

        if len(self.receive_buffer) > VISIBLE_TEXT_LIMIT or widget_text != self.receive_buffer[-len(widget_text) :]:
            self.refresh_visible_tail()
        else:
            self.last_widget_text = widget_text
            self.update_progress()

    def poll_status(self) -> None:
        try:
            while True:
                self.append_status(self.status_queue.get_nowait())
        except queue.Empty:
            pass
        finally:
            self.root.after(POLL_INTERVAL_MS, self.poll_status)

    def on_close(self) -> None:
        self.job_queue.put(None)
        self.worker.join(timeout=2.0)
        if self.worker.is_alive():
            self.worker.terminate()
        self.root.destroy()

    def run(self) -> None:
        self.root.mainloop()


def main() -> None:
    try:
        mp.set_start_method("spawn")
    except RuntimeError:
        pass

    app = InputBoxApp()
    app.run()


if __name__ == "__main__":
    main()
