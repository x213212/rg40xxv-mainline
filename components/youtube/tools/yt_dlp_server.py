#!/usr/bin/env python3
"""Bounded, owner-private persistent yt-dlp resolver service.

The service accepts one newline-terminated JSON request per AF_UNIX connection.
It deliberately never prints request URLs, extracted metadata, or exception
messages because all three may contain signed Google Video credentials.
"""

from __future__ import annotations

import argparse
import calendar
from collections import OrderedDict, deque
import errno
import hashlib
import json
import math
import os
import re
import shutil
import signal
import socket
import stat
import struct
import sys
import tempfile
import threading
import time
import unicodedata
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Deque, Dict, List, Optional, Sequence, Tuple, Union


PROTOCOL_VERSION = 1
MAX_WORKERS = 2
MAX_BACKGROUND_WORKERS = 1
MAX_PENDING_VIDEO_IDS = 8
MAX_WAITERS_PER_VIDEO = 8
MAX_REQUEST_BYTES = 4096
MAX_RESPONSE_BYTES = 2 * 1024 * 1024
MAX_WATCH_URL_BYTES = 256
MAX_FORMATS = 512
MAX_SIGNED_URL_BYTES = 16 * 1024
MAX_HEADER_COUNT = 64
MAX_HEADER_BYTES = 8192
MAX_FEED_ITEMS = 96
MAX_FEED_FETCH_ITEMS = MAX_FEED_ITEMS + 1
MAX_FEED_VALUE_CHARS = 128
MAX_FEED_VALUE_BYTES = 384
MAX_FEED_CACHE_WINDOWS = 16
MAX_FEED_CACHE_BYTES = 2 * 1024 * 1024
FEED_TTL_SECONDS = 180
# A feed is display metadata, not a signed playback endpoint.  Keep the last
# validated snapshot available across ordinary network outages and device
# sleep.  Fresh data is still refreshed after three minutes, but an expired
# snapshot remains an immediate stale-while-revalidate response for seven
# days instead of turning a temporary extractor failure into an empty UI.
FEED_STALE_SECONDS = 7 * 24 * 60 * 60
CLIENT_READ_TIMEOUT_SECONDS = 2.0
CLIENT_WRITE_TIMEOUT_SECONDS = 5.0
LISTEN_BACKLOG = 16
VIDEO_ID_RE = re.compile(r"^[A-Za-z0-9_-]{11}$")
PLAYER_CLIENT_RE = re.compile(r"^[a-z0-9_-]{1,32}$")
PRIORITIES = {"interactive", "background"}
FEED_MODES = {"channel", "search"}
CHANNEL_ID_RE = re.compile(r"^UC[A-Za-z0-9_-]{22}$")
FEED_CACHE_SCHEMA = "rg40xxv-youtube-yt-dlp-feed-v2-playable-aggregate96"
SHORT_URL_RE = re.compile(r"^https://youtu\.be/([A-Za-z0-9_-]{11})$")
WATCH_URL_RE = re.compile(
    r"^https://(?:www\.)?youtube\.com/watch\?v=([A-Za-z0-9_-]{11})$"
)


class ServerError(RuntimeError):
    """A sanitized daemon contract failure."""


class AlreadyRunning(ServerError):
    """The configured socket is already served by another process."""


class SilentLogger:
    """Prevent yt-dlp from logging signed URLs or exception text."""

    @staticmethod
    def debug(_message: str) -> None:
        return None

    @staticmethod
    def info(_message: str) -> None:
        return None

    @staticmethod
    def warning(_message: str) -> None:
        return None

    @staticmethod
    def error(_message: str) -> None:
        return None


@dataclass
class Job:
    video_id: str
    url: str
    player_client: str
    clients: List[socket.socket] = field(default_factory=list)
    queued_monotonic: float = field(default_factory=time.monotonic)
    priority: str = "interactive"
    worker_tid: Optional[int] = None
    promoted: bool = False
    queued: bool = True
    used_background_slot: bool = False


@dataclass
class FeedWaiter:
    client: socket.socket
    offset: int
    limit: int
    cache_state: str
    encoding: str


@dataclass
class FeedJob:
    mode: str
    value: str
    clients: List[FeedWaiter] = field(default_factory=list)
    queued_monotonic: float = field(default_factory=time.monotonic)
    priority: str = "interactive"
    worker_tid: Optional[int] = None
    promoted: bool = False
    queued: bool = True
    used_background_slot: bool = False

    @property
    def key(self) -> Tuple[str, str]:
        return self.mode, self.value


@dataclass
class FeedSnapshot:
    items: Tuple[Dict[str, object], ...]
    has_more: bool
    generated_at: int
    expires_at: int


def diagnostic(event: str, **fields: object) -> None:
    """Write only fixed tokens and counts; callers must never pass URLs."""
    suffix = "".join(f" {key}={value}" for key, value in fields.items())
    print(f"YOUTUBE_YTDLP_SERVER event={event}{suffix}", file=sys.stderr, flush=True)


def video_id_for_url(value: object) -> str:
    if not isinstance(value, str):
        raise ServerError("invalid_url")
    try:
        encoded = value.encode("ascii")
    except UnicodeEncodeError as error:
        raise ServerError("invalid_url") from error
    if not encoded or len(encoded) > MAX_WATCH_URL_BYTES:
        raise ServerError("invalid_url")
    match = SHORT_URL_RE.fullmatch(value) or WATCH_URL_RE.fullmatch(value)
    if match is None or VIDEO_ID_RE.fullmatch(match.group(1)) is None:
        raise ServerError("invalid_url")
    return match.group(1)


def private_runtime_root(path: Path) -> None:
    if not path.is_absolute() or path == Path("/"):
        raise ServerError("runtime_root_invalid")
    path.mkdir(mode=0o700, parents=True, exist_ok=True)
    try:
        info = path.lstat()
    except OSError as error:
        raise ServerError("runtime_root_unavailable") from error
    if (
        not stat.S_ISDIR(info.st_mode)
        or stat.S_ISLNK(info.st_mode)
        or info.st_uid != os.geteuid()
        or stat.S_IMODE(info.st_mode) != 0o700
    ):
        raise ServerError("runtime_root_not_private")


