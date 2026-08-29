#!/bin/sh
set -eu

project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
workspace=$(CDPATH= cd -- "$project/../.." && pwd -P)
rootfs="$workspace/firmware/mnt/rootfs"
temporary=$(mktemp -d)
trap 'status=$?; trap - EXIT; rm -rf -- "$temporary"; exit "$status"' \
	EXIT HUP INT TERM
cc=${CC:-aarch64-linux-gnu-gcc-12}

test -f "$rootfs/usr/include/SDL2/SDL.h"
test -x "$(command -v qemu-aarch64-static)"

common_flags="-std=c11 -O1 -g -Wall -Wextra -Werror -fno-omit-frame-pointer"
sanitizer_flags="-fsanitize=address,undefined -fno-sanitize-recover=all -static-libasan -static-libubsan"

# Native deterministic latch/snapshot coverage includes held A, axis state,
# tick wrap, incomplete ioctl snapshots, and fail-closed retry.
"$project/tests/test-input-latch.sh"
"$project/tests/test-texture-lifetime.sh"

# shellcheck disable=SC2086
"$cc" $common_flags $sanitizer_flags \
	-I"$project/include" \
	-I"$workspace/services/netstream/include" \
	-idirafter "$rootfs/usr/include" \
	-I"$rootfs/usr/include/SDL2" \
	"$project/tests/cover_worker_lifecycle_test.c" \
	"$project/src/cover_cache.c" \
	"$project/src/cover_limits.c" \
	"$rootfs/usr/lib/aarch64-linux-gnu/libSDL2_image.so" \
	"$rootfs/usr/lib/aarch64-linux-gnu/libSDL2.so" \
	-Wl,-rpath-link,"$rootfs/usr/lib/aarch64-linux-gnu" \
	-lm \
	-o "$temporary/cover-worker-lifecycle"

asan_options=detect_leaks=0:halt_on_error=1:abort_on_error=1
ubsan_options=halt_on_error=1:print_stacktrace=1
fixture_bmp="$temporary/large-cover.bmp"
ASAN_OPTIONS=$asan_options UBSAN_OPTIONS=$ubsan_options \
	qemu-aarch64-static -L "$rootfs" \
	"$temporary/cover-worker-lifecycle" "$fixture_bmp" \
	>"$temporary/worker.stdout" 2>"$temporary/worker.stderr"
grep -Fq 'COVER_WORKER_LIFECYCLE_TEST PASS' "$temporary/worker.stdout"

# shellcheck disable=SC2086
"$cc" $common_flags -Wno-error=format-truncation $sanitizer_flags \
	-I"$project/include" \
	-I"$workspace/services/netstream/include" \
	-idirafter "$rootfs/usr/include" \
	-I"$rootfs/usr/include/SDL2" \
	"$project"/src/*.c \
	"$workspace/services/netstream/src/netstream.c" \
	"$rootfs/usr/lib/aarch64-linux-gnu/libasound.so" \
	"$rootfs/usr/lib/aarch64-linux-gnu/libSDL2_ttf.so" \
	"$rootfs/usr/lib/aarch64-linux-gnu/libSDL2_image.so" \
	"$rootfs/usr/lib/aarch64-linux-gnu/libSDL2.so" \
	-Wl,-rpath-link,"$rootfs/usr/lib/aarch64-linux-gnu" \
	-lm \
	-o "$temporary/rg40xxv-shell-sanitized"

rom_root="$temporary/roms"
image_dir="$rom_root/GBA/Imgs"
mkdir -p "$image_dir" "$temporary/state" "$temporary/hardware"
i=0
while test "$i" -lt 32; do
	name=$(printf 'Stress %02d' "$i")
	: >"$rom_root/GBA/$name.gba"
	ln "$fixture_bmp" "$image_dir/$name.bmp"
	i=$((i + 1))
done
cp "$project/tests/fake-launcher.fixture" "$temporary/fake-launcher.sh"
chmod 0755 "$temporary/fake-launcher.sh"

run_ui()
{
	label=$1
	shift
	ASAN_OPTIONS=$asan_options UBSAN_OPTIONS=$ubsan_options \
	SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
	LAUNCH_CAPTURE="$temporary/$label.arguments" FAKE_EVENT_MARKER=1 \
	qemu-aarch64-static -L "$rootfs" \
		"$temporary/rg40xxv-shell-sanitized" \
		--windowed --font "$project/assets/RG40XXV-UI-Sans.otf" \
		--icon-atlas "$project/assets/RG40XXV-Material-Icons.png" \
		--rom-root "$rom_root" \
		--platform-routes "$project/tests/platform-routes.fixture.json" \
		--launcher "$temporary/fake-launcher.sh" \
		--launch-log "$temporary/$label.launch.log" \
		--history-file "$temporary/$label.history.tsv" \
		--favorites-file "$temporary/$label.favorites.tsv" \
		--settings-file "$temporary/$label.settings.conf" \
		--filter-state "$temporary/$label.filters.conf" \
		--state-dir "$temporary/state" \
		--hardware-root "$temporary/hardware" \
		--hardwarectl /not-present/ui-hardwarectl \
		"$@" \
		>"$temporary/$label.stdout" 2>"$temporary/$label.stderr"
	grep -Fq 'UI_RESULT PASS' "$temporary/$label.stdout"
	if grep -Eq 'ERROR: AddressSanitizer|runtime error:' \
		"$temporary/$label.stdout" "$temporary/$label.stderr"; then
		printf '%s\n' "sanitizer finding in $label" >&2
		return 1
	fi
}

run_ui rapid-navigation --navigation-stress --demo-ms 2600
stale=$(sed -n 's/.* cover_stale_dropped=\([0-9][0-9]*\).*/\1/p' \
	"$temporary/rapid-navigation.stdout")
