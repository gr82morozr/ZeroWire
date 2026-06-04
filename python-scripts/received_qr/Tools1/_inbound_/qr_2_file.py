#!/usr/bin/env python3
"""Capture camera frames, detect framed QR transfer pages, and extract received files."""

from __future__ import annotations

import argparse
import base64
import hashlib
import io
import json
import math
import os
import select
import signal
import socket
import sys
import time
import zipfile
import zlib
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional, Set

import numpy as np

try:
    import cv2
except ImportError:  # pragma: no cover
    cv2 = None

try:
    from pyzbar.pyzbar import ZBarSymbol, decode as pyzbar_decode
except ImportError:  # pragma: no cover
    pyzbar_decode = None
    ZBarSymbol = None

PROTOCOL_MAGIC = "AIRQR1"
PROTOCOL_VERSION = 1
DEFAULT_ESP32_HOST = "192.168.1.136"
SPACE_TYPE_SETTLE_MS = 80
MIN_CAMERA_WIDTH = 800
MIN_CAMERA_HEIGHT = 450
HEAVY_PASS_INTERVAL = 6
CYAN_LOW = np.array([78, 110, 110], dtype=np.uint8)
CYAN_HIGH = np.array([102, 255, 255], dtype=np.uint8)


@dataclass
class TransferBuffer:
    session_id: str
    file_id: str
    relative_path: str = ""
    encoding: str = "base64"
    expected_chunks: int = 0
    payload_sha256: str = ""
    zip_sha256: str = ""
    chunks: dict[int, str] = field(default_factory=dict)


@dataclass
class SpaceSender:
    host: str
    port: int
    connect_timeout: float
    sock: Optional[socket.socket] = None
    pending: str = ""
    ready: Optional[bool] = None
    last_error: str = ""
    last_prompt_at: float = 0.0

    def close(self) -> None:
        if self.sock is not None:
            try:
                self.sock.close()
            except OSError:
                pass
            self.sock = None
        self.pending = ""
        self.ready = None
        self.last_error = ""

    def _connect(self) -> None:
        self.sock = socket.create_connection(
            (self.host, self.port),
            timeout=self.connect_timeout,
        )
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.pending = ""
        self.ready = None

    def _drain_state(self) -> None:
        if self.sock is None:
            return
        while True:
            readable, _, _ = select.select([self.sock], [], [], 0)
            if not readable:
                break
            data = self.sock.recv(128)
            if not data:
                raise OSError("socket closed by remote host")
            self.pending += data.decode("ascii", errors="ignore")
            while "\n" in self.pending:
                line, self.pending = self.pending.split("\n", 1)
                line = line.strip()
                if line == "1":
                    self.ready = True
                elif line == "0":
                    self.ready = False

    def refresh_ready(self) -> bool:
        self.close()
        try:
            self._connect()
            deadline = time.monotonic() + self.connect_timeout
            while time.monotonic() < deadline:
                self._drain_state()
                if self.ready is not None:
                    self.last_error = ""
                    return self.ready is True
                time.sleep(0.05)
            self.last_error = "ESP32 status not received"
            self.close()
            return False
        except OSError:
            self.last_error = f"TCP connect/status failed to {self.host}:{self.port}"
            self.close()
            return False

    def status_message(self) -> str:
        if self.ready is True:
            return f"ESP32 INPUT mode ready at {self.host}:{self.port}"
        return "Turn on the INPUT mode of esp32 keyboard"

    def frame_status_message(self) -> str:
        if self.ready is True:
            return f"ESP32 status for frame: READY ({self.host}:{self.port})"
        if self.last_error:
            return f"ESP32 status for frame: {self.last_error}"
        return "ESP32 status for frame: NOT READY - Turn on the INPUT mode of esp32 keyboard"

    def send_space(self) -> bool:
        try:
            if not self.refresh_ready():
                if self.last_error == "":
                    self.last_error = "ESP32 not in INPUT mode"
                return False
            self.sock.sendall(b" ")
            self.last_error = ""
            return True
        except OSError:
            self.last_error = f"TCP send failed to {self.host}:{self.port}"
            self.close()
            return False


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Use the default camera to scan QR transfer pages in real time. "
            "Completed files are rebuilt from base64 ZIP payloads and extracted to disk."
        )
    )
    parser.add_argument(
        "camera",
        nargs="?",
        type=int,
        default=0,
        help="Camera index to use (default: %(default)s).",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("received_qr"),
        help="Directory where received files will be extracted.",
    )
    parser.add_argument(
        "--max-frames",
        type=int,
        default=0,
        help="Optional maximum number of frames to process (0 means run until interrupted).",
    )
    parser.add_argument(
        "--unique",
        action="store_true",
        help="Only save unique QR payloads once.",
    )
    parser.add_argument(
        "--esp32-port",
        type=int,
        default=9000,
        help="ESP32 TCP port for typed-space forwarding (default: %(default)s).",
    )
    parser.add_argument(
        "--connect-timeout",
        type=float,
        default=2.0,
        help="Seconds to wait when opening the ESP32 socket (default: %(default)s).",
    )
    return parser


