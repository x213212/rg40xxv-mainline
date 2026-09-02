#!/usr/bin/env python3
"""Bounded, metadata-first YouTube home/search feed for the native UI.

Stdout is a deliberately small line protocol.  Diagnostics go to stderr and
never include URLs returned by yt-dlp. Metadata is flushed before thumbnail
work; private thumbnail paths arrive later as progressive THUMB records.
"""

from __future__ import annotations

import argparse
import calendar
import concurrent.futures
import datetime
import fcntl
import hashlib
import http.client
import json
import math
import os
import re
import signal
import socket
import stat
import struct
import subprocess
import sys
import tempfile
import time
import unicodedata
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ElementTree
from pathlib import Path
from typing import Any, Dict, List, Sequence, Tuple


SCHEMA = "rg40xxv-youtube-feed-v5-playable-lockup-aggregate-atom"
VIDEO_ID_RE = re.compile(r"^[A-Za-z0-9_-]{11}$")
CHANNEL_ID_RE = re.compile(r"^UC[A-Za-z0-9_-]{22}$")
RFC3339_RE = re.compile(
    r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d{1,6})?(?:Z|[+-]\d{2}:\d{2})$"
)
PAGE_ITEMS = 8
PAGE_FETCH_ITEMS = PAGE_ITEMS + 1
MAX_ITEMS = 96
MIN_ITEMS = 1
MAX_YTDLP_OUTPUT = 2 * 1024 * 1024
MAX_YTDLP_SERVER_REQUEST = 4096
MAX_YTDLP_SERVER_RESPONSE = 2 * 1024 * 1024
MAX_SEARCH_PAGE = 4 * 1024 * 1024
MAX_ATOM_FEED_BYTES = 512 * 1024
MAX_ATOM_NODES = 512
MAX_ATOM_DEPTH = 12
MAX_ATOM_ENTRIES = 32
MAX_THUMBNAIL_BYTES = 512 * 1024
MAX_FEED_CACHE_ENTRIES = 32
MAX_CHANNEL_AGGREGATE_CACHE_ENTRIES = 16
MAX_THUMBNAIL_CACHE_ENTRIES = 64
FEED_LOCK_SLOTS = 32
DEFAULT_TTL_SECONDS = 180
DEFAULT_TIMEOUT_SECONDS = 18
# Channel/search metadata contains only validated IDs and display strings.
# Preserve it through normal network outages and refresh it asynchronously;
# signed playback URLs continue to use the much shorter resolver cache.
DEFAULT_STALE_MAX_SECONDS = 7 * 24 * 60 * 60
HOME_QUERY = "台灣 熱門"
ATOM_NAMESPACE = "{http://www.w3.org/2005/Atom}"
YOUTUBE_NAMESPACE = "{http://www.youtube.com/xml/schemas/2015}"
ATOM_CONTENT_TYPES = {"application/atom+xml", "application/xml", "text/xml"}
CHANNEL_AGGREGATE_DIRECTORY = "channel-aggregates"
YTDLP_SERVER_CACHE_STATES = {"MISS", "COALESCED", "MEMORY", "DISK", "STALE"}

_active_ytdlp: subprocess.Popen[bytes] | None = None


class FeedError(RuntimeError):
    pass


def terminate_ytdlp(child: subprocess.Popen[bytes], force: bool = False) -> None:
    if child.poll() is not None:
        return
    try:
        os.killpg(child.pid, signal.SIGKILL if force else signal.SIGTERM)
    except ProcessLookupError:
        pass


def on_signal(signum: int, _frame: object) -> None:
    child = _active_ytdlp
    if child is not None:
        terminate_ytdlp(child)
    raise SystemExit(128 + signum)


def install_signal_handlers() -> None:
    for signum in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
        signal.signal(signum, on_signal)


def clean_text(value: object, max_chars: int, max_bytes: int) -> str:
    if not isinstance(value, str):
        return ""
    value = unicodedata.normalize("NFKC", value)
    value = "".join(" " if unicodedata.category(char).startswith("C") else char for char in value)
    value = " ".join(value.split())
    value = value[:max_chars]
    while value and len(value.encode("utf-8")) > max_bytes:
        value = value[:-1]
    return value


def require_private_dir(path: Path) -> None:
    if not path.is_absolute() or path == Path("/"):
        raise FeedError("unsafe cache root")
    try:
        path.mkdir(mode=0o700, parents=True, exist_ok=True)
        info = path.lstat()
    except OSError as error:
        raise FeedError("cache unavailable") from error
    if (
        not stat.S_ISDIR(info.st_mode)
        or stat.S_ISLNK(info.st_mode)
        or info.st_uid != os.geteuid()
        or stat.S_IMODE(info.st_mode) != 0o700
    ):
        raise FeedError("unsafe cache root")


def open_private_lock(path: Path) -> int:
    flags = os.O_RDWR | os.O_CREAT | os.O_CLOEXEC
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    try:
        descriptor = os.open(path, flags, 0o600)
    except OSError as error:
        raise FeedError("cache lock unavailable") from error
    info = os.fstat(descriptor)
    if (
        not stat.S_ISREG(info.st_mode)
        or info.st_uid != os.geteuid()
        or info.st_nlink != 1
        or stat.S_IMODE(info.st_mode) != 0o600
    ):
        os.close(descriptor)
        raise FeedError("unsafe cache lock")
    return descriptor


def feed_lock_path(cache_root: Path, key: str) -> Path:
    # A fixed lock pool prevents arbitrary searches from leaving one inode per
    # query forever. Hash collisions only serialize two metadata refreshes.
    slot = int(key[:8], 16) % FEED_LOCK_SLOTS
    return cache_root / f"feed-slot-{slot:02d}.lock"


def prune_feed_cache(cache_root: Path, protected_key: str) -> None:
    candidates: List[Tuple[float, Path]] = []
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
    protected_name = f"{protected_key}.json"
    removable = sorted(
        (item for item in candidates if item[1].name != protected_name),
        key=lambda item: item[0],
    )
    excess = max(0, len(candidates) - MAX_FEED_CACHE_ENTRIES)
    for _mtime, path in removable[:excess]:
        try:
            path.unlink()
        except OSError:
            pass


def require_tool(path: Path) -> None:
    if not path.is_absolute():
        raise FeedError("yt-dlp path must be absolute")
    try:
        info = path.lstat()
    except OSError as error:
        raise FeedError("yt-dlp unavailable") from error
    if not stat.S_ISREG(info.st_mode) or stat.S_ISLNK(info.st_mode) or not os.access(path, os.X_OK):
        raise FeedError("yt-dlp contract failed")


def duration_seconds(value: object) -> int:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return 0
    number = float(value)
    if not math.isfinite(number) or number < 0:
        return 0
    return min(int(round(number)), 10 * 24 * 60 * 60)


def duration_text_seconds(value: object) -> int:
    text = clean_text(value, 24, 48)
    if not text:
        return 0
    parts = text.split(":")
    if not 1 <= len(parts) <= 3 or any(not part.isdigit() for part in parts):
        return 0
    result = 0
    for part in parts:
        result = result * 60 + int(part)
    return min(result, 10 * 24 * 60 * 60)


def published_epoch(raw: Dict[str, object]) -> int:
    for key in ("timestamp", "release_timestamp"):
        value = raw.get(key)
        if isinstance(value, bool) or not isinstance(value, (int, float)):
            continue
        number = float(value)
        if math.isfinite(number) and 0 < number < 4_102_444_800:
            return int(number)
    for key in ("upload_date", "release_date"):
        value = raw.get(key)
        if not isinstance(value, str) or re.fullmatch(r"\d{8}", value) is None:
            continue
        try:
            return int(calendar.timegm(time.strptime(value, "%Y%m%d")))
        except (OverflowError, ValueError):
            continue
    return 0


def published_display(raw: Dict[str, object]) -> str:
    # Prefer an exact machine-readable date over YouTube's localized
    # publishedTimeText (for example, "3 天前").  The official Atom feed
    # and yt-dlp can both supply one of these exact fields.
    epoch = published_epoch(raw)
    if epoch:
        return time.strftime("%Y-%m-%d", time.gmtime(epoch))
    value = clean_text(raw.get("published"), 40, 92)
    if value:
        return value
    return "日期未知"


def renderer_text(value: object) -> str:
    if not isinstance(value, dict):
        return ""
    simple = value.get("simpleText")
    if isinstance(simple, str):
        return simple
    runs = value.get("runs")
    if not isinstance(runs, list):
        return ""
    return "".join(run.get("text", "") for run in runs if isinstance(run, dict))


def feed_entry_is_playable(raw: Dict[str, object]) -> bool:
    availability = raw.get("availability")
    if isinstance(availability, str):
        availability = availability.strip().lower()
        if availability and availability not in {"public", "unlisted"}:
            return False
    return raw.get("live_status") != "is_upcoming"


