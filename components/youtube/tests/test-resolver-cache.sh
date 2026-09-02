#!/usr/bin/env bash
set -euo pipefail

project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
cache_tool=$project/tools/resolver_cache.py
fixture=$(mktemp -d /tmp/rg40xxv-resolver-cache-test.XXXXXXXX)
trap 'find "$fixture" -type f -delete 2>/dev/null || true; find "$fixture" -type l -delete 2>/dev/null || true; find "$fixture" -depth -type d -empty -delete 2>/dev/null || true' EXIT
state=$fixture/state
cache=$fixture/cache
mkdir -m 0700 "$state"
printf '0\n' >"$state/active"
printf '0\n' >"$state/maximum"
: >"$state/calls"
: >"$state/priorities"

cat >"$fixture/fake-resolver" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
url=$1
shift
output=
while (($#)); do
	case $1 in
	--output) output=$2; shift 2 ;;
	*) shift ;;
	esac
done
[[ -n $output ]]
video_id=${url##*/}
state=${FAKE_RESOLVER_STATE:?}
priority=${RG_YOUTUBE_RESOLVE_PRIORITY:?}
[[ $priority == interactive || $priority == background ]]

update_counter()
{
	local direction=$1 active maximum
	exec 9>>"$state/counter.lock"
	flock -x 9
	active=$(<"$state/active")
	maximum=$(<"$state/maximum")
	active=$((active + direction))
	printf '%s\n' "$active" >"$state/active"
	if ((active > maximum)); then printf '%s\n' "$active" >"$state/maximum"; fi
	if ((direction > 0)); then
		printf '%s\n' "$video_id" >>"$state/calls"
		printf '%s:%s\n' "$video_id" "$priority" >>"$state/priorities"
	fi
	flock -u 9
	exec 9>&-
}
update_counter 1
trap 'update_counter -1' EXIT HUP INT TERM
if [[ $video_id == ${FAKE_RESOLVER_FAIL_ID:-} ]]; then
	exit 42
fi
if [[ ${FAKE_RESOLVER_MODE:-valid} == block-descendant ]]; then
	sleep 300 &
	descendant=$!
	printf '%s\n' "$descendant" >"${FAKE_RESOLVER_DESCENDANT_PID:?}"
	: >"${FAKE_RESOLVER_READY:?}"
	cleanup_blocked_resolver()
	{
		trap - EXIT HUP INT TERM
		kill -TERM "$descendant" 2>/dev/null || true
		wait "$descendant" 2>/dev/null || true
		update_counter -1
		: >"${FAKE_RESOLVER_CLEANED:?}"
	}
	trap 'cleanup_blocked_resolver; exit 143' HUP INT TERM
	trap 'cleanup_blocked_resolver' EXIT
	wait "$descendant"
	exit 70
fi
sleep "${FAKE_RESOLVER_SLEEP:-0}"
now=$(date +%s)
expire=$((now + ${FAKE_EXPIRE_OFFSET:-7200}))
duration=${FAKE_DURATION:-120}
case ${FAKE_RESOLVER_MODE:-valid} in
	valid)
		media_url="https://rr1---sn-test.googlevideo.com/videoplayback?expire=$expire&secret=SIGNED_CANARY"
		streams="\"video\":{\"url\":\"$media_url\",\"size\":4096}"
		;;
	missing-expire)
		media_url='https://rr1---sn-test.googlevideo.com/videoplayback?secret=SIGNED_CANARY'
		streams="\"video\":{\"url\":\"$media_url\",\"size\":4096}"
		;;
	duplicate-expire)
		media_url="https://rr1---sn-test.googlevideo.com/videoplayback?expire=$expire&expire=$expire&secret=SIGNED_CANARY"
		streams="\"video\":{\"url\":\"$media_url\",\"size\":4096}"
		;;
	audio-early)
		early=$((now + 700))
		video_url="https://rr1---sn-test.googlevideo.com/videoplayback?expire=$expire&secret=SIGNED_CANARY"
		audio_url="https://rr2---sn-test.googlevideo.com/videoplayback?expire=$early&secret=SIGNED_CANARY"
		streams="\"video\":{\"url\":\"$video_url\",\"size\":4096},\"audio\":{\"url\":\"$audio_url\",\"size\":2048}"
		;;
	*) exit 64 ;;
esac
printf '{"version":1,"source_id":"%s","duration":%s,"streams":{%s}}\n' \
	"$video_id" "$duration" "$streams" >"$output"
