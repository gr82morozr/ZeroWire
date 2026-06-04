#!/usr/bin/env python3
"""Encode files or folders into a framed QR transfer stream."""

from __future__ import annotations

import argparse
import base64
import hashlib
import io
import json
import os
import secrets
import sys
import zipfile
import zlib
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Iterable, List, NoReturn, Optional, Sequence, Tuple

try:
    import tkinter as tk
except ImportError:
    tk = None
# (Data codewords, EC codewords per block, block groups).
# Each block group entry is (number of blocks, data codewords per block).
# Based on standard QR specification for ECC Level L.
VERSION_INFO = {
    1: (19, 7, ((1, 19),)),
    2: (34, 10, ((1, 34),)),
    3: (55, 15, ((1, 55),)),
    4: (80, 20, ((1, 80),)),
    5: (108, 26, ((1, 108),)),
    6: (136, 18, ((2, 68),)),
    7: (156, 20, ((2, 78),)),
    8: (194, 24, ((2, 97),)),
    9: (232, 30, ((2, 116),)),
    10: (274, 18, ((2, 68), (2, 69))),
    11: (324, 20, ((4, 81),)),
    12: (370, 24, ((2, 92), (2, 93))),
    13: (428, 26, ((4, 107),)),
    14: (461, 30, ((3, 115), (1, 116))),
    15: (523, 22, ((5, 87), (1, 88))),
    16: (589, 24, ((5, 98), (1, 99))),
    17: (647, 28, ((1, 107), (5, 108))),
    18: (721, 30, ((5, 120), (1, 121))),
    19: (795, 28, ((3, 113), (4, 114))),
    20: (861, 28, ((3, 107), (5, 108))),
    21: (932, 28, ((4, 116), (4, 117))),
    22: (1006, 28, ((2, 111), (7, 112))),
}
MAX_VERSION = max(VERSION_INFO)
FORMAT_INFO_BITS_L = [
    0b111011111000100,
    0b111001011110011,
    0b111110110101010,
    0b111100010011101,
    0b110011000101111,
    0b110001100011000,
    0b110110001000001,
    0b110100101110110,
]

