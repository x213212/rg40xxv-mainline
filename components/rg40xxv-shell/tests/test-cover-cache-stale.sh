#!/bin/sh
set -eu

project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
workspace=${RG40XXV_WORKSPACE:-$(CDPATH= cd -- "$project/../../../.." && pwd -P)}
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

# A delayed catalog refresh may reallocate catalog.games.  The worker is only
# allowed to consume the immutable path published in its cover_job.
worker_body=$(sed -n '/^static int worker_main/,/^}/p' \
	"$project/src/cover_cache.c")
if printf '%s\n' "$worker_body" | grep -Fq 'ui->catalog.games'; then
	printf '%s\n' 'cover worker dereferences mutable catalog' >&2
	exit 1
fi
printf '%s\n' "$worker_body" | \
	grep -Fq 'load_cover_bounded(ui, job->path, &loaded);'

qemu-aarch64-static -L "$rootfs" "$temporary/cover-cache-stale-test" \
	"$temporary/cache"
