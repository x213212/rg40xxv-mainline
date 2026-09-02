#!/usr/bin/env bash
set -euo pipefail
umask 077

project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
fixture=$(mktemp -d /tmp/rg40xxv-youtube-feed-test.XXXXXXXX)
trap 'jobs -pr | xargs -r kill 2>/dev/null || true; find "$fixture" \( -type f -o -type s \) -delete 2>/dev/null || true; find "$fixture" -depth -type d -empty -delete 2>/dev/null || true' EXIT
cache=$fixture/cache
mkdir -m 0700 "$cache" "$cache/thumbnails"

cat >"$fixture/fake-yt-dlp" <<'EOF'
#!/usr/bin/env python3
import json, os, pathlib, sys, time
with open(os.environ["FAKE_YTDLP_CALLS"], "a", encoding="utf-8") as output:
    output.write("\0".join(sys.argv[1:]) + "\n")
gate = os.environ.get("FAKE_YTDLP_GATE")
pid_file = os.environ.get("FAKE_YTDLP_PID_FILE")
if pid_file:
    pathlib.Path(pid_file).write_text(str(os.getpid()) + "\n", encoding="ascii")
if gate:
    pathlib.Path(gate + ".started").touch()
    while not pathlib.Path(gate + ".release").exists():
        time.sleep(0.01)
if os.environ.get("FAKE_YTDLP_FAIL") == "1":
    raise SystemExit(9)
start = 1
end = 9
if "--playlist-start" in sys.argv:
    start = int(sys.argv[sys.argv.index("--playlist-start") + 1])
if "--playlist-end" in sys.argv:
    end = int(sys.argv[sys.argv.index("--playlist-end") + 1])
entries = []
for index in range(start, end + 1):
    entries.append({
        "id": f"FeedTest{index:03d}",
        "title": f"  影片\t{index}\n台灣熱門  ",
        "channel": f"頻道\r{index}",
        "duration": 60 + index,
	    "upload_date": f"202608{32 - index:02d}",
        "url": "https://rr1---sn-test.googlevideo.com/videoplayback?sig=PRIVATE_TOKEN",
        "thumbnail": "https://evil.invalid/PRIVATE_TOKEN",
    })
entries += [entries[0], {"id": "bad/id", "title": "bad"}]
json.dump({"entries": entries}, sys.stdout, ensure_ascii=False)
EOF
chmod 0755 "$fixture/fake-yt-dlp"

export RG_YOUTUBE_YTDLP=$fixture/fake-yt-dlp
export RG_YOUTUBE_FEED_CACHE=$cache
export RG_YOUTUBE_FEED_TTL=180
export RG_YOUTUBE_FEED_TIMEOUT=5
export RG_YOUTUBE_FEED_DISABLE_HTTP=1
export RG_YOUTUBE_FEED_DISABLE_SERVER=1
export RG_YOUTUBE_FEED_ALLOW_CLI_FALLBACK=1
export FAKE_YTDLP_CALLS=$fixture/calls

python3 - "$cache/thumbnails" <<'PY'
import os, pathlib, sys
root = pathlib.Path(sys.argv[1])
payload = b"\xff\xd8\xff\xe0" + (b"\0" * 124) + b"\xff\xd9"
for index in range(1, 97):
    path = root / f"FeedTest{index:03d}.jpg"
    path.write_bytes(payload)
    os.chmod(path, 0o600)
PY

if ! "$project/tools/youtube_feed.py" home >"$fixture/home.out" 2>"$fixture/home.err"; then
	cat "$fixture/home.err" >&2
	exit 1
fi
[[ $(grep -c $'^ITEM\t' "$fixture/home.out") == 8 ]]
[[ $(grep -c $'^BATCH\t' "$fixture/home.out") == 1 ]]
[[ $(grep -c $'^THUMB\t' "$fixture/home.out") == 8 ]]
grep -Fqx $'BATCH\t8\t8\tmore=NO' "$fixture/home.out"
grep -Fqx $'DONE\t8\t台灣 熱門\tcache=MISS\tnext=8' "$fixture/home.out"
grep -Fqx $'THUMBS\t8' "$fixture/home.out"
expected_item=$'ITEM\tFeedTest001\t影片 1 台灣熱門\t頻道 1\t2026-08-31\t61\thttps://i.ytimg.com/vi/FeedTest001/mqdefault.jpg\thttps://www.youtube.com/watch?v=FeedTest001\t'
grep -Fqx "$expected_item" "$fixture/home.out"
expected_thumb=$'THUMB\tFeedTest001\t'"$cache/thumbnails/FeedTest001.jpg"
grep -Fqx "$expected_thumb" "$fixture/home.out"
done_line=$(grep -n -F $'DONE\t8\t台灣 熱門\tcache=MISS\tnext=8' "$fixture/home.out" | cut -d: -f1)
thumb_line=$(grep -n -m1 -F $'THUMB\t' "$fixture/home.out" | cut -d: -f1)
(( done_line < thumb_line ))
[[ ! -s $fixture/home.err ]]
! grep -R -Fq 'PRIVATE_TOKEN' "$fixture/home.out" "$cache"
[[ $(find "$cache" -maxdepth 1 -type f -name '*.json' -printf '%m\n' | sort -u) == 600 ]]
[[ $(stat -c %a "$cache") == 700 ]]

