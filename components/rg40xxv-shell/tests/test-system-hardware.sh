#!/bin/sh
set -eu

project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
workspace=$(CDPATH= cd -- "$project/../.." && pwd -P)
rootfs="$workspace/firmware/mnt/rootfs"
temporary=$(mktemp -d)
trap 'status=$?; trap - EXIT; rm -rf -- "$temporary"; exit "$status"' \
	EXIT HUP INT TERM

valid="$temporary/valid"
hwmon="$temporary/hwmon"
bad="$temporary/bad"
binary="$temporary/hardware-fixture-test"
rom_root="$temporary/roms"
state_dir="$temporary/netstream"
screenshot="$temporary/system-info.bmp"
ui_stdout="$temporary/ui-stdout"
ui_stderr="$temporary/ui-stderr"
loader="$rootfs/usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1"
library_path="$rootfs/usr/lib/aarch64-linux-gnu:$rootfs/usr/lib"

mkdir -p \
	"$valid/run/rg40xxv-ui" "$valid/run/systemd/timesync" \
	"$valid/proc/sys/kernel" "$valid/proc/net" \
	"$valid/sys/class/net/wlan0" \
	"$valid/sys/class/power_supply/battery" \
	"$valid/sys/class/backlight/panel0" \
	"$valid/sys/class/regulator/regulator.0" \
	"$valid/sys/devices/system/cpu/cpufreq/policy0" \
	"$valid/sys/class/thermal/thermal_zone0"
printf '%s\n' 1787632440 >"$valid/run/rg40xxv-ui/time.epoch"
: >"$valid/run/systemd/timesync/synchronized"
printf '%s\n' 'volume_percent=64' 'muted=0' \
	>"$valid/run/rg40xxv-ui/alsa-volume"
printf '%s\n' '7.2.0-rg40xxv-fixture' \
	>"$valid/proc/sys/kernel/osrelease"
printf '%s\n' '#1 SMP PREEMPT fixture' >"$valid/proc/sys/kernel/version"
printf '%s\n' 'console=tty0 boot_slot=a' >"$valid/proc/cmdline"
cat >"$valid/proc/meminfo" <<'EOF'
MemTotal:       1024000 kB
MemAvailable:    256000 kB
Cached:           64000 kB
SReclaimable:      8000 kB
SwapTotal:        32768 kB
SwapFree:         24576 kB
EOF
cat >"$valid/proc/net/wireless" <<'EOF'
Inter-| sta-|   Quality        |   Discarded packets               | Missed | WE
 face | tus | link level noise |  nwid  crypt   frag  retry   misc | beacon | 22
 wlan0: 0000   63.  -41.  -95.        0      0      0      0      0        0
EOF
printf '%s\n' up >"$valid/sys/class/net/wlan0/operstate"
printf '%s\n' Battery >"$valid/sys/class/power_supply/battery/type"
printf '%s\n' 73 >"$valid/sys/class/power_supply/battery/capacity"
printf '%s\n' Charging >"$valid/sys/class/power_supply/battery/status"
printf '%s\n' 255 >"$valid/sys/class/backlight/panel0/max_brightness"
printf '%s\n' 128 >"$valid/sys/class/backlight/panel0/actual_brightness"
printf '%s\n' 128 >"$valid/sys/class/backlight/panel0/brightness"
printf '%s\n' 4 >"$valid/sys/class/backlight/panel0/min_brightness"
printf '%s\n' 0 >"$valid/sys/class/backlight/panel0/bl_power"
printf '%s\n' vdd-cpu >"$valid/sys/class/regulator/regulator.0/name"
printf '%s\n' 920000 >"$valid/sys/class/regulator/regulator.0/microvolts"
printf '%s\n' 1416000 \
	>"$valid/sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq"
