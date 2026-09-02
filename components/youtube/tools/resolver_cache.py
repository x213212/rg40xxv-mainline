#!/usr/bin/env python3
"""Private, expiring yt-dlp resolver cache for the native YouTube client.

Stdout is an API: only ``acquire`` writes a single claimed configuration path.
Diagnostics never contain a signed media URL.
"""

from __future__ import annotations

import argparse
import fcntl
import hashlib
import json
import math
import os
import re
import signal
import stat
import subprocess
import sys
import tempfile
import time
import urllib.parse
from contextlib import contextmanager
from pathlib import Path
from typing import Dict, Iterator, List, Optional, Sequence, Tuple


CACHE_SCHEMA = "rg40xxv-youtube-resolver-cache-v3-format18-authoritative-size"
PINNED_VIDEO_FORMAT = "18"
VIDEO_ID_RE = re.compile(r"^[A-Za-z0-9_-]{11}$")
CLAIM_RE = re.compile(r"^([A-Za-z0-9_-]{11})\.[A-Za-z0-9_-]+\.json$")
MAX_CONFIG_BYTES = 1024 * 1024
EXPIRY_SAFETY_SECONDS = 10 * 60
MAX_SIGNED_LIFETIME_SECONDS = 24 * 60 * 60
MAX_CACHE_AGE_SECONDS = 15 * 60
MAX_CLOCK_DRIFT_SECONDS = 30
RESOLVER_TIMEOUT_SECONDS = 50
MAX_CACHE_ENTRIES = 96
VIDEO_LOCK_SLOTS = 64
MAX_ACTIVE_CLAIMS = 64
STALE_CLAIM_SECONDS = 60 * 60

_active_children: List[subprocess.Popen[str]] = []
_termination_signal: Optional[int] = None


class CacheError(RuntimeError):
    pass


def diagnostic(action: str, result: str, video_id: str, detail: str = "") -> None:
    suffix = f" detail={detail}" if detail else ""
    print(
        f"YOUTUBE_RESOLVER_CACHE action={action} result={result} "
        f"id={video_id}{suffix}",
        file=sys.stderr,
        flush=True,
    )


def terminate_child(child: subprocess.Popen[str], force: bool = False) -> None:
    if child.poll() is not None:
        return
    try:
        os.killpg(child.pid, signal.SIGKILL if force else signal.SIGTERM)
    except ProcessLookupError:
        pass


def on_signal(signum: int, _frame: object) -> None:
    global _termination_signal
    _termination_signal = signum
    for child in list(_active_children):
        terminate_child(child)


def install_signal_handlers() -> None:
    signal.signal(signal.SIGTERM, on_signal)
    signal.signal(signal.SIGINT, on_signal)
    signal.signal(signal.SIGHUP, on_signal)


def check_termination() -> None:
    if _termination_signal is not None:
        raise SystemExit(128 + _termination_signal)


def video_id_for_url(value: str) -> str:
    if not isinstance(value, str) or len(value) > 2048 or any(
        character in value for character in "\r\n\0"
    ):
        raise CacheError("invalid watch URL")
    parsed = urllib.parse.urlsplit(value)
    if parsed.scheme != "https" or parsed.username or parsed.password:
        raise CacheError("HTTPS watch URL required")
    host = (parsed.hostname or "").lower().rstrip(".")
    video_id = ""
    if host == "youtu.be":
        parts = [part for part in parsed.path.split("/") if part]
        if len(parts) == 1:
            video_id = parts[0]
    elif host in {"youtube.com", "www.youtube.com", "m.youtube.com"}:
        values = urllib.parse.parse_qs(parsed.query, keep_blank_values=True)
        candidates = values.get("v", [])
        if parsed.path == "/watch" and len(candidates) == 1:
            video_id = candidates[0]
    if VIDEO_ID_RE.fullmatch(video_id) is None:
        raise CacheError("unsupported watch URL")
    return video_id