def beep() -> None:
    if os.name == "nt":
        try:
            import winsound

            winsound.Beep(1000, 150)
        except Exception:  # pragma: no cover
            print("\a", end="", flush=True)
    else:
        print("\a", end="", flush=True)


def sha256_hex(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def crc32_hex(data: bytes) -> str:
    return f"{zlib.crc32(data) & 0xFFFFFFFF:08x}"


def parse_protocol_payload(payload: str) -> Optional[dict[str, object]]:
    try:
        message = json.loads(payload)
    except json.JSONDecodeError:
        return None
    if not isinstance(message, dict):
        return None
    if message.get("m") != PROTOCOL_MAGIC or message.get("v") != PROTOCOL_VERSION:
        return None
    return message


def get_buffer(
    transfers: dict[tuple[str, str], TransferBuffer],
    session_id: str,
    file_id: str,
) -> TransferBuffer:
    key = (session_id, file_id)
    if key not in transfers:
        transfers[key] = TransferBuffer(session_id=session_id, file_id=file_id)
    return transfers[key]


def safe_output_path(root: Path, relative_path: str) -> Path:
    destination = (root / relative_path).resolve()
    root_resolved = root.resolve()
    if destination != root_resolved and root_resolved not in destination.parents:
        raise ValueError(f"Blocked path outside output directory: {relative_path}")
    return destination


def extract_zip_bytes(zip_bytes: bytes, output_dir: Path) -> list[Path]:
    extracted: list[Path] = []
    with zipfile.ZipFile(io.BytesIO(zip_bytes)) as archive:
        for member in archive.infolist():
            if member.is_dir():
                continue
            destination = safe_output_path(output_dir, member.filename)
            destination.parent.mkdir(parents=True, exist_ok=True)
            with archive.open(member) as source, destination.open("wb") as target:
                target.write(source.read())
            extracted.append(destination)
    return extracted


def finalize_transfer(
    buffer: TransferBuffer,
    output_dir: Path,
) -> str:
    if buffer.expected_chunks <= 0:
        raise ValueError(f"Missing chunk count for {buffer.session_id}/{buffer.file_id}")
    missing = [index for index in range(1, buffer.expected_chunks + 1) if index not in buffer.chunks]
    if missing:
        raise ValueError(
            f"Missing chunks for {buffer.relative_path or buffer.file_id}: "
            f"{missing[:8]}{'...' if len(missing) > 8 else ''}"
        )
    payload_bytes = "".join(buffer.chunks[index] for index in range(1, buffer.expected_chunks + 1)).encode("ascii")
    if buffer.payload_sha256 and sha256_hex(payload_bytes) != buffer.payload_sha256:
        raise ValueError(f"Payload hash mismatch for {buffer.relative_path or buffer.file_id}")
    if buffer.encoding == "base64":
        zip_bytes = base64.b64decode(payload_bytes, validate=True)
    elif buffer.encoding == "raw":
        zip_bytes = payload_bytes
    else:
        raise ValueError(f"Unsupported encoding {buffer.encoding!r}")
    if buffer.zip_sha256 and sha256_hex(zip_bytes) != buffer.zip_sha256:
        raise ValueError(f"ZIP hash mismatch for {buffer.relative_path or buffer.file_id}")
    extracted = extract_zip_bytes(zip_bytes, output_dir)
    if not extracted:
        raise ValueError(f"No files found in ZIP for {buffer.relative_path or buffer.file_id}")
    if buffer.relative_path:
        return f"Saved {buffer.relative_path}"
    return f"Saved {len(extracted)} file(s) from {buffer.file_id}"


def process_protocol_message(
    message: dict[str, object],
    output_dir: Path,
    transfers: dict[tuple[str, str], TransferBuffer],
    completed: set[tuple[str, str]],
) -> Optional[str]:
    frame_type = str(message.get("t", ""))
    session_id = str(message.get("s", ""))
    file_id = str(message.get("f", ""))

    if frame_type == "session_end":
        return f"Session complete: {session_id}"
    if not session_id or not file_id:
        return None

    key = (session_id, file_id)
    if frame_type == "start":
        buffer = get_buffer(transfers, session_id, file_id)
        path = str(message.get("p", buffer.relative_path))
        encoding = str(message.get("e", buffer.encoding))
        expected_chunks = int(message.get("c", buffer.expected_chunks or 0))
        zip_sha256 = str(message.get("zh", buffer.zip_sha256))
        duplicate = (
            buffer.relative_path == path
            and buffer.encoding == encoding
            and buffer.expected_chunks == expected_chunks
            and buffer.zip_sha256 == zip_sha256
        )
        buffer.relative_path = path
        buffer.encoding = encoding
        buffer.expected_chunks = expected_chunks
        buffer.zip_sha256 = zip_sha256
        if duplicate:
            return None
        return f"Started {buffer.relative_path or file_id} ({buffer.expected_chunks} chunks)"

    if frame_type == "data":
        if key in completed:
            return None
        buffer = get_buffer(transfers, session_id, file_id)
        index = int(message.get("i", 0))
        total = int(message.get("n", 0))
        buffer.relative_path = str(message.get("p", buffer.relative_path))
        buffer.encoding = str(message.get("e", buffer.encoding))
        chunk = str(message.get("d", ""))
        chunk_crc = str(message.get("k", ""))
        if index <= 0 or total <= 0 or not chunk:
            return None
        if chunk_crc and crc32_hex(chunk.encode("ascii")) != chunk_crc:
            return f"Rejected corrupt chunk {index}/{total} for {buffer.relative_path or file_id}"
        buffer.expected_chunks = max(buffer.expected_chunks, total)
        existing = buffer.chunks.get(index)
        if existing is None:
            buffer.chunks[index] = chunk
        elif existing != chunk:
            return f"Conflicting chunk {index}/{buffer.expected_chunks} for {buffer.relative_path or file_id}"
        else:
            return None
        return f"Chunk {index}/{buffer.expected_chunks} for {buffer.relative_path or file_id}"

    if frame_type == "end":
        if key in completed:
            return None
        buffer = get_buffer(transfers, session_id, file_id)
        buffer.relative_path = str(message.get("p", buffer.relative_path))
        buffer.encoding = str(message.get("e", buffer.encoding))
        buffer.expected_chunks = int(message.get("c", buffer.expected_chunks or 0))
        buffer.payload_sha256 = str(message.get("ph", buffer.payload_sha256))
        buffer.zip_sha256 = str(message.get("zh", buffer.zip_sha256))
        try:
            result = finalize_transfer(buffer, output_dir)
        except ValueError as exc:
            return str(exc)
        completed.add(key)
        del transfers[key]
        return result

    return None


def preprocess_frame(frame: "cv2.Mat") -> "cv2.Mat":
    gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
    gray = cv2.bilateralFilter(gray, 5, 50, 50)
    clahe = cv2.createCLAHE(clipLimit=2.5, tileGridSize=(8, 8))
    gray = clahe.apply(gray)
    # Dense QR versions need more pixels per module for reliable decode.
    min_side = min(gray.shape[:2])
    if min_side < 480:
        scale = 4.0
    elif min_side < 720:
        scale = 2.0
    else:
        scale = 1.0
    if scale != 1.0:
        gray = cv2.resize(gray, None, fx=scale, fy=scale, interpolation=cv2.INTER_CUBIC)
    return gray


def quick_gray_frame(frame: "cv2.Mat") -> "cv2.Mat":
    return cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)


