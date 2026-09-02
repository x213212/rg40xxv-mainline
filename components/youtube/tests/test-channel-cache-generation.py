#!/usr/bin/env python3
"""Prove channel aggregate is the atomic generation commit record."""

import importlib.util
import os
import tempfile
from pathlib import Path


PROJECT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location(
    "rg_youtube_feed", PROJECT / "tools" / "youtube_feed.py"
)
assert spec is not None and spec.loader is not None
feed = importlib.util.module_from_spec(spec)
spec.loader.exec_module(feed)


def items(prefix: str):
    return [
        {
            "id": f"{prefix}{index:08d}",
            "title": f"{prefix} title {index}",
            "channel": f"{prefix} channel",
            "published": "2026-08-31",
            "published_epoch": 1_788_112_000 + index,
            "duration": 60 + index,
            "thumbnail": f"https://i.ytimg.com/vi/{prefix}{index:08d}/mqdefault.jpg",
            "watch_url": f"https://youtu.be/{prefix}{index:08d}",
        }
        for index in range(feed.PAGE_ITEMS)
    ]


with tempfile.TemporaryDirectory(prefix="youtube-channel-generation.") as name:
    root = Path(name)
    os.chmod(root, 0o700)
    feed.require_private_dir(feed.channel_aggregate_root(root))
    channel = "UC2j5Kw9qDWCZmU_emgqeguA"
    old = items("OLD")
    new = items("NEW")
    feed.publish_channel_pages(root, channel, old, False, True, 2_000, 300)
    page_path = root / f"{feed.cache_key('channel', channel, 0)}.json"

    # Simulate a crash after the new derived page was replaced but before the
    # new aggregate commit.  Readers must stay on the old coherent aggregate.
    feed.write_cache(
        page_path, "channel", channel, 0, new, True, 2_001, 300
    )
    before_commit = feed.read_effective_cache(
        root, page_path, "channel", channel, 0, 2_002, 60
    )
    assert before_commit is not None
    assert len(before_commit[0]) == feed.PAGE_ITEMS
    assert all(str(item["id"]).startswith("OLD") for item in before_commit[0])

    feed.write_channel_aggregate(
        root, channel, new, False, True, 2_001, 300
    )
    after_commit = feed.read_effective_cache(
        root, page_path, "channel", channel, 0, 2_002, 60
    )
    assert after_commit is not None
    assert len(after_commit[0]) == feed.PAGE_ITEMS
    assert all(str(item["id"]).startswith("NEW") for item in after_commit[0])

print(
    "YOUTUBE_CHANNEL_GENERATION_TEST PASS "
    "pages=DERIVED aggregate=ATOMIC_COMMIT interrupted_fanout=OLD_CONSISTENT"
)
