#!/bin/sh
set -eu

project=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
workspace=${RG40XXV_WORKSPACE:-$(CDPATH= cd -- "$project/../../../.." && pwd -P)}
rootfs="$workspace/firmware/mnt/rootfs"
player="$project/build/rg40xxv-youtube-player-probe"
watch_url=${1:-https://youtu.be/GwtNiL9eEYk}
seconds=${2:-5}
video_format=${YT_H700_VIDEO_FORMAT:-18}
player_client=${YT_H700_PLAYER_CLIENT:-android}
temporary=$(mktemp -d "${TMPDIR:-/tmp}/youtube-h700-real.XXXXXXXX")
config="$temporary/bridge.json"
ready="$temporary/ready.json"
bridge_log="$temporary/bridge.log"
bridge_pid=

cleanup()
{
	if test -n "$bridge_pid" && kill -0 "$bridge_pid" 2>/dev/null; then
		kill -TERM "$bridge_pid" 2>/dev/null || true
		wait "$bridge_pid" 2>/dev/null || true
	fi
	find "$temporary" -type f -delete 2>/dev/null || true
	rmdir "$temporary" 2>/dev/null || true
}
trap cleanup EXIT HUP INT TERM

case $seconds in
	*[!0-9]*|'') printf '%s\n' 'seconds must be an integer' >&2; exit 64 ;;
esac
test "$seconds" -ge 1 && test "$seconds" -le 30
test -x "$player"
test -x "$project/tools/resolve_youtube.py"
test -x "$project/tools/bounded_range_bridge.py"

"$project/tools/resolve_youtube.py" "$watch_url" \
	--output "$config" --video-format "$video_format" --max-height 720 \
	--player-client "$player_client"

"$project/tools/bounded_range_bridge.py" \
	--config "$config" --ready-file "$ready" --chunk-bytes 8388608 \
	>"$bridge_log" 2>&1 &
bridge_pid=$!

attempt=0
while test ! -s "$ready"; do
	attempt=$((attempt + 1))
	if test "$attempt" -ge 200 || ! kill -0 "$bridge_pid" 2>/dev/null; then
		sed -n '1,20p' "$bridge_log" >&2
		printf '%s\n' 'YOUTUBE_REAL_SMOKE FAIL bridge_start' >&2
		exit 2
	fi
	sleep 0.05
done
port=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["port"])' "$ready")
endpoint="http://127.0.0.1:$port/stream/video"
has_audio=$(python3 -c 'import json,sys; print("audio" in json.load(open(sys.argv[1]))["streams"])' "$ready")

target_library_path=/usr/lib:/lib/aarch64-linux-gnu:/usr/lib/aarch64-linux-gnu:/usr/lib/aarch64-linux-gnu/pulseaudio:/usr/lib/aarch64-linux-gnu/blas:/usr/lib/aarch64-linux-gnu/lapack
if test "$has_audio" = True; then
	audio_endpoint="http://127.0.0.1:$port/stream/audio"
	qemu-aarch64-static -L "$rootfs" \
		-E LD_LIBRARY_PATH="$target_library_path" \
		"$player" --headless-av-seconds \
		"$endpoint" "$audio_endpoint" "$seconds"
else
	qemu-aarch64-static -L "$rootfs" \
		-E LD_LIBRARY_PATH="$target_library_path" \
		"$player" --headless-play-seconds "$endpoint" "$seconds"
fi

stats=$(curl -fsS "http://127.0.0.1:$port/stats")
printf 'YOUTUBE_BRIDGE_STATS %s\n' "$stats"
printf 'YOUTUBE_REAL_SMOKE PASS requested_seconds=%s\n' "$seconds"