def threshold_frame(gray: "cv2.Mat") -> "cv2.Mat":
    blurred = cv2.GaussianBlur(gray, (5, 5), 0)
    return cv2.adaptiveThreshold(
        blurred,
        255,
        cv2.ADAPTIVE_THRESH_GAUSSIAN_C,
        cv2.THRESH_BINARY,
        41,
        10,
    )


def otsu_frame(gray: "cv2.Mat") -> "cv2.Mat":
    blurred = cv2.GaussianBlur(gray, (3, 3), 0)
    _, otsu = cv2.threshold(blurred, 0, 255, cv2.THRESH_BINARY + cv2.THRESH_OTSU)
    return otsu


def sharpen_frame(gray: "cv2.Mat") -> "cv2.Mat":
    blurred = cv2.GaussianBlur(gray, (0, 0), 1.2)
    return cv2.addWeighted(gray, 1.7, blurred, -0.7, 0)


def upsample_frame(gray: "cv2.Mat", scale: float) -> "cv2.Mat":
    return cv2.resize(gray, None, fx=scale, fy=scale, interpolation=cv2.INTER_CUBIC)


def order_quad_points(points: "cv2.Mat") -> "cv2.Mat":
    pts = points.reshape(4, 2).astype("float32")
    ordered = pts.copy()
    sums = pts.sum(axis=1)
    diffs = pts[:, 0] - pts[:, 1]
    ordered[0] = pts[sums.argmin()]
    ordered[2] = pts[sums.argmax()]
    ordered[1] = pts[diffs.argmin()]
    ordered[3] = pts[diffs.argmax()]
    return ordered