def nested_dict(value: object, *keys: str) -> Dict[str, object]:
    current = value
    for key in keys:
        if not isinstance(current, dict):
            return {}
        current = current.get(key)
    return current if isinstance(current, dict) else {}


def lockup_entry(value: object) -> Dict[str, object] | None:
    """Decode YouTube's current channel/search card without executing JS."""
    if not isinstance(value, dict):
        return None
    video_id = value.get("contentId")
    content_type = value.get("contentType")
    if (
        not isinstance(video_id, str)
        or VIDEO_ID_RE.fullmatch(video_id) is None
        or content_type not in (None, "LOCKUP_CONTENT_TYPE_VIDEO")
    ):
        return None
    metadata = nested_dict(value, "metadata", "lockupMetadataViewModel")
    title = clean_text(nested_dict(metadata, "title").get("content"), 96, 240)
    content_metadata = nested_dict(
        metadata, "metadata", "contentMetadataViewModel"
    )
    rows = content_metadata.get("metadataRows")
    if not title or not isinstance(rows, list):
        return None
    published = ""
    for row in rows:
        if not isinstance(row, dict):
            continue
        badges = row.get("badges")
        if isinstance(badges, list):
            for badge in badges:
                badge_view = nested_dict(badge, "badgeViewModel")
                style = badge_view.get("badgeStyle")
                if isinstance(style, str) and (
                    "MEMBERS_ONLY" in style
                    or "PREMIUM" in style
                    or "PRIVATE" in style
                ):
                    return None
        parts = row.get("metadataParts")
        if not isinstance(parts, list):
            continue
        for part in parts:
            text = nested_dict(part, "text")
            candidate = clean_text(
                text.get("accessibilityLabel") or text.get("content"), 40, 92
            )
            if candidate and not candidate.startswith(("觀看次數", "觀看次數：")):
                published = candidate
    duration = 0
    thumbnail = nested_dict(value, "contentImage", "thumbnailViewModel")
    overlays = thumbnail.get("overlays")
    if isinstance(overlays, list):
        for overlay in overlays:
            badges = nested_dict(
                overlay, "thumbnailBottomOverlayViewModel"
            ).get("badges")
            if not isinstance(badges, list):
                continue
            for badge in badges:
                text = nested_dict(badge, "thumbnailBadgeViewModel").get("text")
                candidate = duration_text_seconds(text if isinstance(text, str) else "")
                if candidate > 0:
                    duration = candidate
                    break
            if duration:
                break
    return {
        "id": video_id,
        "title": title,
        "channel": "YouTube",
        "duration": duration,
        "published": published,
    }


def parse_video_renderers(
    page: str, *, newest_first: bool = False
) -> List[Dict[str, object]]:
    marker = '"videoRenderer":'
    decoder = json.JSONDecoder()
    entries: List[Dict[str, object]] = []
    offset = 0
    # Pages can repeat a renderer in several shelves.  Inspect a bounded
    # surplus, then let parse_items de-duplicate and cap the public result.
    while len(entries) < 4 * MAX_ITEMS:
        found = page.find(marker, offset)
        if found < 0:
            break
        value_at = found + len(marker)
        try:
            renderer, consumed = decoder.raw_decode(page, value_at)
        except json.JSONDecodeError:
            offset = value_at
            continue
        offset = consumed
        if not isinstance(renderer, dict):
            continue
        entries.append(
            {
                "id": renderer.get("videoId"),
                "title": renderer_text(renderer.get("title")),
                "channel": renderer_text(
                    renderer.get("ownerText")
                    or renderer.get("longBylineText")
                    or renderer.get("shortBylineText")
                ),
                "duration": duration_text_seconds(renderer_text(renderer.get("lengthText"))),
                "published": renderer_text(renderer.get("publishedTimeText")),
            }
        )
    marker = '"lockupViewModel":'
    offset = 0
    while len(entries) < 4 * MAX_ITEMS:
        found = page.find(marker, offset)
        if found < 0:
            break
        value_at = found + len(marker)
        try:
            renderer, consumed = decoder.raw_decode(page, value_at)
        except json.JSONDecodeError:
            offset = value_at
            continue
        offset = consumed
        entry = lockup_entry(renderer)
        if entry is not None:
            entries.append(entry)
    return parse_items(
        {"entries": entries}, limit=MAX_ITEMS, minimum=MIN_ITEMS,
        newest_first=newest_first
    )


def parse_rfc3339(value: object) -> Tuple[str, int]:
    if not isinstance(value, str) or RFC3339_RE.fullmatch(value) is None:
        raise FeedError("Atom published timestamp rejected")
    try:
        parsed = datetime.datetime.fromisoformat(
            value[:-1] + "+00:00" if value.endswith("Z") else value
        )
    except ValueError as error:
        raise FeedError("Atom published timestamp rejected") from error
    if parsed.tzinfo is None:
        raise FeedError("Atom published timezone rejected")
    epoch = int(parsed.timestamp())
    if not 0 < epoch < 4_102_444_800:
        raise FeedError("Atom published range rejected")
    utc = parsed.astimezone(datetime.timezone.utc)
    return utc.date().isoformat(), epoch


def parse_atom_feed(payload: bytes, channel_id: str) -> List[Dict[str, object]]:
    if CHANNEL_ID_RE.fullmatch(channel_id) is None:
        raise FeedError("channel identity rejected")
    if not 2 <= len(payload) <= MAX_ATOM_FEED_BYTES:
        raise FeedError("Atom feed size rejected")
    upper_payload = payload.upper()
    if b"<!DOCTYPE" in upper_payload or b"<!ENTITY" in upper_payload:
        raise FeedError("Atom declarations rejected")
    try:
        root = ElementTree.fromstring(payload)
    except ElementTree.ParseError as error:
        raise FeedError("Atom XML rejected") from error
    if root.tag != f"{ATOM_NAMESPACE}feed":
        raise FeedError("Atom root rejected")

    node_count = 0
    stack = [(root, 1)]
    while stack:
        node, depth = stack.pop()
        node_count += 1
        if (
            node_count > MAX_ATOM_NODES
            or depth > MAX_ATOM_DEPTH
            or len(node.attrib) > 16
        ):
            raise FeedError("Atom tree bounds rejected")
        stack.extend((child, depth + 1) for child in list(node))

    declared_channel = root.findtext(f"{YOUTUBE_NAMESPACE}channelId")
    # The live YouTube Atom endpoint currently emits the feed-level channelId
    # without its leading "UC" for some channels, while every entry retains
    # the canonical full ID.  Accept only those two byte-exact forms; the
    # request URL and each entry are still bound to the full caller ID below.
    if declared_channel not in {channel_id, channel_id[2:]}:
        raise FeedError("Atom channel identity rejected")
    channel_name = clean_text(
        root.findtext(f"{ATOM_NAMESPACE}author/{ATOM_NAMESPACE}name"), 64, 160
    ) or "YouTube"
    entries = root.findall(f"{ATOM_NAMESPACE}entry")
    if not 1 <= len(entries) <= MAX_ATOM_ENTRIES:
        raise FeedError("Atom entry count rejected")

    raw_items: List[Dict[str, object]] = []
    seen = set()
    for entry in entries:
        video_id = entry.findtext(f"{YOUTUBE_NAMESPACE}videoId")
        entry_channel = entry.findtext(f"{YOUTUBE_NAMESPACE}channelId")
        if (
            not isinstance(video_id, str)
            or VIDEO_ID_RE.fullmatch(video_id) is None
            or video_id in seen
            or entry_channel != channel_id
        ):
            raise FeedError("Atom entry identity rejected")
        published, epoch = parse_rfc3339(
            entry.findtext(f"{ATOM_NAMESPACE}published")
        )
        seen.add(video_id)
        raw_items.append(
            {
                "id": video_id,
                "title": entry.findtext(f"{ATOM_NAMESPACE}title"),
                "channel": entry.findtext(
                    f"{ATOM_NAMESPACE}author/{ATOM_NAMESPACE}name"
                ) or channel_name,
                "published": published,
                "timestamp": epoch,
                "duration": 0,
            }
        )
    return parse_items(
        {"entries": raw_items}, limit=MAX_ATOM_ENTRIES,
        minimum=MIN_ITEMS, newest_first=True
    )


