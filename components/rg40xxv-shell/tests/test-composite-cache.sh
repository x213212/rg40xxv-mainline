#!/bin/sh
set -eu

project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
workspace=${RG40XXV_WORKSPACE:-$(CDPATH= cd -- "$project/../../../.." && pwd -P)}
rootfs="$workspace/firmware/mnt/rootfs"
loader="$rootfs/usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1"
library_path="$rootfs/usr/lib/aarch64-linux-gnu:$rootfs/usr/lib"
temporary=$(mktemp -d)
trap 'status=$?; trap - EXIT; rm -rf -- "$temporary"; exit "$status"' \
	EXIT HUP INT TERM

aarch64-linux-gnu-gcc-12 -std=c11 -O2 -Wall -Wextra -Werror \
	-ffunction-sections -fdata-sections \
	-I"$project/include" -I"$workspace/services/netstream/include" \
	-idirafter "$rootfs/usr/include" -I"$rootfs/usr/include/SDL2" \
	"$project/tests/composite_cache_test.c" "$project/src/render.c" \
	"$rootfs/usr/lib/aarch64-linux-gnu/libSDL2.so" -lm \
	-Wl,--gc-sections -Wl,-rpath-link,"$rootfs/usr/lib/aarch64-linux-gnu" \
	-o "$temporary/composite-cache-test"

SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
	qemu-aarch64-static "$loader" --library-path "$library_path" \
	"$temporary/composite-cache-test"

mkdir -m 0700 "$temporary/roms" "$temporary/state" "$temporary/hardware"
cp "$project/tests/fake-hardwarectl.fixture" "$temporary/fake-hardwarectl"
chmod 0755 "$temporary/fake-hardwarectl"
cat >"$temporary/settings.conf" <<'EOF'
language=zh_TW
screen_lock=1
brightness=8
joystick_rgb=0
usb_debug=0
EOF

run_screenshot()
{
	output=$1
	SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
		qemu-aarch64-static "$loader" --library-path "$library_path" \
		"$project/build/rg40xxv-shell" --windowed \
		--font "$project/assets/RG40XXV-UI-Sans.otf" \
		--rom-root "$temporary/roms" --state-dir "$temporary/state" \
		--hardware-root "$temporary/hardware" \
		--settings-file "$temporary/settings.conf" \
		--filter-state "$temporary/filters.conf" \
		--history-file "$temporary/history.tsv" \
		--favorites-file "$temporary/favorites.tsv" \
		--hardwarectl "$temporary/fake-hardwarectl" \
		--hardwarectl-log "$temporary/hardwarectl.log" \
		--settings-preview --content-preview --screenshot "$output" \
		>"$output.stdout" 2>"$output.stderr"
	grep -Fq 'UI_RESULT PASS' "$output.stdout"
}

(unset RG40XXV_TEST_DIRECT_COMPOSITE; run_screenshot "$temporary/cached.bmp")
RG40XXV_TEST_DIRECT_COMPOSITE=1
export RG40XXV_TEST_DIRECT_COMPOSITE
run_screenshot "$temporary/direct.bmp"
cmp "$temporary/cached.bmp" "$temporary/direct.bmp"
printf '%s\n' 'COMPOSITE_CACHE_INTEGRATION PASS pixel_exact=yes'
