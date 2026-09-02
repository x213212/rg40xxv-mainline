#!/usr/bin/env bash
set -euo pipefail

project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
workspace=$(CDPATH= cd -- "$project/../../../.." && pwd -P)
fixture=$(mktemp -d /tmp/rg40xxv-youtube-native-test.XXXXXXXX)
trap 'find "$fixture" -type f -delete 2>/dev/null || true; find "$fixture" -depth -type d -empty -delete 2>/dev/null || true' EXIT
component=$fixture/component
runtime=$fixture/runtime
mkdir -p "$component/bin" "$component/tools" "$runtime"
cp "$project/bin/rg40xxv-youtube-native-session" "$component/bin/"
cp "$project/tools/resolver_cache.py" "$component/tools/"

cat >"$component/tools/resolve" <<'EOF'
#!/usr/bin/env bash
set -eu
output=
while (($#)); do
	if [[ $1 == --output ]]; then output=$2; shift 2; else shift; fi
done
expire=$(( $(date +%s) + 7200 ))
printf '{"version":1,"source_id":"GwtNiL9eEYk","duration":120,"streams":{"video":{"url":"https://rr1---sn-test.googlevideo.com/videoplayback?expire=%s","size":4096,"content_type":"video/mp4","headers":{},"chunk_bytes":1048576}}}\n' "$expire" >"$output"
chmod 0600 "$output"
EOF
cat >"$component/tools/bridge" <<'EOF'
#!/usr/bin/env bash
set -eu
ready=
while (($#)); do
	if [[ $1 == --ready-file ]]; then ready=$2; shift 2; else shift; fi
done
printf '%s\n' '{"port":43210,"streams":["video"]}' >"$ready"
chmod 0600 "$ready"
trap 'exit 0' TERM INT
while :; do sleep 1; done
EOF
cat >"$component/bin/player" <<'EOF'
#!/usr/bin/env bash
set -eu
[[ $1 == --device-play && $2 == http://127.0.0.1:43210/stream/video ]]
printf '%s\n' "$*" >"$RG_YOUTUBE_TEST_PLAYER_MARKER"
EOF
chmod 0755 "$component/tools/resolve" "$component/tools/bridge" \
	"$component/tools/resolver_cache.py" \
	"$component/bin/player" "$component/bin/rg40xxv-youtube-native-session"

RG_YOUTUBE_COMPONENT_ROOT=$component \
RG_YOUTUBE_RESOLVER=$component/tools/resolve \
RG_YOUTUBE_CACHE_TOOL=$component/tools/resolver_cache.py \
RG_YOUTUBE_CACHE_DIR=$runtime/cache \
RG_YOUTUBE_BRIDGE=$component/tools/bridge \
RG_YOUTUBE_PLAYER=$component/bin/player \
RG_YOUTUBE_PYTHON=/usr/bin/python3 \
RG_YOUTUBE_RUNTIME_ROOT=$runtime \
RG_YOUTUBE_TEST_PLAYER_MARKER=$fixture/player.args \
	"$component/bin/rg40xxv-youtube-native-session" \
	'https://youtu.be/GwtNiL9eEYk' >"$fixture/session.out"
grep -Fqx -- '--device-play http://127.0.0.1:43210/stream/video' "$fixture/player.args"
grep -Fqx 'YOUTUBE_NATIVE_SESSION result=RETURNED' "$fixture/session.out"
[[ -z $(find "$runtime" -maxdepth 1 -type d -name 'session.*' -print -quit) ]]
[[ -z $(find "$runtime/cache/claims" -mindepth 1 -print -quit) ]]

target_library_path=/usr/lib:/lib/aarch64-linux-gnu:/usr/lib/aarch64-linux-gnu:/usr/lib/aarch64-linux-gnu/pulseaudio:/usr/lib/aarch64-linux-gnu/blas:/usr/lib/aarch64-linux-gnu/lapack
qemu-aarch64-static -L "$workspace/firmware/mnt/rootfs" \
	-E LD_LIBRARY_PATH="$target_library_path" \
	"$project/build/rg40xxv-youtube-native" --contract >"$fixture/ui.contract"
qemu-aarch64-static -L "$workspace/firmware/mnt/rootfs" \
	-E LD_LIBRARY_PATH="$target_library_path" \
	"$project/build/rg40xxv-youtube-player-probe" --controller-contract \
	>"$fixture/player.contract"
grep -Fqx $'A\tplay' "$fixture/ui.contract"
grep -Fqx $'B\treturn' "$fixture/ui.contract"
grep -Fqx $'MENU+START\texternal-exit\tnot-grabbed' "$fixture/ui.contract"
grep -Fqx $'MENU+START\texternal-exit\tnot-grabbed' "$fixture/player.contract"

env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
	RG_YOUTUBE_NO_CONTROLLER=1 RG_YOUTUBE_WINDOWED=1 \
	RG_YOUTUBE_FONT="$workspace/lab/candidates/rg40xxv-next-v1-src/ui/assets/RG40XXV-UI-Sans.otf" \
	qemu-aarch64-static -L "$workspace/firmware/mnt/rootfs" \
	-E LD_LIBRARY_PATH=/usr/lib:/lib/aarch64-linux-gnu:/usr/lib/aarch64-linux-gnu \
	"$project/build/rg40xxv-youtube-native" --demo-ms 80

printf '%s\n' 'YOUTUBE_NATIVE_MVP_HOST_TEST PASS ui=SDL2 controller=contract session=resolver+bridge+player cleanup=PASS device_visual=PENDING'