def private_cache_root(path: Path) -> None:
    if not path.is_absolute() or path == Path("/"):
        raise ServerError("cache_root_invalid")
    path.mkdir(mode=0o700, parents=True, exist_ok=True)
    try:
        info = path.lstat()
    except OSError as error:
        raise ServerError("cache_root_unavailable") from error
    if (
        not stat.S_ISDIR(info.st_mode)
        or stat.S_ISLNK(info.st_mode)
        or info.st_uid != os.geteuid()
        or stat.S_IMODE(info.st_mode) != 0o700
    ):
        raise ServerError("cache_root_not_private")


def validate_socket_path(runtime_root: Path, socket_path: Path) -> None:
    if not socket_path.is_absolute() or socket_path == Path("/"):
        raise ServerError("socket_path_invalid")
    if len(os.fsencode(socket_path)) > 100:
        raise ServerError("socket_path_too_long")
    try:
        resolved_parent = socket_path.parent.resolve(strict=True)
        resolved_root = runtime_root.resolve(strict=True)
    except OSError as error:
        raise ServerError("socket_parent_unavailable") from error
    if resolved_parent != resolved_root or socket_path.name in {"", ".", ".."}:
        raise ServerError("socket_outside_runtime")


def safe_existing_socket(path: Path) -> os.stat_result:
    try:
        info = path.lstat()
    except OSError as error:
        raise ServerError("socket_lstat_failed") from error
    if (
        not stat.S_ISSOCK(info.st_mode)
        or info.st_uid != os.geteuid()
        or stat.S_IMODE(info.st_mode) != 0o600
    ):
        raise ServerError("socket_collision")
    return info


def probe_existing_socket(path: Path) -> bool:
    probe = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        probe.settimeout(0.25)
        probe.connect(str(path))
        return True
    except (ConnectionRefusedError, FileNotFoundError):
        return False
    except OSError as error:
        if error.errno in {errno.ECONNREFUSED, errno.ENOENT}:
            return False
        raise ServerError("socket_probe_failed") from error
    finally:
        probe.close()


def remove_stale_socket(path: Path) -> None:
    safe_existing_socket(path)
    if probe_existing_socket(path):
        raise AlreadyRunning("already_running")
    try:
        path.unlink()
    except OSError as error:
        raise ServerError("stale_socket_unlink_failed") from error


def load_youtube_dl(module_path: Path) -> Any:
    if not module_path.is_absolute():
        raise ServerError("module_path_not_absolute")
    try:
        info = module_path.lstat()
    except OSError as error:
        raise ServerError("module_path_unavailable") from error
    if stat.S_ISLNK(info.st_mode) or not (
        stat.S_ISREG(info.st_mode) or stat.S_ISDIR(info.st_mode)
    ):
        raise ServerError("module_path_invalid")
    sys.path.insert(0, str(module_path))
    try:
        from yt_dlp import YoutubeDL  # type: ignore[import-not-found]
    except Exception as error:
        raise ServerError("module_import_failed") from error
    return YoutubeDL


def ydl_options(player_client: str, cache_root: Path) -> Dict[str, object]:
    options: Dict[str, object] = {
        "allowed_extractors": ["youtube", "youtube:tab", "youtube:search"],
        "cachedir": str(cache_root),
        "extractor_args": {"youtube": {"player_client": [player_client]}},
        "extractor_retries": 1,
        "logger": SilentLogger(),
        "noplaylist": True,
        "no_warnings": True,
        "quiet": True,
        "retries": 1,
        "skip_download": True,
        "socket_timeout": 30,
    }
    if shutil.which("node") is not None:
        options["js_runtimes"] = {"node": {"path": None}}
    return options


def configure_video_ydl(ydl: Any, player_client: str) -> None:
    ydl.params["extractor_args"] = {
        "youtube": {"player_client": [player_client]}
    }
    ydl.params["noplaylist"] = True
    for key in (
        "extract_flat",
        "ignoreerrors",
        "lazy_playlist",
        "playlistend",
        "playliststart",
    ):
        ydl.params.pop(key, None)


def configure_feed_ydl(ydl: Any) -> None:
    ydl.params["extract_flat"] = True
    ydl.params["ignoreerrors"] = True
    ydl.params["lazy_playlist"] = True
    ydl.params["noplaylist"] = False
    ydl.params["playliststart"] = 1
    ydl.params["playlistend"] = MAX_FEED_FETCH_ITEMS


def clean_feed_text(value: object, max_chars: int, max_bytes: int) -> str:
    if not isinstance(value, str):
        return ""
    value = unicodedata.normalize("NFKC", value)
    value = "".join(
        " " if unicodedata.category(character).startswith("C") else character
        for character in value
    )
    value = " ".join(value.split())[:max_chars]
    while value and len(value.encode("utf-8")) > max_bytes:
        value = value[:-1]
    return value


def parse_feed_request(
    request: Dict[str, object]
) -> Tuple[str, str, int, int, str, str]:
    required = {"version", "op", "mode", "value", "offset", "limit", "priority"}
    if set(request) not in (required, required | {"encoding"}):
        raise ServerError("request_fields_invalid")
    if request.get("version") != PROTOCOL_VERSION or request.get("op") != "feed":
        raise ServerError("request_version_invalid")
    mode = request.get("mode")
    if not isinstance(mode, str) or mode not in FEED_MODES:
        raise ServerError("feed_mode_invalid")
    value = clean_feed_text(request.get("value"), MAX_FEED_VALUE_CHARS, MAX_FEED_VALUE_BYTES)
    if not value or (mode == "channel" and CHANNEL_ID_RE.fullmatch(value) is None):
        raise ServerError("feed_value_invalid")
    offset = request.get("offset")
    limit = request.get("limit")
    if (
        isinstance(offset, bool)
        or not isinstance(offset, int)
        or not 0 <= offset < MAX_FEED_ITEMS
    ):
        raise ServerError("feed_offset_invalid")
    if (
        isinstance(limit, bool)
        or not isinstance(limit, int)
        or not 1 <= limit <= MAX_FEED_ITEMS
        or offset + limit > MAX_FEED_ITEMS
    ):
        raise ServerError("feed_limit_invalid")
    priority = request.get("priority")
    if not isinstance(priority, str) or priority not in PRIORITIES:
        raise ServerError("priority_invalid")
    encoding = request.get("encoding", "json")
    if encoding not in {"json", "tsv1"}:
        raise ServerError("feed_encoding_invalid")
    assert isinstance(encoding, str)
    return mode, value, offset, limit, priority, encoding


