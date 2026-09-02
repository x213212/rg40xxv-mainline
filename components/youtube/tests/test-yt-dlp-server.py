#!/usr/bin/env python3
"""Deterministic host contract for the persistent yt-dlp resolver daemon."""

from __future__ import annotations

import concurrent.futures
import contextlib
import importlib.util
import io
import json
import os
import socket
import stat
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path
from typing import Any, Dict


PROJECT = Path(__file__).resolve().parent.parent
SERVER = PROJECT / "tools" / "yt_dlp_server.py"
RESOLVER = PROJECT / "tools" / "resolve_youtube.py"
MAX_RESPONSE_BYTES = 2 * 1024 * 1024


def load_module(name: str, path: Path) -> Any:
    specification = importlib.util.spec_from_file_location(name, path)
    assert specification is not None and specification.loader is not None
    module = importlib.util.module_from_spec(specification)
    sys.modules[name] = module
    specification.loader.exec_module(module)
    return module


def receive_line(connection: socket.socket) -> Dict[str, object]:
    payload = bytearray()
    while len(payload) <= MAX_RESPONSE_BYTES:
        block = connection.recv(min(65536, MAX_RESPONSE_BYTES + 1 - len(payload)))
        assert block
        payload.extend(block)
        if payload.endswith(b"\n"):
            break
    assert payload.endswith(b"\n")
    assert len(payload) <= MAX_RESPONSE_BYTES
    value = json.loads(payload[:-1].decode("utf-8", "strict"))
    assert isinstance(value, dict)
    return value


def request(socket_path: Path, value: Dict[str, object]) -> Dict[str, object]:
    payload = json.dumps(value, separators=(",", ":")).encode("ascii") + b"\n"
    connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        connection.settimeout(5.0)
        connection.connect(str(socket_path))
        connection.sendall(payload)
        return receive_line(connection)
    finally:
        connection.close()


def extract_request(video_id: str, priority: str = "interactive") -> Dict[str, object]:
    return {
        "version": 1,
        "op": "extract",
        "url": f"https://youtu.be/{video_id}",
        "player_client": "android",
        "priority": priority,
    }


def feed_request(
    mode: str,
    value: str,
    offset: int,
    limit: int = 8,
    priority: str = "interactive",
    encoding: str = "json",
) -> Dict[str, object]:
    result: Dict[str, object] = {
        "version": 1,
        "op": "feed",
        "mode": mode,
        "value": value,
        "offset": offset,
        "limit": limit,
        "priority": priority,
    }
    if encoding != "json":
        result["encoding"] = encoding
    return result


def feed_entries(start: int, count: int) -> list[dict[str, object]]:
    return [
        {
            "id": f"VID{index:08d}",
            "title": f"한국어 제목 {index}",
            "channel": "한국 채널",
            "published": "1일 전",
            "timestamp": 1_700_000_000 + index,
            "duration": index,
            "url": "https://example.invalid/SIGNED_SECRET",
        }
        for index in range(start, start + count)
    ]


def request_tsv(socket_path: Path, value: Dict[str, object]) -> bytes:
    payload = json.dumps(value, ensure_ascii=True, separators=(",", ":")).encode("ascii") + b"\n"
    connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        connection.settimeout(5.0)
        connection.connect(str(socket_path))
        connection.sendall(payload)
        result = bytearray()
        while len(result) <= MAX_RESPONSE_BYTES:
            block = connection.recv(min(65536, MAX_RESPONSE_BYTES + 1 - len(result)))
            if not block:
                break
            result.extend(block)
        assert 0 < len(result) <= MAX_RESPONSE_BYTES
        return bytes(result)
    finally:
        connection.close()


def wait_ready(process: subprocess.Popen[bytes], socket_path: Path) -> None:
    deadline = time.monotonic() + 8.0
    while time.monotonic() < deadline:
        assert process.poll() is None, f"daemon exited early: {process.returncode}"
        if socket_path.exists():
            try:
                response = request(socket_path, {"version": 1, "op": "ping"})
                assert response == {"version": 1, "ok": True, "workers": 2}
                return
            except (ConnectionRefusedError, OSError):
                pass
        time.sleep(0.025)
    raise AssertionError("daemon readiness timed out")


def event_lines(path: Path) -> list[str]:
    return path.read_text(encoding="utf-8").splitlines()


