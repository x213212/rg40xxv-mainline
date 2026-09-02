#!/usr/bin/env python3
"""Loopback HTTP bridge for old FFmpeg and modern YouTube CDN URLs.

The stock H700 FFmpeg 4.x HTTP client starts with an open-ended request that
Google Video currently rejects with HTTP 403.  This bridge presents one normal,
seekable HTTP resource to FFmpeg while fetching the signed upstream URL in
bounded chunks.  Signed URLs are read from a private runtime JSON file and are
never written to logs or HTTP error bodies.
"""

from __future__ import annotations

import argparse
import http.server
import ipaddress
import json
import os
import re
import signal
import socketserver
import ssl
import sys
import tempfile
import threading
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from typing import BinaryIO, Dict, Mapping, Optional, Tuple


CONTENT_RANGE_RE = re.compile(r"^bytes ([0-9]+)-([0-9]+)/([0-9]+)$")
RANGE_RE = re.compile(r"^bytes=([0-9]*)-([0-9]*)$")
FORBIDDEN_UPSTREAM_HEADERS = {
    "connection",
    "content-length",
    "host",
    "proxy-authorization",
    "range",
    "transfer-encoding",
}


class BridgeError(RuntimeError):
    pass


@dataclass(frozen=True)
class StreamSpec:
    name: str
    url: str
    size: int
    content_type: str
    headers: Mapping[str, str]
    chunk_bytes: int


class Counters:
    def __init__(self) -> None:
        self._lock = threading.Lock()
        self.client_requests = 0
        self.upstream_requests = 0
        self.range_retries = 0
        self.bytes_relayed = 0
        self.client_disconnects = 0
        self.failures = 0
        self.last_failure = "none"

    def add(self, field: str, value: int = 1) -> None:
        with self._lock:
            setattr(self, field, getattr(self, field) + value)

    def fail(self, reason: str) -> None:
        with self._lock:
            self.failures += 1
            self.last_failure = reason

    def snapshot(self) -> Dict[str, object]:
        with self._lock:
            return {
                "client_requests": self.client_requests,
                "upstream_requests": self.upstream_requests,
                "range_retries": self.range_retries,
                "bytes_relayed": self.bytes_relayed,
                "client_disconnects": self.client_disconnects,
                "failures": self.failures,
                "last_failure": self.last_failure,
            }


def checked_stream(name: str, raw: object) -> StreamSpec:
    if not re.fullmatch(r"[a-z][a-z0-9_-]{0,31}", name):
        raise BridgeError("invalid stream name")
    if not isinstance(raw, dict):
        raise BridgeError(f"stream {name}: object required")
    url = raw.get("url")
    size = raw.get("size")
    content_type = raw.get("content_type", "application/octet-stream")
    headers = raw.get("headers", {})
    chunk_bytes = raw.get("chunk_bytes", 8 * 1024 * 1024)
    if not isinstance(url, str):
        raise BridgeError(f"stream {name}: URL required")
    parsed = urllib.parse.urlsplit(url)
    if parsed.scheme != "https" or not parsed.hostname or parsed.username:
        raise BridgeError(f"stream {name}: HTTPS URL required")
    if not isinstance(size, int) or size < 1:
        raise BridgeError(f"stream {name}: positive size required")
    if not isinstance(content_type, str) or not content_type.startswith(("video/", "audio/")):
        raise BridgeError(f"stream {name}: invalid content type")
    if not isinstance(headers, dict):
        raise BridgeError(f"stream {name}: headers object required")
    if not isinstance(chunk_bytes, int) or not 64 * 1024 <= chunk_bytes <= 10 * 1024 * 1024:
        raise BridgeError(f"stream {name}: invalid chunk size")
    clean_headers: Dict[str, str] = {}
    for key, value in headers.items():
        if not isinstance(key, str) or not isinstance(value, str):
            raise BridgeError(f"stream {name}: string headers required")
        if key.lower() in FORBIDDEN_UPSTREAM_HEADERS or "\r" in value or "\n" in value:
            continue
        clean_headers[key] = value
    return StreamSpec(name, url, size, content_type, clean_headers, chunk_bytes)


def load_config(path: Path) -> Dict[str, StreamSpec]:
    try:
        mode = path.stat().st_mode & 0o777
        if mode & 0o077:
            raise BridgeError("config must not be group/world accessible")
        raw = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise BridgeError("cannot read runtime config") from error
    if not isinstance(raw, dict) or not isinstance(raw.get("streams"), dict):
        raise BridgeError("streams object required")
    streams = {
        name: checked_stream(name, value)
        for name, value in raw["streams"].items()
    }
    if not streams:
        raise BridgeError("at least one stream required")
    return streams


