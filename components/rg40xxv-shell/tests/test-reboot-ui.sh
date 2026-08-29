#!/bin/sh
set -eu

project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
workspace=$(CDPATH= cd -- "$project/../.." && pwd -P)
rootfs="$workspace/firmware/mnt/rootfs"
loader="$rootfs/usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1"
library_path="$rootfs/usr/lib/aarch64-linux-gnu:$rootfs/usr/lib"
temporary=$(mktemp -d)
trap 'status=$?; trap - EXIT; rm -rf -- "$temporary"; exit "$status"' \
	EXIT HUP INT TERM
binary="$temporary/reboot-ui-test"

aarch64-linux-gnu-gcc-12 -std=c11 -O2 -Wall -Wextra -Werror \
	-I"$project/include" -I"$workspace/services/netstream/include" \
	-idirafter "$rootfs/usr/include" \
	-I"$rootfs/usr/include/SDL2" \
	"$project/tests/reboot_ui_test.c" "$project/src/settings_ui.c" \
	-o "$binary"

qemu-aarch64-static "$loader" --library-path "$library_path" \
	"$binary" >"$temporary/stdout" 2>"$temporary/stderr"

grep -Fq 'REBOOT_UI_TEST PASS' "$temporary/stdout"
grep -Fq 'UI_HARDWARECTL_RESULT command=8 value=0 spawn_error=0 exit=23 signal=0' \
	"$temporary/stderr"
grep -Fq 'UI_HARDWARECTL_STATUS restart_mode · hardware_failed · launch_exit_code 23' \
	"$temporary/stderr"
grep -Fq 'UI_HARDWARECTL_STATUS restart_mode · hardware_applied' \
	"$temporary/stderr"

printf '%s\n' 'Normal reboot confirmation and feedback UI: PASS'