def wait_event(path: Path, expected: str, timeout: float = 3.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.exists() and expected in event_lines(path):
            return
        time.sleep(0.01)
    raise AssertionError(f"event timed out: {expected}")


def wait_event_count(path: Path, expected: str, count: int, timeout: float = 3.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.exists() and event_lines(path).count(expected) >= count:
            return
        time.sleep(0.01)
    raise AssertionError(f"event count timed out: {expected} count={count}")


def write_fake_module(root: Path) -> Path:
    package_root = root / "fake-module"
    package = package_root / "yt_dlp"
    package.mkdir(parents=True)
    source = r'''import os
import pathlib
import threading
import time
import urllib.parse

EVENTS = os.environ["RG_YOUTUBE_FAKE_YTDLP_EVENTS"]
LOCK = threading.Lock()
REAL_SETPRIORITY = os.setpriority

def guarded_setpriority(which, who, value):
    current = os.getpriority(which, who)
    if os.environ.get("RG_YOUTUBE_FAKE_DENY_PROMOTION") == "1" and value < current:
        raise PermissionError("simulated unprivileged nice promotion")
    return REAL_SETPRIORITY(which, who, value)

os.setpriority = guarded_setpriority

def event(value):
    payload = (value + "\n").encode("utf-8")
    descriptor = os.open(EVENTS, os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o600)
    try:
        os.write(descriptor, payload)
    finally:
        os.close(descriptor)

event("import")

class YoutubeDL:
    def __init__(self, options):
        self.params = dict(options)
        event("init:" + str(options.get("cachedir")))
        event("allowed:" + ",".join(options.get("allowed_extractors", [])))

    def extract_info(self, url, download=False):
        assert download is False
        if url.startswith("ytsearch") or "/channel/" in url:
            mode = "search" if url.startswith("ytsearch") else "channel"
            channel_id = (
                url.split("/channel/", 1)[1].split("/", 1)[0]
                if mode == "channel" else None
            )
            event("feed-start:" + mode)
            try:
                gate = os.environ.get("RG_YOUTUBE_FAKE_FEED_BLOCK_GATE")
                if "slowfeed" in url and gate:
                    gate_path = pathlib.Path(gate)
                    deadline = time.monotonic() + 5.0
                    while not gate_path.exists() and time.monotonic() < deadline:
                        time.sleep(0.01)
                    if not gate_path.exists():
                        raise RuntimeError("feed test gate timed out")
                time.sleep(0.20)
                start = int(self.params.get("playliststart", 1)) - 1
                end = int(self.params.get("playlistend", 97))
                assert start == 0 and end == 97
                return {
                    "entries": [
                        {
                            "id": f"VID{index:08d}",
                            "title": f"한국어 제목 {index}",
                            "channel": "한국 채널",
                            "published": "1일 전",
                            "timestamp": 1700000000 + index,
                            "duration": index,
                            "channel_id": channel_id,
                            "url": "https://example.invalid/SIGNED_SECRET",
                        }
                        for index in range(start, end)
                    ]
                }
            finally:
                event("feed-end:" + mode)
        parsed = urllib.parse.urlsplit(url)
        if parsed.hostname == "youtu.be":
            video_id = parsed.path[1:]
        else:
            video_id = urllib.parse.parse_qs(parsed.query)["v"][0]
        event("start:" + video_id)
        event("nice:" + video_id + ":" + str(os.getpriority(os.PRIO_PROCESS, 0)))
        try:
            gate = os.environ.get("RG_YOUTUBE_FAKE_BLOCK_GATE")
            if video_id.startswith("BLOCKER") and gate:
                gate_path = pathlib.Path(gate + "." + video_id)
                deadline = time.monotonic() + 5.0
                while not gate_path.exists() and time.monotonic() < deadline:
                    time.sleep(0.01)
                if not gate_path.exists():
                    raise RuntimeError("test gate timed out")
            time.sleep(0.20)
            if video_id == "FAILVID0001":
                raise RuntimeError("SIGNED_SECRET must never reach daemon logs")
            if video_id == "BIGRESP0001":
                formats = [
                    {
                        "format_id": str(index),
                        "vcodec": "avc1.4d401e",
                        "acodec": "mp4a.40.2",
                        "height": 360,
                        "width": 640,
                        "fps": 30,
                        "filesize": 1234,
                        "url": "https://rr1---sn-test.googlevideo.com/videoplayback?" + "x" * 15000,
                    }
                    for index in range(512)
                ]
            else:
                formats = [{
                    "format_id": "18",
                    "vcodec": "avc1.4d401e",
                    "acodec": "mp4a.40.2",
                    "height": 360,
                    "width": 640,
                    "fps": 30,
                    "tbr": 700.0,
                    "filesize": 1234,
                    "url": "https://rr1---sn-test.googlevideo.com/videoplayback?expire=9999999999&clen=1234&token=SIGNED_SECRET_" + video_id,
                    "http_headers": {"User-Agent": "rg40xxv-test"},
                }]
            return {"id": video_id, "duration": 12.5, "formats": formats}
        finally:
            event("end:" + video_id)
'''
    (package / "__init__.py").write_text(source, encoding="utf-8")
    return package_root


def write_fake_fallback(path: Path, events: Path) -> None:
    source = f'''#!/usr/bin/env python3
import json
import pathlib
import sys
import urllib.parse

url = sys.argv[-1]
parsed = urllib.parse.urlsplit(url)
video_id = parsed.path[1:] if parsed.hostname == "youtu.be" else urllib.parse.parse_qs(parsed.query)["v"][0]
with pathlib.Path({str(events)!r}).open("a", encoding="utf-8") as output:
    output.write(video_id + "\\n")
print(json.dumps({{
    "id": video_id,
    "duration": 5.0,
    "formats": [{{
        "format_id": "18",
        "vcodec": "avc1.4d401e",
        "acodec": "mp4a.40.2",
        "height": 360,
        "width": 640,
        "fps": 30,
        "filesize": 4321,
        "url": "https://rr1---sn-test.googlevideo.com/videoplayback?expire=9999999999&clen=4321&token=SIGNED_SECRET_FALLBACK",
    }}],
}}))
'''
    path.write_text(source, encoding="utf-8")
    path.chmod(0o700)


def invoke_resolver(
    resolver: Any,
    url: str,
    output: Path,
    fallback: Path,
    socket_path: Path,
    priority: str = "interactive",
) -> tuple[int, str, str]:
    old_argv = sys.argv
    old_socket = os.environ.get("RG_YOUTUBE_YTDLP_SOCKET")
    old_priority = os.environ.get("RG_YOUTUBE_RESOLVE_PRIORITY")
    stdout = io.StringIO()
    stderr = io.StringIO()
    try:
        os.environ["RG_YOUTUBE_YTDLP_SOCKET"] = str(socket_path)
        os.environ["RG_YOUTUBE_RESOLVE_PRIORITY"] = priority
        sys.argv = [
            str(RESOLVER),
            url,
            "--output",
            str(output),
            "--yt-dlp",
            str(fallback),
            "--video-format",
            "18",
        ]
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            status = resolver.main()
    finally:
        sys.argv = old_argv
        if old_socket is None:
            os.environ.pop("RG_YOUTUBE_YTDLP_SOCKET", None)
        else:
            os.environ["RG_YOUTUBE_YTDLP_SOCKET"] = old_socket
        if old_priority is None:
            os.environ.pop("RG_YOUTUBE_RESOLVE_PRIORITY", None)
        else:
            os.environ["RG_YOUTUBE_RESOLVE_PRIORITY"] = old_priority
    return status, stdout.getvalue(), stderr.getvalue()


def delayed_protocol_server(
    socket_path: Path, video_id: str, errors: list[BaseException]
) -> None:
    listener: socket.socket | None = None
    try:
        time.sleep(0.05)
        listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        listener.bind(str(socket_path))
        os.chmod(socket_path, 0o600)
        listener.listen(1)
        listener.settimeout(3.0)
        connection, _address = listener.accept()
        with connection:
            request_payload = bytearray()
            while not request_payload.endswith(b"\n"):
                block = connection.recv(4096)
                assert block
                request_payload.extend(block)
            request_value = json.loads(request_payload.decode("ascii"))
            assert request_value["priority"] == "background"
            response = {
                "version": 1,
                "ok": True,
                "metadata": {
                    "id": video_id,
                    "duration": 5.0,
                    "formats": [
                        {
                            "format_id": "18",
                            "vcodec": "avc1.4d401e",
                            "acodec": "mp4a.40.2",
                            "height": 360,
                            "width": 640,
                            "fps": 30,
                            "filesize": 4321,
                            "url": "https://rr1---sn-test.googlevideo.com/videoplayback?expire=9999999999&clen=4321&token=REDACTED_TEST",
                        }
                    ],
                },
            }
            connection.sendall(
                json.dumps(response, separators=(",", ":")).encode("ascii")
                + b"\n"
            )
    except BaseException as error:
        errors.append(error)
    finally:
        if listener is not None:
            listener.close()
        try:
            socket_path.unlink()
        except FileNotFoundError:
            pass


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="rg40xxv-yt-dlp-server.") as raw_root:
        root = Path(raw_root)
        runtime = root / "runtime"
        cache = root / "xdg-cache" / "rg40xxv-youtube" / "yt-dlp"
        socket_path = runtime / "yt-dlp.sock"
        events = root / "events.log"
        server_log = root / "server.log"
        fallback_events = root / "fallback.log"
        fallback = root / "fake-yt-dlp"
        block_gate = root / "release-blockers"
        feed_block_gate = root / "release-feed-blockers"
        module_root = write_fake_module(root)
        write_fake_fallback(fallback, fallback_events)
        environment = os.environ.copy()
        environment["RG_YOUTUBE_FAKE_YTDLP_EVENTS"] = str(events)
        environment["RG_YOUTUBE_FAKE_DENY_PROMOTION"] = "1"
        environment["RG_YOUTUBE_FAKE_BLOCK_GATE"] = str(block_gate)
        environment["RG_YOUTUBE_FAKE_FEED_BLOCK_GATE"] = str(feed_block_gate)
        server_command = [
            sys.executable,
            str(SERVER),
            "--runtime-root",
            str(runtime),
            "--cache-dir",
            str(cache),
            "--socket",
            str(socket_path),
            "--yt-dlp",
            str(module_root),
            "--workers",
            "2",
        ]
        with server_log.open("wb") as log:
            process = subprocess.Popen(
                server_command,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL,
                stderr=log,
                env=environment,
            )
            try:
                try:
                    wait_ready(process, socket_path)
                except AssertionError as error:
                    log.flush()
                    raise AssertionError(
                        f"{error}; daemon_log={server_log.read_text(encoding='utf-8')}"
                    ) from error
                duplicate_start = subprocess.run(
                    server_command,
                    check=False,
                    stdin=subprocess.DEVNULL,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    env=environment,
                    timeout=3.0,
                )
                assert duplicate_start.returncode == 0
                assert b"event=already_running phase=preimport" in duplicate_start.stderr
                assert event_lines(events).count("import") == 1
                assert sum(
                    line.startswith("init:") for line in event_lines(events)
                ) == 2
                socket_info = socket_path.lstat()
                assert stat.S_ISSOCK(socket_info.st_mode)
                assert stat.S_IMODE(socket_info.st_mode) == 0o600
                assert socket_info.st_uid == os.geteuid()
                for directory in (runtime, cache):
                    info = directory.lstat()
                    assert stat.S_ISDIR(info.st_mode) and not stat.S_ISLNK(info.st_mode)
                    assert stat.S_IMODE(info.st_mode) == 0o700
                    assert info.st_uid == os.geteuid()

                duplicate_id = "AAAABBBB001"
                with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
                    futures = [
                        executor.submit(request, socket_path, extract_request(duplicate_id))
                        for _index in range(2)
                    ]
                    duplicate_results = [future.result(timeout=5.0) for future in futures]
                assert all(result.get("ok") is True for result in duplicate_results)
                lines = event_lines(events)
                assert lines.count("import") == 1
                assert sum(line.startswith("init:") for line in lines) == 2
                assert lines.count(f"start:{duplicate_id}") == 1

                korean_query = "한국어 뉴스"
                feed_offsets = (0, 8, 24)
                with concurrent.futures.ThreadPoolExecutor(max_workers=3) as executor:
                    feed_futures = [
                        executor.submit(
                            request,
                            socket_path,
                            feed_request("search", korean_query, offset),
                        )
                        for offset in feed_offsets
                    ]
                    feed_results = [future.result(timeout=5.0) for future in feed_futures]
                assert all(result.get("ok") is True for result in feed_results)
                by_offset = {
                    int(result["feed"]["offset"]): result["feed"]
                    for result in feed_results
                }
                assert set(by_offset) == set(feed_offsets)
                assert {by_offset[offset]["cache"] for offset in feed_offsets} == {
                    "MISS",
                    "COALESCED",
                }
                for offset in feed_offsets:
                    page = by_offset[offset]
                    assert page["count"] == 8
                    assert page["items"][0]["id"] == f"VID{offset:08d}"
                    assert page["items"][0]["title"].startswith("한국어 제목")
                    assert page["items"][0]["channel"] == "한국 채널"
                lines = event_lines(events)
                assert lines.count("feed-start:search") == 1
                encoded_feed = json.dumps(feed_results, ensure_ascii=False)
                assert "SIGNED_SECRET" not in encoded_feed
                assert "example.invalid" not in encoded_feed

                aggregate = request(
                    socket_path,
                    feed_request("search", korean_query, 32, limit=64),
                )
                assert aggregate["feed"]["cache"] == "MEMORY"
                assert aggregate["feed"]["count"] == 64
                assert aggregate["feed"]["next"] is None
                assert aggregate["feed"]["end"] is True
                assert event_lines(events).count("feed-start:search") == 1

                channel_id = "UC" + "A" * 22
                tsv = request_tsv(
                    socket_path,
                    feed_request(
                        "channel", channel_id, 0, encoding="tsv1"
                    ),
                )
                tsv_text = tsv.decode("utf-8", "strict")
                assert tsv_text.startswith("ITEM\tVID00000000\t한국어 제목 0\t한국 채널\t")
                assert "\nBATCH\t8\t8\tmore=NO\n" in tsv_text
                assert f"DONE\t8\t{channel_id}\tcache=MISS\tnext=8\n" in tsv_text
                assert "SIGNED_SECRET" not in tsv_text
                feed_cache_root = cache / "feed"
                feed_cache_files = list(feed_cache_root.glob("*.json"))
                assert len(feed_cache_files) == 2
                assert stat.S_IMODE(feed_cache_root.stat().st_mode) == 0o700
                assert all(
                    stat.S_IMODE(path.stat().st_mode) == 0o600
                    and path.stat().st_nlink == 1
                    for path in feed_cache_files
                )

                live_server_module = load_module(
                    "rg40xxv_yt_dlp_server_live_cache_test", SERVER
                )
                playable = live_server_module.bounded_feed_snapshot(
                    {
                        "entries": [
                            *feed_entries(0, 3),
                            {
                                **feed_entries(3, 1)[0],
                                "availability": "subscriber_only",
                            },
                            {
                                **feed_entries(4, 1)[0],
                                "live_status": "is_upcoming",
                            },
                            {
                                **feed_entries(5, 1)[0],
                                "availability": "unlisted",
                            },
                        ]
                    }
                )
                assert [item["id"] for item in playable.items] == [
                    "VID00000000", "VID00000001", "VID00000002", "VID00000005"
                ]
                bound_channel = "UC" + "B" * 22
                bound_entries = feed_entries(0, 2)
                bound_entries[0]["channel_id"] = bound_channel
                bound_entries[1]["channel_id"] = "UC" + "C" * 22
                bound = live_server_module.bounded_feed_snapshot(
                    {"entries": bound_entries},
                    expected_channel_id=bound_channel,
                )
                assert [item["id"] for item in bound.items] == [
                    "VID00000000"
                ]
                stale_value = "stale-search"
                stale_now = int(time.time())
                safe_stale = live_server_module.bounded_feed_snapshot(
                    {"entries": feed_entries(0, 12)}, stale_now - 300
                )
                safe_stale = live_server_module.FeedSnapshot(
                    safe_stale.items,
                    safe_stale.has_more,
                    stale_now - 300,
                    stale_now - 1,
                )
                live_server_module.write_feed_cache(
                    feed_cache_root, "search", stale_value, safe_stale
                )
                stale_starts = event_lines(events).count("feed-start:search")
                stale_ends = event_lines(events).count("feed-end:search")
                stale_result = request(
                    socket_path,
                    feed_request("search", stale_value, 0),
                )
                assert stale_result["feed"]["cache"] == "STALE"
                assert stale_result["feed"]["count"] == 8
                wait_event_count(
                    events, "feed-start:search", stale_starts + 1
                )
                wait_event_count(
                    events, "feed-end:search", stale_ends + 1,
                )

                invalid_feed = request(
                    socket_path,
                    feed_request("search", korean_query, 95, limit=2),
                )
                assert invalid_feed == {
                    "version": 1,
                    "ok": False,
                    "error": "feed_limit_invalid",
                }

                wave_ids = ["AAAABBBB002", "AAAABBBB003", "AAAABBBB004"]
                with concurrent.futures.ThreadPoolExecutor(max_workers=3) as executor:
                    results = list(
                        executor.map(
                            lambda video_id: request(
                                socket_path, extract_request(video_id)
                            ),
                            wave_ids,
                        )
                    )
                assert all(result.get("ok") is True for result in results)
                lines = event_lines(events)
                assert lines.count("import") == 1
                init_lines = [line for line in lines if line.startswith("init:")]
                assert len(init_lines) == 2
                assert {line.removeprefix("init:") for line in init_lines} == {str(cache)}
                assert lines.count("allowed:youtube,youtube:tab,youtube:search") == 2
                active = 0
                peak = 0
                for line in lines:
                    if line.startswith("start:"):
                        active += 1
                        peak = max(peak, active)
                    elif line.startswith("end:"):
                        active -= 1
                assert active == 0 and peak == 2

                blocker_id = "BLOCKER0001"
                queued_background_id = "QUEUEBG0001"
                queued_interactive_id = "QUEUEIN0001"
                with concurrent.futures.ThreadPoolExecutor(max_workers=3) as executor:
                    blocker_future = executor.submit(
                        request,
                        socket_path,
                        extract_request(blocker_id, "background"),
                    )
                    wait_event(events, f"start:{blocker_id}")
                    queued_background = executor.submit(
                        request,
                        socket_path,
                        extract_request(queued_background_id, "background"),
                    )
                    queued_interactive = executor.submit(
                        request,
                        socket_path,
                        extract_request(queued_interactive_id, "interactive"),
                    )
                    wait_event(events, f"start:{queued_interactive_id}")
                    assert f"start:{queued_background_id}" not in event_lines(events)
                    interactive_while_background = queued_interactive.result(timeout=3.0)
                    assert interactive_while_background.get("ok") is True
                    assert f"end:{blocker_id}" not in event_lines(events)
                    Path(f"{block_gate}.{blocker_id}").touch(mode=0o600)
                    priority_results = [
                        blocker_future.result(timeout=5.0),
                        queued_background.result(timeout=5.0),
                        interactive_while_background,
                    ]
                assert all(result.get("ok") is True for result in priority_results)
                priority_lines = event_lines(events)
                assert priority_lines.index(
                    f"start:{queued_interactive_id}"
                ) < priority_lines.index(f"start:{queued_background_id}")

                feed_start_count = event_lines(events).count("feed-start:search")
                feed_play_id = "FEEDPLAY001"
                with concurrent.futures.ThreadPoolExecutor(max_workers=3) as executor:
                    slow_feed_one = executor.submit(
                        request,
                        socket_path,
                        feed_request(
                            "search", "slowfeed-one", 0, priority="background"
                        ),
                    )
                    wait_event_count(
                        events, "feed-start:search", feed_start_count + 1
                    )
                    slow_feed_two = executor.submit(
                        request,
                        socket_path,
                        feed_request(
                            "search", "slowfeed-two", 0, priority="background"
                        ),
                    )
                    time.sleep(0.05)
                    assert (
                        event_lines(events).count("feed-start:search")
                        == feed_start_count + 1
                    )
                    feed_play = executor.submit(
                        request,
                        socket_path,
                        extract_request(feed_play_id, "interactive"),
                    )
                    wait_event(events, f"start:{feed_play_id}")
                    assert feed_play.result(timeout=3.0).get("ok") is True
                    assert not feed_block_gate.exists()
                    assert (
                        event_lines(events).count("feed-start:search")
                        == feed_start_count + 1
                    )
                    feed_block_gate.touch(mode=0o600)
                    background_feed_results = [
                        slow_feed_one.result(timeout=5.0),
                        slow_feed_two.result(timeout=5.0),
                    ]
                assert all(
                    result.get("ok") is True for result in background_feed_results
                )
                assert (
                    event_lines(events).count("feed-start:search")
                    == feed_start_count + 2
                )

                invalid = request(
                    socket_path,
                    {
                        "version": 1,
                        "op": "extract",
                        "url": "https://youtu.be/AAAABBBB005?list=not-exact",
                        "player_client": "android",
                        "priority": "interactive",
                    },
                )
                assert invalid == {"version": 1, "ok": False, "error": "invalid_url"}

                oversized = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                try:
                    oversized.settimeout(5.0)
                    oversized.connect(str(socket_path))
                    oversized.sendall(b"x" * 4097)
                    assert receive_line(oversized) == {
                        "version": 1,
                        "ok": False,
                        "error": "request_too_large",
                    }
                finally:
                    oversized.close()

                large = request(socket_path, extract_request("BIGRESP0001"))
                assert large == {
                    "version": 1,
                    "ok": False,
                    "error": "response_too_large",
                }

                resolver = load_module("rg40xxv_resolver_daemon_test", RESOLVER)
                resolver.authoritative_media_size = lambda item: int(
                    resolver.media_size(item) or 0
                )
                server_output = root / "server-output.json"
                status, stdout, stderr = invoke_resolver(
                    resolver,
                    "https://youtu.be/SERVERMN001",
                    server_output,
                    fallback,
                    socket_path,
                )
                assert status == 0 and "YOUTUBE_RESOLVE_PASS" in stdout
                assert "SIGNED_SECRET" not in stdout + stderr
                assert stat.S_IMODE(server_output.stat().st_mode) == 0o600
                assert not fallback_events.exists()

                failed_output = root / "failed-output.json"
                status, _stdout, stderr = invoke_resolver(
                    resolver,
                    "https://youtu.be/FAILVID0001",
                    failed_output,
                    fallback,
                    socket_path,
                )
                assert status == 2 and "ResolverServerFailure" in stderr
                assert "SIGNED_SECRET" not in stderr
                assert not fallback_events.exists()

                fallback_output = root / "fallback-output.json"
                status, stdout, stderr = invoke_resolver(
                    resolver,
                    "https://youtu.be/FALLBACK001",
                    fallback_output,
                    fallback,
                    runtime / "missing.sock",
                )
                assert status == 0 and "YOUTUBE_RESOLVE_PASS" in stdout
                assert "SIGNED_SECRET" not in stdout + stderr
                assert fallback_events.read_text(encoding="utf-8") == "FALLBACK001\n"

                invalid_output = root / "invalid-output.json"
                status, _stdout, stderr = invoke_resolver(
                    resolver,
                    "https://youtu.be/AAAABBBB005?list=not-exact",
                    invalid_output,
                    fallback,
                    runtime / "missing.sock",
                )
                assert status == 2 and "RuntimeError" in stderr
                assert fallback_events.read_text(encoding="utf-8") == "FALLBACK001\n"

                original_retry_count = resolver.BACKGROUND_SERVER_RETRY_COUNT
                original_retry_seconds = resolver.BACKGROUND_SERVER_RETRY_SECONDS
                resolver.BACKGROUND_SERVER_RETRY_COUNT = 30
                resolver.BACKGROUND_SERVER_RETRY_SECONDS = 0.01
                try:
                    missing_background_output = root / "missing-background.json"
                    status, _stdout, stderr = invoke_resolver(
                        resolver,
                        "https://youtu.be/BGMISSING01",
                        missing_background_output,
                        fallback,
                        runtime / "missing-background.sock",
                        priority="background",
                    )
                    assert status == 2 and "ResolverServerUnavailable" in stderr
                    assert not missing_background_output.exists()
                    assert (
                        fallback_events.read_text(encoding="utf-8")
                        == "FALLBACK001\n"
                    )

                    delayed_id = "BGDELAY0001"
                    delayed_socket = runtime / "delayed.sock"
                    delayed_errors: list[BaseException] = []
                    delayed_thread = threading.Thread(
                        target=delayed_protocol_server,
                        args=(delayed_socket, delayed_id, delayed_errors),
                        daemon=True,
                    )
                    delayed_thread.start()
                    delayed_output = root / "delayed-background.json"
                    status, stdout, stderr = invoke_resolver(
                        resolver,
                        f"https://youtu.be/{delayed_id}",
                        delayed_output,
                        fallback,
                        delayed_socket,
                        priority="background",
                    )
                    delayed_thread.join(timeout=3.0)
                    assert not delayed_thread.is_alive() and not delayed_errors
                    assert status == 0 and "YOUTUBE_RESOLVE_PASS" in stdout
                    assert "SIGNED_SECRET" not in stdout + stderr
                    assert delayed_output.exists()
                    assert (
                        fallback_events.read_text(encoding="utf-8")
                        == "FALLBACK001\n"
                    )
                finally:
                    resolver.BACKGROUND_SERVER_RETRY_COUNT = original_retry_count
                    resolver.BACKGROUND_SERVER_RETRY_SECONDS = original_retry_seconds

                background_id = "BACKGRND001"
                with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
                    background_future = executor.submit(
                        request,
                        socket_path,
                        extract_request(background_id, "background"),
                    )
                    wait_event(events, f"start:{background_id}")
                    promoted_future = executor.submit(
                        request,
                        socket_path,
                        extract_request(background_id, "interactive"),
                    )
                    background_results = [
                        background_future.result(timeout=5.0),
                        promoted_future.result(timeout=5.0),
                    ]
                assert all(result.get("ok") is True for result in background_results)
                assert f"nice:{background_id}:15" in event_lines(events)
                interactive_after_background = "AFTERBG0001"
                interactive_result = request(
                    socket_path,
                    extract_request(interactive_after_background, "interactive"),
                )
                assert interactive_result.get("ok") is True
                assert (
                    f"nice:{interactive_after_background}:0" in event_lines(events)
                )
            finally:
                process.terminate()
                try:
                    process.wait(timeout=5.0)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5.0)
        assert process.returncode == 0
        assert not socket_path.exists()
        log_text = server_log.read_text(encoding="utf-8")
        assert "SIGNED_SECRET" not in log_text
        duplicate_line = next(
            line
            for line in log_text.splitlines()
            if f"id={duplicate_id}" in line and "event=resolve" in line
        )
        assert "waiters=2" in duplicate_line
        assert "coalesced=1" in duplicate_line
        assert "elapsed_ms=" in duplicate_line

        server_module = load_module("rg40xxv_yt_dlp_server_args_test", SERVER)
        demand_server = server_module.ResolverServer(
            object, root / "demand-runtime", root / "demand.sock",
            root / "demand-cache", 2, "android"
        )
        server_side, client_side = socket.socketpair(socket.AF_UNIX)
        live_job = server_module.FeedJob(
            "channel", "UC2j5Kw9qDWCZmU_emgqeguA",
            clients=[server_module.FeedWaiter(
                server_side, 0, 8, "MISS", "json"
            )],
        )
        assert demand_server.job_has_demand(live_job)
        client_side.close()
        assert not demand_server.job_has_demand(live_job)
        assert live_job.clients == []
        background_refresh = server_module.FeedJob(
            "channel", "UCcL163py441fTFfWy5tBjoQ", priority="background"
        )
        assert demand_server.job_has_demand(background_refresh)
        disk_snapshot = server_module.load_feed_cache(
            cache / "feed", "search", korean_query, int(time.time())
        )
        assert disk_snapshot is not None
        assert disk_snapshot[1] == "DISK"
        assert len(disk_snapshot[0].items) == 96
        assert disk_snapshot[0].has_more is True

        # A validated metadata snapshot from earlier the same day must still
        # make the UI usable during a network/extractor outage.  It is served
        # as STALE and refreshed in the background; signed playback URLs are
        # not part of this cache.
        stale_day_value = "stale-earlier-today"
        stale_day_now = int(time.time())
        stale_day = server_module.bounded_feed_snapshot(
            {"entries": feed_entries(0, 12)}, stale_day_now - 12 * 60 * 60
        )
        stale_day = server_module.FeedSnapshot(
            stale_day.items,
            stale_day.has_more,
            stale_day_now - 12 * 60 * 60,
            stale_day_now - 12 * 60 * 60 + server_module.FEED_TTL_SECONDS,
        )
        server_module.write_feed_cache(
            cache / "feed", "search", stale_day_value, stale_day
        )
        stale_day_loaded = server_module.load_feed_cache(
            cache / "feed", "search", stale_day_value, stale_day_now
        )
        assert stale_day_loaded is not None
        assert stale_day_loaded[1] == "STALE"
        assert len(stale_day_loaded[0].items) == 12
        assert not list((cache / "feed").glob(".feed.*"))
        with contextlib.redirect_stderr(io.StringIO()):
            try:
                server_module.parse_args(["--workers", "3"])
            except SystemExit as error:
                assert error.code == 2
            else:
                raise AssertionError("daemon accepted more than two workers")

    print(
        "YOUTUBE_YTDLP_SERVER_TEST PASS "
        "socket=AF_UNIX_PRIVATE workers=2 objects=PERSISTENT import=ONCE "
        "duplicate=COALESCED restart=EARLY_NO_IMPORT protocol=BOUNDED "
        "feed=AGGREGATE96+LOOKAHEAD+COALESCED_OFFSETS+MEMORY+ATOMIC_DISK "
        "feed_cache=STALE_WHILE_REVALIDATE feed_unicode=KOREAN "
        "feed_tsv=BOUNDED_SAFE background_active=MAX1_INTERACTIVE_RESERVED "
        "fallback=SAFE background_fallback=NEVER delayed_socket=RETRIED "
        "logs=REDACTED priority=INTERACTIVE_QUEUE_FIRST+BACKGROUND_NI15 "
        "background_then_interactive=NI0"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