def run_atom_feed(channel_id: str, timeout_seconds: int) -> List[Dict[str, object]]:
    if CHANNEL_ID_RE.fullmatch(channel_id) is None:
        raise FeedError("channel identity rejected")
    parameters = urllib.parse.urlencode({"channel_id": channel_id})
    target = f"https://www.youtube.com/feeds/videos.xml?{parameters}"
    request = urllib.request.Request(
        target,
        headers={
            "User-Agent": "rg40xxv-youtube-feed/1",
            "Accept": "application/atom+xml,application/xml;q=0.9",
            "Accept-Encoding": "identity",
            "Connection": "close",
        },
    )
    try:
        with urllib.request.urlopen(
            request, timeout=min(timeout_seconds, 4)
        ) as response:
            final = urllib.parse.urlsplit(response.geturl())
            try:
                final_query = urllib.parse.parse_qs(
                    final.query, keep_blank_values=True, strict_parsing=True
                )
            except ValueError as error:
                raise FeedError("Atom response URL rejected") from error
            content_type = response.headers.get_content_type()
            content_length = response.headers.get("Content-Length")
            content_encoding = response.headers.get("Content-Encoding")
            if (
                getattr(response, "status", 200) != 200
                or final.scheme != "https"
                or (final.hostname or "").lower().rstrip(".") != "www.youtube.com"
                or final.username is not None
                or final.password is not None
                or final.port not in (None, 443)
                or final.path != "/feeds/videos.xml"
                or final.fragment
                or final_query != {"channel_id": [channel_id]}
                or content_type not in ATOM_CONTENT_TYPES
                or content_encoding not in (None, "identity")
                or (
                    content_length is not None
                    and (
                        not content_length.isdigit()
                        or int(content_length) > MAX_ATOM_FEED_BYTES
                    )
                )
            ):
                raise FeedError("Atom response rejected")
            payload = response.read(MAX_ATOM_FEED_BYTES + 1)
    except FeedError:
        raise
    except (OSError, ValueError, http.client.HTTPException) as error:
        raise FeedError("Atom feed unavailable") from error
    return parse_atom_feed(payload, channel_id)


def merge_channel_metadata(
    raw_items: List[Dict[str, object]], atom_items: List[Dict[str, object]],
    *, include_atom_only: bool
) -> List[Dict[str, object]]:
    raw_by_id = {str(item["id"]): item for item in raw_items}
    merged: List[Dict[str, object]] = []
    seen = set()
    for atom in atom_items:
        video_id = str(atom["id"])
        raw = raw_by_id.get(video_id)
        if raw is None and not include_atom_only:
            continue
        item = dict(atom if raw is None else raw)
        if raw is not None and item.get("channel") in (None, "", "YouTube"):
            item["channel"] = atom.get("channel", "YouTube")
        item["published"] = atom["published"]
        item["published_epoch"] = atom["published_epoch"]
        merged.append(item)
        seen.add(video_id)
    merged.extend(item for item in raw_items if str(item["id"]) not in seen)
    return merged[:MAX_ITEMS]


def run_channel_fast(
    channel_id: str, timeout_seconds: int
) -> List[Dict[str, object]]:
    if os.environ.get("RG_YOUTUBE_FEED_DISABLE_HTTP") == "1":
        raise FeedError("raw channel HTTP disabled")
    # Atom and the anonymous channel page are independent bounded requests.
    # Running them together keeps first-screen latency at the slower existing
    # request instead of adding the two waits, and never starts per-video
    # extraction for an exact date.
    with concurrent.futures.ThreadPoolExecutor(
        max_workers=2, thread_name_prefix="youtube-channel-fast"
    ) as executor:
        raw_future = executor.submit(
            run_raw_feed, "channel", channel_id, timeout_seconds
        )
        atom_future = executor.submit(run_atom_feed, channel_id, timeout_seconds)
        try:
            raw_items = raw_future.result()
        except FeedError:
            raw_items = []
        try:
            atom_items = atom_future.result()
        except FeedError:
            atom_items = []
    if raw_items and atom_items:
        atom_ids = {str(item["id"]) for item in atom_items}
        raw_items = [
            item for item in raw_items if str(item["id"]) in atom_ids
        ]
        return merge_channel_metadata(
            raw_items, atom_items, include_atom_only=True
        )
    if atom_items:
        return atom_items
    # Anonymous channel HTML contains recommendation shelves.  Without the
    # channel-bound Atom IDs there is no trustworthy way to distinguish those
    # cards from uploads, so fall through to yt-dlp's channel-ID-validated
    # extraction instead of exposing unbound raw entries.
    if raw_items:
        raise FeedError("raw channel entries lack identity authority")
    raise FeedError("fast channel metadata unavailable")


def run_raw_feed(
    mode: str, query: str, timeout_seconds: int
) -> List[Dict[str, object]]:
    if mode == "channel":
        if CHANNEL_ID_RE.fullmatch(query) is None:
            raise FeedError("channel identity rejected")
        target = f"https://www.youtube.com/channel/{query}/videos?hl=zh-TW&gl=TW"
    else:
        parameters = urllib.parse.urlencode(
            {"search_query": query, "hl": "zh-TW", "gl": "TW"}
        )
        target = f"https://www.youtube.com/results?{parameters}"
    request = urllib.request.Request(
        target,
        headers={
            "User-Agent": "Mozilla/5.0 (X11; Linux aarch64) AppleWebKit/537.36 Chrome/124 Safari/537.36",
            "Accept": "text/html,application/xhtml+xml",
            "Accept-Language": "zh-TW,zh;q=0.9,en;q=0.5",
            "Connection": "close",
        },
    )
    try:
        with urllib.request.urlopen(request, timeout=min(timeout_seconds, 8)) as response:
            final = urllib.parse.urlsplit(response.geturl())
            content_type = response.headers.get_content_type()
            length = response.headers.get("Content-Length")
            if (
                final.scheme != "https"
                or (final.hostname or "").lower().rstrip(".") not in {"youtube.com", "www.youtube.com"}
                or content_type not in {"text/html", "application/xhtml+xml"}
                or (length is not None and (not length.isdigit() or int(length) > MAX_SEARCH_PAGE))
            ):
                raise FeedError("raw search response rejected")
            payload = response.read(MAX_SEARCH_PAGE + 1)
    except (OSError, ValueError, http.client.HTTPException) as error:
        raise FeedError("raw search unavailable") from error
    if not 2 <= len(payload) <= MAX_SEARCH_PAGE:
        raise FeedError("raw search size rejected")
    try:
        page = payload.decode("utf-8", "strict")
    except UnicodeDecodeError as error:
        raise FeedError("raw search encoding rejected") from error
    return parse_video_renderers(page, newest_first=mode == "channel")


def run_raw_search(query: str, timeout_seconds: int) -> List[Dict[str, object]]:
    return run_raw_feed("search", query, timeout_seconds)


def parse_items(
    document: object, *, limit: int = MAX_ITEMS, minimum: int = MIN_ITEMS,
    newest_first: bool = False
) -> List[Dict[str, object]]:
    if not isinstance(document, dict) or not isinstance(document.get("entries"), list):
        raise FeedError("yt-dlp metadata shape rejected")
    result: List[Dict[str, object]] = []
    seen = set()
    for raw in document["entries"]:
        if len(result) >= limit:
            break
        if not isinstance(raw, dict):
            continue
        if not feed_entry_is_playable(raw):
            continue
        video_id = raw.get("id")
        if not isinstance(video_id, str) or VIDEO_ID_RE.fullmatch(video_id) is None or video_id in seen:
            continue
        title = clean_text(raw.get("title"), 96, 240)
        channel = clean_text(raw.get("channel") or raw.get("uploader"), 64, 160)
        if not title:
            continue
        if not channel:
            channel = "YouTube"
        seen.add(video_id)
        result.append(
            {
                "id": video_id,
                "title": title,
                "channel": channel,
                "published": published_display(raw),
                "published_epoch": published_epoch(raw),
                "duration": duration_seconds(raw.get("duration")),
                "thumbnail": f"https://i.ytimg.com/vi/{video_id}/mqdefault.jpg",
                "watch_url": f"https://www.youtube.com/watch?v={video_id}",
            }
        )
    if newest_first:
        # Official channel/video playlists are already newest-first.  When
        # absolute dates are present, make that contract explicit while
        # retaining source order for localized relative/unknown dates.
        result = [item for _index, item in sorted(
            enumerate(result),
            key=lambda pair: (
                0 if int(pair[1]["published_epoch"]) > 0 else 1,
                -int(pair[1]["published_epoch"]),
                pair[0],
            ),
        )]
    if len(result) < minimum:
        raise FeedError("too few valid feed entries")
    return result


def require_private_socket(path: Path) -> None:
    if (
        not path.is_absolute()
        or path == Path("/")
        or len(os.fsencode(path)) > 100
    ):
        raise FeedError("yt-dlp server path rejected")
    try:
        info = path.lstat()
    except OSError as error:
        raise FeedError("yt-dlp server unavailable") from error
    if (
        not stat.S_ISSOCK(info.st_mode)
        or stat.S_ISLNK(info.st_mode)
        or info.st_uid != os.geteuid()
        or stat.S_IMODE(info.st_mode) != 0o600
    ):
        raise FeedError("yt-dlp server socket rejected")


