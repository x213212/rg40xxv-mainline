#!/bin/sh
set -eu

project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
workspace=${RG40XXV_WORKSPACE:-$(CDPATH= cd -- "$project/../../../.." && pwd -P)}
rootfs="$workspace/firmware/mnt/rootfs"
temporary=$(mktemp -d)
trap 'status=$?; trap - EXIT; rm -rf -- "$temporary"; exit "$status"' \
	EXIT HUP INT TERM
binary="$temporary/network-flow-test"
loader="$rootfs/usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1"
library_path="$rootfs/usr/lib/aarch64-linux-gnu:$rootfs/usr/lib"

aarch64-linux-gnu-gcc-12 -std=c11 -O2 -Wall -Wextra -Werror \
	-ffunction-sections -fdata-sections -I"$project/include" \
	-I"$workspace/services/netstream/include" \
	-idirafter "$rootfs/usr/include" -I"$rootfs/usr/include/SDL2" \
	"$project/tests/network_flow_test.c" \
	"$project/src/network_ui.c" "$project/src/network_state.c" \
	"$project/src/input_method.c" \
	"$rootfs/usr/lib/aarch64-linux-gnu/libSDL2.so" \
	-Wl,--gc-sections -Wl,-rpath-link,"$rootfs/usr/lib/aarch64-linux-gnu" \
	-o "$binary"

SDL_VIDEODRIVER=dummy qemu-aarch64-static "$loader" \
	--library-path "$library_path" "$binary"