def parse_client_range(value: Optional[str], size: int) -> Tuple[int, int, bool]:
    if value is None:
        return 0, size - 1, False
    match = RANGE_RE.fullmatch(value.strip())
    if match is None:
        raise BridgeError("unsupported Range header")
    first, last = match.groups()
    if not first:
        if not last:
            raise BridgeError("empty Range header")
        suffix = int(last)
        if suffix < 1:
            raise BridgeError("invalid suffix range")
        return max(0, size - suffix), size - 1, True
    start = int(first)
    end = int(last) if last else size - 1
    if start >= size or end < start:
        raise BridgeError("range outside stream")
    return start, min(end, size - 1), True


class BridgeServer(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(
        self,
        address: Tuple[str, int],
        streams: Mapping[str, StreamSpec],
        chunk_bytes: int,
        timeout: float,
    ) -> None:
        super().__init__(address, BridgeHandler)
        self.streams = streams
        self.chunk_bytes = chunk_bytes
        self.upstream_timeout = timeout
        self.counters = Counters()
        self.ssl_context = ssl.create_default_context()


class BridgeHandler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "rg40xxv-range-bridge/1"

    @property
    def bridge(self) -> BridgeServer:
        return self.server  # type: ignore[return-value]

    def log_message(self, _format: str, *_args: object) -> None:
        return

    def send_plain_error(self, status: int, message: str) -> None:
        body = (message + "\n").encode("ascii", "replace")
        self.send_response(status)
        self.send_header("Content-Type", "text/plain; charset=us-ascii")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def selected_stream(self) -> Optional[StreamSpec]:
        prefix = "/stream/"
        if not self.path.startswith(prefix) or "?" in self.path:
            return None
        return self.bridge.streams.get(self.path[len(prefix) :])

    def do_HEAD(self) -> None:  # noqa: N802
        spec = self.selected_stream()
        if spec is None:
            self.send_plain_error(404, "not found")
            return
        try:
            start, end, partial = parse_client_range(
                self.headers.get("Range"), spec.size
            )
        except BridgeError:
            self.send_header_only_range_error(spec.size)
            return
        self.send_stream_headers(spec, start, end, partial)

    def do_GET(self) -> None:  # noqa: N802
        if self.path == "/health":
            self.send_json({"status": "ok", "streams": sorted(self.bridge.streams)})
            return
        if self.path == "/stats":
            self.send_json(self.bridge.counters.snapshot())
            return
        spec = self.selected_stream()
        if spec is None:
            self.send_plain_error(404, "not found")
            return
        try:
            start, end, partial = parse_client_range(
                self.headers.get("Range"), spec.size
            )
        except BridgeError:
            self.send_header_only_range_error(spec.size)
            return
        chunk_bytes = min(self.bridge.chunk_bytes, spec.chunk_bytes)
        first_end = min(end, start + chunk_bytes - 1)
        try:
            upstream, first_end = self.open_upstream_adaptive(
                spec, start, first_end
            )
        except BridgeError as error:
            self.bridge.counters.fail(f"{spec.name}:{error}")
            self.send_plain_error(502, "upstream unavailable")
            return
        except urllib.error.HTTPError as error:
            self.bridge.counters.fail(
                f"{spec.name}:http_{error.code} range={start}-{first_end}"
            )
            self.send_plain_error(502, "upstream unavailable")
            return
        except (OSError, urllib.error.URLError) as error:
            reason = getattr(error, "reason", error)
            self.bridge.counters.fail(f"{spec.name}:{type(reason).__name__}")
            self.send_plain_error(502, "upstream unavailable")
            return

        self.bridge.counters.add("client_requests")
        self.send_stream_headers(spec, start, end, partial)
        position = start
        try:
            while position <= end:
                if position == start:
                    chunk_end = first_end
                else:
                    requested_end = min(end, position + chunk_bytes - 1)
                    upstream, chunk_end = self.open_upstream_adaptive(
                        spec, position, requested_end
                    )
                try:
                    position = self.relay_exact(upstream, position, chunk_end)
                finally:
                    upstream.close()
        except (BrokenPipeError, ConnectionResetError):
            self.close_connection = True
            self.bridge.counters.add("client_disconnects")
        except BridgeError as error:
            # The response already advertised the complete downstream
            # Content-Length.  Keeping this HTTP/1.1 connection alive after
            # an upstream chunk fails leaves the client waiting for body
            # bytes while this handler waits for the next request.
            self.close_connection = True
            self.bridge.counters.fail(f"{spec.name}:{error}")
        except urllib.error.HTTPError as error:
            self.close_connection = True
            self.bridge.counters.fail(f"{spec.name}:http_{error.code}")
        except (OSError, urllib.error.URLError) as error:
            self.close_connection = True
            reason = getattr(error, "reason", error)
            self.bridge.counters.fail(f"{spec.name}:{type(reason).__name__}")

    def send_json(self, value: object) -> None:
        body = json.dumps(value, sort_keys=True, separators=(",", ":")).encode("ascii")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def send_header_only_range_error(self, size: int) -> None:
        self.send_response(416)
        self.send_header("Content-Range", f"bytes */{size}")
        self.send_header("Content-Length", "0")
        self.end_headers()

    def send_stream_headers(
        self, spec: StreamSpec, start: int, end: int, partial: bool
    ) -> None:
        self.send_response(206 if partial else 200)
        self.send_header("Content-Type", spec.content_type)
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Length", str(end - start + 1))
        if partial:
            self.send_header("Content-Range", f"bytes {start}-{end}/{spec.size}")
        self.send_header("Cache-Control", "no-store")
        self.end_headers()

    def open_upstream(
        self, spec: StreamSpec, start: int, end: int
    ) -> BinaryIO:
        headers = dict(spec.headers)
        headers["Accept-Encoding"] = "identity"
        headers["Range"] = f"bytes={start}-{end}"
        request = urllib.request.Request(spec.url, headers=headers, method="GET")
        response = urllib.request.urlopen(
            request,
            timeout=self.bridge.upstream_timeout,
            context=self.bridge.ssl_context,
        )
        status = getattr(response, "status", 0)
        content_range = response.headers.get("Content-Range", "")
        match = CONTENT_RANGE_RE.fullmatch(content_range)
        if status != 206 or match is None:
            response.close()
            raise BridgeError(
                f"invalid_response status={status} start={start} end={end}"
            )
        actual_start, actual_end, total = (int(item) for item in match.groups())
        if actual_start != start or actual_end != end or total != spec.size:
            response.close()
            raise BridgeError(
                "range_mismatch "
                f"requested={start}-{end}/{spec.size} "
                f"received={actual_start}-{actual_end}/{total}"
            )
        self.bridge.counters.add("upstream_requests")
        return response

    def open_upstream_adaptive(
        self, spec: StreamSpec, start: int, requested_end: int
    ) -> Tuple[BinaryIO, int]:
        end = requested_end
        while True:
            try:
                return self.open_upstream(spec, start, end), end
            except urllib.error.HTTPError as error:
                length = end - start + 1
                if error.code != 403 or length <= 64 * 1024:
                    raise
                error.close()
                end = start + max(64 * 1024, length // 2) - 1
                self.bridge.counters.add("range_retries")

    def relay_exact(self, upstream: BinaryIO, start: int, end: int) -> int:
        remaining = end - start + 1
        position = start
        while remaining:
            data = upstream.read(min(128 * 1024, remaining))
            if not data:
                raise BridgeError("short upstream response")
            self.wfile.write(data)
            remaining -= len(data)
            position += len(data)
            self.bridge.counters.add("bytes_relayed", len(data))
        return position


def write_ready(path: Path, port: int, streams: Mapping[str, StreamSpec]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary_name = tempfile.mkstemp(prefix=path.name + ".", dir=path.parent)
    try:
        os.fchmod(fd, 0o600)
        with os.fdopen(fd, "w", encoding="utf-8") as output:
            json.dump({"port": port, "streams": sorted(streams)}, output)
            output.write("\n")
        os.replace(temporary_name, path)
    except BaseException:
        try:
            os.unlink(temporary_name)
        except FileNotFoundError:
            pass
        raise


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", required=True, type=Path)
    parser.add_argument("--ready-file", required=True, type=Path)
    parser.add_argument("--listen", default="127.0.0.1")
    parser.add_argument("--port", default=0, type=int)
    parser.add_argument("--chunk-bytes", default=8 * 1024 * 1024, type=int)
    parser.add_argument("--timeout", default=20.0, type=float)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        if not ipaddress.ip_address(args.listen).is_loopback:
            raise BridgeError("listen address must be loopback")
        if not 0 <= args.port <= 65535:
            raise BridgeError("invalid port")
        if not 64 * 1024 <= args.chunk_bytes <= 10 * 1024 * 1024:
            raise BridgeError("chunk size outside safe CDN bounds")
        if not 1.0 <= args.timeout <= 120.0:
            raise BridgeError("invalid timeout")
        streams = load_config(args.config)
        server = BridgeServer(
            (args.listen, args.port), streams, args.chunk_bytes, args.timeout
        )
        write_ready(args.ready_file, server.server_port, streams)

        def stop_server(_signum: int, _frame: object) -> None:
            threading.Thread(target=server.shutdown, daemon=True).start()

        signal.signal(signal.SIGTERM, stop_server)
        signal.signal(signal.SIGINT, stop_server)
        print(
            "YOUTUBE_BRIDGE_READY "
            f"port={server.server_port} chunk_bytes={args.chunk_bytes} "
            f"streams={','.join(sorted(streams))}",
            flush=True,
        )
        server.serve_forever(poll_interval=0.1)
        server.server_close()
        print("YOUTUBE_BRIDGE_STOPPED", flush=True)
        return 0
    except BridgeError as error:
        print(f"YOUTUBE_BRIDGE_FAIL reason={error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