printf '%s\n' cpu >"$valid/sys/class/thermal/thermal_zone0/type"
printf '%s\n' 53750 >"$valid/sys/class/thermal/thermal_zone0/temp"
printf '%s\n' '0.12 0.07 0.03 1/100 1' >"$valid/proc/loadavg"

mkdir -m 0700 "$rom_root" "$state_dir"
printf '%s\n' 'fixture-wifi-password-MUST-NOT-APPEAR' >"$state_dir/wifi.v1"
chmod 0600 "$state_dir/wifi.v1"

mkdir -p "$hwmon/sys/class/hwmon/hwmon0"
printf '%s\n' 'CPU Vcore' >"$hwmon/sys/class/hwmon/hwmon0/in0_label"
printf '%s\n' 905 >"$hwmon/sys/class/hwmon/hwmon0/in0_input"

mkdir -p "$bad/proc/sys/kernel" "$bad/proc/net" \
	"$bad/run/rg40xxv-ui" \
	"$bad/sys/kernel/debug/opp/cpu/opp:1416000000/supply-0"
printf '%0600d\n' 0 >"$bad/proc/sys/kernel/osrelease"
mkfifo "$bad/proc/meminfo" "$bad/proc/net/wireless" \
	"$bad/run/rg40xxv-ui/alsa-volume"
printf '%s\n' 1416000000 \
	>"$bad/sys/kernel/debug/opp/cpu/opp:1416000000/rate_hz"
printf '%s\n' 1100000 \
	>"$bad/sys/kernel/debug/opp/cpu/opp:1416000000/supply-0/u_volt_target"

aarch64-linux-gnu-gcc-12 -std=c11 -O2 -Wall -Wextra -Werror \
	-I"$project/include" -I"$project/src" -idirafter "$rootfs/usr/include" \
	"$project/src/hardware.c" "$project/src/hardware_io.c" \
	"$project/src/hardware_platform.c" "$project/src/hardware_devices.c" \
	"$project/src/hardware_alsa.c" "$project/tests/hardware_fixture_test.c" \
	"$rootfs/usr/lib/aarch64-linux-gnu/libasound.so" \
	-Wl,-rpath-link,"$rootfs/usr/lib/aarch64-linux-gnu" -o "$binary"

qemu-aarch64-static "$loader" --library-path "$library_path" \
	"$binary" "$valid" valid
qemu-aarch64-static "$loader" --library-path "$library_path" \
	"$binary" "$hwmon" hwmon
timeout 3 qemu-aarch64-static "$loader" --library-path "$library_path" \
	"$binary" "$bad" unavailable

SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
	qemu-aarch64-static "$loader" --library-path "$library_path" \
	"$project/build/rg40xxv-shell" --windowed \
	--font "$project/assets/RG40XXV-UI-Sans.otf" \
	--rom-root "$rom_root" --state-dir "$state_dir" \
	--hardware-root "$valid" --settings-preview \
	--settings-file "$temporary/settings.conf" \
	--filter-state "$temporary/filters.conf" \
	--history-file "$temporary/history.tsv" \
	--favorites-file "$temporary/favorites.tsv" \
	--screenshot "$screenshot" >"$ui_stdout" 2>"$ui_stderr"

grep -Fq 'UI_RESULT PASS' "$ui_stdout"
grep -Fq 'SYSTEM_RESULT PASS taipei=2026/08/25 12:34 kernel=7.2.0-rg40xxv-fixture ram_used=786432000 ram_total=1048576000 wifi=up signal=90 battery=73 battery_status=charging backlight=50 volume=64 muted=0 voltage_uv=920000 voltage_source=regulator' "$ui_stdout"
if grep -Fq 'fixture-wifi-password-MUST-NOT-APPEAR' \
	"$ui_stdout" "$ui_stderr"; then
	printf '%s\n' 'system page leaked a Wi-Fi secret' >&2
	exit 1
fi
test -s "$screenshot"

printf '%s\n' 'Bounded read-only hardware fixtures: PASS'
