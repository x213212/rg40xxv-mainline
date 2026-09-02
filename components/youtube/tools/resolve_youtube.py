#!/usr/bin/env python3
"""Resolve a YouTube watch URL into a private range-bridge configuration."""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import socket
import ssl
import stat
import struct
import subprocess
import sys
import tempfile
import time
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Dict, Iterable, Optional


CONTENT_RANGE_RE = re.compile(r"^bytes ([0-9]+)-([0-9]+)/([0-9]+)$")
VIDEO_ID_RE = re.compile(r"^[A-Za-z0-9_-]{11}$")
SHORT_URL_RE = re.compile(r"^https://youtu\.be/([A-Za-z0-9_-]{11})$")
WATCH_URL_RE = re.compile(
    r"^https://(?:www\.)?youtube\.com/watch\?v=([A-Za-z0-9_-]{11})$"
)
PLAYER_CLIENT_RE = re.compile(r"^[a-z0-9_-]{1,32}$")
RESOLVE_PRIORITIES = {"interactive", "background"}
MAX_MEDIA_BYTES = 128 * 1024 * 1024 * 1024
MAX_SERVER_REQUEST_BYTES = 4096
MAX_SERVER_RESPONSE_BYTES = 2 * 1024 * 1024
SERVER_CONNECT_TIMEOUT_SECONDS = 0.5
SERVER_RESPONSE_TIMEOUT_SECONDS = 48.0
BACKGROUND_SERVER_RETRY_COUNT = 30
BACKGROUND_SERVER_RETRY_SECONDS = 0.1
FORBIDDEN_PROBE_HEADERS = {
    "connection", "content-length", "host", "range", "transfer-encoding"
}


class ResolverServerUnavailable(RuntimeError):
    pass


class ResolverServerFailure(RuntimeError):
    pass


def video_id_for_url(value: object) -> str:
    if not isinstance(value, str):
        raise RuntimeError("invalid YouTube watch URL")
    try:
        encoded = value.encode("ascii")
    except UnicodeEncodeError as error:
        raise RuntimeError("invalid YouTube watch URL") from error
    if not encoded or len(encoded) > 256:
        raise RuntimeError("invalid YouTube watch URL")
    match = SHORT_URL_RE.fullmatch(value) or WATCH_URL_RE.fullmatch(value)
    if match is None or VIDEO_ID_RE.fullmatch(match.group(1)) is None:
        raise RuntimeError("invalid YouTube watch URL")
    return match.group(1)


def require_private_server_socket(path: Path) -> None:
    if not path.is_absolute() or path == Path("/") or len(os.fsencode(path)) > 100:
        raise ResolverServerUnavailable("invalid socket path")
    try:
        parent = path.parent.lstat()
        info = path.lstat()
    except OSError as error:
        raise ResolverServerUnavailable("resolver server unavailable") from error
    if (
        not stat.S_ISDIR(parent.st_mode)
        or stat.S_ISLNK(parent.st_mode)
        or parent.st_uid != os.geteuid()
        or stat.S_IMODE(parent.st_mode) != 0o700
        or not stat.S_ISSOCK(info.st_mode)
        or info.st_uid != os.geteuid()
        or stat.S_IMODE(info.st_mode) != 0o600
    ):
        raise ResolverServerUnavailable("resolver server is not private")


def server_peer_is_owner(connection: socket.socket) -> bool:
    if not hasattr(socket, "SO_PEERCRED"):
        return True
    try:
        _pid, uid, _gid = struct.unpack(
            "3i", connection.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12)
        )
    except (OSError, struct.error):
        return False
    return uid == os.geteuid()