def warp_qr_candidate(frame: "cv2.Mat", points: "cv2.Mat") -> Optional["cv2.Mat"]:
    try:
        quad = order_quad_points(points)
    except Exception:
        return None
    side = int(
        max(
            math.dist(quad[0], quad[1]),
            math.dist(quad[1], quad[2]),
            math.dist(quad[2], quad[3]),
            math.dist(quad[3], quad[0]),
        )
    )
    side = max(160, min(side + 32, 900))
    destination = np.array([
        [0, 0],
        [side - 1, 0],
        [side - 1, side - 1],
        [0, side - 1],
    ], dtype="float32")
    transform = cv2.getPerspectiveTransform(quad, destination)
    return cv2.warpPerspective(frame, transform, (side, side))


def open_camera(index: int) -> "cv2.VideoCapture":
    if cv2 is None:
        sys.exit(
            "Error: OpenCV is required for camera capture. "
            "Install it with `pip install opencv-python`."
        )
    backend = cv2.CAP_DSHOW if os.name == "nt" and hasattr(cv2, "CAP_DSHOW") else cv2.CAP_ANY
    cap = cv2.VideoCapture(index, backend)
    if not cap.isOpened():
        sys.exit(f"Error: Cannot open camera index {index}.")
    for prop, value in (
        (cv2.CAP_PROP_FRAME_WIDTH, MIN_CAMERA_WIDTH),
        (cv2.CAP_PROP_FRAME_HEIGHT, MIN_CAMERA_HEIGHT),
        (cv2.CAP_PROP_FPS, 60),
        (cv2.CAP_PROP_BUFFERSIZE, 1),
        (cv2.CAP_PROP_AUTOFOCUS, 1),
    ):
        try:
            cap.set(prop, value)
        except Exception:
            pass
    try:
        cap.set(cv2.CAP_PROP_FOURCC, cv2.VideoWriter_fourcc(*"MJPG"))
    except Exception:
        pass
    return cap


def decode_with_pyzbar(gray: "cv2.Mat") -> list[tuple[str, list[int]]]:
    if pyzbar_decode is None:
        return []
    results: list[tuple[str, list[int]]] = []
    try:
        symbols = [ZBarSymbol.QRCODE] if ZBarSymbol is not None else None
        decoded = pyzbar_decode(gray, symbols=symbols) if symbols is not None else pyzbar_decode(gray)
    except Exception:
        decoded = []

    for obj in decoded:
        try:
            payload = obj.data.decode("utf-8", errors="replace")
        except Exception:
            payload = obj.data.decode("latin-1", errors="replace")
        if not payload:
            continue
        pts = []
        if hasattr(obj, "polygon") and obj.polygon:
            pts = [coord for point in obj.polygon for coord in (point.x, point.y)]
        elif hasattr(obj, "rect") and obj.rect:
            rect = obj.rect
            pts = [rect.left, rect.top, rect.left + rect.width, rect.top, rect.left + rect.width, rect.top + rect.height, rect.left, rect.top + rect.height]
        if pts:
            results.append((payload, pts))
    return results