def require_private_directory(path: Path, create: bool = True) -> None:
    if not path.is_absolute() or path == Path("/"):
        raise CacheError("cache directory must be a bounded absolute path")
    if create:
        path.mkdir(mode=0o700, parents=True, exist_ok=True)
    try:
        status = path.lstat()
    except OSError as error:
        raise CacheError("cache directory unavailable") from error
    if (
        not stat.S_ISDIR(status.st_mode)
        or stat.S_ISLNK(status.st_mode)
        or status.st_uid != os.geteuid()
        or stat.S_IMODE(status.st_mode) != 0o700
    ):
        raise CacheError("cache directory is not private")


class Layout:
    def __init__(self, root: Path) -> None:
        self.root = root
        self.entries = root / "entries"
        self.locks = root / "locks"
        self.claims = root / "claims"
        self.slots = root / "slots"
        require_private_directory(root)
        for directory in (self.entries, self.locks, self.claims, self.slots):
            require_private_directory(directory)

    def entry(self, video_id: str) -> Path:
        return self.entries / f"{video_id}.json"

    def lock(self, video_id: str) -> Path:
        slot = int(hashlib.sha256(video_id.encode("ascii")).hexdigest()[:8], 16)
        return self.locks / f"video-{slot % VIDEO_LOCK_SLOTS:02d}.lock"

    def maintenance_lock(self) -> Path:
        return self.root / "maintenance.lock"


def open_private_lock(path: Path) -> int:
    flags = os.O_RDWR | os.O_CREAT | os.O_CLOEXEC
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    try:
        descriptor = os.open(path, flags, 0o600)
    except OSError as error:
        raise CacheError("cannot open cache lock") from error
    status = os.fstat(descriptor)
    if (
        not stat.S_ISREG(status.st_mode)
        or status.st_uid != os.geteuid()
        or status.st_nlink != 1
        or stat.S_IMODE(status.st_mode) != 0o600
    ):
        os.close(descriptor)
        raise CacheError("cache lock is not private")
    return descriptor


def private_regular_files(path: Path) -> List[Tuple[float, Path]]:
    files: List[Tuple[float, Path]] = []
    for item in path.iterdir():
        try:
            status = item.lstat()
        except OSError:
            continue
        if (
            stat.S_ISREG(status.st_mode)
            and not stat.S_ISLNK(status.st_mode)
            and status.st_uid == os.geteuid()
            and status.st_nlink == 1
            and stat.S_IMODE(status.st_mode) == 0o600
        ):
            files.append((status.st_mtime, item))
    return files


def maintain_layout(layout: Layout, protected_video_id: str = "") -> None:
    descriptor = open_private_lock(layout.maintenance_lock())
    try:
        fcntl.flock(descriptor, fcntl.LOCK_EX)
        entries = [
            item
            for item in private_regular_files(layout.entries)
            if item[1].suffix == ".json"
            and VIDEO_ID_RE.fullmatch(item[1].stem) is not None
        ]
        removable = sorted(
            (
                item
                for item in entries
                if item[1].stem != protected_video_id
            ),
            key=lambda item: item[0],
        )
        excess = max(0, len(entries) - MAX_CACHE_ENTRIES)
        for _mtime, path in removable[:excess]:
            try:
                path.unlink()
            except OSError:
                pass

        now = time.time()
        for mtime, path in private_regular_files(layout.claims):
            if CLAIM_RE.fullmatch(path.name) is None or now - mtime <= STALE_CLAIM_SECONDS:
                continue
            try:
                path.unlink()
            except OSError:
                pass
    finally:
        fcntl.flock(descriptor, fcntl.LOCK_UN)
        os.close(descriptor)