"$project/tools/youtube_feed.py" home >"$fixture/home-hit.out" 2>"$fixture/home-hit.err"
grep -Fqx $'DONE\t8\t台灣 熱門\tcache=HIT\tnext=8' "$fixture/home-hit.out"
[[ $(wc -l <"$fixture/calls") == 1 ]]

"$project/tools/youtube_feed.py" search $'猫\t咪\n搞笑' >"$fixture/search.out" 2>"$fixture/search.err"
grep -Fqx $'DONE\t8\t猫 咪 搞笑\tcache=MISS\tnext=8' "$fixture/search.out"
[[ $(wc -l <"$fixture/calls") == 2 ]]
grep -F -- '--flat-playlist' "$fixture/calls" >/dev/null
grep -F -- '--dump-single-json' "$fixture/calls" >/dev/null
grep -F -- 'ytsearch96:台灣 熱門' "$fixture/calls" >/dev/null
grep -F -- 'ytsearch96:猫 咪 搞笑' "$fixture/calls" >/dev/null
! grep -R -Fq 'googlevideo.com' "$fixture/home.out" "$fixture/search.out" "$cache"

# A structurally valid expired cache is emitted and flushed before exactly one
# concurrent refresher is allowed through the nonblocking lock.
home_cache=$(python3 - "$cache" <<'PY'
import json, pathlib, sys
for path in pathlib.Path(sys.argv[1]).glob("*.json"):
    value = json.loads(path.read_text(encoding="utf-8"))
    if value.get("mode") == "home":
        print(path)
        raise SystemExit(0)
raise SystemExit(1)
PY
)
python3 - "$home_cache" <<'PY'
import json, pathlib, sys, time
path = pathlib.Path(sys.argv[1])
value = json.loads(path.read_text(encoding="utf-8"))
value["generated_at"] = int(time.time()) - 181
value["expires_at"] = int(time.time()) - 1
path.write_text(json.dumps(value, ensure_ascii=False, separators=(",", ":")) + "\n", encoding="utf-8")
path.chmod(0o600)
PY

export FAKE_YTDLP_GATE=$fixture/refresh-gate
stale_start_ns=$(date +%s%N)
"$project/tools/youtube_feed.py" home >"$fixture/stale-one.out" 2>"$fixture/stale-one.err" &
stale_one_pid=$!
for _ in $(seq 1 200); do
	[ -f "$FAKE_YTDLP_GATE.started" ] && break
	sleep 0.01
done
[ -f "$FAKE_YTDLP_GATE.started" ]
for _ in $(seq 1 100); do
	grep -Fqx $'DONE\t8\t台灣 熱門\tcache=STALE\tnext=8' "$fixture/stale-one.out" 2>/dev/null && break
	sleep 0.01
done
grep -Fqx $'DONE\t8\t台灣 熱門\tcache=STALE\tnext=8' "$fixture/stale-one.out"
stale_done_ns=$(date +%s%N)
stale_done_ms=$(( (stale_done_ns - stale_start_ns) / 1000000 ))
kill -0 "$stale_one_pid"

"$project/tools/youtube_feed.py" home >"$fixture/stale-two.out" 2>"$fixture/stale-two.err" &
stale_two_pid=$!
for _ in $(seq 1 50); do
	if ! kill -0 "$stale_two_pid" 2>/dev/null; then break; fi
	sleep 0.01
done
wait "$stale_two_pid"
grep -Fqx $'DONE\t8\t台灣 熱門\tcache=STALE\tnext=8' "$fixture/stale-two.out"
[ "$(wc -l <"$fixture/calls")" -eq 3 ]
kill -0 "$stale_one_pid"
: >"$FAKE_YTDLP_GATE.release"
wait "$stale_one_pid"
grep -Fqx 'YOUTUBE_FEED refresh=PASS source=STALE' "$fixture/stale-one.err"
[ "$(wc -l <"$fixture/calls")" -eq 3 ]
unset FAKE_YTDLP_GATE

