#!/usr/bin/env bash
set -euo pipefail
project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
workspace=$(CDPATH= cd -- "$project/../../../.." && pwd -P)
rootfs=$workspace/firmware/mnt/rootfs
output=$(mktemp /tmp/rg40xxv-player-controls.XXXXXX)
trap 'rm -f -- "$output"' EXIT HUP INT TERM
cxx=${CXX:-aarch64-linux-gnu-g++-12}
"$cxx" -std=c++17 -O2 -pipe -mcpu=cortex-a53 -Wall -Wextra -Werror \
	-fno-exceptions -fno-rtti -idirafter "$rootfs/usr/include" \
	-I"$rootfs/usr/include/SDL2" \
	"$project/tests/player_controls_unit.cpp" \
	"$project/src/player_controls.cpp" \
	"$rootfs/usr/lib/aarch64-linux-gnu/libmpv.so.1.109.0" \
	"$rootfs/usr/lib/aarch64-linux-gnu/libSDL2.so" \
	"$rootfs/usr/lib/aarch64-linux-gnu/libSDL2_ttf.so" \
	-Wl,-rpath-link,"$rootfs/lib/aarch64-linux-gnu" \
	-Wl,-rpath-link,"$rootfs/usr/lib" \
	-Wl,-rpath-link,"$rootfs/usr/lib/aarch64-linux-gnu" \
	-Wl,-rpath-link,"$rootfs/usr/lib/aarch64-linux-gnu/pulseaudio" \
	-Wl,-rpath-link,"$rootfs/usr/lib/aarch64-linux-gnu/blas" \
	-Wl,-rpath-link,"$rootfs/usr/lib/aarch64-linux-gnu/lapack" \
	-Wl,--allow-shlib-undefined -o "$output"
libs=/usr/lib:/lib/aarch64-linux-gnu:/usr/lib/aarch64-linux-gnu:/usr/lib/aarch64-linux-gnu/pulseaudio:/usr/lib/aarch64-linux-gnu/blas:/usr/lib/aarch64-linux-gnu/lapack
contract=$(qemu-aarch64-static -L "$rootfs" -E LD_LIBRARY_PATH="$libs" \
	"$output" --contract)
self_test=$(qemu-aarch64-static -L "$rootfs" -E LD_LIBRARY_PATH="$libs" \
	"$output" --self-test)
grep -Fqx $'A\tPLAYER=pause-toggle\tmpv=command-async' <<<"$contract"
grep -Fqx $'LEFT_RIGHT\tseek=-10/+10\trepeat=350ms-then-180ms\tmpv=command-async' <<<"$contract"
grep -Fqx $'B\tPLAYER->HOME\tresult=return-home' <<<"$contract"
grep -Fqx $'TIMELINE\tproperties=time-pos+duration+pause\ttransport=mpv-observe-property' <<<"$contract"
grep -Fqx $'OVERLAY\tprogress=current/duration\tvisibility=3s-or-paused\trender=before-present' <<<"$contract"
grep -Fqx 'YOUTUBE_PLAYER_CONTROLS_SELF_TEST PASS pause=async seek=repeat properties=observed overlay=timed back=home' <<<"$self_test"
if rg -n 'mpv_get_property\(' "$project/src/player_controls.cpp"; then
	echo 'blocking mpv_get_property is forbidden' >&2
	exit 1
fi
rg -Fq 'mpv_observe_property' "$project/src/player_controls.cpp"
rg -Fq 'mpv_command_async' "$project/src/player_controls.cpp"
printf '%s\n' 'YOUTUBE_PLAYER_CONTROLS_HOST_TEST PASS input=A+B+LEFT+RIGHT repeat=350/180ms timeline=observed overlay=3s blocking_get=NONE device=UNVERIFIED'
