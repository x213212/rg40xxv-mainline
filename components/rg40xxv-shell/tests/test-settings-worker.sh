#!/bin/sh
set -eu

project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
workspace=$(CDPATH= cd -- "$project/../.." && pwd -P)
temporary=$(mktemp -d)
trap 'status=$?; trap - EXIT; rm -rf -- "$temporary"; exit "$status"' \
	EXIT HUP INT TERM

rootfs="$workspace/firmware/mnt/rootfs"
loader="$rootfs/usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1"
library_path="$rootfs/usr/lib/aarch64-linux-gnu:$rootfs/usr/lib"
helper="$temporary/fake-hardwarectl"
binary="$temporary/settings-worker-test"
log="$temporary/hardwarectl.log"
log_link="$temporary/hardwarectl-link.log"
link_target="$temporary/must-not-change"
fifo_log="$temporary/non-regular-log"

cp "$project/tests/fake-hardwarectl.fixture" "$helper"
chmod 0755 "$helper"
printf '%s\n' 'unchanged' >"$link_target"
mkfifo "$fifo_log"

aarch64-linux-gnu-gcc-12 -std=c11 -O2 -Wall -Wextra -Werror \
	-I"$project/include" -idirafter "$rootfs/usr/include" \
	-I"$rootfs/usr/include/SDL2" \
	"$project/tests/settings_worker_test.c" \
	"$project/src/settings.c" "$project/src/launcher.c" \
	"$rootfs/usr/lib/aarch64-linux-gnu/libSDL2.so" \
	-Wl,-rpath-link,"$rootfs/usr/lib/aarch64-linux-gnu" \
	-o "$binary"

if ! qemu-aarch64-static "$loader" --library-path "$library_path" \
	"$binary" "$helper" "$log" "$log_link" "$link_target" \
	"$fifo_log" \
	>"$temporary/stdout" 2>"$temporary/stderr"; then
	cat "$temporary/stdout" "$temporary/stderr" >&2
	exit 1
fi

grep -Fq 'SETTINGS_WORKER_TEST PASS' "$temporary/stdout"
test ! -e "${helper}.env-leak"
test ! -e "${helper}.concurrent"
test "$(cat "$link_target")" = unchanged
test -L "$log_link"
test "$(stat -c '%a' "$log")" = 600

awk '
	$0 == "BEGIN" { command++; token = 0; next }
	$0 == "END" { next }
	{ token++; values[command "," token] = $0 }
	END {
		if (command != 19) exit 1
		if (values["1,1"] != "<screen-off>") exit 1
		if (values["2,1"] != "<screen-on>") exit 1
		if (values["3,1"] != "<brightness>" || values["3,2"] != "<0>") exit 1
		if (values["4,1"] != "<brightness>" || values["4,2"] != "<100>") exit 1
		if (values["5,1"] != "<joystick-rgb>" || values["5,2"] != "<100>") exit 1
		if (values["6,1"] != "<usb-debug>" || values["6,2"] != "<off>") exit 1
		if (values["7,1"] != "<orderly-shutdown>") exit 1
		if (values["8,1"] != "<reboot-custom>" || values["8,2"] != "") exit 1
		if (values["9,1"] != "<volume>" || values["9,2"] != "<35>") exit 1
		if (values["10,1"] != "<mute-toggle>") exit 1
		if (values["11,1"] != "<brightness>" || values["11,2"] != "<13>") exit 1
		if (values["12,1"] != "<brightness>" || values["12,2"] != "<97>") exit 1
		if (values["13,1"] != "<mute-toggle>") exit 1
		if (values["14,1"] != "<joystick-rgb>" || values["14,2"] != "<63>") exit 1
		if (values["15,1"] != "<usb-debug>" || values["15,2"] != "<on>") exit 1
		if (values["16,1"] != "<mute-toggle>") exit 1
		if (values["17,1"] != "<orderly-shutdown>") exit 1
		if (values["18,1"] != "<brightness>" || values["18,2"] != "<98>") exit 1
		if (values["19,1"] != "<brightness>" || values["19,2"] != "<99>") exit 1
	}
' "${helper}.capture"

if grep -Eq 'DEVICE_CONTROL_TESTING|DEVICE_CONTROL_TEST_ROOT|LD_PRELOAD|LD_LIBRARY_PATH' \
	"${helper}.capture" "$log"; then
	printf '%s\n' 'sensitive parent environment reached hardware helper' >&2
	exit 1
fi

printf '%s\n' 'Serialized nonblocking hardwarectl worker: PASS'
