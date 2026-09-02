#!/usr/bin/env bash
set -euo pipefail
project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
workspace=$(CDPATH= cd -- "$project/../../../.." && pwd -P)
rootfs=$workspace/firmware/mnt/rootfs
binary=$project/build/rg40xxv-youtube-sdl-texture-scene
source_file=$project/src/sdl_texture_scene.cpp
symbols=$(mktemp /tmp/rg40xxv-youtube-texture-symbols.XXXXXXXX)
singleton_lock=$(mktemp /tmp/rg40xxv-youtube-scene-lock.XXXXXXXX)
singleton_first=$(mktemp /tmp/rg40xxv-youtube-scene-first.XXXXXXXX)
singleton_second=$(mktemp /tmp/rg40xxv-youtube-scene-second.XXXXXXXX)
rm -f -- "$singleton_lock"
trap 'rm -f -- "$symbols" "$singleton_lock" "$singleton_first" "$singleton_second"' EXIT HUP INT TERM
"$project/build-sdl-texture-scene.sh"
libs=/usr/lib:/lib/aarch64-linux-gnu:/usr/lib/aarch64-linux-gnu:/usr/lib/aarch64-linux-gnu/pulseaudio:/usr/lib/aarch64-linux-gnu/blas:/usr/lib/aarch64-linux-gnu/lapack
contract=$(qemu-aarch64-static -L "$rootfs" -E LD_LIBRARY_PATH="$libs" "$binary" --contract)
self=$(qemu-aarch64-static -L "$rootfs" -E LD_LIBRARY_PATH="$libs" "$binary" --self-test)
lifecycle=$(qemu-aarch64-static -L "$rootfs" -E LD_LIBRARY_PATH="$libs" \
	"$binary" --prefetch-lifecycle-self-test)
qemu-aarch64-static -L "$rootfs" -E LD_LIBRARY_PATH="$libs" \
	-E RG_YOUTUBE_SCENE_LOCK="$singleton_lock" \
	"$binary" --singleton-self-test 600 >"$singleton_first" 2>&1 &
singleton_pid=$!
for _attempt in $(seq 1 50); do
	grep -Fq 'YOUTUBE_TEXTURE_SINGLETON result=PASS owner=scene' \
		"$singleton_first" && break
	sleep 0.01
done
grep -Fq 'YOUTUBE_TEXTURE_SINGLETON result=PASS owner=scene' "$singleton_first"
set +e
qemu-aarch64-static -L "$rootfs" -E LD_LIBRARY_PATH="$libs" \
	-E RG_YOUTUBE_SCENE_LOCK="$singleton_lock" \
	"$binary" --singleton-self-test 0 >"$singleton_second" 2>&1
singleton_busy_status=$?
set -e
[ "$singleton_busy_status" -eq 75 ]
grep -Fq 'YOUTUBE_TEXTURE_SINGLETON result=BUSY reason=scene-active' \
	"$singleton_second"
wait "$singleton_pid"
grep -Fqx 'YOUTUBE_TEXTURE_SINGLETON_SELF_TEST result=PASS' "$singleton_first"
qemu-aarch64-static -L "$rootfs" -E LD_LIBRARY_PATH="$libs" \
	-E RG_YOUTUBE_SCENE_LOCK="$singleton_lock" \
	"$binary" --singleton-self-test 0 | \
	grep -Fqx 'YOUTUBE_TEXTURE_SINGLETON_SELF_TEST result=PASS'