cancelled=$(sed -n 's/.* cover_queue_cancelled=\([0-9][0-9]*\).*/\1/p' \
	"$temporary/rapid-navigation.stdout")
visible_evictions=$(sed -n 's/.* cover_visible_evictions=\([0-9][0-9]*\).*/\1/p' \
	"$temporary/rapid-navigation.stdout")
test -n "$stale"
test -n "$cancelled"
test "$stale" -gt 0
test $((stale + cancelled)) -gt 0
test "$visible_evictions" -eq 0

# Force more than TEXT_CACHE_MAX unique system labels through the game-library
# filter panel, then keep cycling every filter row. Under SDL's dummy video
# driver this covers filter state and texture-cache lifetime with ASan/UBSan;
# the production KMSDRM/Panfrost path is verified separately on the device.
system=0
while test "$system" -lt 190; do
	directory=$(printf 'SYSTEM-%03d' "$system")
	mkdir -p "$rom_root/$directory"
	: >"$rom_root/$directory/Game-$directory.gba"
	system=$((system + 1))
done
run_ui filter-panel --filter-stress --demo-ms 4200

# Exercise real input_init -> suspend -> input_init reacquire without the
# benchmark-only force-ready path.
run_ui neutral-latch-lifecycle --launch-once --demo-ms 1500
grep -Fq 'UI_LAUNCH_TRANSITION PRESENTED phase=starting' \
	"$temporary/neutral-latch-lifecycle.stderr"
grep -Fq 'UI_LAUNCH_TRANSITION PRESENTED phase=returned' \
	"$temporary/neutral-latch-lifecycle.stderr"

# Repeat full launch suspend/resume because each launch destroys and recreates
# the renderer, text cache, cover worker, input, audio, and monitor workers.
lifecycle=1
while test "$lifecycle" -le 4; do
	label="lifecycle-$lifecycle"
	run_ui "$label" --navigation-stress --launch-once --demo-ms 1500
	grep -Fq 'UI_LAUNCH_TRANSITION PRESENTED phase=starting' \
		"$temporary/$label.stderr"
	grep -Fq 'UI_LAUNCH_TRANSITION PRESENTED phase=returned' \
		"$temporary/$label.stderr"
	test -s "$temporary/$label.arguments"
	lifecycle=$((lifecycle + 1))
done

printf 'ARM64 ASan+UBSan navigation/lifecycle: PASS stale=%s cancelled=%s visible_evictions=%s cycles=%s\n' \
	"$stale" "$cancelled" "$visible_evictions" "$lifecycle"
