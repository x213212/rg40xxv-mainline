#!/usr/bin/env bash
set -euo pipefail

project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
workspace=$(CDPATH= cd -- "$project/../../../.." && pwd -P)
rootfs=$workspace/firmware/mnt/rootfs
binary=$project/build/rg40xxv-youtube-embedded-scene
source_file=$project/src/embedded_scene.cpp
fixture=$(mktemp -d /tmp/rg40xxv-youtube-embedded-test.XXXXXXXX)
trap 'find "$fixture" -type f -delete 2>/dev/null || true; find "$fixture" -depth -type d -empty -delete 2>/dev/null || true' EXIT

probe_source_before=$(sha256sum "$project/src/player_probe.cpp")
probe_binary_before=$(sha256sum "$project/build/rg40xxv-youtube-player-probe")
native_binary_before=$(sha256sum "$project/build/rg40xxv-youtube-native")
frontend_source_before=$(sha256sum "$project/src/native_frontend.cpp")

"$project/build-embedded-scene.sh" >"$fixture/build.log"

[[ $probe_source_before == "$(sha256sum "$project/src/player_probe.cpp")" ]]
[[ $probe_binary_before == "$(sha256sum "$project/build/rg40xxv-youtube-player-probe")" ]]
[[ $native_binary_before == "$(sha256sum "$project/build/rg40xxv-youtube-native")" ]]
[[ $frontend_source_before == "$(sha256sum "$project/src/native_frontend.cpp")" ]]

target_library_path=/usr/lib:/lib/aarch64-linux-gnu:/usr/lib/aarch64-linux-gnu:/usr/lib/aarch64-linux-gnu/pulseaudio:/usr/lib/aarch64-linux-gnu/blas:/usr/lib/aarch64-linux-gnu/lapack
qemu-aarch64-static -L "$rootfs" \
	-E LD_LIBRARY_PATH="$target_library_path" \
	"$binary" --contract >"$fixture/contract"
qemu-aarch64-static -L "$rootfs" \
	-E LD_LIBRARY_PATH="$target_library_path" \
	"$binary" --self-test >"$fixture/self-test"
qemu-aarch64-static -L "$rootfs" \
	-E LD_LIBRARY_PATH="$target_library_path" \
	"$binary" --mpv-cache-self-test >"$fixture/cache-test"
printf '%s\n' \
	'#!/bin/sh' \
	'printf "%s\\n" "YOUTUBE_ENDPOINT_READY video=http://127.0.0.1:43210/stream/video audio=http://127.0.0.1:43210/stream/audio"' \
	'IFS= read -r _value || exit 0' \
	>"$fixture/mock-broker"
chmod 0755 "$fixture/mock-broker"
qemu-aarch64-static -L "$rootfs" \
	-E LD_LIBRARY_PATH="$target_library_path" \
	"$binary" --broker-pipe-self-test "$fixture/mock-broker" \
	'https://youtu.be/GwtNiL9eEYk' >"$fixture/broker-test"

grep -Fqx $'SDL_WINDOW\tcount=1\tbackend=KMSDRM\tflags=OPENGL' \
	"$fixture/contract"
grep -Fqx $'LIBMPV\tcreate=once\tinitialize=once\trender-context=once\tlifetime=process' \
	"$fixture/contract"
grep -Fqx $'A\tHOME->PLAYER\tloadfile-async\tpending=latched-auto-play' \
	"$fixture/contract"
grep -Fqx $'B\tPLAYER->HOME\tstop-async\tpending=cancel-auto-play' \
	"$fixture/contract"
grep -Fqx $'MENU+START\texternal-exit\tnot-grabbed\tnot-handled' \
	"$fixture/contract"
grep -Fqx $'SIGUSR1\tscreenshot=GLES-glReadPixels+SDL-SaveBMP\tsource=render-memory' \
	"$fixture/contract"
grep -Fqx 'YOUTUBE_EMBEDDED_SELF_TEST PASS scenes=HOME+PLAYER endpoint=loopback input=external-exit screenshot=render-memory' \
	"$fixture/self-test"
grep -Fqx 'YOUTUBE_EMBEDDED_CACHE_TEST PASS max=33554432 back=8388608 readahead=8 cache_pause=no' \
	"$fixture/cache-test"
grep -Fqx 'YOUTUBE_EMBEDDED_BROKER_TEST PASS stdout=nonblocking endpoint=loopback audio=separate cleanup=stdin-eof' \
	"$fixture/broker-test"
grep -Fqx $'BROKER\targv=--broker ABS_PATH WATCH_URL\tstdout=nonblocking\tprotocol=YOUTUBE_ENDPOINT_READY\tcleanup=stdin-EOF+SIGTERM' \
	"$fixture/contract"

dynamic=$(aarch64-linux-gnu-readelf -d "$binary")
grep -Fq 'Shared library: [libmpv.so.1]' <<<"$dynamic"
grep -Fq 'Shared library: [libSDL2-2.0.so.0]' <<<"$dynamic"
grep -Fq 'Shared library: [libSDL2_ttf-2.0.so.0]' <<<"$dynamic"
grep -Fq 'Shared library: [libGLESv2.so.2]' <<<"$dynamic"

symbols=$(aarch64-linux-gnu-readelf --dyn-syms --wide "$binary")
for symbol in mpv_initialize mpv_render_context_create \
	mpv_render_context_render mpv_render_context_free SDL_GL_CreateContext \
	SDL_GL_SwapWindow glReadPixels SDL_SaveBMP; do
	grep -Fq "$symbol" <<<"$symbols"
done
if grep -Fq 'SDL_CreateRenderer' <<<"$symbols"; then
	printf '%s\n' 'unexpected SDL_Renderer: GLES context must remain SDL-owned' >&2
	exit 1
fi
if rg -n 'EVIOCGRAB|SDL_SetWindowGrab' "$source_file"; then
	printf '%s\n' 'input grab is forbidden' >&2
	exit 1
fi
for option in 'demuxer-max-bytes", "33554432' \
	'demuxer-max-back-bytes", "8388608' \
	'demuxer-readahead-secs", "8' 'cache-pause", "no'; do
	rg -Fq "$option" "$source_file"
done
rg -Fq 'O_NONBLOCK' "$source_file"
rg -Fq '"audio-add", app->audio_endpoint, "select"' "$source_file"
rg -Fq 'app->play_requested = true' "$source_file"
rg -Fq 'pending-auto-play=CANCELED broker=alive' "$source_file"

printf '%s\n' 'YOUTUBE_EMBEDDED_HOST_TEST PASS build=AArch64 scenes=HOME+PLAYER window=one context=one mpv=lifetime GLES=render-api screenshot=SIGUSR1 existing_targets=untouched device_visual=PENDING'