chmod 0600 "$output"
EOF
chmod 0755 "$fixture/fake-resolver" "$cache_tool"

run_cache()
{
	FAKE_RESOLVER_STATE=$state \
	RG_YOUTUBE_RESOLVER=$fixture/fake-resolver \
	RG_YOUTUBE_CACHE_DIR=$cache \
		"$cache_tool" "$@"
}

url_a=https://youtu.be/AAAABBBB001
url_b=https://youtu.be/AAAABBBB002
url_c=https://youtu.be/AAAABBBB003
url_d=https://youtu.be/AAAABBBB004
url_e=https://youtu.be/AAAABBBB005
url_f=https://youtu.be/AAAABBBB006

# One asynchronous coordinator accepts SELECTED, PREVIOUS and NEXT.  The
# process set may contain three videos, but the global resolver slots remain
# strictly capped at two concurrent yt-dlp jobs.
FAKE_RESOLVER_STATE=$state FAKE_RESOLVER_SLEEP=0.15 \
RG_YOUTUBE_RESOLVER=$fixture/fake-resolver RG_YOUTUBE_CACHE_DIR=$cache \
	"$cache_tool" prefetch-set "$url_a" "$url_b" "$url_c" \
	2>"$fixture/prefetch-set.log"
[[ $(grep -Ec 'AAAABBBB00[123]' "$state/calls") == 3 ]]
[[ $(<"$state/maximum") -le 2 ]]
if run_cache prefetch-set "$url_a" "$url_b" "$url_c" "$url_d" \
	2>"$fixture/too-many.log"; then
	printf '%s\n' 'prefetch-set accepted more than three URLs' >&2
	exit 1
fi

# The coordinator result describes the selected URL (argv[0]) only.  A
# speculative neighbour may fail without poisoning the selected card, while a
# selected-card failure must still be reported.
selected_ok=https://youtu.be/AAAABBBB015
neighbour_fail=https://youtu.be/AAAABBBB016
neighbour_ok=https://youtu.be/AAAABBBB017
FAKE_RESOLVER_STATE=$state FAKE_RESOLVER_FAIL_ID=AAAABBBB016 \
RG_YOUTUBE_RESOLVER=$fixture/fake-resolver RG_YOUTUBE_CACHE_DIR=$cache \
	"$cache_tool" prefetch-set "$selected_ok" "$neighbour_fail" \
	2>"$fixture/neighbour-failure.log"
if FAKE_RESOLVER_STATE=$state FAKE_RESOLVER_FAIL_ID=AAAABBBB016 \
   RG_YOUTUBE_RESOLVER=$fixture/fake-resolver RG_YOUTUBE_CACHE_DIR=$cache \
   "$cache_tool" prefetch-set "$neighbour_fail" "$neighbour_ok" \
   2>"$fixture/selected-failure.log"; then
	printf '%s\n' 'selected prefetch failure was hidden' >&2
	exit 1
fi

# N same-ID processes share the stable per-video flock and resolve only once.
# url_c is already warm from the neighbour prefetch above, so use url_d here.
pids=()
for _attempt in {1..6}; do
	FAKE_RESOLVER_STATE=$state FAKE_RESOLVER_SLEEP=0.12 \
	RG_YOUTUBE_RESOLVER=$fixture/fake-resolver RG_YOUTUBE_CACHE_DIR=$cache \
		"$cache_tool" prefetch "$url_d" 2>>"$fixture/dedupe.log" &
	pids+=("$!")
done
for child in "${pids[@]}"; do wait "$child"; done
[[ $(grep -Fxc 'AAAABBBB004' "$state/calls") == 1 ]]

# Independently launched IDs cannot exceed the two global resolver slots.
printf '0\n' >"$state/active"
printf '0\n' >"$state/maximum"
pids=()
for url in "$url_e" "$url_f" https://youtu.be/AAAABBBB007 https://youtu.be/AAAABBBB014; do
	FAKE_RESOLVER_STATE=$state FAKE_RESOLVER_SLEEP=0.2 \
	RG_YOUTUBE_RESOLVER=$fixture/fake-resolver RG_YOUTUBE_CACHE_DIR=$cache \
		"$cache_tool" prefetch "$url" 2>>"$fixture/slots.log" &
	pids+=("$!")
done
for child in "${pids[@]}"; do wait "$child"; done
[[ $(<"$state/maximum") == 2 ]]
if grep -Ev '^[A-Za-z0-9_-]{11}:background$' "$state/priorities" | grep -q .; then
	printf '%s\n' 'prefetch resolver did not receive background priority' >&2
	exit 1