def receive_server_response(connection: socket.socket) -> Dict[str, object]:
    payload = bytearray()
    while len(payload) <= MAX_SERVER_RESPONSE_BYTES:
        block = connection.recv(
            min(65536, MAX_SERVER_RESPONSE_BYTES + 1 - len(payload))
        )
        if not block:
            raise ResolverServerFailure("resolver server response truncated")
        payload.extend(block)
        newline = payload.find(b"\n")
        if newline >= 0:
            if newline != len(payload) - 1:
                raise ResolverServerFailure("resolver server trailing data")
            break
    if not payload.endswith(b"\n") or len(payload) > MAX_SERVER_RESPONSE_BYTES:
        raise ResolverServerFailure("resolver server response too large")
    try:
        value = json.loads(payload[:-1].decode("utf-8", "strict"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ResolverServerFailure("resolver server JSON invalid") from error
    if not isinstance(value, dict) or value.get("version") != 1:
        raise ResolverServerFailure("resolver server protocol invalid")
    return value


def metadata_from_server(
    socket_path: Path, url: str, player_client: str, priority: str
) -> Dict[str, object]:
    require_private_server_socket(socket_path)
    request = (
        json.dumps(
            {
                "version": 1,
                "op": "extract",
                "url": url,
                "player_client": player_client,
                "priority": priority,
            },
            ensure_ascii=True,
            separators=(",", ":"),
        ).encode("ascii")
        + b"\n"
    )
    if len(request) > MAX_SERVER_REQUEST_BYTES:
        raise ResolverServerFailure("resolver server request too large")
    connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        connection.settimeout(SERVER_CONNECT_TIMEOUT_SECONDS)
        try:
            connection.connect(str(socket_path))
        except OSError as error:
            raise ResolverServerUnavailable("resolver server connect failed") from error
        if not server_peer_is_owner(connection):
            raise ResolverServerUnavailable("resolver server peer rejected")
        connection.settimeout(SERVER_RESPONSE_TIMEOUT_SECONDS)
        try:
            connection.sendall(request)
            response = receive_server_response(connection)
        except socket.timeout as error:
            raise ResolverServerFailure("resolver server timed out") from error
        except OSError as error:
            raise ResolverServerFailure("resolver server I/O failed") from error
    finally:
        connection.close()
    if response.get("ok") is not True or set(response) != {
        "version", "ok", "metadata"
    }:
        raise ResolverServerFailure("resolver server extraction failed")
    metadata = response.get("metadata")
    if not isinstance(metadata, dict):
        raise ResolverServerFailure("resolver server metadata invalid")
    if metadata.get("id") != video_id_for_url(url):
        raise ResolverServerFailure("resolver server identity mismatch")
    formats = metadata.get("formats")
    if not isinstance(formats, list) or not 1 <= len(formats) <= 512:
        raise ResolverServerFailure("resolver server formats invalid")
    return metadata


def metadata_from_subprocess(
    executable: str, url: str, player_client: str
) -> Dict[str, object]:
    command = [executable]
    if shutil.which("node") is not None:
        command.extend(("--js-runtimes", "node"))
    command.extend(
        (
            "--extractor-args",
            f"youtube:player_client={player_client}",
            "--no-playlist",
            "--no-warnings",
            "-J",
            url,
        )
    )
    completed = subprocess.run(
        command,
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        timeout=45,
    )
    if len(completed.stdout) > MAX_SERVER_RESPONSE_BYTES:
        raise RuntimeError("yt-dlp output too large")
    try:
        metadata = json.loads(completed.stdout.decode("utf-8", "strict"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise RuntimeError("yt-dlp JSON invalid") from error
    if not isinstance(metadata, dict) or metadata.get("id") != video_id_for_url(url):
        raise RuntimeError("yt-dlp identity mismatch")
    return metadata


def extract_metadata(args: argparse.Namespace) -> Dict[str, object]:
    priority = os.environ.get("RG_YOUTUBE_RESOLVE_PRIORITY", "interactive")
    if priority not in RESOLVE_PRIORITIES:
        raise RuntimeError("invalid resolver priority")
    configured_socket = os.environ.get("RG_YOUTUBE_YTDLP_SOCKET")
    if configured_socket:
        attempts = BACKGROUND_SERVER_RETRY_COUNT if priority == "background" else 1
        for attempt in range(attempts):
            try:
                return metadata_from_server(
                    Path(configured_socket), args.url, args.player_client, priority
                )
            except ResolverServerUnavailable:
                if attempt + 1 < attempts:
                    time.sleep(BACKGROUND_SERVER_RETRY_SECONDS)
                    continue
                if priority == "background":
                    raise
    elif priority == "background":
        raise ResolverServerUnavailable("background resolver server required")
    return metadata_from_subprocess(args.yt_dlp, args.url, args.player_client)


def media_size(item: Dict[str, object]) -> Optional[int]:
    value = item.get("filesize")
    if isinstance(value, (int, float)) and value > 0:
        return int(value)
    url = item.get("url")
    if isinstance(url, str):
        values = urllib.parse.parse_qs(urllib.parse.urlsplit(url).query)
        if values.get("clen", [""])[0].isdigit():
            return int(values["clen"][0])
    value = item.get("filesize_approx")
    if isinstance(value, (int, float)) and value > 0:
        return int(value)
    return None


def is_http(item: Dict[str, object]) -> bool:
    url = item.get("url")
    return isinstance(url, str) and urllib.parse.urlsplit(url).scheme == "https"


def select_video(
    formats: Iterable[Dict[str, object]], requested: Optional[str], max_height: int
) -> Dict[str, object]:
    candidates = []
    for item in formats:
        codec = str(item.get("vcodec") or "none")
        height = int(item.get("height") or 0)
        if (
            is_http(item)
            and media_size(item)
            and codec.startswith(("avc1", "h264"))
            and 0 < height <= max_height
        ):
            candidates.append(item)
    if requested is not None:
        for item in candidates:
            if str(item.get("format_id")) == requested:
                return item
        raise RuntimeError("requested H.264 format is unavailable")
    if not candidates:
        raise RuntimeError("no bounded H.264 stream at requested resolution")
    return max(
        candidates,
        key=lambda item: (
            int(item.get("fps") or 0),
            int(item.get("height") or 0),
            float(item.get("tbr") or 0),
            1 if str(item.get("acodec") or "none") == "none" else 0,
        ),
    )


def select_audio(formats: Iterable[Dict[str, object]]) -> Optional[Dict[str, object]]:
    candidates = []
    for item in formats:
        codec = str(item.get("acodec") or "none")
        if (
            is_http(item)
            and media_size(item)
            and codec.startswith(("mp4a", "aac"))
        ):
            candidates.append(item)
    if not candidates:
        return None
    # Current Android-VR audio-only URLs can authorize byte zero but return 403
    # for every non-zero Range.  A progressive MP4 (normally format 18) carries
    # the same AAC track and remains seekable, so prefer that transport source.
    return max(
        candidates,
        key=lambda item: (
            1 if str(item.get("vcodec") or "none") != "none" else 0,
            float(item.get("abr") or 0),
            -int(item.get("height") or 0),
        ),
    )


def authoritative_media_size(item: Dict[str, object]) -> int:
    url = item.get("url")
    if not isinstance(url, str):
        raise RuntimeError("media URL missing")
    headers: Dict[str, str] = {}
    raw_headers = item.get("http_headers")
    if isinstance(raw_headers, dict):
        for key, value in raw_headers.items():
            if (
                isinstance(key, str)
                and isinstance(value, str)
                and key.lower() not in FORBIDDEN_PROBE_HEADERS
                and "\r" not in value
                and "\n" not in value
            ):
                headers[key] = value
    headers["Accept-Encoding"] = "identity"
    headers["Range"] = "bytes=0-0"
    request = urllib.request.Request(url, headers=headers, method="GET")
    with urllib.request.urlopen(
        request, timeout=8, context=ssl.create_default_context()
    ) as response:
        final = urllib.parse.urlsplit(response.geturl())
        host = (final.hostname or "").lower().rstrip(".")
        match = CONTENT_RANGE_RE.fullmatch(response.headers.get("Content-Range", ""))
        if (
            getattr(response, "status", 0) != 206
            or final.scheme != "https"
            or not (host == "googlevideo.com" or host.endswith(".googlevideo.com"))
            or match is None
        ):
            raise RuntimeError("authoritative media size unavailable")
        start, end, total = (int(part) for part in match.groups())
        if start != 0 or end < start or total <= end or total > MAX_MEDIA_BYTES:
            raise RuntimeError("authoritative media range invalid")
        return total


def stream_config(
    item: Dict[str, object], media_kind: str, authoritative_size: int
) -> Dict[str, object]:
    headers = item.get("http_headers")
    return {
        "url": item["url"],
        "size": authoritative_size,
        "content_type": f"{media_kind}/mp4",
        "headers": headers if isinstance(headers, dict) else {},
        "chunk_bytes": 1024 * 1024,
    }


def write_private_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary_name = tempfile.mkstemp(prefix=path.name + ".", dir=path.parent)
    try:
        os.fchmod(fd, 0o600)
        with os.fdopen(fd, "w", encoding="utf-8") as output:
            json.dump(value, output, separators=(",", ":"))
            output.write("\n")
        os.replace(temporary_name, path)
    except BaseException:
        try:
            os.unlink(temporary_name)
        except FileNotFoundError:
            pass
        raise


def parse_args() -> argparse.Namespace:
    bundled = Path(__file__).resolve().parent.parent / "vendor" / "yt-dlp"
    default_resolver = os.environ.get(
        "YT_DLP_BIN", str(bundled) if bundled.is_file() else "yt-dlp"
    )
    parser = argparse.ArgumentParser()
    parser.add_argument("url")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--yt-dlp", default=default_resolver)
    parser.add_argument("--video-format")
    parser.add_argument("--max-height", default=720, type=int)
    parser.add_argument("--player-client", default="android")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if not 240 <= args.max_height <= 1080:
        print("YOUTUBE_RESOLVE_FAIL reason=invalid_max_height", file=sys.stderr)
        return 64
    if PLAYER_CLIENT_RE.fullmatch(args.player_client) is None:
        print("YOUTUBE_RESOLVE_FAIL reason=invalid_player_client", file=sys.stderr)
        return 64
    try:
        video_id_for_url(args.url)
        metadata = extract_metadata(args)
        formats = metadata.get("formats")
        if not isinstance(formats, list):
            raise RuntimeError("formats missing")
        video = select_video(formats, args.video_format, args.max_height)
        video_size = authoritative_media_size(video)
        streams = {"video": stream_config(video, "video", video_size)}
        integrated_audio = str(video.get("acodec") or "none") != "none"
        audio = None if integrated_audio else select_audio(formats)
        if audio is not None:
            streams["audio"] = stream_config(
                audio, "audio", authoritative_media_size(audio)
            )
        write_private_json(
            args.output,
            {
                "version": 1,
                "source_id": str(metadata.get("id") or "unknown"),
                "duration": float(metadata.get("duration") or 0),
                "streams": streams,
            },
        )
        print(
            "YOUTUBE_RESOLVE_PASS "
            f"id={metadata.get('id', 'unknown')} "
            f"video_format={video.get('format_id', 'unknown')} "
            f"codec={video.get('vcodec', 'unknown')} "
            f"width={int(video.get('width') or 0)} "
            f"height={int(video.get('height') or 0)} "
            f"fps={float(video.get('fps') or 0):g} "
            f"video_bytes={video_size} "
            f"audio_format={audio.get('format_id') if audio else 'integrated-or-none'} "
            f"duration={float(metadata.get('duration') or 0):.3f}"
        )
        return 0
    except (
        FileNotFoundError,
        json.JSONDecodeError,
        ResolverServerUnavailable,
        ResolverServerFailure,
        RuntimeError,
        subprocess.SubprocessError,
    ) as error:
        print(
            f"YOUTUBE_RESOLVE_FAIL reason={type(error).__name__}",
            file=sys.stderr,
        )
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