"$project/tools/youtube_feed.py" home >"$fixture/refreshed-hit.out" 2>"$fixture/refreshed-hit.err"
grep -Fqx $'DONE\t8\t台灣 熱門\tcache=HIT\tnext=8' "$fixture/refreshed-hit.out"
[ "$(wc -l <"$fixture/calls")" -eq 3 ]

# Refresh failure retains the validated stale bytes and still serves them.
python3 - "$home_cache" <<'PY'
import json, pathlib, sys, time
path = pathlib.Path(sys.argv[1])
value = json.loads(path.read_text(encoding="utf-8"))
value["generated_at"] = int(time.time()) - 181
value["expires_at"] = int(time.time()) - 1
path.write_text(json.dumps(value, ensure_ascii=False, separators=(",", ":")) + "\n", encoding="utf-8")
path.chmod(0o600)
PY
stale_before=$(sha256sum "$home_cache")
export FAKE_YTDLP_FAIL=1
"$project/tools/youtube_feed.py" home >"$fixture/stale-fail.out" 2>"$fixture/stale-fail.err"
grep -Fqx $'DONE\t8\t台灣 熱門\tcache=STALE\tnext=8' "$fixture/stale-fail.out"
grep -Fqx 'YOUTUBE_FEED refresh=FAIL source=STALE retained=1' "$fixture/stale-fail.err"
[ "$stale_before" = "$(sha256sum "$home_cache")" ]

# The default stale window spans ordinary sleep/offline periods.  A feed from
# earlier the same day is rendered immediately while a failed refresh retains
# the exact validated bytes.
python3 - "$home_cache" <<'PY'
import json, pathlib, sys, time
path = pathlib.Path(sys.argv[1])
value = json.loads(path.read_text(encoding="utf-8"))
value["generated_at"] = int(time.time()) - 12 * 60 * 60
value["expires_at"] = value["generated_at"] + 180
path.write_text(json.dumps(value, ensure_ascii=False, separators=(",", ":")) + "\n", encoding="utf-8")
path.chmod(0o600)
PY
stale_day_before=$(sha256sum "$home_cache")
"$project/tools/youtube_feed.py" home >"$fixture/stale-day.out" 2>"$fixture/stale-day.err"
grep -Fqx $'DONE\t8\t台灣 熱門\tcache=STALE\tnext=8' "$fixture/stale-day.out"
grep -Fqx 'YOUTUBE_FEED refresh=FAIL source=STALE retained=1' "$fixture/stale-day.err"
[ "$stale_day_before" = "$(sha256sum "$home_cache")" ]

# Cache older than the bounded stale window must not be rendered as stale.
python3 - "$home_cache" <<'PY'
import json, pathlib, sys, time
path = pathlib.Path(sys.argv[1])
value = json.loads(path.read_text(encoding="utf-8"))
value["generated_at"] = int(time.time()) - 241
value["expires_at"] = int(time.time()) - 61
path.write_text(json.dumps(value, ensure_ascii=False, separators=(",", ":")) + "\n", encoding="utf-8")
path.chmod(0o600)
PY
if RG_YOUTUBE_FEED_STALE_MAX=60 "$project/tools/youtube_feed.py" home \
	>"$fixture/too-old.out" 2>"$fixture/too-old.err"; then
	printf '%s\n' 'expired feed outside stale window unexpectedly succeeded' >&2
	exit 1
fi
! grep -Fq 'cache=STALE' "$fixture/too-old.out"
unset FAKE_YTDLP_FAIL

# Canceling a stale refresh must terminate and reap its separate yt-dlp
# process group; no network descendant may survive the HOME helper.
python3 - "$home_cache" <<'PY'
import json, pathlib, sys, time
path = pathlib.Path(sys.argv[1])
value = json.loads(path.read_text(encoding="utf-8"))
value["generated_at"] = int(time.time()) - 181
value["expires_at"] = int(time.time()) - 1
path.write_text(json.dumps(value, ensure_ascii=False, separators=(",", ":")) + "\n", encoding="utf-8")
path.chmod(0o600)
PY
cancel_before=$(sha256sum "$home_cache")
export FAKE_YTDLP_GATE=$fixture/cancel-gate
export FAKE_YTDLP_PID_FILE=$fixture/cancel-child.pid
"$project/tools/youtube_feed.py" home >"$fixture/cancel.out" 2>"$fixture/cancel.err" &
cancel_helper_pid=$!
for _ in $(seq 1 200); do
	[ -f "$FAKE_YTDLP_GATE.started" ] && [ -s "$FAKE_YTDLP_PID_FILE" ] && break
	sleep 0.01