def create_claim(
    layout: Layout, video_id: str, value: Dict[str, object]
) -> Path:
    descriptor = open_private_lock(layout.maintenance_lock())
    try:
        fcntl.flock(descriptor, fcntl.LOCK_EX)
        active_claims = [
            item
            for item in private_regular_files(layout.claims)
            if CLAIM_RE.fullmatch(item[1].name) is not None
        ]
        if len(active_claims) >= MAX_ACTIVE_CLAIMS:
            raise CacheError("active claim limit reached")
        claim_descriptor, claim_name = tempfile.mkstemp(
            prefix=f"{video_id}.", suffix=".json", dir=layout.claims
        )
        os.fchmod(claim_descriptor, 0o600)
        os.close(claim_descriptor)
        claim = Path(claim_name)
        claim.unlink()
        write_private_json(claim, value)
        return claim
    finally:
        fcntl.flock(descriptor, fcntl.LOCK_UN)
        os.close(descriptor)


@contextmanager
def video_lock(layout: Layout, video_id: str) -> Iterator[None]:
    descriptor = open_private_lock(layout.lock(video_id))
    try:
        fcntl.flock(descriptor, fcntl.LOCK_EX)
        check_termination()
        yield
    finally:
        fcntl.flock(descriptor, fcntl.LOCK_UN)
        os.close(descriptor)


@contextmanager
def resolver_slot(layout: Layout) -> Iterator[int]:
    descriptors = [
        open_private_lock(layout.slots / "resolver-0.lock"),
        open_private_lock(layout.slots / "resolver-1.lock"),
    ]
    selected = -1
    try:
        while selected < 0:
            check_termination()
            for index, descriptor in enumerate(descriptors):
                try:
                    fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    selected = index
                    break
                except BlockingIOError:
                    continue
            if selected < 0:
                time.sleep(0.05)
        yield selected
    finally:
        if selected >= 0:
            fcntl.flock(descriptors[selected], fcntl.LOCK_UN)
        for descriptor in descriptors:
            os.close(descriptor)


