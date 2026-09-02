#!/bin/sh
set -eu

project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
workspace=${RG40XXV_WORKSPACE:-$(CDPATH= cd -- "$project/../../../.." && pwd -P)}
rootfs="$workspace/firmware/mnt/rootfs"
temporary=$(mktemp -d)
trap 'status=$?; trap - EXIT; rm -rf -- "$temporary"; exit "$status"' \
	EXIT HUP INT TERM

aarch64-linux-gnu-gcc-12 -std=c11 -O2 -Wall -Wextra -Werror \
	-I"$project/include" -I"$workspace/services/netstream/include" \
	-idirafter "$rootfs/usr/include" \
	-I"$rootfs/usr/include/SDL2" \
	"$project/tests/settings_state_test.c" \
	"$project/src/settings_ui.c" \
	"$rootfs/usr/lib/aarch64-linux-gnu/libSDL2.so" \
	-Wl,-rpath-link,"$rootfs/usr/lib/aarch64-linux-gnu" \
	-o "$temporary/settings-state-test"

qemu-aarch64-static -L "$rootfs" "$temporary/settings-state-test"