done
[ -f "$FAKE_YTDLP_GATE.started" ]
grep -Fqx $'DONE\t8\t台灣 熱門\tcache=STALE\tnext=8' "$fixture/cancel.out"
cancel_child_pid=$(cat "$FAKE_YTDLP_PID_FILE")
kill -TERM "$cancel_helper_pid"
set +e
wait "$cancel_helper_pid"
cancel_status=$?
set -e
[ "$cancel_status" -eq 143 ]
for _ in $(seq 1 100); do
	if ! kill -0 "$cancel_child_pid" 2>/dev/null; then break; fi
	sleep 0.01
done
! kill -0 "$cancel_child_pid" 2>/dev/null
[ "$cancel_before" = "$(sha256sum "$home_cache")" ]
unset FAKE_YTDLP_GATE FAKE_YTDLP_PID_FILE

# Curated channels use immutable UC identities and direct official channel
# video URLs.  They must never fall back to an ambiguous name search.
for channel_id in \
	UC2j5Kw9qDWCZmU_emgqeguA UCcL163py441fTFfWy5tBjoQ \
	UCfc5rX7XNwEvY0cgXygkDbQ UCvTe3Z7TZsjGzUERx4Ce6zA \
	UC6hWBu1hfbNNk8Yeokd5aZQ UCvoBl4rnVsetDKA_Tdk-jeA \
	UC9K0rLE1SMh86nVxzkCBpNA UC0lbAQVpenvfA2QqzsRtL_g; do
	"$project/tools/youtube_feed.py" channel "$channel_id" \
		>"$fixture/channel-$channel_id.out" \
		2>"$fixture/channel-$channel_id.err"
	grep -Fqx $'DONE\t8\t'"$channel_id"$'\tcache=MISS\tnext=8' \
		"$fixture/channel-$channel_id.out"
	grep -F -- "https://www.youtube.com/channel/$channel_id/videos" \
		"$fixture/calls" >/dev/null
done
if "$project/tools/youtube_feed.py" channel '總裁聊聊' \
	>"$fixture/channel-invalid.out" 2>"$fixture/channel-invalid.err"; then
	printf '%s\n' 'channel name unexpectedly accepted instead of UC identity' >&2
	exit 1
fi
grep -Fq 'channel requires one exact UC channel ID' \
	"$fixture/channel-invalid.err"

printf '%s\n' \
	"YOUTUBE_FEED_SWR_BENCHMARK PASS stale_done_ms=$stale_done_ms refresh_calls=1 concurrent_wait=NONE failure_retains=1 cancel_descendants=0 stale_max_s=60 device_latency=PENDING"

python3 - "$project/tools/youtube_feed.py" <<'PY'
import importlib.util, pathlib, sys
spec = importlib.util.spec_from_file_location("youtube_feed", pathlib.Path(sys.argv[1]))
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
cards = []
for index in range(1, 25):
    cards.append('"videoRenderer":{"videoId":"RawSearch%02d","title":{"runs":[{"text":"原生 %d"}]},"ownerText":{"runs":[{"text":"頻道"}]},"lengthText":{"simpleText":"1:%02d"}}' % (index, index, index))
page = "{" + "},{".join([cards[0]] + cards) + "}"
items = module.parse_video_renderers(page)
assert len(items) == 24
assert items[0]["id"] == "RawSearch01"
assert items[0]["duration"] == 61
assert len({item["id"] for item in items}) == 24

public_lockup = {
    "contentId": "PublicVid01",
    "contentType": "LOCKUP_CONTENT_TYPE_VIDEO",
    "metadata": {"lockupMetadataViewModel": {
        "title": {"content": "公開影片"},
        "metadata": {"contentMetadataViewModel": {"metadataRows": [{
            "metadataParts": [
                {"text": {"content": "觀看次數：3萬次"}},
                {"text": {"content": "2 天前"}},
            ]
        }]}}
    }},
    "contentImage": {"thumbnailViewModel": {"overlays": [{
        "thumbnailBottomOverlayViewModel": {"badges": [{
            "thumbnailBadgeViewModel": {"text": "12:34"}
        }]}
    }]}},
}
member_lockup = {
    **public_lockup,
    "contentId": "MemberVid01",
    "metadata": {"lockupMetadataViewModel": {
        "title": {"content": "會員影片"},
        "metadata": {"contentMetadataViewModel": {"metadataRows": [{
            "badges": [{"badgeViewModel": {
                "badgeStyle": "BADGE_MEMBERS_ONLY",
                "badgeText": "頻道會員專屬",
            }}]
        }]}}
    }},
}
lockup_page = (
    '{"lockupViewModel":' + __import__('json').dumps(member_lockup, ensure_ascii=False) + '}'
    '{"lockupViewModel":' + __import__('json').dumps(public_lockup, ensure_ascii=False) + '}'
)
lockups = module.parse_video_renderers(lockup_page)
assert [item["id"] for item in lockups] == ["PublicVid01"]
assert lockups[0]["published"] == "2 天前"
assert lockups[0]["duration"] == 754