def parse_ytdlp_server_feed(
    document: object, mode: str, query: str, offset: int, limit: int
) -> Tuple[List[Dict[str, object]], bool]:
    server_mode = "channel" if mode == "channel" else "search"
    if (
        not isinstance(document, dict)
        or document.get("version") != 1
        or document.get("ok") is not True
        or not isinstance(document.get("feed"), dict)
    ):
        raise FeedError("yt-dlp server response rejected")
    feed = document["feed"]
    assert isinstance(feed, dict)
    count = feed.get("count")
    next_offset = feed.get("next")
    end = feed.get("end")
    raw_items = feed.get("items")
    if (
        feed.get("mode") != server_mode
        or feed.get("value") != query
        or feed.get("offset") != offset
        or isinstance(count, bool)
        or not isinstance(count, int)
        or not 1 <= count <= limit
        or not isinstance(end, bool)
        or feed.get("cache") not in YTDLP_SERVER_CACHE_STATES
        or not isinstance(raw_items, list)
        or len(raw_items) != count
        or (
            next_offset is not None
            and (
                isinstance(next_offset, bool)
                or not isinstance(next_offset, int)
                or next_offset != offset + count
                or not 0 < next_offset <= MAX_ITEMS
            )
        )
        or end != (next_offset is None)
    ):
        raise FeedError("yt-dlp server feed shape rejected")
    synthetic = {
        "entries": [
            {
                "id": item.get("id"),
                "title": item.get("title"),
                "channel": item.get("channel"),
                "published": item.get("published"),
                "timestamp": item.get("published_epoch"),
                "duration": item.get("duration"),
            }
            for item in raw_items
            if isinstance(item, dict)
        ]
    }
    items = parse_items(
        synthetic, limit=limit, minimum=MIN_ITEMS,
        newest_first=server_mode == "channel"
    )
    if len(items) != len(raw_items):
        raise FeedError("yt-dlp server entries rejected")
    return items, not end