def read_private_json(path: Path) -> Dict[str, object]:
    flags = os.O_RDONLY | os.O_CLOEXEC
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    try:
        descriptor = os.open(path, flags)
    except OSError as error:
        raise CacheError("private configuration unavailable") from error
    try:
        status = os.fstat(descriptor)
        if (
            not stat.S_ISREG(status.st_mode)
            or status.st_uid != os.geteuid()
            or status.st_nlink != 1
            or stat.S_IMODE(status.st_mode) != 0o600
            or status.st_size < 2
            or status.st_size > MAX_CONFIG_BYTES
        ):
            raise CacheError("configuration is not a private bounded file")
        payload = b""
        while len(payload) <= MAX_CONFIG_BYTES:
            block = os.read(descriptor, min(65536, MAX_CONFIG_BYTES + 1 - len(payload)))
            if not block:
                break
            payload += block
        if len(payload) != status.st_size:
            raise CacheError("configuration changed while reading")
    finally:
        os.close(descriptor)
    try:
        value = json.loads(payload.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise CacheError("invalid configuration JSON") from error
    if not isinstance(value, dict):
        raise CacheError("configuration object required")
    return value


def signed_expiry(url: object, now: int) -> int:
    if not isinstance(url, str) or len(url) > 16384:
        raise CacheError("bounded signed URL required")
    parsed = urllib.parse.urlsplit(url)
    host = (parsed.hostname or "").lower().rstrip(".")
    if (
        parsed.scheme != "https"
        or parsed.username
        or parsed.password
        or not (host == "googlevideo.com" or host.endswith(".googlevideo.com"))
    ):
        raise CacheError("signed URL host rejected")
    values = urllib.parse.parse_qs(parsed.query, keep_blank_values=True)
    expiry_values = values.get("expire", [])
    if len(expiry_values) != 1 or not expiry_values[0].isdigit():
        raise CacheError("signed URL expiry missing")
    expiry = int(expiry_values[0])
    if expiry <= now + EXPIRY_SAFETY_SECONDS:
        raise CacheError("signed URL inside safety window")
    if expiry > now + MAX_SIGNED_LIFETIME_SECONDS:
        raise CacheError("signed URL lifetime implausible")
    return expiry


def validate_config(
    value: Dict[str, object],
    video_id: str,
    now: int,
    now_monotonic: float,
    require_cache_metadata: bool,
) -> Tuple[Dict[str, object], int]:
    if value.get("version") != 1 or value.get("source_id") != video_id:
        raise CacheError("resolver identity mismatch")
    streams = value.get("streams")
    if not isinstance(streams, dict) or "video" not in streams or not 1 <= len(streams) <= 2:
        raise CacheError("invalid stream set")
    duration = value.get("duration")
    if (
        not isinstance(duration, (int, float))
        or isinstance(duration, bool)
        or not math.isfinite(float(duration))
        or float(duration) <= 0.0
    ):
        raise CacheError("finite positive duration required")
    expiries: List[int] = []
    for name, raw_stream in streams.items():
        if name not in {"video", "audio"} or not isinstance(raw_stream, dict):
            raise CacheError("invalid stream entry")
        size = raw_stream.get("size")
        if not isinstance(size, int) or isinstance(size, bool) or size < 1:
            raise CacheError("invalid stream size")
        expiries.append(signed_expiry(raw_stream.get("url"), now))
    expiry = min(expiries)
    if expiry - now <= math.ceil(float(duration)) + EXPIRY_SAFETY_SECONDS:
        raise CacheError("signed URL cannot cover playback plus safety window")
    metadata = value.get("resolver_cache")
    if require_cache_metadata:
        if not isinstance(metadata, dict):
            raise CacheError("cache metadata missing")
        resolved_at = metadata.get("resolved_at")
        resolved_monotonic = metadata.get("resolved_monotonic")
        if (
            metadata.get("schema") != CACHE_SCHEMA
            or metadata.get("video_id") != video_id
            or metadata.get("video_format") != PINNED_VIDEO_FORMAT
            or metadata.get("expires_at") != expiry
            or not isinstance(resolved_at, int)
            or isinstance(resolved_at, bool)
            or not isinstance(resolved_monotonic, (int, float))
            or isinstance(resolved_monotonic, bool)
            or not math.isfinite(float(resolved_monotonic))
            or resolved_at > now + 60
            or resolved_at >= expiry
        ):
            raise CacheError("cache metadata mismatch")
        wall_age = now - resolved_at
        monotonic_age = now_monotonic - float(resolved_monotonic)
        if (
            wall_age < -MAX_CLOCK_DRIFT_SECONDS
            or monotonic_age < -MAX_CLOCK_DRIFT_SECONDS
            or wall_age > MAX_CACHE_AGE_SECONDS
            or monotonic_age > MAX_CACHE_AGE_SECONDS
            or abs(wall_age - monotonic_age) > MAX_CLOCK_DRIFT_SECONDS
        ):
            raise CacheError("cache age or clock drift rejected")
    return value, expiry


def write_private_json(path: Path, value: Dict[str, object]) -> None:
    descriptor, temporary_name = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    temporary = Path(temporary_name)
    try:
        os.fchmod(descriptor, 0o600)
        payload = (json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n").encode()
        if len(payload) > MAX_CONFIG_BYTES:
            raise CacheError("configuration exceeds cache limit")
        written = 0
        while written < len(payload):
            written += os.write(descriptor, payload[written:])
        os.fsync(descriptor)
        os.close(descriptor)
        descriptor = -1
        os.replace(temporary, path)
        directory_fd = os.open(path.parent, os.O_RDONLY | os.O_CLOEXEC | os.O_DIRECTORY)
        try:
            os.fsync(directory_fd)
        finally:
            os.close(directory_fd)
    finally:
        if descriptor >= 0:
            os.close(descriptor)
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


def resolver_path() -> Path:
    configured = os.environ.get("RG_YOUTUBE_RESOLVER")
    path = Path(configured) if configured else Path(__file__).resolve().parent / "resolve_youtube.py"
    if not path.is_absolute():
        raise CacheError("resolver path must be absolute")
    try:
        status = path.lstat()
    except OSError as error:
        raise CacheError("resolver unavailable") from error
    if not stat.S_ISREG(status.st_mode) or stat.S_ISLNK(status.st_mode) or not os.access(path, os.X_OK):
        raise CacheError("resolver is not a safe executable")
    return path


def run_resolver(
    url: str, video_id: str, layout: Layout, now: int, now_monotonic: float,
    background: bool = False,
) -> Tuple[Dict[str, object], int]:
    descriptor, raw_name = tempfile.mkstemp(prefix=f".resolve.{video_id}.", dir=layout.entries)
    os.close(descriptor)
    raw_path = Path(raw_name)
    raw_path.unlink()
    command = [
        str(resolver_path()),
        url,
        "--output",
        str(raw_path),
        "--video-format",
        PINNED_VIDEO_FORMAT,
        "--max-height",
        "720",
        "--player-client",
        "android",
    ]
    resolver_environment = os.environ.copy()
    resolver_environment["RG_YOUTUBE_RESOLVE_PRIORITY"] = (
        "background" if background else "interactive"
    )
    child: Optional[subprocess.Popen[str]] = None
    try:
        child = subprocess.Popen(
            command,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            start_new_session=True,
            preexec_fn=(lambda: os.nice(15)) if background else None,
            env=resolver_environment,
        )
        _active_children.append(child)
        try:
            _stdout, _stderr = child.communicate(timeout=RESOLVER_TIMEOUT_SECONDS)
        except subprocess.TimeoutExpired:
            terminate_child(child)
            try:
                child.communicate(timeout=1)
            except subprocess.TimeoutExpired:
                terminate_child(child, force=True)
                child.communicate()
            raise CacheError("resolver timeout")
        check_termination()
        if child.returncode != 0:
            raise CacheError(f"resolver status {child.returncode}")
        value = read_private_json(raw_path)
        value, expiry = validate_config(value, video_id, now, now_monotonic, False)
        value["resolver_cache"] = {
            "schema": CACHE_SCHEMA,
            "video_id": video_id,
            "video_format": PINNED_VIDEO_FORMAT,
            "resolved_at": now,
            "resolved_monotonic": now_monotonic,
            "expires_at": expiry,
        }
        return value, expiry
    finally:
        if child is not None and child in _active_children:
            _active_children.remove(child)
        if child is not None and child.poll() is None:
            terminate_child(child, force=True)
            child.wait()
        try:
            raw_path.unlink()
        except FileNotFoundError:
            pass


def cached_entry(
    layout: Layout, video_id: str, now: int, now_monotonic: float
) -> Optional[Tuple[Dict[str, object], int]]:
    path = layout.entry(video_id)
    try:
        value = read_private_json(path)
        return validate_config(value, video_id, now, now_monotonic, True)
    except CacheError:
        try:
            path.unlink()
        except FileNotFoundError:
            pass
        return None


def resolve_and_publish(
    url: str, video_id: str, layout: Layout, now: int, now_monotonic: float,
    background: bool = False,
) -> int:
    with resolver_slot(layout):
        value, expiry = run_resolver(
            url, video_id, layout, now, now_monotonic, background
        )
        write_private_json(layout.entry(video_id), value)
        maintain_layout(layout, video_id)
        return expiry


def prefetch_one(url: str, layout: Layout) -> None:
    video_id = video_id_for_url(url)
    now = int(time.time())
    now_monotonic = time.monotonic()
    with video_lock(layout, video_id):
        hit = cached_entry(layout, video_id, now, now_monotonic)
        if hit is not None:
            diagnostic("prefetch", "HIT", video_id, f"valid_for={hit[1] - now}")
            return
        expiry = resolve_and_publish(
            url, video_id, layout, now, now_monotonic, background=True
        )
        diagnostic("prefetch", "STORED", video_id, f"valid_for={expiry - now}")


def acquire(url: str, layout: Layout) -> Path:
    video_id = video_id_for_url(url)
    now = int(time.time())
    now_monotonic = time.monotonic()
    with video_lock(layout, video_id):
        hit = cached_entry(layout, video_id, now, now_monotonic)
        source = "HIT"
        if hit is None:
            resolve_and_publish(url, video_id, layout, now, now_monotonic)
            hit = cached_entry(layout, video_id, now, now_monotonic)
            if hit is None:
                raise CacheError("published configuration unavailable")
            source = "RESOLVED"
        # A claim is single-use, but the strictly validated cache entry is not.
        # Publish an independent 0600 snapshot so returning HOME or replaying a
        # video inside the bounded cache lifetime does not start yt-dlp again.
        claim = create_claim(layout, video_id, hit[0])
        diagnostic("acquire", source, video_id)
        return claim


def release_claim(value: str, layout: Layout) -> None:
    claim = Path(value)
    try:
        resolved_parent = claim.parent.resolve(strict=True)
    except OSError as error:
        raise CacheError("claim parent unavailable") from error
    if not claim.is_absolute() or resolved_parent != layout.claims.resolve(strict=True):
        raise CacheError("claim outside private cache")
    match = CLAIM_RE.fullmatch(claim.name)
    if match is None:
        raise CacheError("invalid claim name")
    read_private_json(claim)
    claim.unlink()
    diagnostic("release", "REMOVED", match.group(1))


def cache_root() -> Path:
    configured = os.environ.get("RG_YOUTUBE_CACHE_DIR")
    return Path(configured) if configured else Path("/run/rg40xxv-youtube-native/cache")


def prefetch_set(urls: Sequence[str]) -> int:
    if not 1 <= len(urls) <= 3:
        raise CacheError("prefetch-set accepts selected and at most two neighbours")
    unique: List[str] = []
    seen = set()
    for url in urls:
        video_id = video_id_for_url(url)
        if video_id not in seen:
            seen.add(video_id)
            unique.append(url)
    children: List[subprocess.Popen[str]] = []
    try:
        for url in unique:
            child = subprocess.Popen(
                [sys.executable, str(Path(__file__).resolve()), "prefetch", url],
                stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL,
                stderr=None,
                text=True,
                start_new_session=True,
            )
            children.append(child)
            _active_children.append(child)
        selected_status = 0
        for index, child in enumerate(children):
            while child.poll() is None:
                check_termination()
                try:
                    child.wait(timeout=0.1)
                except subprocess.TimeoutExpired:
                    continue
            # Neighbour prefetch is speculative.  Only the first URL is the
            # selected card, so a neighbour failure must not make the scene
            # quarantine a selected URL that was cached successfully.
            if index == 0 and child.returncode != 0:
                selected_status = 2
        return selected_status
    finally:
        for child in children:
            if child in _active_children:
                _active_children.remove(child)
            if child.poll() is None:
                terminate_child(child)
        for child in children:
            if child.poll() is None:
                try:
                    child.wait(timeout=1)
                except subprocess.TimeoutExpired:
                    terminate_child(child, force=True)
                    child.wait()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="action", required=True)
    prefetch_parser = subparsers.add_parser("prefetch")
    prefetch_parser.add_argument("url")
    set_parser = subparsers.add_parser("prefetch-set")
    set_parser.add_argument("urls", nargs="+")
    acquire_parser = subparsers.add_parser("acquire")
    acquire_parser.add_argument("url")
    release_parser = subparsers.add_parser("release")
    release_parser.add_argument("claim")
    return parser.parse_args()


def main() -> int:
    install_signal_handlers()
    args = parse_args()
    try:
        if args.action == "prefetch-set":
            return prefetch_set(args.urls)
        layout = Layout(cache_root())
        maintain_layout(layout)
        if args.action == "prefetch":
            prefetch_one(args.url, layout)
        elif args.action == "acquire":
            print(acquire(args.url, layout), flush=True)
        elif args.action == "release":
            release_claim(args.claim, layout)
        return 0
    except CacheError as error:
        print(
            f"YOUTUBE_RESOLVER_CACHE action={getattr(args, 'action', 'unknown')} "
            f"result=FAIL reason={error}",
            file=sys.stderr,
        )
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