parsed = module.parse_items({"entries": [
    {"id": "PublicVid02", "title": "公開", "availability": "public"},
    {"id": "MemberVid02", "title": "會員", "availability": "subscriber_only"},
    {"id": "Upcoming001", "title": "即將開始", "live_status": "is_upcoming"},
]})
assert [item["id"] for item in parsed] == ["PublicVid02"]
PY

# The official Atom feed supplies exact YYYY-MM-DD metadata without any
# per-video extraction.  Its transport identity and XML work are all bounded.
python3 - "$project/tools/youtube_feed.py" \
	"$project/tests/fixtures/youtube-channel-atom.xml" <<'PY'
import email.message
import importlib.util
import pathlib
import sys

tool = pathlib.Path(sys.argv[1])
payload = pathlib.Path(sys.argv[2]).read_bytes()
spec = importlib.util.spec_from_file_location("youtube_feed_atom_test", tool)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
channel_id = "UC2j5Kw9qDWCZmU_emgqeguA"

items = module.parse_atom_feed(payload, channel_id)
assert [item["id"] for item in items] == ["AtomDate001", "AtomDate002", "AtomDate003"]
assert [item["published"] for item in items] == ["2026-08-30", "2026-08-29", "2026-08-28"]
assert all(item["published_epoch"] > 0 for item in items)
assert all(item["duration"] == 0 for item in items)

# YouTube's live endpoint may omit the leading UC only on the feed-level
# channelId.  Entry identities remain canonical and therefore authoritative.
short_root = payload.replace(
    ("<yt:channelId>" + channel_id + "</yt:channelId>").encode(),
    ("<yt:channelId>" + channel_id[2:] + "</yt:channelId>").encode(),
    1,
)
short_root_items = module.parse_atom_feed(short_root, channel_id)
assert [item["id"] for item in short_root_items] == [
    "AtomDate001", "AtomDate002", "AtomDate003"
]

raw = module.parse_items({"entries": [{
    "id": "AtomDate001", "title": "raw title", "channel": "raw channel",
    "published": "3 天前", "duration": 61,
}, {
    "id": "RawOnly0001", "title": "raw only", "channel": "raw channel",
    "published": "1 週前", "duration": 62,
}]}, newest_first=True)
merged = module.merge_channel_metadata(raw, items, include_atom_only=True)
assert merged[0]["id"] == "AtomDate001"
assert merged[0]["published"] == "2026-08-30"
assert merged[0]["duration"] == 61
assert merged[-1]["id"] == "RawOnly0001"

assert module.published_display({
    "published": "3 天前", "upload_date": "20260830"
}) == "2026-08-30"
assert module.published_display({
    "published": "3 天前", "upload_date": "20260800"
}) == "3 天前"
assert module.published_display({"upload_date": "20260231"}) == "日期未知"
assert module.published_display({}) == "日期未知"

class Response:
    def __init__(self, data, url, content_type="application/atom+xml", length=None):
        self.data = data
        self.url = url
        self.status = 200
        self.headers = email.message.Message()
        self.headers["Content-Type"] = content_type
        if length is not None:
            self.headers["Content-Length"] = str(length)
    def __enter__(self):
        return self
    def __exit__(self, *_args):
        return False
    def geturl(self):
        return self.url
    def read(self, limit):
        return self.data[:limit]

expected_url = "https://www.youtube.com/feeds/videos.xml?channel_id=" + channel_id
module.urllib.request.urlopen = lambda request, timeout: Response(
    payload, expected_url, length=len(payload)
)
downloaded = module.run_atom_feed(channel_id, 5)
assert downloaded[0]["published"] == "2026-08-30"

for response in (
    Response(payload, expected_url.replace("www.youtube.com", "evil.invalid")),
    Response(payload, expected_url, content_type="text/html"),
    Response(payload, expected_url, length=module.MAX_ATOM_FEED_BYTES + 1),
):
    module.urllib.request.urlopen = lambda request, timeout, response=response: response
    try:
        module.run_atom_feed(channel_id, 5)
    except module.FeedError:
        pass
    else:
        raise AssertionError("unsafe Atom transport unexpectedly accepted")