MAX_MODULES = MAX_VERSION * 4 + 17
MAX_WINDOW_HEIGHT = 800
MAX_WINDOW_WIDTH = 800
DEFAULT_DATA_MAX_VERSION = min(MAX_VERSION, 14)
CANVAS_MODULE_SIZE = max(4, min(12, 860 // MAX_MODULES))
CANVAS_MARGIN = 4 * CANVAS_MODULE_SIZE
CANVAS_STATUS_HEIGHT = 72
CANVAS_SIZE = MAX_MODULES * CANVAS_MODULE_SIZE + CANVAS_MARGIN * 2
QR_FRAME_COLOR = "#00E5FF"
QR_FRAME_WIDTH = max(20, CANVAS_MODULE_SIZE * 3)
QR_CORNER_MARKER_SIZE = max(24, CANVAS_MODULE_SIZE * 4)
QR_QUIET_ZONE_MODULES = 4
QR_FRAME_CLEARANCE_MODULES = 2
PROTOCOL_MAGIC = "AIRQR1"
PROTOCOL_VERSION = 1


@dataclass
class TransferFile:
    source: Path
    relative_path: str


@dataclass
class PageSpec:
    payload: bytes
    version: int
    label: str
    matrix: Optional["QRMatrix"] = None
    rendered_console: Optional[str] = None


@dataclass
class PreparedFile:
    source: Path
    relative_path: str
    file_id: str
    file_index: int
    zip_bytes: bytes
    payload_bytes: bytes
    data_chunks: list[str]


def center_window(window: "tk.Tk", width: int, height: int) -> None:
    window.update_idletasks()
    screen_w = window.winfo_screenwidth()
    x = max(0, (screen_w - width) // 2)
    y = 0
    window.geometry(f"{width}x{height}+{x}+{y}")


def build_gui() -> tuple["tk.Tk", "tk.Canvas", "tk.Label"]:
    root = tk.Tk()
    root.title("file_2_qr")
    root.resizable(False, False)
    width = MAX_WINDOW_WIDTH
    height = MAX_WINDOW_HEIGHT
    root.configure(bg="white")
    root.geometry(f"{width}x{height}")
    root.minsize(width, height)
    root.maxsize(width, height)
    canvas = tk.Canvas(
        root,
        width=width,
        height=height - CANVAS_STATUS_HEIGHT,
        bg="white",
        highlightthickness=0,
    )
    canvas.place(x=0, y=CANVAS_STATUS_HEIGHT, width=width, height=height - CANVAS_STATUS_HEIGHT)
    status = tk.Label(root, text="", bg="white", anchor="w", justify="left")
    status.place(x=0, y=0, width=width, height=CANVAS_STATUS_HEIGHT)
    center_window(root, width, height)
    return root, canvas, status


def render_matrix_on_canvas(canvas: "tk.Canvas", matrix: QRMatrix) -> None:
    canvas.delete("all")
    canvas_width = int(canvas["width"])
    canvas_height = int(canvas["height"])
    size = matrix.size
    quiet_modules = QR_QUIET_ZONE_MODULES
    frame_clearance_modules = QR_FRAME_CLEARANCE_MODULES
    # Keep the QR large, but preserve a true quiet zone plus extra clearance before the cyan frame.
    total_modules = size + (quiet_modules + frame_clearance_modules) * 2 + 2
    module_size = max(1, min(canvas_width // total_modules, canvas_height // total_modules))
    qr_pixels = size * module_size
    quiet_pixels = quiet_modules * module_size
    frame_gap = max(module_size, frame_clearance_modules * module_size)
    frame_width = max(QR_FRAME_WIDTH, module_size * 3)
    marker = max(QR_CORNER_MARKER_SIZE, module_size * 4)
    framed_pixels = qr_pixels + (quiet_pixels + frame_gap) * 2
    footprint_w = framed_pixels + marker
    footprint_h = framed_pixels + marker
    footprint_x = (canvas_width - footprint_w) // 2
    footprint_y = (canvas_height - footprint_h) // 2
    qr_x0 = footprint_x + marker // 2 + quiet_pixels + frame_gap
    qr_y0 = footprint_y + marker // 2 + quiet_pixels + frame_gap

    image = tk.PhotoImage(width=canvas_width, height=canvas_height)
    image.put("white", to=(0, 0, canvas_width, canvas_height))
    for row in range(size):
        for col in range(size):
            if matrix.modules[row][col]:
                x0 = qr_x0 + col * module_size
                y0 = qr_y0 + row * module_size
                x1 = x0 + module_size
                y1 = y0 + module_size
                image.put("black", to=(x0, y0, x1, y1))
    canvas.create_image(0, 0, image=image, anchor="nw")
    canvas.create_rectangle(
        qr_x0 - quiet_pixels - frame_gap,
        qr_y0 - quiet_pixels - frame_gap,
        qr_x0 + qr_pixels + quiet_pixels + frame_gap,
        qr_y0 + qr_pixels + quiet_pixels + frame_gap,
        outline=QR_FRAME_COLOR,
        width=frame_width,
    )
    x0 = qr_x0 - quiet_pixels - frame_gap
    y0 = qr_y0 - quiet_pixels - frame_gap
    x1 = qr_x0 + qr_pixels + quiet_pixels + frame_gap
    y1 = qr_y0 + qr_pixels + quiet_pixels + frame_gap
    for left, top in (
        (x0 - marker // 2, y0 - marker // 2),
        (x1 - marker // 2, y0 - marker // 2),
        (x0 - marker // 2, y1 - marker // 2),
        (x1 - marker // 2, y1 - marker // 2),
    ):
        canvas.create_rectangle(
            left,
            top,
            left + marker,
            top + marker,
            outline=QR_FRAME_COLOR,
            fill=QR_FRAME_COLOR,
            width=0,
        )
    canvas.image = image


class QRGuiPlayer:
    def __init__(
        self,
        root: "tk.Tk",
        canvas: "tk.Canvas",
        status: "tk.Label",
        page_specs: list[PageSpec],
    ) -> None:
        self.root = root
        self.canvas = canvas
        self.status = status
        self.page_specs = page_specs
        self.current = 0
        root.bind("<space>", self.next_page)
        root.bind("<Right>", self.next_page)
        root.bind("<BackSpace>", self.previous_page)
        root.bind("<Left>", self.previous_page)
        root.bind("<p>", self.previous_page)
        root.bind("<r>", self.repeat_page)
        root.bind("<q>", self.quit)
        root.bind("<Escape>", self.quit)
        self.show_page(0)

    def show_page(self, index: int) -> None:
        self.current = index
        spec = self.page_specs[index]
        if spec.matrix is None:
            spec.matrix = build_matrix_for_chunk(spec.payload, spec.version)
        render_matrix_on_canvas(self.canvas, spec.matrix)
        self.status.config(
            text=(
                f"Page {index + 1}/{len(self.page_specs)}\n"
                f"{spec.label}\n"
                "Space/Right = next, Backspace/Left/P = previous, R = repeat, Q/Esc = quit"
            )
        )
        center_window(
            self.root,
            int(self.canvas["width"]),
            int(self.canvas["height"]) + CANVAS_STATUS_HEIGHT,
        )

    def next_page(self, event: Optional[object] = None) -> None:
        if self.current + 1 < len(self.page_specs):
            self.show_page(self.current + 1)
        else:
            self.root.destroy()

    def previous_page(self, event: Optional[object] = None) -> None:
        if self.current > 0:
            self.show_page(self.current - 1)

    def repeat_page(self, event: Optional[object] = None) -> None:
        self.show_page(self.current)

    def quit(self, event: Optional[object] = None) -> None:
        self.root.destroy()


class QRMatrix:
    def __init__(self, version: int) -> None:
        self.version = version
        self.size = version * 4 + 17
        self.modules: List[List[Optional[bool]]] = [
            [None] * self.size for _ in range(self.size)
        ]
        self.data_mask: List[List[bool]] = [
            [False] * self.size for _ in range(self.size)
        ]

    def set_module(self, row: int, col: int, value: bool, is_data: bool = False) -> None:
        self.modules[row][col] = value
        self.data_mask[row][col] = is_data

    def reserve_module(self, row: int, col: int) -> None:
        self.modules[row][col] = False
        self.data_mask[row][col] = False

    def is_empty(self, row: int, col: int) -> bool:
        return self.modules[row][col] is None

    def render(self, border: int = 4) -> str:
        out = []
        pad = "  "
        quiet = pad * border
        dark = "██"
        light = "  "
        line_light = quiet + (light * self.size) + quiet
        for _ in range(border):
            out.append(line_light)
        for row in self.modules:
            line = quiet
            for module in row:
                line += dark if module else light
            line += quiet
            out.append(line)
        for _ in range(border):
            out.append(line_light)
        return "\n".join(out)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Encode files or folders into a framed QR transfer stream."
        )
    )
    parser.add_argument(
        "input",
        nargs="+",
        type=Path,
        help="One or more files or folders to encode into QR pages.",
    )
    parser.add_argument(
        "--version",
        type=int,
        choices=list(VERSION_INFO),
        default=MAX_VERSION,
        help=(
            "Maximum QR version to use (default: %(default)s). "
            f"Version {MAX_VERSION} supports up to {get_max_payload(MAX_VERSION)} bytes per chunk."
        ),
    )
    parser.add_argument(
        "--data-max-version",
        type=int,
        choices=list(VERSION_INFO),
        default=DEFAULT_DATA_MAX_VERSION,
        help=(
            "Maximum QR version to use for DATA pages only (default: %(default)s). "
            "Lower values increase the page count but improve scan reliability."
        ),
    )
    parser.add_argument(
        "--border",
        type=int,
        default=4,
        help="Number of quiet-zone modules to print around the QR code (default: %(default)s).",
    )
    parser.add_argument(
        "--console",
        action="store_true",
        help="Render QR pages in the console instead of a GUI.",
    )
    parser.add_argument(
        "--session-id",
        default="",
        help="Optional stable session ID. Defaults to a short random 4-byte ID.",
    )
    parser.add_argument(
        "--control-repeats",
        type=int,
        default=1,
        help="How many times to repeat control pages (default: %(default)s).",
    )
    return parser


def split_into_chunks(data: bytes, max_length: int) -> List[bytes]:
    return [data[i : i + max_length] for i in range(0, len(data), max_length)]


def get_max_payload(version: int) -> int:
    data_codewords = VERSION_INFO[version][0]
    count_bits = 8 if version <= 9 else 16
    overhead_bits = 4 + count_bits
    return max(0, ((data_codewords * 8) - overhead_bits) // 8)


def choose_version(chunk_bytes: int, max_version: int) -> int:
    for version in range(1, max_version + 1):
        if chunk_bytes <= get_max_payload(version):
            return version
    return max_version


def fail(message: str) -> NoReturn:
    sys.exit(f"[file_2_qr] {message}")


def make_session_id() -> str:
    return secrets.token_hex(4).upper()


def normalize_session_id(value: str) -> str:
    session_id = value.strip()
    if not session_id:
        return make_session_id()
    try:
        session_id.encode("ascii")
    except UnicodeEncodeError:
        fail("Session ID must be ASCII.")
    return session_id


def ensure_ascii_path(path: str) -> str:
    normalized = path.replace("\\", "/")
    try:
        normalized.encode("ascii")
    except UnicodeEncodeError:
        fail(f"Non-ASCII relative paths are not supported by this protocol: {path}")
    return normalized


def collect_transfer_files(inputs: Sequence[Path]) -> list[TransferFile]:
    files: list[TransferFile] = []
    for input_path in inputs:
        if not input_path.exists():
            fail(f"Path not found: {input_path}")
        if input_path.is_file():
            files.append(
                TransferFile(
                    source=input_path,
                    relative_path=ensure_ascii_path(input_path.name),
                )
            )
            continue
        if not input_path.is_dir():
            fail(f"Unsupported path: {input_path}")
        root_name = input_path.name
        for child in sorted(path for path in input_path.rglob("*") if path.is_file()):
            relative = Path(root_name) / child.relative_to(input_path)
            files.append(
                TransferFile(
                    source=child,
                    relative_path=ensure_ascii_path(relative.as_posix()),
                )
            )
    if not files:
        fail("No files found to transfer.")
    return files


def zip_single_file(source: Path, relative_path: str) -> bytes:
    payload = io.BytesIO()
    with zipfile.ZipFile(payload, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        archive.write(source, arcname=relative_path)
    return payload.getvalue()


def sha256_hex(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def crc32_hex(data: bytes) -> str:
    return f"{zlib.crc32(data) & 0xFFFFFFFF:08x}"


def frame_bytes(frame_type: str, **payload: object) -> bytes:
    session_id = payload.get("s", "")
    if not isinstance(session_id, str) or not session_id:
        fail(f"Frame {frame_type} is missing required session id.")
    frame = {
        "m": PROTOCOL_MAGIC,
        "v": PROTOCOL_VERSION,
        "t": frame_type,
        **payload,
    }
    encoded = json.dumps(frame, separators=(",", ":"), ensure_ascii=True)
    return encoded.encode("ascii")


def build_data_chunks(
    session_id: str,
    file_id: str,
    relative_path: str,
    encoded_payload: bytes,
    max_version: int,
    sample_page_id: str,
) -> list[str]:
    max_bytes = get_max_payload(max_version)
    chunk_size = max_bytes
    while chunk_size > 0:
        chunks = split_into_chunks(encoded_payload, chunk_size)
        total = len(chunks)
        frames = [
            frame_bytes(
                "data",
                s=session_id,
                f=file_id,
                g=sample_page_id,
                i=index,
                n=total,
                p=relative_path,
                e="base64",
                k=crc32_hex(chunk),
                d=chunk.decode("ascii"),
            )
            for index, chunk in enumerate(chunks, start=1)
        ]
        oversize = [len(frame) - max_bytes for frame in frames if len(frame) > max_bytes]
        if not oversize:
            return [chunk.decode("ascii") for chunk in chunks]
        chunk_size -= max(oversize)
    fail(f"Unable to fit framed payload into version {max_version}-L QR codes.")


def build_transfer_pages(
    files: Sequence[TransferFile],
    max_version: int,
    data_max_version: int,
    session_id: str,
    control_repeats: int,
) -> list[PageSpec]:
    page_specs: list[PageSpec] = []
    prepared_files: list[PreparedFile] = []
    sample_page_id = f"{session_id}-999999"

    for file_index, item in enumerate(files, start=1):
        file_id = f"{file_index:04d}"
        zip_bytes = zip_single_file(item.source, item.relative_path)
        payload_bytes = base64.b64encode(zip_bytes)
        data_chunks = build_data_chunks(
            session_id,
            file_id,
            item.relative_path,
            payload_bytes,
            data_max_version,
            sample_page_id,
        )
        prepared_files.append(
            PreparedFile(
                source=item.source,
                relative_path=item.relative_path,
                file_id=file_id,
                file_index=file_index,
                zip_bytes=zip_bytes,
                payload_bytes=payload_bytes,
                data_chunks=data_chunks,
            )
        )

    file_page_totals = {
        prepared.file_id: control_repeats + len(prepared.data_chunks) + control_repeats
        for prepared in prepared_files
    }
    total_pages = control_repeats + sum(file_page_totals.values()) + control_repeats
    page_index = 0

    def next_page_meta() -> tuple[int, str]:
        nonlocal page_index
        page_index += 1
        return page_index, f"{session_id}-{page_index:06d}"

    for repeat_index in range(control_repeats):
        session_page_index, session_page_id = next_page_meta()
        session_start = frame_bytes(
            "session_start",
            s=session_id,
            g=session_page_id,
            c=len(files),
            sc=total_pages,
            si=session_page_index,
            ri=repeat_index + 1,
            rc=control_repeats,
        )
        session_start_version = choose_version(len(session_start), max_version)
        page_specs.append(
            PageSpec(
                payload=session_start,
                version=session_start_version,
                label=(
                    f"SESSION START (Session ID: {session_id}, Total Files: {len(files)})\n"
                    f"Session QR: {session_page_index}/{total_pages}"
                ),
            )
        )

    for prepared in prepared_files:
        file_page_total = file_page_totals[prepared.file_id]
        file_page_index = 0
        for repeat_index in range(control_repeats):
            file_page_index += 1
            session_page_index, session_page_id = next_page_meta()
            start_frame = frame_bytes(
                "start",
                s=session_id,
                f=prepared.file_id,
                g=session_page_id,
                p=prepared.relative_path,
                e="base64",
                c=len(prepared.data_chunks),
                o=prepared.source.stat().st_size,
                z=len(prepared.zip_bytes),
                zh=sha256_hex(prepared.zip_bytes),
                fi=prepared.file_index,
                fc=len(files),
                qi=file_page_index,
                qc=file_page_total,
                si=session_page_index,
                sc=total_pages,
                ri=repeat_index + 1,
                rc=control_repeats,
            )
            start_version = choose_version(len(start_frame), max_version)
            if len(start_frame) > get_max_payload(start_version):
                fail(f"START frame for {prepared.relative_path} exceeds version {max_version}-L capacity.")
            page_specs.append(
                PageSpec(
                    payload=start_frame,
                    version=start_version,
                    label=(
                        f"FILE START (File: {prepared.relative_path})\n"
                        f"File: {prepared.file_index}/{len(files)} | QR: {file_page_index}/{file_page_total}"
                    ),
                )
            )
        for chunk_index, chunk_text in enumerate(prepared.data_chunks, start=1):
            file_page_index += 1
            session_page_index, session_page_id = next_page_meta()
            data_frame = frame_bytes(
                "data",
                s=session_id,
                f=prepared.file_id,
                g=session_page_id,
                i=chunk_index,
                n=len(prepared.data_chunks),
                p=prepared.relative_path,
                e="base64",
                k=crc32_hex(chunk_text.encode("ascii")),
                d=chunk_text,
                fi=prepared.file_index,
                fc=len(files),
                qi=file_page_index,
                qc=file_page_total,
                si=session_page_index,
                sc=total_pages,
            )
            page_specs.append(
                PageSpec(
                    payload=data_frame,
                    version=choose_version(len(data_frame), max_version),
                    label=(
                        f"FILE DATA (File: {prepared.relative_path})\n"
                        f"File: {prepared.file_index}/{len(files)} | "
                        f"Chunk: {chunk_index}/{len(prepared.data_chunks)} | "
                        f"QR: {file_page_index}/{file_page_total}"
                    ),
                )
            )
        for repeat_index in range(control_repeats):
            file_page_index += 1
            session_page_index, session_page_id = next_page_meta()
            end_frame = frame_bytes(
                "end",
                s=session_id,
                f=prepared.file_id,
                g=session_page_id,
                p=prepared.relative_path,
                e="base64",
                c=len(prepared.data_chunks),
                ph=sha256_hex(prepared.payload_bytes),
                zh=sha256_hex(prepared.zip_bytes),
                fi=prepared.file_index,
                fc=len(files),
                qi=file_page_index,
                qc=file_page_total,
                si=session_page_index,
                sc=total_pages,
                ri=repeat_index + 1,
                rc=control_repeats,
            )
            end_version = choose_version(len(end_frame), max_version)
            if len(end_frame) > get_max_payload(end_version):
                fail(f"END frame for {prepared.relative_path} exceeds version {max_version}-L capacity.")
            page_specs.append(
                PageSpec(
                    payload=end_frame,
                    version=end_version,
                    label=(
                        f"FILE END (File: {prepared.relative_path})\n"
                        f"File: {prepared.file_index}/{len(files)} | QR: {file_page_index}/{file_page_total}"
                    ),
                )
            )
    for repeat_index in range(control_repeats):
        session_page_index, session_page_id = next_page_meta()
        session_end = frame_bytes(
            "session_end",
            s=session_id,
            g=session_page_id,
            c=len(files),
            sc=total_pages,
            si=session_page_index,
            ri=repeat_index + 1,
            rc=control_repeats,
        )
        session_end_version = choose_version(len(session_end), max_version)
        page_specs.append(
            PageSpec(
                payload=session_end,
                version=session_end_version,
                label=(
                    f"SESSION END (Session ID: {session_id}, Total Files: {len(files)})\n"
                    f"Session QR: {session_page_index}/{total_pages}"
                ),
            )
        )
    return page_specs


def bits_of_int(value: int, bit_count: int) -> List[int]:
    return [(value >> shift) & 1 for shift in reversed(range(bit_count))]


def get_bit(value: int, bit_index: int) -> int:
    return (value >> bit_index) & 1


def bytes_to_bits(data: bytes) -> List[int]:
    bits: List[int] = []
    for byte in data:
        bits.extend(bits_of_int(byte, 8))
    return bits


def get_alignment_locations(version: int) -> List[int]:
    if version == 1:
        return []
    num_align = version // 7 + 2
    step = ((version * 8 + num_align * 3 + 5) // (num_align * 4 - 4)) * 2
    size = version * 4 + 17
    positions = [6]
    for pos in range(size - 7, 6, -step):
        positions.append(pos)
        if len(positions) == num_align:
            break
    return sorted(positions)


def build_codewords(data_bytes: bytes, version: int) -> List[int]:
    mode_bits = [0, 1, 0, 0]
    length_bits = bits_of_int(len(data_bytes), 8 if version <= 9 else 16)
    payload_bits = bytes_to_bits(data_bytes)
    bits: List[int] = mode_bits + length_bits + payload_bits

    terminator = min(4, max(0, VERSION_INFO[version][0] * 8 - len(bits)))
    bits.extend([0] * terminator)

    while len(bits) % 8 != 0:
        bits.append(0)

    data_codeword_count = VERSION_INFO[version][0]
    codewords = [
        sum(bit << (7 - index) for index, bit in enumerate(bits[i : i + 8]))
        for i in range(0, len(bits), 8)
    ]

    pad_bytes = [0xEC, 0x11]
    pad_index = 0
    while len(codewords) < data_codeword_count:
        codewords.append(pad_bytes[pad_index])
        pad_index ^= 1
    return codewords


def gf_tables() -> Tuple[List[int], List[int]]:
    exp = [1] * 512
    log = [0] * 256
    x = 1
    for i in range(1, 255):
        x <<= 1
        if x & 0x100:
            x ^= 0x11D
        exp[i] = x
        log[x] = i
    for i in range(255, 512):
        exp[i] = exp[i - 255]
    return exp, log


def gf_mul(a: int, b: int, exp: List[int], log: List[int]) -> int:
    if a == 0 or b == 0:
        return 0
    return exp[log[a] + log[b]]


def build_generator(ec_count: int, exp: List[int], log: List[int]) -> List[int]:
    poly = [1]
    for i in range(ec_count):
        poly = poly_convolve(poly, [1, exp[i]], exp, log)
    return poly


def poly_convolve(a: List[int], b: List[int], exp: List[int], log: List[int]) -> List[int]:
    result = [0] * (len(a) + len(b) - 1)
    for i, ai in enumerate(a):
        if ai == 0:
            continue
        for j, bj in enumerate(b):
            if bj == 0:
                continue
            result[i + j] ^= gf_mul(ai, bj, exp, log)
    return result


def rs_encode_block(data: List[int], ec_count: int, exp: List[int], log: List[int]) -> List[int]:
    gen = build_generator(ec_count, exp, log)
    block = data + [0] * ec_count
    for i in range(len(data)):
        coef = block[i]
        if coef == 0:
            continue
        for j in range(len(gen)):
            block[i + j] ^= gf_mul(gen[j], coef, exp, log)
    return block[-ec_count:]


def build_blocks(codewords: List[int], version: int) -> Tuple[List[List[int]], List[List[int]]]:
    _, ec_cw, groups = VERSION_INFO[version]
    blocks: List[List[int]] = []
    offset = 0
    for count, data_codewords in groups:
        for _ in range(count):
            block = codewords[offset : offset + data_codewords]
            blocks.append(block)
            offset += data_codewords
    exp, log = gf_tables()
    ec_blocks = [rs_encode_block(block, ec_cw, exp, log) for block in blocks]
    return blocks, ec_blocks


def interleave(blocks: List[List[int]], ec_blocks: List[List[int]]) -> List[int]:
    result: List[int] = []
    for i in range(max(len(block) for block in blocks)):
        for block in blocks:
            if i < len(block):
                result.append(block[i])
    for i in range(max(len(ec) for ec in ec_blocks)):
        for ec in ec_blocks:
            if i < len(ec):
                result.append(ec[i])
    return result


def place_finder(matrix: QRMatrix, top: int, left: int) -> None:
    pattern = [
        [1, 1, 1, 1, 1, 1, 1],
        [1, 0, 0, 0, 0, 0, 1],
        [1, 0, 1, 1, 1, 0, 1],
        [1, 0, 1, 1, 1, 0, 1],
        [1, 0, 1, 1, 1, 0, 1],
        [1, 0, 0, 0, 0, 0, 1],
        [1, 1, 1, 1, 1, 1, 1],
    ]
    for dy, row in enumerate(pattern):
        for dx, value in enumerate(row):
            matrix.set_module(top + dy, left + dx, bool(value), is_data=False)
    for dx in range(-1, 8):
        if 0 <= left + dx < matrix.size:
            y = top - 1
            if 0 <= y < matrix.size:
                matrix.reserve_module(y, left + dx)
            y = top + 7
            if 0 <= y < matrix.size:
                matrix.reserve_module(y, left + dx)
    for dy in range(7):
        if 0 <= top + dy < matrix.size:
            x = left - 1
            if 0 <= x < matrix.size:
                matrix.reserve_module(top + dy, x)
            x = left + 7
            if 0 <= x < matrix.size:
                matrix.reserve_module(top + dy, x)


def place_alignment(matrix: QRMatrix, centers: Sequence[int]) -> None:
    for row_center in centers:
        for col_center in centers:
            # Skip alignment patterns that overlap with finder patterns or their separators
            # Finders + Separators occupy 0..8 and size-9..size-1
            if row_center < 9 and col_center < 9: continue
            if row_center < 9 and col_center > matrix.size - 10: continue
            if row_center > matrix.size - 10 and col_center < 9: continue

            for dy in range(-2, 3):
                for dx in range(-2, 3):
                    r, c = row_center + dy, col_center + dx
                    if not (0 <= r < matrix.size and 0 <= c < matrix.size):
                        continue
                    is_black = max(abs(dx), abs(dy)) != 1
                    if matrix.is_empty(r, c):
                        matrix.set_module(
                            r,
                            c,
                            is_black,
                            is_data=False,
                        )


def place_timing_patterns(matrix: QRMatrix) -> None:
    for i in range(8, matrix.size - 8):
        value = i % 2 == 0
        if matrix.is_empty(6, i):
            matrix.set_module(6, i, value, is_data=False)
        if matrix.is_empty(i, 6):
            matrix.set_module(i, 6, value, is_data=False)


def reserve_format_areas(matrix: QRMatrix) -> None:
    for i in range(9):
        if i != 6:
            matrix.reserve_module(8, i)
            matrix.reserve_module(i, 8)
    for i in range(8):
        matrix.reserve_module(8, matrix.size - 1 - i)
        matrix.reserve_module(matrix.size - 1 - i, 8)
    matrix.reserve_module(8, 8)


def reserve_version_areas(matrix: QRMatrix) -> None:
    if matrix.version < 7:
        return
    for offset in range(6):
        for bit_index in range(3):
            matrix.reserve_module(offset, matrix.size - 11 + bit_index)
            matrix.reserve_module(matrix.size - 11 + bit_index, offset)


def place_dark_module(matrix: QRMatrix) -> None:
    matrix.set_module(matrix.size - 8, 8, True, is_data=False)


def build_function_patterns(matrix: QRMatrix) -> None:
    place_finder(matrix, 0, 0)
    place_finder(matrix, 0, matrix.size - 7)
    place_finder(matrix, matrix.size - 7, 0)
    if matrix.version >= 2:
        place_alignment(matrix, get_alignment_locations(matrix.version))
    place_timing_patterns(matrix)
    reserve_format_areas(matrix)
    reserve_version_areas(matrix)
    place_dark_module(matrix)


def next_bit(data_bits: Iterable[int]) -> int:
    for bit in data_bits:
        yield bit


def place_data_bits(matrix: QRMatrix, codewords: List[int]) -> None:
    bits = []
    for byte in codewords:
        bits.extend(bits_of_int(byte, 8))
    bit_iter = iter(bits)
    col = matrix.size - 1
    upward = True
    while col > 0:
        if col == 6:
            col -= 1
        for row_index in range(matrix.size):
            row = matrix.size - 1 - row_index if upward else row_index
            for c in (col, col - 1):
                if matrix.is_empty(row, c):
                    try:
                        value = bool(next(bit_iter))
                    except StopIteration:
                        value = False
                    matrix.set_module(row, c, value, is_data=True)
        upward = not upward
        col -= 2


def mask_function(mask: int, row: int, col: int) -> bool:
    if mask == 0:
        return (row + col) % 2 == 0
    if mask == 1:
        return row % 2 == 0
    if mask == 2:
        return col % 3 == 0
    if mask == 3:
        return (row + col) % 3 == 0
    if mask == 4:
        return ((row // 2) + (col // 3)) % 2 == 0
    if mask == 5:
        return (row * col) % 2 + (row * col) % 3 == 0
    if mask == 6:
        return ((row * col) % 2 + (row * col) % 3) % 2 == 0
    return ((row + col) % 2 + (row * col) % 3) % 2 == 0


def apply_mask(matrix: QRMatrix, mask: int) -> QRMatrix:
    masked = QRMatrix(matrix.version)
    masked.modules = [row[:] for row in matrix.modules]
    masked.data_mask = [row[:] for row in matrix.data_mask]
    for row in range(masked.size):
        for col in range(masked.size):
            if masked.data_mask[row][col] and mask_function(mask, row, col):
                masked.modules[row][col] = not masked.modules[row][col]
    return masked


def format_bits_for_mask(mask: int) -> List[int]:
    format_value = FORMAT_INFO_BITS_L[mask]
    return bits_of_int(format_value, 15)


def add_format_information(matrix: QRMatrix, mask: int) -> None:
    format_value = FORMAT_INFO_BITS_L[mask]

    for i in range(6):
        matrix.set_module(i, 8, bool(get_bit(format_value, i)), is_data=False)
    matrix.set_module(7, 8, bool(get_bit(format_value, 6)), is_data=False)
    matrix.set_module(8, 8, bool(get_bit(format_value, 7)), is_data=False)
    matrix.set_module(8, 7, bool(get_bit(format_value, 8)), is_data=False)
    for i in range(9, 15):
        matrix.set_module(8, 14 - i, bool(get_bit(format_value, i)), is_data=False)

    for i in range(8):
        matrix.set_module(8, matrix.size - 1 - i, bool(get_bit(format_value, i)), is_data=False)
    for i in range(8, 15):
        matrix.set_module(matrix.size - 15 + i, 8, bool(get_bit(format_value, i)), is_data=False)


def add_version_information(matrix: QRMatrix) -> None:
    if matrix.version < 7:
        return
    rem = matrix.version
    for _ in range(12):
        rem = (rem << 1) ^ ((rem >> 11) * 0x1F25)
    version_bits = (matrix.version << 12) | rem
    for bit_index in range(18):
        bit = bool(get_bit(version_bits, bit_index))
        row = matrix.size - 11 + (bit_index % 3)
        col = bit_index // 3
        matrix.set_module(row, col, bit, is_data=False)
        matrix.set_module(col, row, bit, is_data=False)


def penalty_consecutive(sequence: Sequence[bool]) -> int:
    penalty = 0
    run_color = sequence[0]
    run_length = 1
    for value in sequence[1:]:
        if value == run_color:
            run_length += 1
        else:
            if run_length >= 5:
                penalty += 3 + (run_length - 5)
            run_color = value
            run_length = 1
    if run_length >= 5:
        penalty += 3 + (run_length - 5)
    return penalty


def penalty_block(matrix: QRMatrix) -> int:
    penalty = 0
    for row in range(matrix.size - 1):
        for col in range(matrix.size - 1):
            block = [
                matrix.modules[row][col],
                matrix.modules[row][col + 1],
                matrix.modules[row + 1][col],
                matrix.modules[row + 1][col + 1],
            ]
            if all(block) or not any(block):
                penalty += 3
    return penalty


def penalty_pattern(matrix: QRMatrix) -> int:
    penalty = 0
    p1 = [False, False, False, False, True, False, True, True, True, False, True]
    p2 = [True, False, True, True, True, False, True, False, False, False, False]
    for row in matrix.modules:
        for start in range(len(row) - 10):
            seg = row[start:start + 11]
            if seg == p1 or seg == p2:
                penalty += 40
    for col in range(matrix.size):
        column = [matrix.modules[row][col] for row in range(matrix.size)]
        for start in range(len(column) - 10):
            seg = column[start:start + 11]
            if seg == p1 or seg == p2:
                penalty += 40
    return penalty


def penalty_dark_ratio(matrix: QRMatrix) -> int:
    total = matrix.size * matrix.size
    dark = sum(1 for row in matrix.modules for module in row if module)
    five_percent = abs(dark * 20 - total * 10) // total
    return five_percent * 10


def evaluate_mask(matrix: QRMatrix) -> int:
    return (
        sum(penalty_consecutive(row) for row in matrix.modules)
        + sum(penalty_consecutive([row[col] for row in matrix.modules]) for col in range(matrix.size))
        + penalty_block(matrix)
        + penalty_pattern(matrix)
        + penalty_dark_ratio(matrix)
    )


def build_matrix_for_chunk(chunk: bytes, version: int) -> QRMatrix:
    codewords = build_codewords(chunk, version)
    blocks, ec_blocks = build_blocks(codewords, version)
    all_codewords = interleave(blocks, ec_blocks)
    matrix = QRMatrix(version)
    build_function_patterns(matrix)
    place_data_bits(matrix, all_codewords)

    best_mask = 0
    best_matrix = matrix
    best_score = float("inf")
    for mask in range(8):
        masked = apply_mask(matrix, mask)
        add_format_information(masked, mask)
        add_version_information(masked)
        score = evaluate_mask(masked)
        if score < best_score:
            best_score = score
            best_matrix = masked
            best_mask = mask
    return best_matrix


def cache_page_specs(page_specs: Sequence[PageSpec], border: int) -> None:
    for spec in page_specs:
        if spec.matrix is None:
            spec.matrix = build_matrix_for_chunk(spec.payload, spec.version)
        if spec.rendered_console is None:
            spec.rendered_console = spec.matrix.render(border=border)


def clear_screen() -> None:
    if os.name == "nt":
        os.system("cls")
    else:
        os.system("clear")


def wait_for_action() -> str:
    prompt = "Space/Right=next, Left/P/Backspace=previous, R=repeat, Q=quit..."
    print(prompt, end="", flush=True)
    if os.name == "nt":
        import msvcrt

        while True:
            key = msvcrt.getwch()
            if key in {"\x00", "\xe0"}:
                ext = msvcrt.getwch()
                if ext == "M":
                    print("\r" + " " * len(prompt) + "\r", end="", flush=True)
                    return "next"
                if ext == "K":
                    print("\r" + " " * len(prompt) + "\r", end="", flush=True)
                    return "previous"
                continue
            if key == " ":
                print("\r" + " " * len(prompt) + "\r", end="", flush=True)
                return "next"
            if key in {"p", "P", "\b"}:
                print("\r" + " " * len(prompt) + "\r", end="", flush=True)
                return "previous"
            if key in {"r", "R"}:
                print("\r" + " " * len(prompt) + "\r", end="", flush=True)
                return "repeat"
            if key.lower() == "q":
                sys.exit(0)
    else:
        import termios
        import tty

        fd = sys.stdin.fileno()
        old_settings = termios.tcgetattr(fd)
        try:
            tty.setraw(fd)
            while True:
                key = sys.stdin.read(1)
                if key == "\x1b":
                    next1 = sys.stdin.read(1)
                    if next1 == "[":
                        next2 = sys.stdin.read(1)
                        if next2 == "C":
                            print("\r" + " " * len(prompt) + "\r", end="", flush=True)
                            return "next"
                        if next2 == "D":
                            print("\r" + " " * len(prompt) + "\r", end="", flush=True)
                            return "previous"
                    continue
                if key == " ":
                    print("\r" + " " * len(prompt) + "\r", end="", flush=True)
                    return "next"
                if key.lower() == "p":
                    print("\r" + " " * len(prompt) + "\r", end="", flush=True)
                    return "previous"
                if key.lower() == "r":
                    print("\r" + " " * len(prompt) + "\r", end="", flush=True)
                    return "repeat"
                if key.lower() == "q":
                    sys.exit(0)
        finally:
            termios.tcsetattr(fd, termios.TCSADRAIN, old_settings)


def main() -> None:
    parser = build_parser()
    args = parser.parse_args()
    if args.data_max_version > args.version:
        fail("--data-max-version cannot be greater than --version.")
    files = collect_transfer_files(args.input)
    session_id = normalize_session_id(args.session_id)
    page_specs = build_transfer_pages(
        files,
        args.version,
        args.data_max_version,
        session_id,
        max(1, args.control_repeats),
    )
    cache_page_specs(page_specs, args.border)

    if args.console or tk is None:
        total = len(page_specs)
        index = 0
        while 0 <= index < total:
            spec = page_specs[index]
            clear_screen()
            print(
                f"[file_2_qr] Session {session_id} | Page {index + 1}/{total}\n"
                f"{spec.label}\n"
                f"{len(spec.payload)} bytes using version {spec.version}-L"
            )
            print(spec.rendered_console)
            if index == total - 1:
                break
            action = wait_for_action()
            if action == "previous":
                index = max(0, index - 1)
            elif action == "repeat":
                continue
            else:
                index += 1
        print(f"[file_2_qr] Finished session {session_id}.")
        return

    root, canvas, status = build_gui()
    QRGuiPlayer(root, canvas, status, page_specs)
    root.mainloop()


if __name__ == "__main__":
    main()