def feed_target(mode: str, value: str) -> str:
    if mode == "channel":
        return f"https://www.youtube.com/channel/{value}/videos"
    return f"ytsearch{MAX_FEED_FETCH_ITEMS}:{value}"


def feed_published_epoch(raw: Dict[str, object]) -> int:
    for key in ("timestamp", "release_timestamp"):
        value = raw.get(key)
        if (
            isinstance(value, (int, float))
            and not isinstance(value, bool)
            and math.isfinite(float(value))
            and 0 < float(value) < 4_102_444_800
        ):
            return int(value)
    for key in ("upload_date", "release_date"):
        value = raw.get(key)
        if not isinstance(value, str) or re.fullmatch(r"\d{8}", value) is None:
            continue
        try:
            return int(calendar.timegm(time.strptime(value, "%Y%m%d")))
        except (OverflowError, ValueError):
            continue
    return 0


def feed_published(raw: Dict[str, object], epoch: int) -> str:
    value = clean_feed_text(raw.get("published"), 40, 92)
    if value:
        return value
    if epoch:
        return time.strftime("%Y-%m-%d", time.gmtime(epoch))
    return ""


def feed_entry_is_playable(raw: Dict[str, object]) -> bool:
    """Reject entries that an anonymous player cannot open.

    Flat playlist extraction deliberately avoids a per-video request, but it
    still exposes YouTube's availability marker.  Treat an absent marker as
    the normal public case and allow the two explicit anonymous states.  All
    other named states (members-only, private, premium, authentication) must
    never reach the selectable catalog.  Upcoming premieres are metadata, not
    a stream that can be started immediately.
    """
    availability = raw.get("availability")
    if isinstance(availability, str):
        availability = availability.strip().lower()
        if availability and availability not in {"public", "unlisted"}:
            return False
    live_status = raw.get("live_status")
    return live_status != "is_upcoming"


def bounded_feed_snapshot(
    value: object, now: Optional[int] = None,
    expected_channel_id: Optional[str] = None,
) -> FeedSnapshot:
    if not isinstance(value, dict) or not isinstance(value.get("entries"), list):
        raise ServerError("feed_metadata_invalid")
    items: List[Dict[str, object]] = []
    seen = set()
    for raw in value["entries"]:
        if len(items) >= MAX_FEED_FETCH_ITEMS:
            break
        if not isinstance(raw, dict):
            continue
        if not feed_entry_is_playable(raw):
            continue
        if expected_channel_id is not None:
            entry_channel_id = (
                raw.get("channel_id")
                or raw.get("uploader_id")
                or raw.get("playlist_channel_id")
            )
            if entry_channel_id != expected_channel_id:
                continue
        video_id = raw.get("id")
        title = clean_feed_text(raw.get("title"), 96, 240)
        if (
            not isinstance(video_id, str)
            or VIDEO_ID_RE.fullmatch(video_id) is None
            or video_id in seen
            or not title
        ):
            continue
        epoch = feed_published_epoch(raw)
        duration = bounded_number(raw.get("duration"))
        if isinstance(duration, float):
            duration = int(round(duration))
        if not isinstance(duration, int) or not 0 <= duration <= 10 * 24 * 60 * 60:
            duration = 0
        items.append(
            {
                "id": video_id,
                "title": title,
                "channel": clean_feed_text(
                    raw.get("channel") or raw.get("uploader"), 64, 160
                ) or "YouTube",
                "published": feed_published(raw, epoch),
                "published_epoch": epoch,
                "duration": duration,
            }
        )
        seen.add(video_id)
    if not items:
        raise ServerError("feed_metadata_empty")
    has_more = len(items) > MAX_FEED_ITEMS
    generated_at = int(time.time()) if now is None else now
    return FeedSnapshot(
        tuple(items[:MAX_FEED_ITEMS]),
        has_more,
        generated_at,
        generated_at + FEED_TTL_SECONDS,
    )


def feed_response(
    snapshot: FeedSnapshot,
    mode: str,
    value: str,
    offset: int,
    limit: int,
    cache_state: str,
    encoding: str,
) -> bytes:
    items = snapshot.items[offset : offset + limit]
    if not items:
        raise ServerError("feed_page_empty")
    next_offset = offset + len(items)
    has_next = next_offset < len(snapshot.items) or (
        next_offset >= len(snapshot.items) and snapshot.has_more
    )
    if encoding == "tsv1":
        tsv_cache_state = (
            cache_state
            if cache_state in {"MISS", "STALE"}
            else "HIT"
        )
        lines = []
        for item in items:
            video_id = str(item["id"])
            lines.append(
                "\t".join(
                    (
                        "ITEM",
                        video_id,
                        str(item["title"]),
                        str(item["channel"]),
                        str(item["published"]),
                        str(item["duration"]),
                        f"https://i.ytimg.com/vi/{video_id}/mqdefault.jpg",
                        f"https://www.youtube.com/watch?v={video_id}",
                        "",
                    )
                )
            )
        lines.append(f"BATCH\t{len(items)}\t{len(items)}\tmore=NO")
        next_value = str(next_offset) if has_next and next_offset < MAX_FEED_ITEMS else "END"
        lines.append(
            f"DONE\t{len(items)}\t{value}\tcache={tsv_cache_state}\tnext={next_value}"
        )
        payload = ("\n".join(lines) + "\n").encode("utf-8")
        if len(payload) > MAX_RESPONSE_BYTES:
            raise ServerError("response_too_large")
        return payload
    return encode_response(
        {
            "version": PROTOCOL_VERSION,
            "ok": True,
            "feed": {
                "mode": mode,
                "value": value,
                "offset": offset,
                "count": len(items),
                "next": next_offset if has_next and next_offset < MAX_FEED_ITEMS else None,
                "end": not has_next or next_offset >= MAX_FEED_ITEMS,
                "cache": cache_state,
                "items": list(items),
            },
        }
    )


def bounded_number(value: object) -> Optional[object]:
    if isinstance(value, bool):
        return None
    if isinstance(value, int):
        return value
    if isinstance(value, float) and math.isfinite(value):
        return value
    return None


def bounded_headers(value: object) -> Dict[str, str]:
    if not isinstance(value, dict) or len(value) > MAX_HEADER_COUNT:
        return {}
    result: Dict[str, str] = {}
    total = 0
    for key, item in value.items():
        if not isinstance(key, str) or not isinstance(item, str):
            continue
        if any(character in key or character in item for character in "\r\n\0"):
            continue
        total += len(key.encode("utf-8")) + len(item.encode("utf-8"))
        if total > MAX_HEADER_BYTES:
            raise ServerError("metadata_headers_too_large")
        result[key] = item
    return result