for rejected in (
    payload.replace(b"<feed ", b"<!DOCTYPE feed><feed ", 1),
    payload.replace(channel_id.encode(), b"UC0000000000000000000000"),
    b"<feed xmlns='http://www.w3.org/2005/Atom'>" + b"<x>" * 13 + b"</x>" * 13 + b"</feed>",
    b"x" * (module.MAX_ATOM_FEED_BYTES + 1),
):
    try:
        module.parse_atom_feed(rejected, channel_id)
    except module.FeedError:
        pass
    else:
        raise AssertionError("unsafe Atom XML unexpectedly accepted")
PY

# The production deep path talks to the owner-private persistent server once,
# persists the bounded aggregate, and fans it out into atomic eight-card pages.
cat >"$fixture/fake-feed-server.py" <<'PY'
#!/usr/bin/env python3
import json, os, pathlib, socket, sys

socket_path = pathlib.Path(sys.argv[1])
ready_path = pathlib.Path(sys.argv[2])
request_path = pathlib.Path(sys.argv[3])
server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
server.bind(str(socket_path))
os.chmod(socket_path, 0o600)
server.listen(1)
ready_path.touch()
client, _ = server.accept()
payload = bytearray()
while b"\n" not in payload:
    block = client.recv(4097 - len(payload))
    if not block:
        raise SystemExit(2)
    payload.extend(block)
request = json.loads(bytes(payload).split(b"\n", 1)[0].decode("utf-8"))
request_path.write_text(json.dumps(request, sort_keys=True) + "\n", encoding="utf-8")
# The request is newline-framed, so the production client must keep its write
# side open while waiting.  A premature SHUT_WR looks like a dead peer to the
# persistent server's queued-job liveness check.
client.setblocking(False)
try:
    pending = client.recv(1, socket.MSG_PEEK)
except BlockingIOError:
    pass
else:
    if pending == b"":
        raise SystemExit(3)
client.setblocking(True)
offset = request["offset"]
limit = request["limit"]
all_items = [{
    "id": f"SockFeed{index:03d}",
    "title": f"socket 影片 {index}",
    "channel": "socket 頻道",
    "published": "relative",
    "published_epoch": 1788134400 - index * 86400,
    "duration": 120 + index,
} for index in range(1, 97)]
items = all_items[offset:offset + limit]
next_offset = offset + len(items)
response = {
    "version": 1,
    "ok": True,
    "feed": {
        "mode": request["mode"],
        "value": request["value"],
        "offset": offset,
        "count": len(items),
        "next": next_offset,
        "end": False,
        "cache": "MISS",
        "items": items,
    },
}
client.sendall((json.dumps(response, separators=(",", ":")) + "\n").encode("utf-8"))
client.close()
server.close()
PY
chmod 0755 "$fixture/fake-feed-server.py"

socket_cache=$fixture/socket-cache
mkdir -m 0700 "$socket_cache"
socket_path=$fixture/yt-dlp.sock
socket_channel=UC2j5Kw9qDWCZmU_emgqeguA
"$fixture/fake-feed-server.py" "$socket_path" "$fixture/socket.ready" \
	"$fixture/socket.request" &
socket_server_pid=$!
for _ in $(seq 1 200); do
	[ -S "$socket_path" ] && [ -f "$fixture/socket.ready" ] && break
	sleep 0.01
done
[ -S "$socket_path" ]
socket_calls_before=$(wc -l <"$fixture/calls")
unset RG_YOUTUBE_FEED_DISABLE_SERVER RG_YOUTUBE_FEED_ALLOW_CLI_FALLBACK
RG_YOUTUBE_FEED_CACHE=$socket_cache \
RG_YOUTUBE_YTDLP_SOCKET=$socket_path \
RG_YOUTUBE_FEED_METADATA_ONLY=1 \
	"$project/tools/youtube_feed.py" prewarm "$socket_channel" \
	>"$fixture/socket-prewarm.out" 2>"$fixture/socket-prewarm.err"
wait "$socket_server_pid"
grep -Fqx 'YOUTUBE_FEED_PREWARM result=PASS warmed=1 total=1' \
	"$fixture/socket-prewarm.err"
[ "$(wc -l <"$fixture/calls")" -eq "$socket_calls_before" ]
python3 - "$fixture/socket.request" <<'PY'
import json, pathlib, sys
request = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))
assert request == {
    "version": 1, "op": "feed", "mode": "channel",
    "value": "UC2j5Kw9qDWCZmU_emgqeguA", "offset": 0,
    "limit": 96, "priority": "background",
}
PY
socket_hit_start_ns=$(date +%s%N)
RG_YOUTUBE_FEED_CACHE=$socket_cache \
RG_YOUTUBE_YTDLP_SOCKET=$socket_path \
RG_YOUTUBE_FEED_METADATA_ONLY=1 \
	"$project/tools/youtube_feed.py" channel "$socket_channel" 24 \
	>"$fixture/socket-offset24.out" 2>"$fixture/socket-offset24.err"
