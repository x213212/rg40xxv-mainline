#!/bin/sh
set -eu

project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
workspace=${RG40XXV_WORKSPACE:-$(CDPATH= cd -- "$project/../../../.." && pwd -P)}
rootfs="$workspace/firmware/mnt/rootfs"
temporary=$(mktemp -d)
trap 'status=$?; trap - EXIT; rm -rf -- "$temporary"; exit "$status"' \
	EXIT HUP INT TERM

make_fixture()
{
	root=$1
	temperature=$2
	mkdir -p \
		"$root/sys/class/thermal/thermal_zone0" \
		"$root/sys/class/regulator" \
		"$root/sys/class/hwmon/hwmon0" \
		"$root/sys/class/power_supply/battery" \
		"$root/sys/class/backlight/panel0"
	printf '%s\n' cpu >"$root/sys/class/thermal/thermal_zone0/type"
	printf '%s\n' "$temperature" >"$root/sys/class/thermal/thermal_zone0/temp"
	printf '%s\n' 'CPU Vcore' >"$root/sys/class/hwmon/hwmon0/in0_label"
	printf '%s\n' 905 >"$root/sys/class/hwmon/hwmon0/in0_input"
	printf '%s\n' Battery >"$root/sys/class/power_supply/battery/type"
	printf '%s\n' 73 >"$root/sys/class/power_supply/battery/capacity"
	printf '%s\n' Charging >"$root/sys/class/power_supply/battery/status"
	printf '%s\n' 255 >"$root/sys/class/backlight/panel0/max_brightness"
	printf '%s\n' 128 >"$root/sys/class/backlight/panel0/actual_brightness"
	printf '%s\n' 128 >"$root/sys/class/backlight/panel0/brightness"
	printf '%s\n' 4 >"$root/sys/class/backlight/panel0/min_brightness"
	printf '%s\n' 0 >"$root/sys/class/backlight/panel0/bl_power"
}

fixture_a="$temporary/fixture-a"
fixture_b="$temporary/fixture-b"
binary="$temporary/hardware-discovery-cache-test"
loader="$rootfs/usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1"
library_path="$rootfs/usr/lib/aarch64-linux-gnu:$rootfs/usr/lib"

make_fixture "$fixture_a" 53750
make_fixture "$fixture_b" 48750

aarch64-linux-gnu-gcc-12 -std=c11 -O2 -Wall -Wextra -Werror \
	-I"$project/include" -I"$project/src" -idirafter "$rootfs/usr/include" \
	"$project/src/hardware.c" "$project/src/hardware_io.c" \
	"$project/src/hardware_platform.c" "$project/src/hardware_devices.c" \
	"$project/src/hardware_alsa.c" \
	"$project/tests/hardware_discovery_cache_test.c" \
	"$rootfs/usr/lib/aarch64-linux-gnu/libasound.so" \
	-Wl,-rpath-link,"$rootfs/usr/lib/aarch64-linux-gnu" -o "$binary"

qemu-aarch64-static "$loader" --library-path "$library_path" \
	"$binary" "$fixture_a" "$fixture_b"
