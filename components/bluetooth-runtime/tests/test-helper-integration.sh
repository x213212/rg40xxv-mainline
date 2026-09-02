#!/usr/bin/env bash
set -euo pipefail

project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
workspace=${RG40XXV_WORKSPACE:-$(CDPATH= cd -- "$project/../../../.." && pwd -P)}
rootfs=${RG40XXV_STOCK_ROOTFS:-$workspace/firmware/mnt/rootfs}
target_cc=${AARCH64_CC:-aarch64-linux-gnu-gcc-12}
host_cc=${HOST_CC:-cc}
temporary=$(mktemp -d)
daemon_pid=
service_pid=

cleanup()
{
	local status=$?

	trap - EXIT HUP INT TERM
	if [[ ${service_pid:-} =~ ^[0-9]+$ && $service_pid -gt 1 ]]; then
		kill "$service_pid" >/dev/null 2>&1 || :
		wait "$service_pid" >/dev/null 2>&1 || :
	fi
	if [[ ${daemon_pid:-} =~ ^[0-9]+$ && $daemon_pid -gt 1 ]]; then
		kill "$daemon_pid" >/dev/null 2>&1 || :
	fi
	rm -rf -- "$temporary"
	exit "$status"
}
trap cleanup EXIT HUP INT TERM

for tool in "$target_cc" "$host_cc" dbus-daemon qemu-aarch64-static; do
	command -v "$tool" >/dev/null 2>&1 || {
		printf 'missing helper integration dependency: %s\n' "$tool" >&2
		exit 1
	}
done

fake_ctl=$temporary/fake-bluetoothctl
fake_service=$temporary/fake-bluez
helper=$temporary/rg40xxv-bluetooth-control
ready=$temporary/bluez.ready
pair_marker=$fake_ctl.marker
definition=$(printf '%s' \
	'-DRG40XXV_BLUETOOTHCTL_PATH="'"$fake_ctl"'"')

install -m 0755 "$project/tests/fake-bluetoothctl.sh" "$fake_ctl"
"$host_cc" -std=c11 -O1 -g -Wall -Wextra -Werror -Wformat=2 -Wshadow \
	-I"$rootfs/usr/include/dbus-1.0" \
	-I"$rootfs/usr/lib/aarch64-linux-gnu/dbus-1.0/include" \
	"$project/tests/fake_bluez.c" /lib/x86_64-linux-gnu/libdbus-1.so.3 \
	-o "$fake_service"
"$target_cc" -std=c11 -O1 -g -Wall -Wextra -Werror -Wformat=2 -Wshadow \
	"$definition" -DRG40XXV_DISCOVERY_SECONDS=0 \
	-I"$project/src" -I"$rootfs/usr/include/dbus-1.0" \
	-I"$rootfs/usr/lib/aarch64-linux-gnu/dbus-1.0/include" \
	"$project/src/rg40xxv-bluetooth-control.c" \
	"$project/src/bluetooth_model.c" \
	"$rootfs/usr/lib/aarch64-linux-gnu/libdbus-1.so.3.19.13" \
	-Wl,-rpath-link,"$rootfs/usr/lib/aarch64-linux-gnu" \
	-o "$helper"

install -d -m 0700 "$temporary/runtime"
daemon_info=$(XDG_RUNTIME_DIR=$temporary/runtime \
	dbus-daemon --session --fork --print-address=1 --print-pid=1)
bus_address=$(printf '%s\n' "$daemon_info" | sed -n '1p')
daemon_pid=$(printf '%s\n' "$daemon_info" | sed -n '2p')
[[ $bus_address == unix:* ]]
[[ $daemon_pid =~ ^[0-9]+$ && $daemon_pid -gt 1 ]]

DBUS_SYSTEM_BUS_ADDRESS=$bus_address \
	"$fake_service" "$ready" "$pair_marker" &
service_pid=$!
for unused in {1..100}; do
	[[ -f $ready ]] && break
	kill -0 "$service_pid" 2>/dev/null
	sleep 0.01
done
[[ -f $ready ]]