socket_hit_end_ns=$(date +%s%N)
socket_hit_ms=$(( (socket_hit_end_ns - socket_hit_start_ns) / 1000000 ))
grep -Fqx $'DONE\t8\t'"$socket_channel"$'\tcache=HIT\tnext=32' \
	"$fixture/socket-offset24.out"
grep -Fq $'ITEM\tSockFeed025\t' "$fixture/socket-offset24.out"
[[ $(grep -c $'^ITEM\t' "$fixture/socket-offset24.out") == 8 ]]
! grep -q $'^THUMB' "$fixture/socket-offset24.out"
(( socket_hit_ms < 750 ))
[[ $(find "$socket_cache/channel-aggregates" -type f -name '*.json' | wc -l) == 1 ]]
[[ $(find "$socket_cache/channel-aggregates" -type f -name '*.json' -printf '%m\n' | sort -u) == 600 ]]

# With no persistent service, production mode must fail closed rather than
# silently restoring per-page yt-dlp startup. Recovery/tests opt in explicitly.
no_fallback_cache=$fixture/no-fallback-cache
mkdir -m 0700 "$no_fallback_cache"
if RG_YOUTUBE_FEED_CACHE=$no_fallback_cache \
	RG_YOUTUBE_YTDLP_SOCKET=$fixture/missing.sock \
	RG_YOUTUBE_FEED_METADATA_ONLY=1 \
	"$project/tools/youtube_feed.py" channel "$socket_channel" 24 \
	>"$fixture/no-fallback.out" 2>"$fixture/no-fallback.err"; then
	printf '%s\n' 'production deep page unexpectedly used CLI fallback' >&2
	exit 1
fi
[ "$(wc -l <"$fixture/calls")" -eq "$socket_calls_before" ]
grep -Fq 'yt-dlp server unavailable' "$fixture/no-fallback.err"

# Recovery fallback still performs one channel-wide 1..97 extraction. Two
# simultaneous deep misses share the channel lock and publish only one result.
export RG_YOUTUBE_FEED_DISABLE_SERVER=1
export RG_YOUTUBE_FEED_ALLOW_CLI_FALLBACK=1
deep_cache=$fixture/deep-cache
mkdir -m 0700 "$deep_cache"
export RG_YOUTUBE_FEED_CACHE=$deep_cache
export FAKE_YTDLP_CALLS=$fixture/deep-calls
export FAKE_YTDLP_GATE=$fixture/deep-gate
deep_channel=UCcL163py441fTFfWy5tBjoQ
RG_YOUTUBE_FEED_METADATA_ONLY=1 \
	"$project/tools/youtube_feed.py" channel "$deep_channel" 24 \
	>"$fixture/deep-one.out" 2>"$fixture/deep-one.err" &
deep_one_pid=$!
for _ in $(seq 1 200); do
	[ -f "$FAKE_YTDLP_GATE.started" ] && break
	sleep 0.01
done
[ -f "$FAKE_YTDLP_GATE.started" ]
RG_YOUTUBE_FEED_METADATA_ONLY=1 \
	"$project/tools/youtube_feed.py" channel "$deep_channel" 24 \
	>"$fixture/deep-two.out" 2>"$fixture/deep-two.err" &
deep_two_pid=$!
sleep 0.05
[ "$(wc -l <"$fixture/deep-calls")" -eq 1 ]
: >"$FAKE_YTDLP_GATE.release"
wait "$deep_one_pid"
wait "$deep_two_pid"
grep -Fqx $'DONE\t8\t'"$deep_channel"$'\tcache=MISS\tnext=32' \
	"$fixture/deep-one.out"
grep -Fqx $'DONE\t8\t'"$deep_channel"$'\tcache=HIT\tnext=32' \
	"$fixture/deep-two.out"
[ "$(wc -l <"$fixture/deep-calls")" -eq 1 ]
python3 - "$fixture/deep-calls" <<'PY'
import pathlib, sys
arguments = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8").rstrip("\n").split("\0")
assert arguments[arguments.index("--playlist-start") + 1] == "1"
assert arguments[arguments.index("--playlist-end") + 1] == "97"
PY
unset FAKE_YTDLP_GATE

deep_hit_start_ns=$(date +%s%N)
RG_YOUTUBE_FEED_METADATA_ONLY=1 \
	"$project/tools/youtube_feed.py" channel "$deep_channel" 24 \
	>"$fixture/deep-hit.out" 2>"$fixture/deep-hit.err"