def bounded_format(value: object) -> Optional[Dict[str, object]]:
    if not isinstance(value, dict):
        return None
    result: Dict[str, object] = {}
    for key in ("format_id", "vcodec", "acodec"):
        item = value.get(key)
        if isinstance(item, str) and len(item.encode("utf-8")) <= 256:
            result[key] = item
    for key in (
        "filesize",
        "filesize_approx",
        "width",
        "height",
        "fps",
        "tbr",
        "abr",
    ):
        item = bounded_number(value.get(key))
        if item is not None:
            result[key] = item
    url = value.get("url")
    if isinstance(url, str):
        try:
            size = len(url.encode("utf-8"))
        except UnicodeEncodeError:
            size = MAX_SIGNED_URL_BYTES + 1
        if 0 < size <= MAX_SIGNED_URL_BYTES:
            result["url"] = url
    headers = bounded_headers(value.get("http_headers"))
    if headers:
        result["http_headers"] = headers
    return result


def bounded_metadata(value: object, expected_id: str) -> Dict[str, object]:
    if not isinstance(value, dict) or value.get("id") != expected_id:
        raise ServerError("metadata_identity_mismatch")
    raw_formats = value.get("formats")
    if not isinstance(raw_formats, list) or not 1 <= len(raw_formats) <= MAX_FORMATS:
        raise ServerError("metadata_formats_invalid")
    formats = []
    for raw_format in raw_formats:
        item = bounded_format(raw_format)
        if item is not None:
            formats.append(item)
    if not formats:
        raise ServerError("metadata_formats_empty")
    result: Dict[str, object] = {"id": expected_id, "formats": formats}
    duration = bounded_number(value.get("duration"))
    if duration is not None:
        result["duration"] = duration
    return result


def encode_response(value: Dict[str, object]) -> bytes:
    try:
        payload = json.dumps(
            value, ensure_ascii=True, allow_nan=False, separators=(",", ":")
        ).encode("ascii") + b"\n"
    except (TypeError, ValueError) as error:
        raise ServerError("response_json_invalid") from error
    if len(payload) > MAX_RESPONSE_BYTES:
        raise ServerError("response_too_large")
    return payload


def error_response(reason: str) -> bytes:
    safe_reason = reason if re.fullmatch(r"[a-z0-9_]{1,64}", reason) else "internal_error"
    return encode_response(
        {"version": PROTOCOL_VERSION, "ok": False, "error": safe_reason}
    )


def feed_cache_key(mode: str, value: str) -> str:
    return hashlib.sha256(
        f"{FEED_CACHE_SCHEMA}\0{mode}\0{value}".encode("utf-8")
    ).hexdigest()


def feed_cache_path(cache_root: Path, mode: str, value: str) -> Path:
    return cache_root / f"{feed_cache_key(mode, value)}.json"