fi

# TERM of the focus-prefetch helper is forwarded into the resolver's separate
# process group, including descendants; no orphan may outlive HOME activation.
cleanup_url=https://youtu.be/AAAABBBB013
FAKE_RESOLVER_STATE=$state FAKE_RESOLVER_MODE=block-descendant \
FAKE_RESOLVER_DESCENDANT_PID=$fixture/descendant.pid \
FAKE_RESOLVER_READY=$fixture/resolver.ready \
FAKE_RESOLVER_CLEANED=$fixture/resolver.cleaned \
RG_YOUTUBE_RESOLVER=$fixture/fake-resolver RG_YOUTUBE_CACHE_DIR=$cache \
	setsid "$cache_tool" prefetch "$cleanup_url" \
	2>"$fixture/cleanup.log" &
cleanup_pid=$!
for _attempt in {1..300}; do
	[[ -f $fixture/resolver.ready ]] && break
	sleep 0.01
done
[[ -f $fixture/resolver.ready ]]
descendant_pid=$(<"$fixture/descendant.pid")
[[ $descendant_pid =~ ^[0-9]+$ ]]
kill -TERM -- "-$cleanup_pid"
if wait "$cleanup_pid"; then
	printf '%s\n' 'terminated cache helper unexpectedly succeeded' >&2
	exit 1
fi
for _attempt in {1..300}; do
	[[ ! -e /proc/$descendant_pid ]] && break
	sleep 0.01
done
[[ ! -e /proc/$descendant_pid ]]
[[ -f $fixture/resolver.cleaned ]]
[[ ! -e $cache/entries/AAAABBBB013.json ]]

# A cache miss on explicit PLAY/acquire is marked interactive for the daemon.
interactive_url=https://youtu.be/AAAABBBB012
interactive_claim=$(run_cache acquire "$interactive_url" 2>"$fixture/interactive.log")
grep -Fqx 'AAAABBBB012:interactive' "$state/priorities"
run_cache release "$interactive_claim" 2>>"$fixture/interactive.log"

# Cache layout and entries are owner-only.  Acquire atomically publishes a
# single-use snapshot while retaining the validated entry for bounded replay.
[[ $(stat -c %a "$cache") == 700 ]]
for directory in entries locks claims slots; do [[ $(stat -c %a "$cache/$directory") == 700 ]]; done
entry=$cache/entries/AAAABBBB001.json
[[ $(stat -c %a "$entry") == 600 ]]
python3 - "$entry" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as source:
    value = json.load(source)
assert value["resolver_cache"]["video_format"] == "18"
assert value["resolver_cache"]["schema"] == (
    "rg40xxv-youtube-resolver-cache-v3-format18-authoritative-size"
)
PY
calls_before_replay=$(grep -Fxc 'AAAABBBB001' "$state/calls")
claim=$(run_cache acquire "$url_a" 2>"$fixture/acquire.log")
[[ $claim == "$cache"/claims/AAAABBBB001.*.json ]]
[[ -f $entry && ! -L $entry && $(stat -c %a "$entry") == 600 ]]
[[ -f $claim && ! -L $claim && $(stat -c %a "$claim") == 600 ]]
[[ $(stat -c %h "$entry") == 1 && $(stat -c %h "$claim") == 1 ]]
cmp -s "$entry" "$claim"
run_cache release "$claim" 2>"$fixture/release.log"
[[ ! -e $claim ]]

# Replaying the same video from a second cache-tool process remains a HIT and
# never starts another resolver/yt-dlp extraction.
replay_claim=$(run_cache acquire "$url_a" 2>"$fixture/replay-acquire.log")
[[ $(grep -Fxc 'AAAABBBB001' "$state/calls") == "$calls_before_replay" ]]
grep -Fq 'result=HIT' "$fixture/replay-acquire.log"
[[ -f $entry && -f $replay_claim ]]
run_cache release "$replay_claim" 2>"$fixture/replay-release.log"
[[ ! -e $replay_claim && -f $entry ]]

# Persistent entries and crash-left claims are bounded.  Per-video locking is
# a fixed 64-slot pool, so arbitrary video IDs cannot grow the lock directory.
python3 - "$cache" <<'PY'
import os, pathlib, sys, time
root = pathlib.Path(sys.argv[1])
for index in range(110):
    path = root / "entries" / f"Z{index:010d}.json"
    path.write_text("{}\n", encoding="ascii")
    path.chmod(0o600)
    os.utime(path, (index + 1, index + 1))