deep_hit_end_ns=$(date +%s%N)
deep_hit_ms=$(( (deep_hit_end_ns - deep_hit_start_ns) / 1000000 ))
grep -Fqx $'DONE\t8\t'"$deep_channel"$'\tcache=HIT\tnext=32' \
	"$fixture/deep-hit.out"
[ "$(wc -l <"$fixture/deep-calls")" -eq 1 ]
(( deep_hit_ms < 750 ))

# Canceling a cold deep prewarm kills/reaps its compatibility child and never
# publishes a partial aggregate or page cache.
cancel_deep_cache=$fixture/cancel-deep-cache
mkdir -m 0700 "$cancel_deep_cache"
export RG_YOUTUBE_FEED_CACHE=$cancel_deep_cache
export FAKE_YTDLP_CALLS=$fixture/cancel-deep-calls
export FAKE_YTDLP_GATE=$fixture/cancel-deep-gate
export FAKE_YTDLP_PID_FILE=$fixture/cancel-deep-child.pid
RG_YOUTUBE_FEED_METADATA_ONLY=1 \
	"$project/tools/youtube_feed.py" prewarm "$deep_channel" \
	>"$fixture/cancel-deep.out" 2>"$fixture/cancel-deep.err" &
cancel_deep_pid=$!
for _ in $(seq 1 200); do
	[ -f "$FAKE_YTDLP_GATE.started" ] && [ -s "$FAKE_YTDLP_PID_FILE" ] && break
	sleep 0.01
done
[ -f "$FAKE_YTDLP_GATE.started" ]
cancel_deep_child_pid=$(cat "$FAKE_YTDLP_PID_FILE")
kill -TERM "$cancel_deep_pid"
set +e
wait "$cancel_deep_pid"
cancel_deep_status=$?
set -e
[ "$cancel_deep_status" -eq 143 ]
for _ in $(seq 1 100); do
	if ! kill -0 "$cancel_deep_child_pid" 2>/dev/null; then break; fi
	sleep 0.01
done
! kill -0 "$cancel_deep_child_pid" 2>/dev/null
[[ $(find "$cancel_deep_cache" -type f -name '*.json' | wc -l) == 0 ]]
unset FAKE_YTDLP_GATE FAKE_YTDLP_PID_FILE

printf '%s\n' \
	"YOUTUBE_FEED_DEEP_BENCHMARK PASS socket_offset24_hit_ms=$socket_hit_ms recovery_offset24_hit_ms=$deep_hit_ms socket_requests=1 cli_aggregate_calls=1 concurrent_fill=COALESCED cancel_descendants=0 partial_cache=NONE device_latency=PENDING"

# Restore the original cache for the global bounded-cache checks below.
export RG_YOUTUBE_FEED_CACHE=$cache
export FAKE_YTDLP_CALLS=$fixture/calls

# Metadata, pooled locks, and thumbnails persist across helper processes but
# remain bounded on p7.
python3 - "$cache" <<'PY'
import os, pathlib, sys
root = pathlib.Path(sys.argv[1])
for index in range(40):
    path = root / (f"{index:064x}.json")
    path.write_text("{}\n", encoding="ascii")
    path.chmod(0o600)
    os.utime(path, (index + 1, index + 1))
payload = b"\xff\xd8\xff\xe0" + (b"\0" * 124) + b"\xff\xd9"
for index in range(80):
    path = root / "thumbnails" / f"Thumb{index:06d}.jpg"
    path.write_bytes(payload)
    path.chmod(0o600)
    os.utime(path, (index + 1, index + 1))
PY
"$project/tools/youtube_feed.py" home \
	>"$fixture/bounded-restart.out" 2>"$fixture/bounded-restart.err"
[[ $(find "$cache" -maxdepth 1 -type f -name '*.json' | wc -l) -le 32 ]]
[[ $(find "$cache" -maxdepth 1 -type f -name 'feed-slot-*.lock' | wc -l) -le 32 ]]
[[ $(find "$cache/thumbnails" -maxdepth 1 -type f -name '*.jpg' | wc -l) -le 64 ]]

printf '%s\n' 'YOUTUBE_FEED_HOST_TEST PASS home=8 search=8 page=8 next=8 raw_video_renderer=PASS metadata=BEFORE_THUMBNAILS thumbnails=PROGRESSIVE_PRIVATE_JPEG cross_process_cache=HIT metadata_max=32 thumbnails_max=64 locks=FIXED_POOL cache=PRIVATE_TTL+STALE_WHILE_REFRESH protocol=BOUNDED_TSV signed_url=NONE device=UNTOUCHED'