def decode_with_opencv(frame: "cv2.Mat", detector: "cv2.QRCodeDetector") -> list[tuple[str, list[int]]]:
    results: list[tuple[str, list[int]]] = []
    try:
        payload, points, _ = detector.detectAndDecode(frame)
    except Exception:
        payload, points = "", None
    if payload:
        if points is not None and hasattr(points, "reshape"):
            pts = [int(coord) for coord in points.reshape(-1, 2).flatten()]
        elif points is not None:
            pts = [int(coord) for pair in points for coord in pair]
        else:
            pts = []
        results.append((payload, pts))

    curved = getattr(detector, "detectAndDecodeCurved", None)
    if callable(curved):
        try:
            payload, points = curved(frame)[:2]
        except Exception:
            payload, points = "", None
        if payload:
            if points is not None and hasattr(points, "reshape"):
                pts = [int(coord) for coord in points.reshape(-1, 2).flatten()]
            elif points is not None:
                pts = [int(coord) for pair in points for coord in pair]
            else:
                pts = []
            results.append((payload, pts))

    try:
        result = detector.detectAndDecodeMulti(frame)
    except Exception:
        return results
    if not result:
        return results
    if len(result) == 4:
        _, data, points, _ = result
    else:
        data, points, _ = result
    decoded: list[str] = []
    if isinstance(data, (list, tuple)):
        decoded = [item for item in data if item]
    elif isinstance(data, str) and data:
        decoded = [data]
    if points is None:
        for payload in decoded:
            results.append((payload, []))
        return results
    for payload, qr_points in zip(decoded, points):
        if not payload or qr_points is None:
            continue
        if hasattr(qr_points, "reshape"):
            pts = [int(coord) for coord in qr_points.reshape(-1, 2).flatten()]
        else:
            pts = [int(coord) for pair in qr_points for coord in pair]
        results.append((payload, pts))
    return results


def detect_points_only(frame: "cv2.Mat", detector: "cv2.QRCodeDetector") -> list[list[int]]:
    try:
        found, points = detector.detectMulti(frame)
    except Exception:
        found, points = False, None
    if found and points is not None:
        return [[int(coord) for coord in qr_points.reshape(-1, 2).flatten()] for qr_points in points]
    try:
        found, points = detector.detect(frame)
    except Exception:
        found, points = False, None
    if found and points is not None:
        return [[int(coord) for coord in points.reshape(-1, 2).flatten()]]
    return []


def crop_box_region(image: "cv2.Mat", pts: list[int], pad_ratio: float = 0.18) -> Optional["cv2.Mat"]:
    if len(pts) < 8:
        return None
    quad = np.array(pts, dtype=np.float32).reshape(4, 2)
    x0 = max(0, int(np.floor(quad[:, 0].min())))
    y0 = max(0, int(np.floor(quad[:, 1].min())))
    x1 = min(image.shape[1], int(np.ceil(quad[:, 0].max())))
    y1 = min(image.shape[0], int(np.ceil(quad[:, 1].max())))
    width = x1 - x0
    height = y1 - y0
    if width <= 8 or height <= 8:
        return None
    pad = int(max(width, height) * pad_ratio)
    x0 = max(0, x0 - pad)
    y0 = max(0, y0 - pad)
    x1 = min(image.shape[1], x1 + pad)
    y1 = min(image.shape[0], y1 + pad)
    return image[y0:y1, x0:x1]