grep -Fqx $'SDL_RENDERER\tcount=1\tdriver=accelerated+opengles2\tpresent=SDL_RenderPresent' <<<"$contract"
grep -Fqx $'LIBMPV\tcreate=once\tinitialize=once\trender-context=once\tapi=sw\tlifetime=process' <<<"$contract"
grep -Fqx $'FRAME\tfixed-cpu-buffer=640x480x4\tstreaming-texture=RGBX8888\trender=MPV_RENDER_PARAM_SW_*' <<<"$contract"
grep -Fqx $'HOME\tfeed=paged-cache-96\tpage=8\tmetadata=title+published+duration-before-thumbnails\tthumbnail=progressive-placeholder\tsearch=up\tchannels=8-fixed-ids\tchannel-snapshots=immediate-reuse\tchannel-prewarm=metadata-only' <<<"$contract"
grep -Fqx $'HOME_RENDER\tidle=event-driven\tpoll-ms=4\tpresent=catalog+input+status-only\tplayer=frame-driven-vsync' <<<"$contract"
grep -Fqx $'HOME_INPUT\tdefault=first-card\tDPAD=navigate+repeat-250/90ms\tL1_R1=previous-next-channel\tA=play-or-search-refresh\tX=channel-selector\tB=overview-or-exit' <<<"$contract"
grep -Fqx $'PLAYER_INPUT\tA=pause-toggle\tLEFT_RIGHT=seek-10s\tB=HOME' <<<"$contract"
grep -Fqx $'QUALITY\tproduction=format18-360p\tbattery=fixed-safe-profile\tac-720p=PENDING_CEDRUS_EVIDENCE' <<<"$contract"
grep -Fqx $'PREFETCH\tfocus-stable-ms=500\taction=resolver-cache-selected+neighbours\tmax-coordinators=1\tmax-resolvers=2\tfailure-retry=focus-change-only\tendpoint=activate-only\tbroker-grace-ms=1500' <<<"$contract"
grep -Fqx 'YOUTUBE_PLAYER_CONTROLS_SELF_TEST PASS pause=async seek=repeat properties=observed overlay=timed back=home' <<<"$self"
grep -Fqx 'YOUTUBE_TEXTURE_PREFETCH_POLICY_TEST PASS rapid_moves=40 debounce_ms=500 stable_decisions=1 failure_retry=FOCUS_CHANGE_ONLY max_inflight=1 endpoint=ACTIVATE_ONLY device=PENDING' <<<"$self"
grep -Fqx 'YOUTUBE_TEXTURE_SELF_TEST PASS scenes=HOME+PLAYER feed=ASYNC input=HOME+PLAYER texture=streaming buffer=fixed' <<<"$self"
grep -Fqx 'YOUTUBE_TEXTURE_PREFETCH_LIFECYCLE_TEST PASS rapid_moves=40 focus_cache_decisions=1 focus_broker_decisions=0 focus_bridge_decisions=0 max_cache_inflight=1 activation_broker_decisions=1 device=PENDING' <<<"$lifecycle"
aarch64-linux-gnu-readelf --dyn-syms --wide "$binary" >"$symbols"
for s in SDL_CreateRenderer SDL_CreateTexture SDL_RenderPresent IMG_Load mpv_render_context_create mpv_render_context_render mpv_render_context_set_update_callback mpv_render_context_update; do
	grep -Fq "$s" "$symbols"
done
rg -Fq 'MPV_RENDER_API_TYPE_SW' "$source_file"
rg -Fq 'char format[] = "0bgr"' "$source_file"
rg -Fq 'O_NONBLOCK' "$source_file"
rg -Fq 'SDL_JOYBUTTONDOWN && !a->controller' "$source_file"
rg -Fq 'home_catalog_poll' "$source_file"
rg -Fq 'a.home_view.catalog_revision != a.catalog.revision' "$source_file"
rg -Fq 'a.scene == Scene::home ? kHomePollDelayMs : 1U' "$source_file"
rg -Fq 'home_catalog_open_channel_selector' "$source_file"
rg -Fq 'home_catalog_apply_channel_selector' "$source_file"
rg -Fq 'start_prefetch_for_selection(a, url, previous_url, next_url)' "$source_file"
rg -Fq 'make_text(renderer, view->small_font, "360P", kLight,' \
	"$project/src/home_view.cpp"
rg -Fq 'player_controls_tick' "$source_file"
rg -Fq 'mpv_render_context_set_update_callback' "$source_file"
rg -Fq 'n == 0 ? "eof-before-ready" : "pipe-read"' "$source_file"
rg -Fq 'retry=on-next-A' "$source_file"
rg -Fq 'O_CLOEXEC | O_NOFOLLOW' "$source_file"
rg -Fq 'flock(descriptor, LOCK_EX | LOCK_NB)' "$source_file"
rg -Fq 'a->broker_generation != a->selection_generation' "$source_file"
rg -Fq 'rg40xxv_youtube::player_controls_commands_pending(a->controls)' \
	"$source_file"
rg -Fq 'MPV_EVENT_START_FILE' "$source_file"
rg -Fq 'end->playlist_entry_id == a->active_playlist_entry' "$source_file"
rg -Fq 'queue_stop_if_ready(a)' "$source_file"
if rg -n 'demuxer-max-(back-)?bytes|demuxer-readahead-secs|cache-pause' \
	"$source_file"; then
	printf '%s\n' 'YOUTUBE_TEXTURE_HOST_TEST FAIL reason=unproven-cache-override' >&2
	exit 1
fi
if rg -n 'SDL_GL_SwapWindow|EVIOCGRAB|SDL_SetWindowGrab' "$source_file"; then exit 1; fi
printf '%s\n' 'YOUTUBE_TEXTURE_HOST_TEST PASS build=AArch64 scenes=HOME+PLAYER home=feed+thumbnails+search controls=pause+seek+progress renderer=SDL-opengles2 mpv=SW-update-gated device_visual=PENDING'
