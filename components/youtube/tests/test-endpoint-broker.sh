#!/usr/bin/env bash
set -euo pipefail
umask 077

project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
fixture=$(mktemp -d /tmp/rg40xxv-youtube-broker-test.XXXXXXXX)
trap 'jobs -pr | xargs -r kill 2>/dev/null || true; find "$fixture" -type f -delete 2>/dev/null || true; find "$fixture" -depth -type d -empty -delete 2>/dev/null || true' EXIT
component=$fixture/component
runtime=$fixture/runtime
cache=$runtime/cache
mkdir -p "$component/tools" "$runtime"
cp "$project/tools/endpoint_broker.py" "$project/tools/resolver_cache.py" "$component/tools/"

cat >"$component/tools/resolve" <<'EOF'
#!/usr/bin/env bash
set -eu
printf '%s\n' call >>"${RG_YOUTUBE_TEST_RESOLVER_CALLS:?}"
url=$1
case $url in
	*youtu.be/*) source_id=${url##*/} ;;
	*watch?v=*) source_id=${url##*v=} ;;
	*) exit 64 ;;
esac
output=
while (($#)); do
	if [[ $1 == --output ]]; then output=$2; shift 2; else shift; fi
done
expire=$(( $(date +%s) + 7200 ))
printf '{"version":1,"source_id":"%s","duration":120,"streams":{"video":{"url":"https://rr1---sn-test.googlevideo.com/videoplayback?expire=%s&sig=PRIVATE_TOKEN","size":4096,"content_type":"video/mp4","headers":{},"chunk_bytes":1048576}}}\n' "$source_id" "$expire" >"$output"
chmod 0600 "$output"
EOF

cat >"$component/tools/bridge" <<'EOF'
#!/usr/bin/env bash
set -eu
config= ready=
while (($#)); do
	case $1 in
		--config) config=$2; shift 2 ;;
		--ready-file) ready=$2; shift 2 ;;
		*) shift ;;
	esac
done
[[ -f $config && $(stat -c %a "$config") == 600 ]]
grep -Fq 'PRIVATE_TOKEN' "$config"
if [[ ${RG_YOUTUBE_TEST_AUDIO:-0} == 1 ]]; then
	printf '%s\n' '{"port":43210,"streams":["audio","video"]}' >"$ready"
else
	printf '%s\n' '{"port":43210,"streams":["video"]}' >"$ready"
fi
chmod 0600 "$ready"
trap 'touch "$RG_YOUTUBE_TEST_BRIDGE_STOPPED"; exit 0' TERM INT HUP
while :; do sleep 1; done
EOF
chmod 0755 "$component/tools/resolve" "$component/tools/bridge" \
	"$component/tools/endpoint_broker.py" "$component/tools/resolver_cache.py"

run_broker()
{
	local fifo=$1 output=$2 errors=$3
	shift 3
	RG_YOUTUBE_CACHE_TOOL=$component/tools/resolver_cache.py \
	RG_YOUTUBE_RESOLVER=$component/tools/resolve \
	RG_YOUTUBE_BRIDGE=$component/tools/bridge \
	RG_YOUTUBE_CACHE_DIR=$cache \
	RG_YOUTUBE_RUNTIME_ROOT=$runtime \
	RG_YOUTUBE_TEST_BRIDGE_STOPPED=$fixture/bridge.stopped \
	RG_YOUTUBE_TEST_RESOLVER_CALLS=$fixture/resolver.calls \
	RG_YOUTUBE_TEST_AUDIO=${RG_YOUTUBE_TEST_AUDIO:-0} \
	"$component/tools/endpoint_broker.py" 'https://youtu.be/GwtNiL9eEYk' \
		"$@" <"$fifo" >"$output" 2>"$errors" &
	broker_pid=$!
}

fifo=$fixture/control
mkfifo "$fifo"
run_broker "$fifo" "$fixture/out" "$fixture/error"
pid=$broker_pid
exec 3>"$fifo"
for _ in {1..200}; do [[ -s $fixture/out ]] && break; sleep 0.025; done
[[ -s $fixture/out ]]
grep -Fqx 'YOUTUBE_ENDPOINT_READY video=http://127.0.0.1:43210/stream/video audio=none' "$fixture/out"
! grep -R -Fq 'PRIVATE_TOKEN' "$fixture/out" "$fixture/error"
[[ -z $(find "$cache/claims" -mindepth 1 -print -quit) ]]
kill -TERM "$pid"
wait "$pid"
for _ in {1..40}; do [[ -f $fixture/bridge.stopped ]] && break; sleep 0.025; done
[[ -f $fixture/bridge.stopped ]]
[[ -z $(find "$runtime" -maxdepth 1 -type d -name 'broker.*' -print -quit) ]]

rm -f "$fixture/bridge.stopped"
fifo2=$fixture/control-eof
mkfifo "$fifo2"
RG_YOUTUBE_TEST_AUDIO=1 run_broker "$fifo2" "$fixture/out-eof" "$fixture/error-eof"
pid=$broker_pid
exec 4>"$fifo2"
for _ in {1..200}; do [[ -s $fixture/out-eof ]] && break; sleep 0.025; done
[[ -s $fixture/out-eof ]]
grep -Fqx 'YOUTUBE_ENDPOINT_READY video=http://127.0.0.1:43210/stream/video audio=http://127.0.0.1:43210/stream/audio' "$fixture/out-eof"
exec 4>&-
wait "$pid"
for _ in {1..40}; do [[ -f $fixture/bridge.stopped ]] && break; sleep 0.025; done
[[ -f $fixture/bridge.stopped ]]
[[ -z $(find "$runtime" -maxdepth 1 -type d -name 'broker.*' -print -quit) ]]
[[ $(wc -l <"$fixture/resolver.calls") == 1 ]]
[[ -f $cache/entries/GwtNiL9eEYk.json ]]

# PLAYER cancels a delayed background resolve.  Returning HOME arms exactly
# one retry; repeated protocol polling never creates another resolver job.
rm -f "$fixture/bridge.stopped"
fifo3=$fixture/control-prefetch
mkfifo "$fifo3"
run_broker "$fifo3" "$fixture/out-prefetch" "$fixture/error-prefetch" \
	--prefetch-delay 1 --prefetch-url 'https://youtu.be/jNQXAC9IVRw'
pid=$broker_pid
exec 5>"$fifo3"
for _ in {1..200}; do [[ -s $fixture/out-prefetch ]] && break; sleep 0.025; done
[[ -s $fixture/out-prefetch ]]
printf 'PLAY\n' >&5
sleep 1.2
[[ $(wc -l <"$fixture/resolver.calls") == 1 ]]
printf 'HOME\n' >&5
for _ in {1..200}; do
	[[ $(wc -l <"$fixture/resolver.calls") == 2 ]] && break
	sleep 0.025
done
[[ $(wc -l <"$fixture/resolver.calls") == 2 ]]
sleep 0.2
[[ $(wc -l <"$fixture/resolver.calls") == 2 ]]
exec 5>&-
wait "$pid"

printf '%s\n' 'YOUTUBE_ENDPOINT_BROKER_HOST_TEST PASS protocol=one-line signed_url_stdout=NONE cache_claim=release-after-ready replay_cache=HIT prefetch_api=DIAGNOSTIC_COMPAT_ONLY process_group_cleanup=PASS runtime_artifacts=0 stdin_eof=PASS device=UNTOUCHED'