def detect_colored_border_roi(frame: "cv2.Mat") -> tuple[Optional["cv2.Mat"], Optional[list[int]]]:
    hsv = cv2.cvtColor(frame, cv2.COLOR_BGR2HSV)
    mask = cv2.inRange(hsv, CYAN_LOW, CYAN_HIGH)
    kernel = np.ones((5, 5), dtype=np.uint8)
    mask = cv2.morphologyEx(mask, cv2.MORPH_CLOSE, kernel, iterations=2)
    mask = cv2.dilate(mask, kernel, iterations=1)
    contours, _ = cv2.findContours(mask, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
    frame_area = frame.shape[0] * frame.shape[1]
    selected: list[tuple[int, int, int, int]] = []
    for contour in contours:
        area = cv2.contourArea(contour)
        if area < frame_area * 0.0015:
            continue
        x, y, w, h = cv2.boundingRect(contour)
        if w <= 20 or h <= 20:
            continue
        selected.append((x, y, x + w, y + h))
    if not selected:
        return None, None
    x0 = min(rect[0] for rect in selected)
    y0 = min(rect[1] for rect in selected)
    x1 = max(rect[2] for rect in selected)
    y1 = max(rect[3] for rect in selected)
    if (x1 - x0) * (y1 - y0) < frame_area * 0.05:
        return None, None
    inset = max(8, int(min(x1 - x0, y1 - y0) * 0.06))
    x0 = min(max(0, x0 + inset), frame.shape[1] - 1)
    y0 = min(max(0, y0 + inset), frame.shape[0] - 1)
    x1 = max(x0 + 1, min(frame.shape[1], x1 - inset))
    y1 = max(y0 + 1, min(frame.shape[0], y1 - inset))
    return frame[y0:y1, x0:x1], [x0, y0, x1, y0, x1, y1, x0, y1]


def offset_boxes(boxes: list[list[int]], x_offset: int, y_offset: int) -> list[list[int]]:
    shifted: list[list[int]] = []
    for pts in boxes:
        if len(pts) < 8:
            continue
        out: list[int] = []
        for index, coord in enumerate(pts):
            out.append(coord + (x_offset if index % 2 == 0 else y_offset))
        shifted.append(out)
    return shifted


def find_quad_candidates(gray: "cv2.Mat") -> list[list[int]]:
    edges = cv2.Canny(gray, 60, 180)
    edges = cv2.dilate(edges, np.ones((3, 3), dtype=np.uint8), iterations=1)
    contours, _ = cv2.findContours(edges, cv2.RETR_LIST, cv2.CHAIN_APPROX_SIMPLE)
    image_area = gray.shape[0] * gray.shape[1]
    candidates: list[tuple[float, list[int]]] = []
    for contour in contours:
        area = cv2.contourArea(contour)
        if area < image_area * 0.01 or area > image_area * 0.85:
            continue
        perimeter = cv2.arcLength(contour, True)
        approx = cv2.approxPolyDP(contour, 0.04 * perimeter, True)
        if len(approx) != 4 or not cv2.isContourConvex(approx):
            continue
        rect = cv2.minAreaRect(contour)
        width, height = rect[1]
        if width <= 1 or height <= 1:
            continue
        aspect = max(width, height) / max(1.0, min(width, height))
        if aspect > 1.6:
            continue
        pts = approx.reshape(4, 2).astype(int).flatten().tolist()
        candidates.append((area, pts))
    candidates.sort(key=lambda item: item[0], reverse=True)
    deduped: list[list[int]] = []
    seen_centers: list[tuple[float, float]] = []
    for _, pts in candidates:
        quad = np.array(pts, dtype=np.float32).reshape(4, 2)
        center = tuple(quad.mean(axis=0))
        if any(abs(center[0] - sx) < 24 and abs(center[1] - sy) < 24 for sx, sy in seen_centers):
            continue
        seen_centers.append(center)
        deduped.append(pts)
        if len(deduped) >= 6:
            break
    return deduped


def merge_results(results: list[tuple[str, list[int]]]) -> tuple[list[str], list[list[int]]]:
    merged: dict[str, list[int]] = {}
    for payload, pts in results:
        if payload not in merged or (pts and len(pts) > len(merged[payload])):
            merged[payload] = pts
    return list(merged), list(merged.values())


def merge_payloads(results: list[tuple[str, list[int]]]) -> list[str]:
    seen: dict[str, None] = {}
    for payload, _ in results:
        if payload:
            seen[payload] = None
    return list(seen)


def decode_warped_candidates(
    frame: "cv2.Mat",
    boxes: list[list[int]],
    detector: "cv2.QRCodeDetector",
) -> list[tuple[str, list[int]]]:
    results: list[tuple[str, list[int]]] = []
    for pts in boxes[:3]:
        if len(pts) < 8:
            continue
        warped = warp_qr_candidate(frame, np.array(pts, dtype="float32").reshape(4, 2))
        if warped is None:
            continue
        gray = preprocess_frame(warped)
        variants = [gray, sharpen_frame(gray), threshold_frame(gray), otsu_frame(gray)]
        rotations = [
            lambda img: img,
            lambda img: cv2.rotate(img, cv2.ROTATE_90_CLOCKWISE),
            lambda img: cv2.rotate(img, cv2.ROTATE_180),
            lambda img: cv2.rotate(img, cv2.ROTATE_90_COUNTERCLOCKWISE),
        ]
        for variant in variants:
            for rotate in rotations:
                rotated = rotate(variant)
                decoded = decode_with_opencv(rotated, detector)
                if pyzbar_decode is not None:
                    decoded.extend(decode_with_pyzbar(rotated))
                if decoded:
                    results.extend(decoded)
                    return results
    return results


def decode_roi_candidates(
    frame: "cv2.Mat",
    quick_gray: "cv2.Mat",
    boxes: list[list[int]],
    detector: "cv2.QRCodeDetector",
) -> list[tuple[str, list[int]]]:
    results: list[tuple[str, list[int]]] = []
    for pts in boxes[:4]:
        for source in (frame, quick_gray):
            roi = crop_box_region(source, pts)
            if roi is None:
                continue
            gray_roi = roi if len(roi.shape) == 2 else quick_gray_frame(roi)
            variants = [gray_roi, sharpen_frame(gray_roi), threshold_frame(gray_roi)]
            for variant in variants:
                decoded = decode_with_opencv(variant, detector)
                if pyzbar_decode is not None:
                    decoded.extend(decode_with_pyzbar(variant))
                if decoded:
                    results.extend(decoded)
                    return results
    return results


def find_display_boxes(frame: "cv2.Mat", detector: "cv2.QRCodeDetector") -> list[list[int]]:
    boxes = detect_points_only(frame, detector)
    if boxes:
        return boxes
    quick_gray = quick_gray_frame(frame)
    boxes = find_quad_candidates(quick_gray)
    if boxes:
        return boxes
    return []


def detect_qr(
    frame: "cv2.Mat",
    detector: "cv2.QRCodeDetector",
    use_heavy_pass: bool,
) -> tuple[list[str], list[list[int]]]:
    roi_frame, roi_box = detect_colored_border_roi(frame)
    if roi_frame is not None and roi_box is not None:
        roi_quick_gray = quick_gray_frame(roi_frame)
        roi_candidates: list[tuple[str, list[int]]] = []
        roi_candidates.extend(decode_with_opencv(roi_frame, detector))
        roi_candidates.extend(decode_with_opencv(roi_quick_gray, detector))
        roi_payloads = merge_payloads(roi_candidates)
        if roi_payloads:
            return roi_payloads, [roi_box]
        if use_heavy_pass:
            roi_gray = preprocess_frame(roi_frame)
            roi_sharp = sharpen_frame(roi_gray)
            roi_thresh = threshold_frame(roi_gray)
            roi_boxes = detect_points_only(roi_frame, detector) or find_quad_candidates(roi_quick_gray)
            roi_heavy: list[tuple[str, list[int]]] = []
            roi_heavy.extend(decode_with_opencv(roi_sharp, detector))
            roi_heavy.extend(decode_with_opencv(roi_thresh, detector))
            if pyzbar_decode is not None:
                roi_heavy.extend(decode_with_pyzbar(roi_sharp))
                roi_heavy.extend(decode_with_pyzbar(roi_thresh))
            roi_heavy.extend(decode_roi_candidates(roi_frame, roi_quick_gray, roi_boxes, detector))
            roi_heavy.extend(decode_warped_candidates(roi_frame, roi_boxes, detector))
            roi_payloads = merge_payloads(roi_candidates + roi_heavy)
            if roi_payloads:
                return roi_payloads, [roi_box]

    quick_gray = quick_gray_frame(frame)
    candidates: list[tuple[str, list[int]]] = []
    candidates.extend(decode_with_opencv(frame, detector))
    candidates.extend(decode_with_opencv(quick_gray, detector))
    payloads = merge_payloads(candidates)
    boxes = find_display_boxes(frame, detector)
    if payloads or not use_heavy_pass:
        return payloads, boxes

    gray = preprocess_frame(frame)
    sharp = sharpen_frame(gray)
    thresh = threshold_frame(gray)
    sharp_thresh = threshold_frame(sharp)
    heavy_candidates: list[tuple[str, list[int]]] = []
    contour_boxes = find_quad_candidates(quick_gray)
    if not contour_boxes:
        contour_boxes = find_quad_candidates(threshold_frame(quick_gray))
    detect_boxes = boxes[:] if boxes else []
    if not detect_boxes:
        detect_boxes = detect_points_only(frame, detector)
    if contour_boxes:
        existing = {tuple(box) for box in detect_boxes}
        for quad in contour_boxes:
            if tuple(quad) not in existing:
                detect_boxes.append(quad)
    heavy_candidates.extend(decode_roi_candidates(frame, quick_gray, detect_boxes, detector))
    heavy_candidates.extend(decode_warped_candidates(frame, detect_boxes, detector))
    if not heavy_candidates:
        heavy_candidates.extend(decode_with_opencv(sharp_thresh, detector))
        if pyzbar_decode is not None:
            heavy_candidates.extend(decode_with_pyzbar(sharp_thresh))
    payloads = merge_payloads(candidates + heavy_candidates)
    if not boxes:
        boxes = detect_boxes
    return payloads, boxes


def read_latest_frame(cap: "cv2.VideoCapture") -> tuple[bool, Optional["cv2.Mat"]]:
    for _ in range(4):
        try:
            if not cap.grab():
                break
        except Exception:
            break
    return cap.read()


def draw_boxes(frame: "cv2.Mat", boxes: list[list[int]]) -> "cv2.Mat":
    for pts in boxes:
        if len(pts) < 8:
            continue
        points = [(pts[i], pts[i + 1]) for i in range(0, len(pts), 2)]
        for j in range(len(points)):
            start = points[j]
            end = points[(j + 1) % len(points)]
            cv2.line(frame, start, end, (0, 255, 0), 2)
    return frame


def cleanup_capture(
    cap: Optional["cv2.VideoCapture"],
    space_sender: Optional[SpaceSender],
) -> None:
    if cap is not None:
        try:
            cap.release()
        except Exception:
            pass
    if space_sender is not None:
        try:
            space_sender.close()
        except Exception:
            pass
    if cv2 is not None:
        try:
            cv2.destroyAllWindows()
            cv2.waitKey(1)
            cv2.waitKey(1)
        except Exception:
            pass


def main() -> None:
    parser = build_parser()
    args = parser.parse_args()
    if cv2 is None:
        sys.exit(
            "Error: OpenCV is not installed. Install it with `pip install opencv-python`."
        )

    if pyzbar_decode is None:
        print("Warning: pyzbar is not installed. Falling back to OpenCV-only detection.")

    detector = cv2.QRCodeDetector()
    for attr, value in (("setEpsX", 0.2), ("setEpsY", 0.2)):
        setter = getattr(detector, attr, None)
        if callable(setter):
            setter(value)
    cap: Optional["cv2.VideoCapture"] = None
    space_sender: Optional[SpaceSender] = None
    stop_requested = False

    def handle_stop(signum: int, _frame: object | None) -> None:
        nonlocal stop_requested
        stop_requested = True
        print(f"\nStopping on signal {signum}...")

    previous_sigint = signal.getsignal(signal.SIGINT)
    previous_sigterm = signal.getsignal(signal.SIGTERM)
    signal.signal(signal.SIGINT, handle_stop)
    signal.signal(signal.SIGTERM, handle_stop)

    try:
        cap = open_camera(args.camera)
        space_sender = SpaceSender(DEFAULT_ESP32_HOST, args.esp32_port, args.connect_timeout)
        print(f"Opening camera #{args.camera}. Press ESC or Q to quit.")
        seen: Set[str] = set()
        last_seen: dict[str, float] = {}
        transfers: dict[tuple[str, str], TransferBuffer] = {}
        completed: set[tuple[str, str]] = set()
        frame_count = 0
        repeat_delay = 2.0
        args.output.mkdir(parents=True, exist_ok=True)

        while not stop_requested:
            if args.max_frames and frame_count >= args.max_frames:
                break
            frame_count += 1
            success, frame = read_latest_frame(cap)
            if not success:
                print("Warning: failed to read camera frame.")
                time.sleep(0.1)
                continue

            decoded, boxes = detect_qr(
                frame,
                detector,
                use_heavy_pass=(frame_count % HEAVY_PASS_INTERVAL == 0),
            )
            for payload in decoded:
                if stop_requested:
                    break
                if not payload:
                    continue
                now = time.monotonic()
                if args.unique and payload in seen:
                    continue
                if payload in last_seen and now - last_seen[payload] < repeat_delay:
                    continue
                message = parse_protocol_payload(payload)
                if message is None:
                    print("Ignored non-protocol QR payload.")
                    continue
                last_seen[payload] = now
                seen.add(payload)
                try:
                    status = process_protocol_message(message, args.output, transfers, completed)
                except Exception as exc:
                    status = f"Error processing transfer frame: {exc}"
                ready = space_sender.refresh_ready()
                print(space_sender.frame_status_message())
                if not ready:
                    continue
                try:
                    if space_sender.sock is None:
                        raise OSError("socket not connected after READY status")
                    space_sender.sock.sendall(b" ")
                    time.sleep(SPACE_TYPE_SETTLE_MS / 1000.0)
                    space_sender.last_error = ""
                except OSError:
                    space_sender.last_error = f"TCP send failed to {space_sender.host}:{space_sender.port}"
                    space_sender.close()
                    print(space_sender.frame_status_message())
                    continue
                beep()
                if status:
                    print(status)

            frame = draw_boxes(frame, boxes)
            cv2.imshow("qr_2_file", frame)
            key = cv2.waitKey(1) & 0xFF
            if key in {27, ord("q"), ord("Q")}:
                break
    except KeyboardInterrupt:
        print("\nStopping on Ctrl-C...")
    finally:
        signal.signal(signal.SIGINT, previous_sigint)
        signal.signal(signal.SIGTERM, previous_sigterm)
        cleanup_capture(cap, space_sender)
        print(f"Saved received data under {args.output}")


if __name__ == "__main__":
    main()