def run_ytdlp_server(
    mode: str, query: str, offset: int, limit: int, timeout_seconds: int,
    priority: str
) -> Tuple[List[Dict[str, object]], bool]:
    if os.environ.get("RG_YOUTUBE_FEED_DISABLE_SERVER") == "1":
        raise FeedError("yt-dlp server disabled")
    server_mode = "channel" if mode == "channel" else "search"
    if server_mode == "channel" and CHANNEL_ID_RE.fullmatch(query) is None:
        raise FeedError("channel identity rejected")
    if (
        not 0 <= offset < MAX_ITEMS
        or not 1 <= limit <= MAX_ITEMS
        or priority not in {"interactive", "background"}
    ):
        raise FeedError("yt-dlp server request rejected")
    default_socket = "/run/rg40xxv-youtube/yt-dlp.sock"
    socket_path = Path(
        os.environ.get("RG_YOUTUBE_YTDLP_SOCKET", default_socket)
    )
    require_private_socket(socket_path)
    request = {
        "version": 1,
        "op": "feed",
        "mode": server_mode,
        "value": query,
        "offset": offset,
        "limit": limit,
        "priority": priority,
    }
    encoded = (
        json.dumps(request, ensure_ascii=False, separators=(",", ":")) + "\n"
    ).encode("utf-8")
    if len(encoded) > MAX_YTDLP_SERVER_REQUEST:
        raise FeedError("yt-dlp server request size rejected")
    client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    client.set_inheritable(False)
    client.settimeout(timeout_seconds + 2)
    try:
        client.connect(str(socket_path))
        if not hasattr(socket, "SO_PEERCRED"):
            raise FeedError("yt-dlp server credentials unavailable")
        pid, uid, _gid = struct.unpack(
            "3i", client.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12)
        )
        if pid <= 1 or uid != os.geteuid():
            raise FeedError("yt-dlp server credentials rejected")
        client.sendall(encoded)
        # A newline terminates the request.  Do not half-close the write side:
        # the persistent server peeks this socket while a queued feed waits and
        # EOF from SHUT_WR is indistinguishable from a client that went away.
        # That race canceled the job and closed without a response, which the
        # native UI reported as an offline/truncated feed.
        response = bytearray()
        while b"\n" not in response:
            block = client.recv(
                min(65536, MAX_YTDLP_SERVER_RESPONSE + 1 - len(response))
            )
            if not block:
                raise FeedError("yt-dlp server response truncated")
            response.extend(block)
            if len(response) > MAX_YTDLP_SERVER_RESPONSE:
                raise FeedError("yt-dlp server response size rejected")
        payload, trailing = bytes(response).split(b"\n", 1)
        if trailing:
            raise FeedError("yt-dlp server response framing rejected")
    except FeedError:
        raise
    except (OSError, ValueError, struct.error) as error:
        raise FeedError("yt-dlp server unavailable") from error
    finally:
        client.close()
    try:
        document = json.loads(payload.decode("utf-8", "strict"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise FeedError("yt-dlp server JSON rejected") from error
    return parse_ytdlp_server_feed(document, mode, query, offset, limit)


def _run_ytdlp_cli(
    tool: Path, mode: str, query: str, offset: int, timeout_seconds: int,
    *, channel_aggregate: bool = False
) -> List[Dict[str, object]]:
    global _active_ytdlp
    require_tool(tool)
    if channel_aggregate and mode != "channel":
        raise FeedError("aggregate mode rejected")
    if mode == "channel":
        if CHANNEL_ID_RE.fullmatch(query) is None:
            raise FeedError("channel identity rejected")
        target = f"https://www.youtube.com/channel/{query}/videos"
    else:
        target = f"ytsearch{MAX_ITEMS}:{query}"
    playlist_start = 1 if channel_aggregate else offset + 1
    playlist_end = MAX_ITEMS + 1 if channel_aggregate else offset + PAGE_FETCH_ITEMS
    parse_limit = MAX_ITEMS + 1 if channel_aggregate else PAGE_FETCH_ITEMS
    command = [
        str(tool),
        "--no-config",
        "--flat-playlist",
        "--dump-single-json",
        "--skip-download",
        "--no-warnings",
        "--ignore-errors",
        "--playlist-end",
        str(playlist_end),
        "--playlist-start",
        str(playlist_start),
        "--socket-timeout",
        "8",
        "--retries",
        "1",
        "--extractor-retries",
        "1",
        target,
    ]
    with tempfile.TemporaryFile(mode="w+b") as output:
        child = subprocess.Popen(
            command,
            stdin=subprocess.DEVNULL,
            stdout=output,
            stderr=subprocess.DEVNULL,
            start_new_session=True,
        )
        _active_ytdlp = child
        try:
            try:
                child.wait(timeout=timeout_seconds)
            except subprocess.TimeoutExpired as error:
                terminate_ytdlp(child)
                try:
                    child.wait(timeout=1.0)
                except subprocess.TimeoutExpired:
                    terminate_ytdlp(child, force=True)
                    child.wait()
                raise FeedError("yt-dlp timed out") from error
        finally:
            _active_ytdlp = None
            if child.poll() is None:
                terminate_ytdlp(child)
                try:
                    child.wait(timeout=1.0)
                except subprocess.TimeoutExpired:
                    terminate_ytdlp(child, force=True)
                    child.wait()
        if child.returncode != 0:
            raise FeedError("yt-dlp failed")
        size = output.tell()
        if size < 2 or size > MAX_YTDLP_OUTPUT:
            raise FeedError("yt-dlp output size rejected")
        output.seek(0)
        payload = output.read(MAX_YTDLP_OUTPUT + 1)
    try:
        document = json.loads(payload.decode("utf-8", "strict"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise FeedError("yt-dlp JSON rejected") from error
    parsed = parse_items(
        document, limit=parse_limit,
        minimum=MIN_ITEMS, newest_first=mode == "channel"
    )
    page = parsed[:parse_limit]
    if not page:
        raise FeedError("feed page empty")
    return page


def run_ytdlp(
    tool: Path, mode: str, query: str, offset: int, timeout_seconds: int,
    *, channel_aggregate: bool = False
) -> List[Dict[str, object]]:
    limit = (
        MAX_ITEMS
        if channel_aggregate
        else min(PAGE_FETCH_ITEMS, MAX_ITEMS - offset)
    )
    server_offset = 0 if channel_aggregate else offset
    try:
        items, _has_more = run_ytdlp_server(
            mode, query, server_offset, limit, timeout_seconds, "interactive"
        )
        return items
    except FeedError:
        if os.environ.get("RG_YOUTUBE_FEED_ALLOW_CLI_FALLBACK") != "1":
            raise
        # Compatibility for recovery images that do not yet run the
        # persistent service. Recovery/tests must opt in explicitly, so the
        # normal p7 path can never regress to per-page extractor startup.
        return _run_ytdlp_cli(
            tool, mode, query, offset, timeout_seconds,
            channel_aggregate=channel_aggregate
        )


def valid_jpeg(payload: bytes) -> bool:
    return (
        128 <= len(payload) <= MAX_THUMBNAIL_BYTES
        and payload.startswith(b"\xff\xd8\xff")
        and payload.endswith(b"\xff\xd9")
    )


def read_thumbnail(path: Path) -> bool:
    flags = os.O_RDONLY | os.O_CLOEXEC
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    try:
        descriptor = os.open(path, flags)
    except FileNotFoundError:
        return False
    except OSError:
        return False
    try:
        info = os.fstat(descriptor)
        if (
            not stat.S_ISREG(info.st_mode)
            or info.st_uid != os.geteuid()
            or info.st_nlink != 1
            or stat.S_IMODE(info.st_mode) != 0o600
            or not 128 <= info.st_size <= MAX_THUMBNAIL_BYTES
        ):
            return False
        payload = os.read(descriptor, info.st_size + 1)
        return len(payload) == info.st_size and valid_jpeg(payload)
    finally:
        os.close(descriptor)


def download_thumbnail(video_id: str, path: Path, timeout_seconds: int) -> bool:
    if read_thumbnail(path):
        return True
    url = f"https://i.ytimg.com/vi/{video_id}/mqdefault.jpg"
    request = urllib.request.Request(
        url,
        headers={
            "User-Agent": "Mozilla/5.0 (X11; Linux aarch64) AppleWebKit/537.36 Chrome/124 Safari/537.36",
            "Accept": "image/jpeg",
            "Connection": "close",
        },
    )
    try:
        with urllib.request.urlopen(request, timeout=min(timeout_seconds, 6)) as response:
            final = urllib.parse.urlsplit(response.geturl())
            host = (final.hostname or "").lower().rstrip(".")
            content_type = response.headers.get_content_type()
            length = response.headers.get("Content-Length")
            if (
                final.scheme != "https"
                or not (host == "ytimg.com" or host.endswith(".ytimg.com"))
                or content_type not in {"image/jpeg", "image/jpg"}
                or (length is not None and (not length.isdigit() or int(length) > MAX_THUMBNAIL_BYTES))
            ):
                return False
            payload = response.read(MAX_THUMBNAIL_BYTES + 1)
    except (OSError, ValueError):
        return False
    if not valid_jpeg(payload):
        return False
    descriptor, temporary = tempfile.mkstemp(prefix=".thumb.", dir=path.parent)
    try:
        os.fchmod(descriptor, 0o600)
        view = memoryview(payload)
        while view:
            written = os.write(descriptor, view)
            view = view[written:]
        os.fsync(descriptor)
        os.close(descriptor)
        descriptor = -1
        os.replace(temporary, path)
        return True
    finally:
        if descriptor >= 0:
            os.close(descriptor)
        try:
            os.unlink(temporary)
        except OSError:
            pass


def prune_thumbnail_cache(thumbnail_root: Path, wanted: set[str]) -> None:
    candidates: List[Tuple[float, Path]] = []
    now = time.time()
    for path in thumbnail_root.iterdir():
        if re.fullmatch(r"[A-Za-z0-9_-]{11}\.jpg", path.name) is None:
            continue
        try:
            info = path.lstat()
        except OSError:
            continue
        if not stat.S_ISREG(info.st_mode) or stat.S_ISLNK(info.st_mode):
            continue
        video_id = path.name[:-4]
        if video_id not in wanted and now - info.st_mtime > 24 * 60 * 60:
            try:
                path.unlink()
            except OSError:
                pass
            continue
        if (
            info.st_uid == os.geteuid()
            and info.st_nlink == 1
            and stat.S_IMODE(info.st_mode) == 0o600
        ):
            candidates.append((info.st_mtime, path))
    excess = max(0, len(candidates) - MAX_THUMBNAIL_CACHE_ENTRIES)
    removable = sorted(
        (item for item in candidates if item[1].name[:-4] not in wanted),
        key=lambda item: item[0],
    )
    for _mtime, path in removable[:excess]:
        try:
            path.unlink()
        except OSError:
            pass


def stream_thumbnails(
    cache_root: Path, items: List[Dict[str, object]], timeout_seconds: int
) -> int:
    thumbnail_root = cache_root / "thumbnails"
    require_private_dir(thumbnail_root)
    wanted = {str(item["id"]) for item in items}
    prune_thumbnail_cache(thumbnail_root, wanted)
    jobs: List[Tuple[Dict[str, object], Path]] = [
        (item, thumbnail_root / f"{item['id']}.jpg") for item in items
    ]
    ready_count = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=4, thread_name_prefix="youtube-thumb") as executor:
        futures = {
            executor.submit(download_thumbnail, str(item["id"]), path, timeout_seconds): (str(item["id"]), path)
            for item, path in jobs
        }
        for future in concurrent.futures.as_completed(futures):
            video_id, path = futures[future]
            try:
                ready = future.result()
            except Exception:  # No worker failure is allowed to corrupt the stdout protocol.
                ready = False
            if ready:
                print(f"THUMB\t{video_id}\t{path}", flush=True)
                ready_count += 1
    prune_thumbnail_cache(thumbnail_root, wanted)
    print(f"THUMBS\t{ready_count}", flush=True)
    return ready_count


def cache_key(mode: str, query: str, offset: int) -> str:
    return hashlib.sha256(
        f"{SCHEMA}\0{mode}\0{query}\0{offset}".encode("utf-8")
    ).hexdigest()


def channel_aggregate_key(channel_id: str) -> str:
    return hashlib.sha256(
        f"{SCHEMA}\0channel-aggregate\0{channel_id}".encode("utf-8")
    ).hexdigest()


def channel_aggregate_root(cache_root: Path) -> Path:
    return cache_root / CHANNEL_AGGREGATE_DIRECTORY


def channel_aggregate_path(cache_root: Path, channel_id: str) -> Path:
    return channel_aggregate_root(cache_root) / f"{channel_aggregate_key(channel_id)}.json"


def prune_channel_aggregate_cache(root: Path, protected_key: str) -> None:
    candidates: List[Tuple[float, Path]] = []
    for path in root.iterdir():
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
    protected_name = f"{protected_key}.json"
    removable = sorted(
        (item for item in candidates if item[1].name != protected_name),
        key=lambda item: item[0],
    )
    excess = max(0, len(candidates) - MAX_CHANNEL_AGGREGATE_CACHE_ENTRIES)
    for _mtime, path in removable[:excess]:
        try:
            path.unlink()
        except OSError:
            pass


def validate_cached(
    value: object, mode: str, query: str, offset: int, now: int, stale_max: int
) -> Tuple[List[Dict[str, object]], str, bool]:
    if not isinstance(value, dict):
        raise FeedError("cache shape rejected")
    if (
        value.get("schema") != SCHEMA or value.get("mode") != mode
        or value.get("query") != query or value.get("offset") != offset
        or not isinstance(value.get("has_more"), bool)
    ):
        raise FeedError("cache identity rejected")
    expires_at = value.get("expires_at")
    generated_at = value.get("generated_at")
    if (
        isinstance(expires_at, bool)
        or not isinstance(expires_at, int)
        or isinstance(generated_at, bool)
        or not isinstance(generated_at, int)
        or generated_at > now + 30
        or expires_at <= generated_at
        or expires_at > generated_at + 600
    ):
        raise FeedError("cache timestamps rejected")
    if expires_at > now:
        cache_state = "HIT"
    elif now - expires_at <= stale_max:
        cache_state = "STALE"
    else:
        raise FeedError("cache outside stale window")
    items = value.get("items")
    if not isinstance(items, list) or not MIN_ITEMS <= len(items) <= PAGE_ITEMS:
        raise FeedError("cache item count rejected")
    # Re-validate every cached item and regenerate all URLs from the trusted ID.
    synthetic = {
        "entries": [
            {
                "id": item.get("id"),
                "title": item.get("title"),
                "channel": item.get("channel"),
                "published": item.get("published"),
                "timestamp": item.get("published_epoch"),
                "duration": item.get("duration"),
            }
            for item in items
            if isinstance(item, dict)
        ]
    }
    return (
        parse_items(
            synthetic, limit=PAGE_ITEMS, minimum=MIN_ITEMS,
            newest_first=mode == "channel"
        ),
        cache_state,
        bool(value["has_more"]),
    )


def read_cache(
    path: Path, mode: str, query: str, offset: int, now: int, stale_max: int
) -> Tuple[List[Dict[str, object]], str, bool] | None:
    flags = os.O_RDONLY | os.O_CLOEXEC
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    try:
        descriptor = os.open(path, flags)
    except FileNotFoundError:
        return None
    except OSError as error:
        raise FeedError("cache read failed") from error
    try:
        info = os.fstat(descriptor)
        if (
            not stat.S_ISREG(info.st_mode)
            or info.st_uid != os.geteuid()
            or info.st_nlink != 1
            or stat.S_IMODE(info.st_mode) != 0o600
            or info.st_size < 2
            or info.st_size > 256 * 1024
        ):
            raise FeedError("unsafe cache entry")
        payload = os.read(descriptor, info.st_size + 1)
        if len(payload) != info.st_size:
            raise FeedError("cache changed while reading")
    finally:
        os.close(descriptor)
    try:
        return validate_cached(
            json.loads(payload.decode("utf-8", "strict")),
            mode,
            query,
            offset,
            now,
            stale_max,
        )
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise FeedError("cache JSON rejected") from error


def validate_channel_aggregate(
    value: object, channel_id: str, now: int, stale_max: int
) -> Tuple[List[Dict[str, object]], str, bool, bool]:
    if not isinstance(value, dict):
        raise FeedError("aggregate cache shape rejected")
    if (
        value.get("schema") != SCHEMA
        or value.get("kind") != "channel-aggregate"
        or value.get("channel_id") != channel_id
        or not isinstance(value.get("complete"), bool)
        or not isinstance(value.get("has_more"), bool)
    ):
        raise FeedError("aggregate cache identity rejected")
    expires_at = value.get("expires_at")
    generated_at = value.get("generated_at")
    if (
        isinstance(expires_at, bool)
        or not isinstance(expires_at, int)
        or isinstance(generated_at, bool)
        or not isinstance(generated_at, int)
        or generated_at > now + 30
        or expires_at <= generated_at
        or expires_at > generated_at + 600
    ):
        raise FeedError("aggregate cache timestamps rejected")
    if expires_at > now:
        cache_state = "HIT"
    elif now - expires_at <= stale_max:
        cache_state = "STALE"
    else:
        raise FeedError("aggregate cache outside stale window")
    raw_items = value.get("items")
    if not isinstance(raw_items, list) or not MIN_ITEMS <= len(raw_items) <= MAX_ITEMS:
        raise FeedError("aggregate cache item count rejected")
    synthetic = {
        "entries": [
            {
                "id": item.get("id"),
                "title": item.get("title"),
                "channel": item.get("channel"),
                "published": item.get("published"),
                "timestamp": item.get("published_epoch"),
                "duration": item.get("duration"),
            }
            for item in raw_items
            if isinstance(item, dict)
        ]
    }
    items = parse_items(
        synthetic, limit=MAX_ITEMS, minimum=MIN_ITEMS, newest_first=True
    )
    if len(items) != len(raw_items):
        raise FeedError("aggregate cache entries rejected")
    return (
        items,
        cache_state,
        bool(value["complete"]),
        bool(value["has_more"]),
    )


def read_channel_aggregate(
    cache_root: Path, channel_id: str, now: int, stale_max: int
) -> Tuple[List[Dict[str, object]], str, bool, bool] | None:
    path = channel_aggregate_path(cache_root, channel_id)
    flags = os.O_RDONLY | os.O_CLOEXEC
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    try:
        descriptor = os.open(path, flags)
    except FileNotFoundError:
        return None
    except OSError as error:
        raise FeedError("aggregate cache read failed") from error
    try:
        info = os.fstat(descriptor)
        if (
            not stat.S_ISREG(info.st_mode)
            or info.st_uid != os.geteuid()
            or info.st_nlink != 1
            or stat.S_IMODE(info.st_mode) != 0o600
            or info.st_size < 2
            or info.st_size > MAX_ATOM_FEED_BYTES
        ):
            raise FeedError("unsafe aggregate cache entry")
        payload = os.read(descriptor, info.st_size + 1)
        if len(payload) != info.st_size:
            raise FeedError("aggregate cache changed while reading")
    finally:
        os.close(descriptor)
    try:
        return validate_channel_aggregate(
            json.loads(payload.decode("utf-8", "strict")),
            channel_id,
            now,
            stale_max,
        )
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise FeedError("aggregate cache JSON rejected") from error


def page_from_channel_aggregate(
    items: List[Dict[str, object]], offset: int, complete: bool,
    aggregate_has_more: bool
) -> Tuple[List[Dict[str, object]], bool] | None:
    if offset >= len(items):
        return None
    page = items[offset : offset + PAGE_ITEMS]
    # A partial fast aggregate is deliberately not authoritative for a short
    # final page.  Requesting that boundary triggers one complete bounded
    # channel extraction instead of falsely ending the feed.
    if not complete and len(page) < PAGE_ITEMS:
        return None
    has_more = (
        offset + len(page) < len(items)
        or (
            len(page) == PAGE_ITEMS
            and offset + PAGE_ITEMS < MAX_ITEMS
            and (not complete or aggregate_has_more)
        )
    )
    return page, has_more


def write_cache(
    path: Path, mode: str, query: str, offset: int,
    items: List[Dict[str, object]], has_more: bool, now: int, ttl: int
) -> None:
    value = {
        "schema": SCHEMA,
        "mode": mode,
        "query": query,
        "offset": offset,
        "has_more": has_more,
        "generated_at": now,
        "expires_at": now + ttl,
        "items": items,
    }
    payload = (json.dumps(value, ensure_ascii=False, separators=(",", ":")) + "\n").encode("utf-8")
    descriptor, temporary = tempfile.mkstemp(prefix=".feed.", dir=path.parent)
    try:
        os.fchmod(descriptor, 0o600)
        view = memoryview(payload)
        while view:
            written = os.write(descriptor, view)
            view = view[written:]
        os.fsync(descriptor)
        os.close(descriptor)
        descriptor = -1
        os.replace(temporary, path)
        prune_feed_cache(path.parent, path.stem)
    finally:
        if descriptor >= 0:
            os.close(descriptor)
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass


def write_channel_aggregate(
    cache_root: Path, channel_id: str, items: List[Dict[str, object]],
    complete: bool, has_more: bool, now: int, ttl: int
) -> None:
    root = channel_aggregate_root(cache_root)
    require_private_dir(root)
    key = channel_aggregate_key(channel_id)
    path = root / f"{key}.json"
    value = {
        "schema": SCHEMA,
        "kind": "channel-aggregate",
        "channel_id": channel_id,
        "complete": complete,
        "has_more": has_more,
        "generated_at": now,
        "expires_at": now + ttl,
        "items": items[:MAX_ITEMS],
    }
    payload = (
        json.dumps(value, ensure_ascii=False, separators=(",", ":")) + "\n"
    ).encode("utf-8")
    if len(payload) > MAX_ATOM_FEED_BYTES:
        raise FeedError("aggregate cache output rejected")
    descriptor, temporary = tempfile.mkstemp(prefix=".aggregate.", dir=root)
    try:
        os.fchmod(descriptor, 0o600)
        view = memoryview(payload)
        while view:
            written = os.write(descriptor, view)
            view = view[written:]
        os.fsync(descriptor)
        os.close(descriptor)
        descriptor = -1
        os.replace(temporary, path)
        prune_channel_aggregate_cache(root, key)
    finally:
        if descriptor >= 0:
            os.close(descriptor)
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass


def publish_channel_pages(
    cache_root: Path, channel_id: str, items: List[Dict[str, object]],
    complete: bool, aggregate_has_more: bool, now: int, ttl: int
) -> None:
    # The aggregate is the commit record for one coherent channel generation.
    # Keep the previous aggregate authoritative while atomically replacing the
    # derived pages; publish the new aggregate only after the complete fan-out.
    # A crash can therefore leave old-consistent data or new-consistent data,
    # never a new aggregate paired with an older page.
    for offset in range(0, min(len(items), MAX_ITEMS), PAGE_ITEMS):
        page = page_from_channel_aggregate(
            items, offset, complete, aggregate_has_more
        )
        if page is None:
            break
        page_items, page_has_more = page
        key = cache_key("channel", channel_id, offset)
        write_cache(
            cache_root / f"{key}.json", "channel", channel_id, offset,
            page_items, page_has_more, now, ttl
        )
    write_channel_aggregate(
        cache_root, channel_id, items, complete, aggregate_has_more, now, ttl
    )


def emit_metadata(
    items: List[Dict[str, object]], query: str, cache_state: str,
    offset: int, has_more: bool
) -> None:
    emitted = 0
    for batch_offset in range(0, len(items), PAGE_ITEMS):
        batch = items[batch_offset : batch_offset + PAGE_ITEMS]
        for item in batch:
            print(
                "\t".join(
                    [
                        "ITEM",
                        str(item["id"]),
                        str(item["title"]),
                        str(item["channel"]),
                        str(item["published"]),
                        str(item["duration"]),
                        str(item["thumbnail"]),
                        str(item["watch_url"]),
                        "",
                    ]
                )
            )
        emitted += len(batch)
        more = "YES" if emitted < len(items) else "NO"
        print(f"BATCH\t{len(batch)}\t{emitted}\tmore={more}")
    next_value = str(offset + len(items)) if has_more else "END"
    print(
        f"DONE\t{len(items)}\t{query}\tcache={cache_state}"
        f"\tnext={next_value}"
    )
    sys.stdout.flush()


def fetch_items(
    tool: Path, mode: str, query: str, offset: int, timeout_seconds: int
) -> Tuple[List[Dict[str, object]], bool]:
    try:
        if os.environ.get("RG_YOUTUBE_FEED_DISABLE_HTTP") == "1":
            raise FeedError("raw search disabled")
        raw_items = run_raw_feed(mode, query, timeout_seconds)
        page = raw_items[offset : offset + PAGE_FETCH_ITEMS]
        if not page:
            raise FeedError("raw page unavailable")
    except FeedError:
        page = run_ytdlp(tool, mode, query, offset, timeout_seconds)
    has_more = len(page) > PAGE_ITEMS and offset + PAGE_ITEMS < MAX_ITEMS
    return page[:PAGE_ITEMS], has_more


def read_effective_cache(
    cache_root: Path, path: Path, mode: str, query: str, offset: int,
    now: int, stale_max: int
) -> Tuple[List[Dict[str, object]], str, bool] | None:
    try:
        page_cache = read_cache(path, mode, query, offset, now, stale_max)
    except FeedError:
        page_cache = None
    if mode != "channel":
        return page_cache
    try:
        aggregate = read_channel_aggregate(cache_root, query, now, stale_max)
    except FeedError:
        aggregate = None
    if aggregate is None:
        return page_cache
    aggregate_items, aggregate_state, complete, aggregate_has_more = aggregate
    aggregate_page = page_from_channel_aggregate(
        aggregate_items, offset, complete, aggregate_has_more
    )
    if aggregate_page is None:
        return page_cache
    items, has_more = aggregate_page
    # For channels the aggregate is the generation commit record.  Derived
    # page files are only a legacy/failure fallback when no usable aggregate
    # exists; never let a newer/older page mask a committed aggregate.
    return items, aggregate_state, has_more


def safe_read_channel_aggregate(
    cache_root: Path, channel_id: str, now: int, stale_max: int
) -> Tuple[List[Dict[str, object]], str, bool, bool] | None:
    try:
        return read_channel_aggregate(
            cache_root, channel_id, now, stale_max
        )
    except FeedError:
        return None


def apply_exact_published(
    items: List[Dict[str, object]], exact_items: List[Dict[str, object]]
) -> List[Dict[str, object]]:
    exact = {
        str(item["id"]): item
        for item in exact_items
        if (
            isinstance(item.get("published"), str)
            and re.fullmatch(r"\d{4}-\d{2}-\d{2}", str(item["published"]))
            and isinstance(item.get("published_epoch"), int)
            and int(item["published_epoch"]) > 0
        )
    }
    result: List[Dict[str, object]] = []
    for source in items:
        item = dict(source)
        dated = exact.get(str(item["id"]))
        if dated is not None:
            item["published"] = dated["published"]
            item["published_epoch"] = dated["published_epoch"]
        result.append(item)
    return result


def fetch_complete_channel(
    tool: Path, channel_id: str, timeout_seconds: int,
    exact_items: List[Dict[str, object]], *, priority: str = "interactive"
) -> Tuple[List[Dict[str, object]], bool]:
    try:
        fetched, aggregate_has_more = run_ytdlp_server(
            "channel", channel_id, 0, MAX_ITEMS, timeout_seconds, priority
        )
    except FeedError:
        if os.environ.get("RG_YOUTUBE_FEED_ALLOW_CLI_FALLBACK") != "1":
            raise
        fetched_with_sentinel = _run_ytdlp_cli(
            tool, "channel", channel_id, 0, timeout_seconds,
            channel_aggregate=True
        )
        aggregate_has_more = len(fetched_with_sentinel) > MAX_ITEMS
        fetched = fetched_with_sentinel[:MAX_ITEMS]
    return (
        apply_exact_published(fetched[:MAX_ITEMS], exact_items),
        aggregate_has_more,
    )


def refresh_channel_page(
    tool: Path, cache_root: Path, channel_id: str, offset: int,
    timeout_seconds: int, now: int, ttl: int, stale_max: int
) -> Tuple[List[Dict[str, object]], bool]:
    if offset == 0:
        try:
            channel_items = run_channel_fast(channel_id, timeout_seconds)
        except FeedError:
            channel_items = run_ytdlp(
                tool, "channel", channel_id, 0, timeout_seconds
            )
        if not channel_items:
            raise FeedError("channel first page empty")
        page_items = channel_items[:PAGE_ITEMS]
        page_has_more = (
            len(channel_items) > PAGE_ITEMS
            or (len(page_items) == PAGE_ITEMS and PAGE_ITEMS < MAX_ITEMS)
        )
        existing = safe_read_channel_aggregate(
            cache_root, channel_id, now, stale_max
        )
        if existing is None:
            aggregate_items = channel_items[:MAX_ITEMS]
            aggregate_complete = False
            aggregate_has_more = len(channel_items) >= PAGE_ITEMS
        else:
            seen = {str(item["id"]) for item in page_items}
            aggregate_items = page_items + [
                item for item in existing[0]
                if str(item["id"]) not in seen
            ]
            aggregate_items = aggregate_items[:MAX_ITEMS]
            aggregate_complete = existing[2]
            aggregate_has_more = existing[3] or page_has_more
        page_has_more = page_has_more or len(aggregate_items) > PAGE_ITEMS
        publish_channel_pages(
            cache_root, channel_id, aggregate_items, aggregate_complete,
            aggregate_has_more, now, ttl
        )
        return page_items, page_has_more

    existing = safe_read_channel_aggregate(
        cache_root, channel_id, now, stale_max
    )
    exact_items = existing[0] if existing is not None else []
    aggregate_items, aggregate_has_more = fetch_complete_channel(
        tool, channel_id, timeout_seconds, exact_items
    )
    publish_channel_pages(
        cache_root, channel_id, aggregate_items, True,
        aggregate_has_more, now, ttl
    )
    page = page_from_channel_aggregate(
        aggregate_items, offset, True, aggregate_has_more
    )
    if page is None:
        raise FeedError("channel aggregate page empty")
    return page


def refresh_page_cache(
    tool: Path, cache_root: Path, path: Path, mode: str, query: str,
    offset: int, timeout_seconds: int, now: int, ttl: int, stale_max: int
) -> Tuple[List[Dict[str, object]], bool]:
    if mode == "channel":
        return refresh_channel_page(
            tool, cache_root, query, offset, timeout_seconds,
            now, ttl, stale_max
        )
    items, has_more = fetch_items(
        tool, mode, query, offset, timeout_seconds
    )
    write_cache(path, mode, query, offset, items, has_more, now, ttl)
    return items, has_more


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="bounded native YouTube metadata feed")
    parser.add_argument("mode", choices=("home", "search", "channel", "prewarm"))
    parser.add_argument("query", nargs="?")
    parser.add_argument("offset", nargs="?")
    parser.add_argument("channels", nargs="*")
    args = parser.parse_args(argv)
    if args.mode == "home":
        if args.query is not None or args.offset is not None or args.channels:
            parser.error("home takes no query")
        args.query = HOME_QUERY
        args.offset = "0"
    elif args.mode == "prewarm":
        values = [
            value for value in (args.query, args.offset, *args.channels) if value
        ]
        if not values or any(
            CHANNEL_ID_RE.fullmatch(value) is None for value in values
        ):
            parser.error("prewarm requires exact UC channel IDs")
        args.channels = values
        args.query = ""
        args.offset = 0
    elif args.mode == "search":
        args.query = clean_text(args.query, 80, 192)
        if not args.query:
            parser.error("search requires a non-empty query")
    else:
        if args.query is None or CHANNEL_ID_RE.fullmatch(args.query) is None:
            parser.error("channel requires one exact UC channel ID")
    if args.mode != "prewarm":
        if args.offset is None:
            args.offset = 0
        else:
            try:
                args.offset = int(args.offset, 10)
            except ValueError:
                parser.error("offset must be a decimal page boundary")
        if not 0 <= args.offset < MAX_ITEMS or args.offset % PAGE_ITEMS != 0:
            parser.error("offset must be a bounded page boundary")
    return args


def bounded_env_int(name: str, default: int, minimum: int, maximum: int) -> int:
    raw = os.environ.get(name, str(default))
    try:
        value = int(raw, 10)
    except ValueError as error:
        raise FeedError(f"invalid {name}") from error
    if not minimum <= value <= maximum:
        raise FeedError(f"invalid {name}")
    return value


def wait_for_ytdlp_server(timeout_seconds: float = 3.0) -> bool:
    """Wait only in the background prewarmer; the SDL UI stays responsive."""
    socket_path = Path(
        os.environ.get(
            "RG_YOUTUBE_YTDLP_SOCKET",
            "/run/rg40xxv-youtube/yt-dlp.sock",
        )
    )
    deadline = time.monotonic() + timeout_seconds
    while not _active_ytdlp and time.monotonic() < deadline:
        try:
            # yt_dlp_server binds only after both persistent workers have
            # imported and initialized, so the validated socket inode itself
            # is the readiness signal.  Do not consume a client connection
            # merely to probe it.
            require_private_socket(socket_path)
            return True
        except FeedError:
            time.sleep(0.05)
    return False


def prewarm_channels(
    channels: Sequence[str], tool: Path, cache_root: Path,
    timeout_seconds: int, ttl: int, stale_max: int
) -> int:
    try:
        os.nice(15)
    except OSError:
        pass
    # The UI and daemon intentionally start in parallel.  On the H700 the
    # one-time yt-dlp import can take over a second, so a prewarmer that races
    # the socket would otherwise finish with warmed=0 and never fill the
    # channel snapshots.  Waiting here does not delay or block SDL rendering.
    if (
        os.environ.get("RG_YOUTUBE_FEED_ALLOW_CLI_FALLBACK") != "1"
        and not wait_for_ytdlp_server()
    ):
        print(
            f"YOUTUBE_FEED_PREWARM result=FAIL warmed=0 total={len(channels)}"
            " reason=server-not-ready",
            file=sys.stderr,
            flush=True,
        )
        return 1
    warmed = 0
    for channel_id in channels:
        lock_key = channel_aggregate_key(channel_id)
        lock = open_private_lock(feed_lock_path(cache_root, lock_key))
        locked = False
        try:
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                locked = True
            except BlockingIOError:
                continue
            now = int(time.time())
            current = safe_read_channel_aggregate(
                cache_root, channel_id, now, stale_max
            )
            # A complete stale snapshot is already sufficient for instant
            # channel switching.  Do not enqueue eight expensive 97-entry
            # refreshes at application start; the channel the user actually
            # opens refreshes through the normal stale-while-revalidate path.
            if current is not None and current[2]:
                warmed += 1
                continue
            exact_items = current[0] if current is not None else []
            if os.environ.get("RG_YOUTUBE_FEED_DISABLE_HTTP") != "1":
                try:
                    exact_items = exact_items + run_atom_feed(
                        channel_id, timeout_seconds
                    )
                except FeedError:
                    pass
            try:
                items, has_more = fetch_complete_channel(
                    tool, channel_id, timeout_seconds, exact_items,
                    priority="background"
                )
                publish_channel_pages(
                    cache_root, channel_id, items, True, has_more,
                    int(time.time()), ttl
                )
                warmed += 1
            except FeedError:
                pass
        finally:
            if locked:
                fcntl.flock(lock, fcntl.LOCK_UN)
            os.close(lock)
    result = "PASS" if warmed == len(channels) else "PARTIAL"
    print(
        f"YOUTUBE_FEED_PREWARM result={result} warmed={warmed} total={len(channels)}",
        file=sys.stderr,
        flush=True,
    )
    return 0 if warmed == len(channels) else 1


def maybe_stream_thumbnails(
    cache_root: Path, items: List[Dict[str, object]], timeout_seconds: int
) -> None:
    if os.environ.get("RG_YOUTUBE_FEED_METADATA_ONLY") != "1":
        stream_thumbnails(cache_root, items, timeout_seconds)


def main(argv: Sequence[str]) -> int:
    os.umask(0o077)
    install_signal_handlers()
    args = parse_args(argv)
    project = Path(__file__).resolve().parent.parent
    tool = Path(os.environ.get("RG_YOUTUBE_YTDLP", str(project / "vendor" / "yt-dlp")))
    cache_root = Path(os.environ.get("RG_YOUTUBE_FEED_CACHE", "/run/rg40xxv-youtube-feed-cache"))
    ttl = bounded_env_int("RG_YOUTUBE_FEED_TTL", DEFAULT_TTL_SECONDS, 15, 600)
    timeout_seconds = bounded_env_int("RG_YOUTUBE_FEED_TIMEOUT", DEFAULT_TIMEOUT_SECONDS, 2, 60)
    stale_max = bounded_env_int(
        "RG_YOUTUBE_FEED_STALE_MAX",
        DEFAULT_STALE_MAX_SECONDS,
        60,
        7 * 24 * 60 * 60,
    )
    require_private_dir(cache_root)
    if args.mode in {"channel", "prewarm"}:
        require_private_dir(channel_aggregate_root(cache_root))
    if args.mode == "prewarm":
        return prewarm_channels(
            args.channels, tool, cache_root, timeout_seconds, ttl, stale_max
        )
    key = cache_key(args.mode, args.query, args.offset)
    prune_feed_cache(cache_root, key)
    lock_key = (
        channel_aggregate_key(args.query)
        if args.mode == "channel"
        else key
    )
    lock = open_private_lock(feed_lock_path(cache_root, lock_key))
    try:
        now = int(time.time())
        path = cache_root / f"{key}.json"
        try:
            cached = read_effective_cache(
                cache_root, path, args.mode, args.query, args.offset,
                now, stale_max
            )
        except FeedError:
            cached = None

        if cached is not None and cached[1] == "HIT":
            items, cache_state, has_more = cached
            emit_metadata(items, args.query, cache_state, args.offset, has_more)
            maybe_stream_thumbnails(cache_root, items, timeout_seconds)
            return 0

        if cached is not None and cached[1] == "STALE":
            items, cache_state, has_more = cached
            # Metadata contains only validated video IDs and display strings,
            # never signed media URLs.  Make it visible before any network
            # refresh, then let at most one helper refresh the atomic cache.
            emit_metadata(items, args.query, cache_state, args.offset, has_more)
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                maybe_stream_thumbnails(cache_root, items, timeout_seconds)
                return 0
            try:
                refreshed_at = int(time.time())
                try:
                    current = read_effective_cache(
                        cache_root, path, args.mode, args.query, args.offset,
                        refreshed_at, stale_max
                    )
                except FeedError:
                    current = None
                if current is None or current[1] == "STALE":
                    try:
                        refresh_page_cache(
                            tool, cache_root, path, args.mode, args.query,
                            args.offset, timeout_seconds, int(time.time()),
                            ttl, stale_max
                        )
                        print(
                            "YOUTUBE_FEED refresh=PASS source=STALE",
                            file=sys.stderr,
                            flush=True,
                        )
                    except FeedError:
                        print(
                            "YOUTUBE_FEED refresh=FAIL source=STALE retained=1",
                            file=sys.stderr,
                            flush=True,
                        )
            finally:
                fcntl.flock(lock, fcntl.LOCK_UN)
            maybe_stream_thumbnails(cache_root, items, timeout_seconds)
            return 0

        # A true miss has nothing safe to display. Serialize the network fill
        # and double-check after taking the lock so concurrent helpers issue
        # at most one request.
        fcntl.flock(lock, fcntl.LOCK_EX)
        try:
            fill_now = int(time.time())
            try:
                current = read_effective_cache(
                    cache_root, path, args.mode, args.query, args.offset,
                    fill_now, stale_max
                )
            except FeedError:
                current = None
            if current is None:
                items, has_more = refresh_page_cache(
                    tool, cache_root, path, args.mode, args.query,
                    args.offset, timeout_seconds, int(time.time()), ttl,
                    stale_max
                )
                cache_state = "MISS"
            else:
                items, cache_state, has_more = current
            emit_metadata(items, args.query, cache_state, args.offset, has_more)
        finally:
            fcntl.flock(lock, fcntl.LOCK_UN)
        maybe_stream_thumbnails(cache_root, items, timeout_seconds)
        return 0
    finally:
        os.close(lock)


if __name__ == "__main__":
    try:
        raise SystemExit(main(sys.argv[1:]))
    except FeedError as error:
        print(f"YOUTUBE_FEED result=FAIL reason={error}", file=sys.stderr)
        raise SystemExit(1)