for index in range(70):
    path = root / "claims" / f"Y{index:010d}.stale{index}.json"
    path.write_text("{}\n", encoding="ascii")
    path.chmod(0o600)
    old = time.time() - 7200
    os.utime(path, (old, old))
PY
run_cache prefetch "$url_a" 2>"$fixture/bounds.log"
[[ $(find "$cache/entries" -maxdepth 1 -type f -name '*.json' | wc -l) -le 96 ]]
[[ $(find "$cache/locks" -maxdepth 1 -type f -name 'video-*.lock' | wc -l) -le 64 ]]
[[ -z $(find "$cache/claims" -maxdepth 1 -type f -name '*.json' -print -quit) ]]

# Expiry is strict and must cover duration plus a ten-minute safety window.
bad_url=https://youtu.be/AAAABBBB008
if FAKE_RESOLVER_STATE=$state FAKE_EXPIRE_OFFSET=700 \
   RG_YOUTUBE_RESOLVER=$fixture/fake-resolver RG_YOUTUBE_CACHE_DIR=$cache \
   "$cache_tool" prefetch "$bad_url" 2>"$fixture/short-expiry.log"; then
	printf '%s\n' 'short signed lifetime was cached' >&2
	exit 1
fi
[[ ! -e $cache/entries/AAAABBBB008.json ]]
for mode in missing-expire duplicate-expire audio-early; do
	id=AAAABBBB00$((9 + $(wc -l <"$state/calls") % 10))
	case $mode in
	missing-expire) invalid_url=https://youtu.be/AAAABBBB009 ;;
	duplicate-expire) invalid_url=https://youtu.be/AAAABBBB010 ;;
	audio-early) invalid_url=https://youtu.be/AAAABBBB011 ;;
	esac
	if FAKE_RESOLVER_STATE=$state FAKE_RESOLVER_MODE=$mode \
	   RG_YOUTUBE_RESOLVER=$fixture/fake-resolver RG_YOUTUBE_CACHE_DIR=$cache \
	   "$cache_tool" prefetch "$invalid_url" 2>>"$fixture/invalid-expiry.log"; then
		printf 'invalid expiry cached mode=%s\n' "$mode" >&2
		exit 1
	fi
done

# Age, wall/monotonic drift, wrong mode, symlink and hardlink are invalidated.
run_cache prefetch "$url_a" 2>/dev/null
python3 - "$entry" <<'PY'
import json, sys
path = sys.argv[1]
with open(path, encoding="utf-8") as source:
    value = json.load(source)
value["resolver_cache"]["resolved_at"] -= 1000
value["resolver_cache"]["resolved_monotonic"] -= 1000
with open(path, "w", encoding="utf-8") as output:
    json.dump(value, output, separators=(",", ":"))
    output.write("\n")
PY
chmod 0600 "$entry"
before=$(grep -Fxc 'AAAABBBB001' "$state/calls")
run_cache prefetch "$url_a" 2>"$fixture/stale.log"
after=$(grep -Fxc 'AAAABBBB001' "$state/calls")
[[ $after == $((before + 1)) ]]

chmod 0644 "$entry"
run_cache prefetch "$url_a" 2>"$fixture/mode.log"
[[ $(stat -c %a "$entry") == 600 ]]
cp "$entry" "$fixture/outside"
find "$entry" -delete
ln -s "$fixture/outside" "$entry"
run_cache prefetch "$url_a" 2>"$fixture/symlink.log"
[[ -f $entry && ! -L $entry && $(<"$fixture/outside") == *SIGNED_CANARY* ]]
ln "$entry" "$fixture/hardlink"
run_cache prefetch "$url_a" 2>"$fixture/hardlink.log"
[[ -f $entry && $(stat -c %h "$entry") == 1 && -f $fixture/hardlink ]]

# Signed URL/token data stays out of all cache-tool diagnostics.
if rg -q 'SIGNED_CANARY|googlevideo[.]com' "$fixture"/*.log; then
	printf '%s\n' 'signed URL leaked to diagnostics' >&2
	exit 1
fi

printf '%s\n' 'YOUTUBE_RESOLVER_CACHE_HOST_TEST PASS private=0600 key=video-id+format18+expiry neighbours=selected+previous+next expiry=duration+600 age=900 drift=30 dedupe=fixed-lock-pool workers_max=2 entries_max=96 claims_max=64 atomic_claim=PASS cross_process_replay=HIT process_group_descendants=0 signed_log_leak=NONE device=UNTOUCHED'
