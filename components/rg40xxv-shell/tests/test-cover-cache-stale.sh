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
"$cc" \
	-std=c11 -O2 -Wall -Wextra -Werror \
	-I"$project/include" \
	-I"$workspace/services/netstream/include" \
	-idirafter "$rootfs/usr/include" \
	-I"$rootfs/usr/include/SDL2" \
	"$project/tests/cover_cache_stale_test.c" \
	"$project/src/cover_cache.c" \
	"$project/src/cover_limits.c" \
	"$rootfs/usr/lib/aarch64-linux-gnu/libSDL2_image.so" \
	"$rootfs/usr/lib/aarch64-linux-gnu/libSDL2.so" \
	-Wl,-rpath-link,"$rootfs/usr/lib/aarch64-linux-gnu" \
	-lm \
	-o "$temporary/cover-cache-stale-test"

qemu-aarch64-static -L "$rootfs" "$temporary/cover-cache-stale-test"