run_helper()
{
	local output=$1
	shift
	DBUS_SYSTEM_BUS_ADDRESS=$bus_address \
		qemu-aarch64-static -L "$rootfs" "$helper" "$@" >"$output"
}

run_helper "$temporary/status" status
grep -Fqx $'RG40XXV_BLUETOOTH_SNAPSHOT\t2' "$temporary/status"
grep -Fqx $'A\t1\t1\t0\t10:22:33:44:55:66' "$temporary/status"
grep -Fqx $'D\tAA:BB:CC:DD:EE:FF\tGamepad%20%E6%89%8B%E6%8A%8A\tcontroller\t0\t0\t0\t-42' \
	"$temporary/status"
grep -Fqx $'D\t11:22:33:44:55:66\tHeadset%20%E8%80%B3%E6%A9%9F\taudio\t1\t1\t0\t-36' \
	"$temporary/status"

run_helper "$temporary/scan" scan
grep -Fqx $'A\t1\t1\t0\t10:22:33:44:55:66' "$temporary/scan"
run_helper "$temporary/power-off" power off
grep -Fqx $'A\t1\t0\t0\t10:22:33:44:55:66' "$temporary/power-off"
run_helper "$temporary/power-on" power on
grep -Fqx $'A\t1\t1\t0\t10:22:33:44:55:66' "$temporary/power-on"

run_helper "$temporary/pair" pair aa:bb:cc:dd:ee:ff
grep -Fqx $'D\tAA:BB:CC:DD:EE:FF\tGamepad%20%E6%89%8B%E6%8A%8A\tcontroller\t1\t1\t1\t-42' \
	"$temporary/pair"
grep -Fqx -- $'--timeout\t90\tpair\tAA:BB:CC:DD:EE:FF' \
	"$fake_ctl.invocation"

run_helper "$temporary/connect-headset" connect 11:22:33:44:55:66
grep -Fqx $'D\t11:22:33:44:55:66\tHeadset%20%E8%80%B3%E6%A9%9F\taudio\t1\t1\t1\t-36' \
	"$temporary/connect-headset"
run_helper "$temporary/connect" connect AA:BB:CC:DD:EE:FF
grep -Fqx $'D\tAA:BB:CC:DD:EE:FF\tGamepad%20%E6%89%8B%E6%8A%8A\tcontroller\t1\t1\t1\t-42' \
	"$temporary/connect"
grep -Fqx $'D\t11:22:33:44:55:66\tHeadset%20%E8%80%B3%E6%A9%9F\taudio\t1\t1\t1\t-36' \
	"$temporary/connect"
run_helper "$temporary/disconnect" disconnect AA:BB:CC:DD:EE:FF
grep -Fqx $'D\tAA:BB:CC:DD:EE:FF\tGamepad%20%E6%89%8B%E6%8A%8A\tcontroller\t1\t1\t0\t-42' \
	"$temporary/disconnect"
grep -Fqx $'D\t11:22:33:44:55:66\tHeadset%20%E8%80%B3%E6%A9%9F\taudio\t1\t1\t1\t-36' \
	"$temporary/disconnect"
run_helper "$temporary/forget" forget AA:BB:CC:DD:EE:FF
if grep -Fq $'D\tAA:BB:CC:DD:EE:FF\t' "$temporary/forget"; then
	printf '%s\n' 'forgotten device remained in helper snapshot' >&2
	exit 1
fi
grep -Fqx $'D\t11:22:33:44:55:66\tHeadset%20%E8%80%B3%E6%A9%9F\taudio\t1\t1\t1\t-36' \
	"$temporary/forget"

set +e
qemu-aarch64-static -L "$rootfs" "$helper" connect AA:BB:CC:DD:EE:GG \
	>"$temporary/invalid.stdout" 2>"$temporary/invalid.stderr"
invalid_status=$?
set -e
[[ $invalid_status == 64 ]]
grep -Fq 'usage:' "$temporary/invalid.stderr"

printf 'BLUETOOTH_HELPER_INTEGRATION_TEST PASS\n'
