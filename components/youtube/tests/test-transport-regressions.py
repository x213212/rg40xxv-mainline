#!/usr/bin/env python3
"""Offline regression checks for authoritative size and failed body closure."""

from __future__ import annotations

import importlib.util
import io
import sys
from pathlib import Path
from typing import Any


PROJECT = Path(__file__).resolve().parent.parent


def load_module(name: str, path: Path) -> Any:
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


resolver = load_module("rg40xxv_resolve_youtube", PROJECT / "tools/resolve_youtube.py")
bridge = load_module(
    "rg40xxv_bounded_range_bridge", PROJECT / "tools/bounded_range_bridge.py"
)


class ProbeResponse:
    status = 206
    headers = {"Content-Range": "bytes 0-0/86531162"}

    def __enter__(self) -> "ProbeResponse":
        return self

    def __exit__(self, *_args: object) -> None:
        return None

    @staticmethod
    def geturl() -> str:
        return "https://rr1---sn-test.googlevideo.com/videoplayback?token=private"


captured: dict[str, object] = {}


def fake_urlopen(request: Any, **kwargs: object) -> ProbeResponse:
    captured["headers"] = {key.lower(): value for key, value in request.header_items()}
    captured["method"] = request.get_method()
    captured["timeout"] = kwargs.get("timeout")
    return ProbeResponse()


original_urlopen = resolver.urllib.request.urlopen
resolver.urllib.request.urlopen = fake_urlopen
try:
    item = {
        "url": "https://rr1---sn-test.googlevideo.com/videoplayback?token=private",
        "filesize_approx": 86534537,
        "http_headers": {
            "User-Agent": "rg40xxv-test",
            "Range": "bytes=999-1000",
            "Host": "attacker.invalid",
        },
    }
    assert resolver.media_size(item) == 86534537
    assert resolver.authoritative_media_size(item) == 86531162
finally:
    resolver.urllib.request.urlopen = original_urlopen

assert captured["method"] == "GET"
assert captured["timeout"] == 8
assert captured["headers"] == {
    "accept-encoding": "identity",
    "range": "bytes=0-0",
    "user-agent": "rg40xxv-test",
}


class ShortResponse:
    def __init__(self) -> None:
        self.first = True

    def read(self, _size: int) -> bytes:
        if self.first:
            self.first = False
            return b"xx"
        return b""

    def close(self) -> None:
        return None


specification = bridge.StreamSpec(
    "video",
    "https://rr1---sn-test.googlevideo.com/videoplayback?token=private",
    4,
    "video/mp4",
    {},
    64 * 1024,
)


class FakeServer:
    streams = {"video": specification}
    chunk_bytes = 64 * 1024
    counters = bridge.Counters()


handler = object.__new__(bridge.BridgeHandler)
handler.server = FakeServer()
handler.path = "/stream/video"
handler.command = "GET"
handler.headers = {}
handler.wfile = io.BytesIO()
handler.close_connection = False
handler.send_stream_headers = lambda *_args: None
handler.open_upstream_adaptive = lambda *_args: (ShortResponse(), 3)
handler.do_GET()

snapshot = handler.server.counters.snapshot()
assert handler.wfile.getvalue() == b"xx"
assert handler.close_connection is True
assert snapshot["bytes_relayed"] == 2
assert snapshot["failures"] == 1
assert snapshot["last_failure"] == "video:short upstream response"

print(
    "YOUTUBE_TRANSPORT_REGRESSION_TEST PASS "
    "resolver_size=CDN_CONTENT_RANGE bridge_midbody_failure=CONNECTION_CLOSE"
)
