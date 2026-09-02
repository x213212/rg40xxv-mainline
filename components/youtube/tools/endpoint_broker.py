#!/usr/bin/env python3
"""Convert one private resolver-cache claim into safe loopback endpoints.

The frontend launches this asynchronously, keeps the broker's stdin pipe open,
then consumes exactly one stdout record.  Signed upstream URLs only exist in a
0600 cache claim and in the bridge's process memory.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import select
import signal
import stat
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path
from typing import Optional, Sequence


WATCH_URL_RE = re.compile(
    r"^https://(?:youtu\.be/[A-Za-z0-9_-]{11}|(?:www\.)?youtube\.com/watch\?v=[A-Za-z0-9_-]{11}(?:[&#].*)?)$"
)
READY_PREFIX = "YOUTUBE_ENDPOINT_READY"
MAX_READY_LINE_BYTES = 4096


class BrokerError(RuntimeError):
    pass


def require_private_dir(path: Path) -> None:
    try:
        info = path.lstat()
    except OSError as error:
        raise BrokerError("private runtime unavailable") from error
    if (
        not stat.S_ISDIR(info.st_mode)
        or stat.S_ISLNK(info.st_mode)
        or info.st_uid != os.geteuid()
        or stat.S_IMODE(info.st_mode) != 0o700
    ):
        raise BrokerError("private runtime unsafe")


def require_private_file(path: Path) -> None:
    try:
        info = path.lstat()
    except OSError as error:
        raise BrokerError("private runtime file unavailable") from error
    if (
        not stat.S_ISREG(info.st_mode)
        or stat.S_ISLNK(info.st_mode)
        or info.st_uid != os.geteuid()
        or info.st_nlink != 1
        or stat.S_IMODE(info.st_mode) != 0o600
    ):
        raise BrokerError("private runtime file unsafe")


class Broker:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.stop = threading.Event()
        self.acquire: Optional[subprocess.Popen[bytes]] = None
        self.bridge: Optional[subprocess.Popen[bytes]] = None
        self.prefetch: Optional[subprocess.Popen[bytes]] = None
        self.prefetch_thread: Optional[threading.Thread] = None
        self.prefetch_cancel = threading.Event()
        self.prefetch_lock = threading.Lock()
        self.claim: Optional[Path] = None
        self.runtime: Optional[Path] = None

    def on_signal(self, _signum: int, _frame: object) -> None:
        self.stop.set()
        self.prefetch_cancel.set()
        self.terminate(self.acquire)
        self.terminate(self.bridge)
        self.terminate(self.prefetch)

    @staticmethod
    def terminate(child: Optional[subprocess.Popen[bytes]], force: bool = False) -> None:
        if child is None or child.poll() is not None:
            return
        try:
            os.killpg(child.pid, signal.SIGKILL if force else signal.SIGTERM)
        except ProcessLookupError:
            pass

    def wait_child(self, child: subprocess.Popen[bytes], timeout: float) -> bytes:
        deadline = time.monotonic() + timeout
        while child.poll() is None:
            if self.stop.is_set():
                raise BrokerError("terminated")
            if time.monotonic() >= deadline:
                self.terminate(child)
                raise BrokerError("operation timed out")
            time.sleep(0.025)
        output = child.communicate()[0]
        if child.returncode != 0:
            raise BrokerError("operation failed")
        return output

    def acquire_claim(self) -> Path:
        tool = self.args.cache_tool
        if not tool.is_absolute() or not tool.is_file() or tool.is_symlink() or not os.access(tool, os.X_OK):
            raise BrokerError("cache tool contract failed")
        env = os.environ.copy()
        env["RG_YOUTUBE_CACHE_DIR"] = str(self.args.cache_dir)
        self.acquire = subprocess.Popen(
            [str(tool), "acquire", self.args.url],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            start_new_session=True,
            env=env,
        )
        output = self.wait_child(self.acquire, self.args.acquire_timeout)
        self.acquire = None
        try:
            claim = Path(output.decode("utf-8", "strict").strip())
            expected_parent = (self.args.cache_dir / "claims").resolve(strict=True)
            if not claim.is_absolute() or claim.parent.resolve(strict=True) != expected_parent:
                raise BrokerError("cache claim outside private cache")
        except (OSError, UnicodeDecodeError) as error:
            raise BrokerError("cache claim unavailable") from error
        require_private_file(claim)
        return claim

    def release_claim(self) -> None:
        if self.claim is None:
            return
        tool = self.args.cache_tool
        env = os.environ.copy()
        env["RG_YOUTUBE_CACHE_DIR"] = str(self.args.cache_dir)
        child = subprocess.Popen(
            [str(tool), "release", str(self.claim)],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            start_new_session=True,
            env=env,
        )
        try:
            child.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            self.terminate(child, force=True)
            child.wait()
        if child.returncode != 0:
            raise BrokerError("cache claim release failed")
        self.claim = None

    def make_runtime(self) -> tuple[Path, Path]:
        root = self.args.runtime_root
        root.mkdir(mode=0o700, parents=True, exist_ok=True)
        require_private_dir(root)
        self.runtime = Path(tempfile.mkdtemp(prefix="broker.", dir=root))
        os.chmod(self.runtime, 0o700)
        require_private_dir(self.runtime)
        return self.runtime / "ready.json", self.runtime / "bridge.log"

    def start_bridge(self, ready: Path, bridge_log: Path) -> None:
        bridge = self.args.bridge
        if not bridge.is_absolute() or not bridge.is_file() or bridge.is_symlink() or not os.access(bridge, os.X_OK):
            raise BrokerError("bridge contract failed")
        assert self.claim is not None
        with bridge_log.open("xb", buffering=0) as log:
            os.chmod(bridge_log, 0o600)
            self.bridge = subprocess.Popen(
                [str(bridge), "--config", str(self.claim), "--ready-file", str(ready),
                 "--listen", "127.0.0.1", "--port", "0", "--chunk-bytes", str(self.args.chunk_bytes)],
                stdin=subprocess.DEVNULL,
                stdout=log,
                stderr=subprocess.STDOUT,
                start_new_session=True,
            )
            deadline = time.monotonic() + self.args.ready_timeout
            while not ready.exists():
                if self.stop.is_set():
                    raise BrokerError("terminated")
                if self.bridge.poll() is not None:
                    raise BrokerError("bridge start failed")
                if time.monotonic() >= deadline:
                    raise BrokerError("bridge ready timed out")
                time.sleep(0.025)
        require_private_file(ready)

    def start_next_prefetch(self) -> None:
        if not self.args.prefetch_urls or self.stop.is_set() or self.prefetch_cancel.is_set():
            return
        tool = self.args.cache_tool
        env = os.environ.copy()
        env["RG_YOUTUBE_CACHE_DIR"] = str(self.args.cache_dir)
        self.prefetch = subprocess.Popen(
            [str(tool), "prefetch-set", *self.args.prefetch_urls],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            start_new_session=True,
            env=env,
        )

    def delayed_next_prefetch(self) -> None:
        if self.prefetch_cancel.wait(self.args.prefetch_delay) or self.stop.is_set():
            return
        self.start_next_prefetch()

    def schedule_next_prefetch(self) -> None:
        if not self.args.prefetch_urls or self.stop.is_set():
            return
        with self.prefetch_lock:
            if self.prefetch_thread is not None and self.prefetch_thread.is_alive():
                self.prefetch_thread.join(timeout=0.1)
                if self.prefetch_thread.is_alive():
                    return
            if self.prefetch is not None and self.prefetch.poll() is None:
                return
            self.prefetch_cancel.clear()
            self.prefetch_thread = threading.Thread(
                target=self.delayed_next_prefetch, daemon=True
            )
            self.prefetch_thread.start()

    def cancel_next_prefetch(self) -> None:
        self.prefetch_cancel.set()
        child = self.prefetch
        self.terminate(child)
        if child is not None and child.poll() is None:
            try:
                child.wait(timeout=0.25)
            except subprocess.TimeoutExpired:
                self.terminate(child, force=True)
                child.wait()
        self.prefetch = None

    @staticmethod
    def endpoint_record(ready: Path) -> str:
        try:
            value = json.loads(ready.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as error:
            raise BrokerError("bridge readiness invalid") from error
        port = value.get("port") if isinstance(value, dict) else None
        streams = value.get("streams") if isinstance(value, dict) else None
        if not isinstance(port, int) or not 1 <= port <= 65535 or not isinstance(streams, list):
            raise BrokerError("bridge readiness invalid")
        if "video" not in streams or any(not isinstance(item, str) for item in streams):
            raise BrokerError("bridge video unavailable")
        audio = f"http://127.0.0.1:{port}/stream/audio" if "audio" in streams else "none"
        line = f"{READY_PREFIX} video=http://127.0.0.1:{port}/stream/video audio={audio}"
        if len(line.encode("ascii")) > MAX_READY_LINE_BYTES:
            raise BrokerError("endpoint protocol too long")
        return line

    def watch_stdin(self) -> None:
        pending = b""
        try:
            while not self.stop.is_set():
                readable, _, _ = select.select([sys.stdin.buffer], [], [], 0.25)
                if not readable:
                    continue
                block = os.read(sys.stdin.fileno(), 128)
                if block == b"":
                    self.stop.set()
                    self.terminate(self.bridge)
                    return
                pending += block
                if len(pending) > 256:
                    self.stop.set()
                    self.terminate(self.bridge)
                    return
                while b"\n" in pending:
                    raw_command, pending = pending.split(b"\n", 1)
                    if raw_command == b"PLAY":
                        self.cancel_next_prefetch()
                    elif raw_command == b"HOME":
                        self.schedule_next_prefetch()
                    else:
                        self.stop.set()
                        self.terminate(self.bridge)
                        return
        except (OSError, ValueError):
            return

    def cleanup(self) -> None:
        self.stop.set()
        self.prefetch_cancel.set()
        if self.prefetch_thread is not None:
            self.prefetch_thread.join(timeout=1.0)
            self.prefetch_thread = None
        self.terminate(self.acquire)
        self.terminate(self.bridge)
        self.terminate(self.prefetch)
        for child in (self.acquire, self.bridge, self.prefetch):
            if child is None:
                continue
            try:
                child.wait(timeout=1.0)
            except subprocess.TimeoutExpired:
                self.terminate(child, force=True)
                child.wait()
        self.acquire = None
        self.bridge = None
        self.prefetch = None
        try:
            self.release_claim()
        except BrokerError:
            pass
        if self.runtime is not None:
            for path in self.runtime.iterdir():
                try:
                    path.unlink()
                except FileNotFoundError:
                    pass
            try:
                self.runtime.rmdir()
            except OSError:
                pass
            self.runtime = None

    def run(self) -> int:
        try:
            ready, bridge_log = self.make_runtime()
            self.claim = self.acquire_claim()
            self.start_bridge(ready, bridge_log)
            record = self.endpoint_record(ready)
            self.release_claim()
            print(record, flush=True)
            self.schedule_next_prefetch()
            threading.Thread(target=self.watch_stdin, daemon=True).start()
            while not self.stop.wait(0.1):
                if self.bridge is None or self.bridge.poll() is not None:
                    raise BrokerError("bridge exited")
            return 0
        except BrokerError as error:
            print(f"YOUTUBE_ENDPOINT_BROKER result=FAIL reason={error}", file=sys.stderr, flush=True)
            return 2
        finally:
            self.cleanup()


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("url")
    parser.add_argument("--prefetch-url", dest="prefetch_urls", action="append", default=[])
    parser.add_argument("--prefetch-delay", type=float, default=3.0)
    parser.add_argument("--cache-tool", type=Path, default=Path(os.environ.get("RG_YOUTUBE_CACHE_TOOL", "/opt/rg40xxv/youtube/tools/resolver_cache.py")))
    parser.add_argument("--bridge", type=Path, default=Path(os.environ.get("RG_YOUTUBE_BRIDGE", "/opt/rg40xxv/youtube/tools/bounded_range_bridge.py")))
    parser.add_argument("--cache-dir", type=Path, default=Path(os.environ.get("RG_YOUTUBE_CACHE_DIR", "/run/rg40xxv-youtube-native/cache")))
    parser.add_argument("--runtime-root", type=Path, default=Path(os.environ.get("RG_YOUTUBE_RUNTIME_ROOT", "/run/rg40xxv-youtube-native")))
    parser.add_argument("--chunk-bytes", type=int, default=8 * 1024 * 1024)
    parser.add_argument("--ready-timeout", type=float, default=10.0)
    parser.add_argument("--acquire-timeout", type=float, default=55.0)
    args = parser.parse_args(argv)
    if WATCH_URL_RE.fullmatch(args.url) is None:
        parser.error("URL must name one exact HTTPS YouTube video")
    if len(args.prefetch_urls) > 2:
        parser.error("at most two prefetch URLs are accepted")
    if any(WATCH_URL_RE.fullmatch(url) is None for url in args.prefetch_urls):
        parser.error("prefetch URL must name one exact HTTPS YouTube video")
    if not 0.0 <= args.prefetch_delay <= 30.0:
        parser.error("prefetch delay outside safe bounds")
    if not 64 * 1024 <= args.chunk_bytes <= 10 * 1024 * 1024:
        parser.error("chunk bytes outside safe bounds")
    if not 0.5 <= args.ready_timeout <= 30.0 or not 1.0 <= args.acquire_timeout <= 90.0:
        parser.error("timeout outside safe bounds")
    for name in ("cache_dir", "runtime_root"):
        path = getattr(args, name)
        if not path.is_absolute() or path == Path("/"):
            parser.error(f"{name} must be a bounded absolute path")
    return args


def main() -> int:
    broker = Broker(parse_args(sys.argv[1:]))
    for signum in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
        signal.signal(signum, broker.on_signal)
    return broker.run()


if __name__ == "__main__":
    raise SystemExit(main())