def load_feed_cache(
    cache_root: Path, mode: str, value: str, now: int
) -> Optional[Tuple[FeedSnapshot, str]]:
    path = feed_cache_path(cache_root, mode, value)
    flags = os.O_RDONLY | os.O_CLOEXEC
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    try:
        descriptor = os.open(path, flags)
    except FileNotFoundError:
        return None
    except OSError:
        raise ServerError("feed_cache_read_failed")
    try:
        info = os.fstat(descriptor)
        if (
            not stat.S_ISREG(info.st_mode)
            or info.st_uid != os.geteuid()
            or info.st_nlink != 1
            or stat.S_IMODE(info.st_mode) != 0o600
            or not 2 <= info.st_size <= MAX_FEED_CACHE_BYTES
        ):
            raise ServerError("feed_cache_unsafe")
        payload = bytearray()
        while len(payload) <= info.st_size:
            block = os.read(descriptor, min(65536, info.st_size + 1 - len(payload)))
            if not block:
                break
            payload.extend(block)
        if len(payload) != info.st_size:
            raise ServerError("feed_cache_changed")
    finally:
        os.close(descriptor)
    try:
        document = json.loads(payload.decode("utf-8", "strict"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ServerError("feed_cache_json_invalid") from error
    if (
        not isinstance(document, dict)
        or document.get("schema") != FEED_CACHE_SCHEMA
        or document.get("mode") != mode
        or document.get("value") != value
        or not isinstance(document.get("has_more"), bool)
        or not isinstance(document.get("items"), list)
    ):
        raise ServerError("feed_cache_identity_invalid")
    generated_at = document.get("generated_at")
    expires_at = document.get("expires_at")
    if (
        isinstance(generated_at, bool)
        or not isinstance(generated_at, int)
        or isinstance(expires_at, bool)
        or not isinstance(expires_at, int)
        or generated_at > now + 30
        or expires_at <= generated_at
        or expires_at > generated_at + 600
        or now - expires_at > FEED_STALE_SECONDS
    ):
        raise ServerError("feed_cache_timestamp_invalid")
    synthetic = []
    for item in document["items"]:
        if not isinstance(item, dict):
            continue
        synthetic.append(
            {
                "id": item.get("id"),
                "title": item.get("title"),
                "channel": item.get("channel"),
                "published": item.get("published"),
                "timestamp": item.get("published_epoch"),
                "duration": item.get("duration"),
            }
        )
    if not 1 <= len(synthetic) <= MAX_FEED_ITEMS:
        raise ServerError("feed_cache_items_invalid")
    checked = bounded_feed_snapshot({"entries": synthetic}, generated_at)
    snapshot = FeedSnapshot(
        checked.items,
        bool(document["has_more"]),
        generated_at,
        expires_at,
    )
    return snapshot, "DISK" if expires_at > now else "STALE"


def prune_feed_cache(cache_root: Path, protected: Path) -> None:
    candidates = []
    for path in cache_root.iterdir():
        if re.fullmatch(r"[0-9a-f]{64}\.json", path.name) is None:
            continue
        try:
            info = path.lstat()
        except OSError:
            continue
        if (
            stat.S_ISREG(info.st_mode)
            and not stat.S_ISLNK(info.st_mode)
            and info.st_uid == os.geteuid()
            and info.st_nlink == 1
            and stat.S_IMODE(info.st_mode) == 0o600
        ):
            candidates.append((info.st_mtime, path))
    excess = max(0, len(candidates) - MAX_FEED_CACHE_WINDOWS)
    removable = sorted(
        (item for item in candidates if item[1] != protected),
        key=lambda item: item[0],
    )
    for _mtime, path in removable[:excess]:
        try:
            path.unlink()
        except OSError:
            pass


def write_feed_cache(
    cache_root: Path, mode: str, value: str, snapshot: FeedSnapshot
) -> None:
    path = feed_cache_path(cache_root, mode, value)
    document = {
        "schema": FEED_CACHE_SCHEMA,
        "mode": mode,
        "value": value,
        "generated_at": snapshot.generated_at,
        "expires_at": snapshot.expires_at,
        "has_more": snapshot.has_more,
        "items": list(snapshot.items),
    }
    try:
        payload = (
            json.dumps(
                document,
                ensure_ascii=False,
                allow_nan=False,
                separators=(",", ":"),
            )
            + "\n"
        ).encode("utf-8")
    except (TypeError, ValueError) as error:
        raise ServerError("feed_cache_json_invalid") from error
    if not 2 <= len(payload) <= MAX_FEED_CACHE_BYTES:
        raise ServerError("feed_cache_size_invalid")
    descriptor = -1
    temporary = ""
    directory_descriptor = -1
    try:
        descriptor, temporary = tempfile.mkstemp(prefix=".feed.", dir=cache_root)
        os.fchmod(descriptor, 0o600)
        view = memoryview(payload)
        while view:
            written = os.write(descriptor, view)
            if written <= 0:
                raise ServerError("feed_cache_write_failed")
            view = view[written:]
        os.fsync(descriptor)
        os.close(descriptor)
        descriptor = -1
        os.replace(temporary, path)
        temporary = ""
        directory_flags = os.O_RDONLY | os.O_CLOEXEC
        if hasattr(os, "O_DIRECTORY"):
            directory_flags |= os.O_DIRECTORY
        directory_descriptor = os.open(cache_root, directory_flags)
        os.fsync(directory_descriptor)
        prune_feed_cache(cache_root, path)
    except OSError as error:
        raise ServerError("feed_cache_write_failed") from error
    finally:
        if descriptor >= 0:
            os.close(descriptor)
        if directory_descriptor >= 0:
            os.close(directory_descriptor)
        if temporary:
            try:
                os.unlink(temporary)
            except OSError:
                pass


def read_request(client: socket.socket) -> Dict[str, object]:
    client.settimeout(CLIENT_READ_TIMEOUT_SECONDS)
    payload = bytearray()
    while len(payload) <= MAX_REQUEST_BYTES:
        block = client.recv(min(1024, MAX_REQUEST_BYTES + 1 - len(payload)))
        if not block:
            raise ServerError("request_truncated")
        payload.extend(block)
        newline = payload.find(b"\n")
        if newline >= 0:
            if newline != len(payload) - 1:
                raise ServerError("request_trailing_data")
            break
    if not payload.endswith(b"\n") or len(payload) > MAX_REQUEST_BYTES:
        raise ServerError("request_too_large")
    try:
        value = json.loads(payload[:-1].decode("utf-8", "strict"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ServerError("request_json_invalid") from error
    if not isinstance(value, dict):
        raise ServerError("request_object_required")
    return value


def peer_is_owner(client: socket.socket) -> bool:
    if not hasattr(socket, "SO_PEERCRED"):
        return True
    try:
        _pid, uid, _gid = struct.unpack(
            "3i", client.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12)
        )
    except (OSError, struct.error):
        return False
    return uid == os.geteuid()


def send_payload(client: socket.socket, payload: bytes) -> None:
    try:
        client.settimeout(CLIENT_WRITE_TIMEOUT_SECONDS)
        client.sendall(payload)
    except OSError:
        pass
    finally:
        client.close()


def socket_peer_alive(client: socket.socket) -> bool:
    """Check peer HUP without consuming protocol bytes or blocking."""
    flags = socket.MSG_PEEK
    if hasattr(socket, "MSG_DONTWAIT"):
        flags |= socket.MSG_DONTWAIT
    previous_timeout = client.gettimeout()
    try:
        # Python implements positive socket timeouts with an internal poll
        # before recv(), which defeats MSG_DONTWAIT.  This descriptor is owned
        # by one queued/running job, so temporarily select nonblocking mode.
        client.setblocking(False)
        payload = client.recv(1, flags)
    except (BlockingIOError, InterruptedError):
        return True
    except OSError:
        return False
    finally:
        try:
            client.settimeout(previous_timeout)
        except OSError:
            pass
    return payload != b""


def set_native_thread_priority(native_tid: int, priority: str) -> None:
    desired = 15 if priority == "background" else 0
    try:
        observed = os.getpriority(os.PRIO_PROCESS, native_tid)
        if observed != desired:
            os.setpriority(os.PRIO_PROCESS, native_tid, desired)
            observed = os.getpriority(os.PRIO_PROCESS, native_tid)
    except (AttributeError, OSError) as error:
        raise ServerError("worker_priority_failed") from error
    if observed != desired:
        raise ServerError("worker_priority_failed")


class ResolverServer:
    def __init__(
        self,
        youtube_dl: Any,
        runtime_root: Path,
        socket_path: Path,
        cache_root: Path,
        workers: int,
        default_player_client: str,
    ) -> None:
        self.youtube_dl = youtube_dl
        self.runtime_root = runtime_root
        self.socket_path = socket_path
        self.cache_root = cache_root
        self.feed_cache_root = cache_root / "feed"
        self.worker_count = workers
        self.default_player_client = default_player_client
        self.stop = threading.Event()
        self.interactive_jobs: Deque[Union[Job, FeedJob]] = deque()
        self.background_jobs: Deque[Union[Job, FeedJob]] = deque()
        self.jobs_ready = threading.Condition()
        self.pending: Dict[str, Job] = {}
        self.feed_pending: Dict[Tuple[str, str], FeedJob] = {}
        self.feed_memory: "OrderedDict[Tuple[str, str], FeedSnapshot]" = OrderedDict()
        self.pending_lock = threading.Lock()
        self.listener: Optional[socket.socket] = None
        self.socket_identity: Optional[Tuple[int, int]] = None
        self.workers: List[threading.Thread] = []
        self.worker_ready: List[threading.Event] = []
        self.worker_errors: List[Optional[str]] = [None] * workers
        self.background_active = 0

    def job_has_demand(self, job: Union[Job, FeedJob]) -> bool:
        # A stale-cache refresh deliberately has no waiting client.  Every
        # other job must retain at least one live AF_UNIX peer.
        if isinstance(job, FeedJob) and job.priority == "background" and not job.clients:
            return True
        dead: List[socket.socket] = []
        with self.pending_lock:
            if isinstance(job, FeedJob):
                live_waiters: List[FeedWaiter] = []
                for waiter in job.clients:
                    if socket_peer_alive(waiter.client):
                        live_waiters.append(waiter)
                    else:
                        dead.append(waiter.client)
                job.clients = live_waiters
                live = bool(live_waiters)
            else:
                live_clients: List[socket.socket] = []
                for client in job.clients:
                    if socket_peer_alive(client):
                        live_clients.append(client)
                    else:
                        dead.append(client)
                job.clients = live_clients
                live = bool(live_clients)
        for client in dead:
            try:
                client.close()
            except OSError:
                pass
        return live

    def discard_undemanded_job(self, job: Union[Job, FeedJob]) -> None:
        with self.pending_lock:
            if isinstance(job, FeedJob):
                if self.feed_pending.get(job.key) is job:
                    del self.feed_pending[job.key]
            elif self.pending.get(job.video_id) is job:
                del self.pending[job.video_id]
        diagnostic(
            "cancel", kind="feed" if isinstance(job, FeedJob) else "resolve",
            stage="queued", waiters=0
        )

    def take_job(self) -> Optional[Union[Job, FeedJob]]:
        while not self.stop.is_set():
            job: Optional[Union[Job, FeedJob]] = None
            with self.jobs_ready:
                if self.interactive_jobs:
                    job = self.interactive_jobs.popleft()
                    job.queued = False
                elif (
                    self.background_jobs
                    and self.background_active < MAX_BACKGROUND_WORKERS
                ):
                    job = self.background_jobs.popleft()
                    job.queued = False
                    job.used_background_slot = True
                    self.background_active += 1
                else:
                    self.jobs_ready.wait(timeout=0.25)
                    continue
            assert job is not None
            if self.job_has_demand(job):
                return job
            self.discard_undemanded_job(job)
            self.release_background_slot(job)
        return None

    def release_background_slot(self, job: Union[Job, FeedJob]) -> None:
        if not job.used_background_slot:
            return
        with self.jobs_ready:
            job.used_background_slot = False
            self.background_active -= 1
            if self.background_active < 0:
                raise ServerError("background_slot_underflow")
            self.jobs_ready.notify_all()

    def enqueue_job(self, job: Union[Job, FeedJob]) -> None:
        with self.jobs_ready:
            jobs = (
                self.interactive_jobs
                if job.priority == "interactive"
                else self.background_jobs
            )
            jobs.append(job)
            self.jobs_ready.notify()

    def promote_queued_job(self, job: Union[Job, FeedJob]) -> None:
        with self.jobs_ready:
            if not job.queued:
                return
            try:
                self.background_jobs.remove(job)
            except ValueError:
                return
            self.interactive_jobs.appendleft(job)
            self.jobs_ready.notify()

    def on_signal(self, _signum: int, _frame: object) -> None:
        self.stop.set()

    def worker_loop(self, index: int, ready: threading.Event) -> None:
        try:
            ydl = self.youtube_dl(
                ydl_options(self.default_player_client, self.cache_root)
            )
        except Exception:
            self.worker_errors[index] = "worker_init_failed"
            ready.set()
            return
        ready.set()
        while not self.stop.is_set():
            job = self.take_job()
            if job is None:
                return
            try:
                if isinstance(job, FeedJob):
                    configure_feed_ydl(ydl)
                    target = feed_target(job.mode, job.value)
                else:
                    configure_video_ydl(ydl, job.player_client)
                    target = job.url

                original_urlopen = getattr(ydl, "urlopen", None)

                def demand_checked_urlopen(request: Any) -> Any:
                    if not self.job_has_demand(job):
                        raise ServerError("request_canceled")
                    if not callable(original_urlopen):
                        raise ServerError("urlopen_unavailable")
                    return original_urlopen(request)

                def extract() -> Any:
                    if not self.job_has_demand(job):
                        raise ServerError("request_canceled")
                    if not callable(original_urlopen):
                        return ydl.extract_info(target, download=False)
                    ydl.urlopen = demand_checked_urlopen
                    try:
                        return ydl.extract_info(target, download=False)
                    finally:
                        ydl.urlopen = original_urlopen

                if job.priority == "background":
                    result_box: List[Any] = []
                    error_box: List[Exception] = []

                    def background_extract() -> None:
                        try:
                            native_tid = threading.get_native_id()
                            with self.pending_lock:
                                job.worker_tid = native_tid
                            set_native_thread_priority(native_tid, "background")
                            result_box.append(extract())
                        except Exception as error:
                            error_box.append(error)

                    extraction = threading.Thread(
                        target=background_extract,
                        name=f"yt-dlp-background-{index}",
                        daemon=True,
                    )
                    extraction.start()
                    extraction.join()
                    if error_box:
                        raise error_box[0]
                    if not result_box:
                        raise ServerError("extract_failed")
                    raw_metadata = result_box[0]
                else:
                    native_tid = threading.get_native_id()
                    with self.pending_lock:
                        job.worker_tid = native_tid
                    set_native_thread_priority(native_tid, "interactive")
                    raw_metadata = extract()
                if isinstance(job, FeedJob):
                    snapshot = bounded_feed_snapshot(
                        raw_metadata,
                        expected_channel_id=(
                            job.value if job.mode == "channel" else None
                        ),
                    )
                    write_feed_cache(
                        self.feed_cache_root, job.mode, job.value, snapshot
                    )
                    with self.pending_lock:
                        self.feed_memory[job.key] = snapshot
                        self.feed_memory.move_to_end(job.key)
                        while len(self.feed_memory) > MAX_FEED_CACHE_WINDOWS:
                            self.feed_memory.popitem(last=False)
                    payload = b""
                else:
                    metadata = bounded_metadata(raw_metadata, job.video_id)
                    payload = encode_response(
                        {
                            "version": PROTOCOL_VERSION,
                            "ok": True,
                            "metadata": metadata,
                        }
                    )
                result = "PASS"
            except ServerError as error:
                payload = error_response(str(error))
                result = "FAIL"
            except Exception:
                payload = error_response("extract_failed")
                result = "FAIL"
            if isinstance(job, FeedJob):
                self.job_has_demand(job)
                with self.pending_lock:
                    current = self.feed_pending.get(job.key)
                    if current is job:
                        del self.feed_pending[job.key]
                    job.worker_tid = None
                    waiters = list(job.clients)
                    job.clients.clear()
                for waiter in waiters:
                    if result == "PASS":
                        try:
                            waiter_payload = feed_response(
                                snapshot,
                                job.mode,
                                job.value,
                                waiter.offset,
                                waiter.limit,
                                waiter.cache_state,
                                waiter.encoding,
                            )
                        except ServerError as error:
                            waiter_payload = error_response(str(error))
                    else:
                        waiter_payload = payload
                    send_payload(waiter.client, waiter_payload)
                diagnostic(
                    "feed",
                    result=result,
                    mode=job.mode,
                    waiters=len(waiters),
                    coalesced=max(0, len(waiters) - 1),
                    items=len(snapshot.items) if result == "PASS" else 0,
                    priority=job.priority,
                    promoted=int(job.promoted),
                    elapsed_ms=max(
                        0, int((time.monotonic() - job.queued_monotonic) * 1000)
                    ),
                    worker=index,
                )
            else:
                self.job_has_demand(job)
                with self.pending_lock:
                    current = self.pending.get(job.video_id)
                    if current is job:
                        del self.pending[job.video_id]
                    job.worker_tid = None
                    clients = list(job.clients)
                    job.clients.clear()
                for client in clients:
                    send_payload(client, payload)
                diagnostic(
                    "resolve",
                    result=result,
                    id=job.video_id,
                    waiters=len(clients),
                    coalesced=max(0, len(clients) - 1),
                    priority=job.priority,
                    promoted=int(job.promoted),
                    elapsed_ms=max(
                        0, int((time.monotonic() - job.queued_monotonic) * 1000)
                    ),
                    worker=index,
                )
            self.release_background_slot(job)

    def start_workers(self) -> None:
        for index in range(self.worker_count):
            ready = threading.Event()
            worker = threading.Thread(
                target=self.worker_loop,
                args=(index, ready),
                name=f"yt-dlp-worker-{index}",
                daemon=True,
            )
            self.worker_ready.append(ready)
            self.workers.append(worker)
            worker.start()
        for ready in self.worker_ready:
            if not ready.wait(10.0):
                raise ServerError("worker_init_timeout")
        if any(self.worker_errors):
            raise ServerError("worker_init_failed")

    def bind(self) -> None:
        try:
            self.socket_path.lstat()
        except FileNotFoundError:
            pass
        else:
            remove_stale_socket(self.socket_path)
        listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            listener.bind(str(self.socket_path))
            os.chmod(self.socket_path, 0o600)
            info = safe_existing_socket(self.socket_path)
            listener.listen(LISTEN_BACKLOG)
            listener.settimeout(0.5)
        except OSError as error:
            listener.close()
            if error.errno == errno.EADDRINUSE:
                safe_existing_socket(self.socket_path)
                if probe_existing_socket(self.socket_path):
                    raise AlreadyRunning("already_running") from error
            raise ServerError(f"socket_bind_failed_errno_{error.errno}") from error
        self.listener = listener
        self.socket_identity = (info.st_dev, info.st_ino)

    def cached_feed(
        self, mode: str, value: str
    ) -> Optional[Tuple[FeedSnapshot, str]]:
        key = (mode, value)
        now = int(time.time())
        with self.pending_lock:
            snapshot = self.feed_memory.get(key)
            if snapshot is not None:
                if now - snapshot.expires_at <= FEED_STALE_SECONDS:
                    self.feed_memory.move_to_end(key)
                    return snapshot, "MEMORY" if snapshot.expires_at > now else "STALE"
                del self.feed_memory[key]
        try:
            cached = load_feed_cache(self.feed_cache_root, mode, value, now)
        except ServerError:
            return None
        if cached is not None:
            with self.pending_lock:
                self.feed_memory[key] = cached[0]
                self.feed_memory.move_to_end(key)
                while len(self.feed_memory) > MAX_FEED_CACHE_WINDOWS:
                    self.feed_memory.popitem(last=False)
        return cached

    def queue_feed_refresh(self, mode: str, value: str) -> None:
        key = (mode, value)
        with self.pending_lock:
            if key in self.feed_pending:
                return
            if len(self.pending) + len(self.feed_pending) >= MAX_PENDING_VIDEO_IDS:
                return
            job = FeedJob(mode, value, priority="background")
            self.feed_pending[key] = job
            self.enqueue_job(job)

    def dispatch_feed(self, client: socket.socket, request: Dict[str, object]) -> None:
        mode, value, offset, limit, priority, encoding = parse_feed_request(request)
        cached = self.cached_feed(mode, value)
        if cached is not None:
            snapshot, cache_state = cached
            try:
                payload = feed_response(
                    snapshot, mode, value, offset, limit, cache_state, encoding
                )
            except ServerError:
                cached = None
            else:
                send_payload(client, payload)
                if cache_state == "STALE":
                    self.queue_feed_refresh(mode, value)
                return

        key = (mode, value)
        with self.pending_lock:
            existing = self.feed_pending.get(key)
            if existing is not None:
                if len(existing.clients) >= MAX_WAITERS_PER_VIDEO:
                    raise ServerError("too_many_waiters")
                if existing.priority == "background" and priority == "interactive":
                    existing.priority = "interactive"
                    self.promote_queued_job(existing)
                    if existing.worker_tid is not None:
                        try:
                            set_native_thread_priority(existing.worker_tid, "interactive")
                            existing.promoted = True
                        except ServerError:
                            pass
                existing.clients.append(
                    FeedWaiter(client, offset, limit, "COALESCED", encoding)
                )
                return
            if len(self.pending) + len(self.feed_pending) >= MAX_PENDING_VIDEO_IDS:
                raise ServerError("server_busy")
            job = FeedJob(
                mode,
                value,
                clients=[FeedWaiter(client, offset, limit, "MISS", encoding)],
                priority=priority,
            )
            self.feed_pending[key] = job
            self.enqueue_job(job)

    def dispatch(self, client: socket.socket, request: Dict[str, object]) -> None:
        if request == {"version": PROTOCOL_VERSION, "op": "ping"}:
            send_payload(
                client,
                encode_response(
                    {
                        "version": PROTOCOL_VERSION,
                        "ok": True,
                        "workers": self.worker_count,
                    }
                ),
            )
            return
        if request.get("op") == "feed":
            self.dispatch_feed(client, request)
            return
        if set(request) != {
            "version", "op", "url", "player_client", "priority"
        }:
            raise ServerError("request_fields_invalid")
        if request.get("version") != PROTOCOL_VERSION or request.get("op") != "extract":
            raise ServerError("request_version_invalid")
        video_id = video_id_for_url(request.get("url"))
        player_client = request.get("player_client")
        if not isinstance(player_client, str) or PLAYER_CLIENT_RE.fullmatch(player_client) is None:
            raise ServerError("player_client_invalid")
        priority = request.get("priority")
        if not isinstance(priority, str) or priority not in PRIORITIES:
            raise ServerError("priority_invalid")
        url = request["url"]
        assert isinstance(url, str)
        with self.pending_lock:
            existing = self.pending.get(video_id)
            if existing is not None:
                if existing.player_client != player_client:
                    raise ServerError("request_conflict")
                if len(existing.clients) >= MAX_WAITERS_PER_VIDEO:
                    raise ServerError("too_many_waiters")
                if existing.priority == "background" and priority == "interactive":
                    existing.priority = "interactive"
                    self.promote_queued_job(existing)
                    if existing.worker_tid is not None:
                        try:
                            set_native_thread_priority(
                                existing.worker_tid, "interactive"
                            )
                            existing.promoted = True
                        except ServerError:
                            # NI=15 is safe; inability to promote affects latency,
                            # never foreground responsiveness of other work.
                            pass
                existing.clients.append(client)
                return
            if len(self.pending) + len(self.feed_pending) >= MAX_PENDING_VIDEO_IDS:
                raise ServerError("server_busy")
            job = Job(
                video_id,
                url,
                player_client,
                clients=[client],
                priority=priority,
            )
            self.pending[video_id] = job
            self.enqueue_job(job)

    def serve(self) -> None:
        assert self.listener is not None
        diagnostic(
            "ready", workers=self.worker_count, socket_mode="0600", protocol=1
        )
        while not self.stop.is_set():
            try:
                client, _address = self.listener.accept()
            except socket.timeout:
                continue
            except OSError:
                if self.stop.is_set():
                    break
                raise ServerError("socket_accept_failed")
            try:
                if not peer_is_owner(client):
                    raise ServerError("peer_not_owner")
                request = read_request(client)
                self.dispatch(client, request)
                client = None  # type: ignore[assignment]
            except (OSError, ServerError) as error:
                reason = str(error) if isinstance(error, ServerError) else "request_io_failed"
                send_payload(client, error_response(reason))

    def cleanup(self) -> None:
        self.stop.set()
        if self.listener is not None:
            self.listener.close()
            self.listener = None
        with self.pending_lock:
            clients = [client for job in self.pending.values() for client in job.clients]
            clients.extend(
                waiter.client
                for job in self.feed_pending.values()
                for waiter in job.clients
            )
            self.pending.clear()
            self.feed_pending.clear()
        shutdown_payload = error_response("server_stopping")
        for client in clients:
            send_payload(client, shutdown_payload)
        with self.jobs_ready:
            self.interactive_jobs.clear()
            self.background_jobs.clear()
            self.jobs_ready.notify_all()
        for worker in self.workers:
            worker.join(timeout=1.0)
        if self.socket_identity is not None:
            try:
                info = self.socket_path.lstat()
                if (info.st_dev, info.st_ino) == self.socket_identity:
                    self.socket_path.unlink()
            except FileNotFoundError:
                pass
            except OSError:
                pass

    def run(self) -> int:
        try:
            self.start_workers()
            self.bind()
            self.serve()
            return 0
        finally:
            self.cleanup()


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    runtime_default = Path(
        os.environ.get("RG_YOUTUBE_RUNTIME_ROOT", "/run/rg40xxv-youtube")
    )
    xdg_cache_default = Path(
        os.environ.get("XDG_CACHE_HOME", str(Path.home() / ".cache"))
    )
    cache_default = Path(
        os.environ.get(
            "RG_YOUTUBE_YTDLP_CACHE",
            str(xdg_cache_default / "rg40xxv-youtube" / "yt-dlp"),
        )
    )
    bundled = Path(__file__).resolve().parent.parent / "vendor" / "yt-dlp"
    parser = argparse.ArgumentParser()
    parser.add_argument("--runtime-root", type=Path, default=runtime_default)
    parser.add_argument("--cache-dir", type=Path, default=cache_default)
    parser.add_argument(
        "--socket",
        dest="socket_path",
        type=Path,
        default=Path(
            os.environ.get(
                "RG_YOUTUBE_YTDLP_SOCKET", str(runtime_default / "yt-dlp.sock")
            )
        ),
    )
    parser.add_argument("--yt-dlp", type=Path, default=bundled)
    parser.add_argument("--workers", type=int, default=MAX_WORKERS)
    parser.add_argument("--player-client", default="android")
    args = parser.parse_args(argv)
    if not 1 <= args.workers <= MAX_WORKERS:
        parser.error("workers must be between one and two")
    if PLAYER_CLIENT_RE.fullmatch(args.player_client) is None:
        parser.error("invalid player client")
    return args


def main(argv: Optional[Sequence[str]] = None) -> int:
    os.umask(0o077)
    args = parse_args(sys.argv[1:] if argv is None else argv)
    try:
        private_runtime_root(args.runtime_root)
        private_cache_root(args.cache_dir)
        private_cache_root(args.cache_dir / "feed")
        validate_socket_path(args.runtime_root, args.socket_path)
        try:
            args.socket_path.lstat()
        except FileNotFoundError:
            pass
        else:
            safe_existing_socket(args.socket_path)
            if probe_existing_socket(args.socket_path):
                diagnostic("already_running", phase="preimport")
                return 0
        youtube_dl = load_youtube_dl(args.yt_dlp)
        server = ResolverServer(
            youtube_dl,
            args.runtime_root,
            args.socket_path,
            args.cache_dir,
            args.workers,
            args.player_client,
        )
        for signum in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
            signal.signal(signum, server.on_signal)
        return server.run()
    except AlreadyRunning:
        diagnostic("already_running")
        return 0
    except ServerError as error:
        diagnostic("fatal", reason=str(error))
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
