#!/usr/bin/env bash
set -euo pipefail

export LC_ALL=C

TEST_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(cd -- "$TEST_DIR/.." && pwd)
FIXTURE=$(mktemp -d /tmp/rg40xxv-device-control-test.power-monitor.XXXXXX)
ROOTFS="$FIXTURE/rootfs"
trap 'rm -rf -- "$FIXTURE"' EXIT

mkdir -p \
	"$ROOTFS/sys/class/power_supply/battery" \
	"$ROOTFS/sys/class/power_supply/axp20x-usb" \
	"$ROOTFS/sys/devices/system/cpu/cpufreq/policy0" \
	"$ROOTFS/sys/class/regulator/regulator.3" \
	"$ROOTFS/sys/class/devfreq/1800000.gpu/power" \
	"$ROOTFS/sys/class/devfreq/1400000.dmc/power" \
	"$ROOTFS/sys/class/backlight/backlight" \
	"$ROOTFS/sys/class/net/wlan0/statistics" \
	"$ROOTFS/proc/sys/kernel/random"

printf 'Battery\n' >"$ROOTFS/sys/class/power_supply/battery/type"
printf 'Charging\n' >"$ROOTFS/sys/class/power_supply/battery/status"
printf '92\n' >"$ROOTFS/sys/class/power_supply/battery/capacity"
printf '4184000\n' >"$ROOTFS/sys/class/power_supply/battery/voltage_now"
printf '898000\n' >"$ROOTFS/sys/class/power_supply/battery/current_now"
printf 'sentinel-must-not-be-read\n' >"$ROOTFS/sys/class/power_supply/battery/health"
printf 'sentinel-must-not-be-read\n' >"$ROOTFS/sys/class/power_supply/battery/uevent"

printf 'USB\n' >"$ROOTFS/sys/class/power_supply/axp20x-usb/type"
printf '1\n' >"$ROOTFS/sys/class/power_supply/axp20x-usb/online"
printf '1\n' >"$ROOTFS/sys/class/power_supply/axp20x-usb/present"
printf '4496000\n' >"$ROOTFS/sys/class/power_supply/axp20x-usb/voltage_now"
printf '1000000\n' >"$ROOTFS/sys/class/power_supply/axp20x-usb/input_current_limit"
printf 'DCP\n' >"$ROOTFS/sys/class/power_supply/axp20x-usb/usb_type"

printf '1416000\n' >"$ROOTFS/sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq"
printf 'schedutil\n' >"$ROOTFS/sys/devices/system/cpu/cpufreq/policy0/scaling_governor"
printf 'vdd-cpu\n' >"$ROOTFS/sys/class/regulator/regulator.3/name"
printf '1040000\n' >"$ROOTFS/sys/class/regulator/regulator.3/microvolts"

printf 'mali-gpu\n' >"$ROOTFS/sys/class/devfreq/1800000.gpu/name"
printf '420000000\n' >"$ROOTFS/sys/class/devfreq/1800000.gpu/cur_freq"
printf 'simple_ondemand\n' >"$ROOTFS/sys/class/devfreq/1800000.gpu/governor"
printf 'active\n' >"$ROOTFS/sys/class/devfreq/1800000.gpu/power/runtime_status"
printf 'dmc\n' >"$ROOTFS/sys/class/devfreq/1400000.dmc/name"
printf '792000000\n' >"$ROOTFS/sys/class/devfreq/1400000.dmc/cur_freq"
printf 'performance\n' >"$ROOTFS/sys/class/devfreq/1400000.dmc/governor"
printf 'active\n' >"$ROOTFS/sys/class/devfreq/1400000.dmc/power/runtime_status"

printf '1250\n' >"$ROOTFS/sys/class/backlight/backlight/actual_brightness"
printf '1250\n' >"$ROOTFS/sys/class/backlight/backlight/brightness"
printf '2499\n' >"$ROOTFS/sys/class/backlight/backlight/max_brightness"
printf '0\n' >"$ROOTFS/sys/class/backlight/backlight/bl_power"
printf 'up\n' >"$ROOTFS/sys/class/net/wlan0/operstate"
printf '123456\n' >"$ROOTFS/sys/class/net/wlan0/statistics/rx_bytes"
printf '654321\n' >"$ROOTFS/sys/class/net/wlan0/statistics/tx_bytes"
printf '1234.50 100.00\n' >"$ROOTFS/proc/uptime"
printf '0.25 0.20 0.10 1/100 1\n' >"$ROOTFS/proc/loadavg"
printf '11111111-2222-3333-4444-555555555555\n' >"$ROOTFS/proc/sys/kernel/random/boot_id"

MONITOR="$ROOT/power-monitor/rg40xxv-power-monitor"
export RG40XXV_POWER_MONITOR_ROOT="$ROOTFS"

before_hashes=$(find "$ROOTFS/sys" -type f -printf '%p\0' | sort -z | xargs -0 sha256sum)
before_mtimes=$(find "$ROOTFS/sys" -type f -printf '%p %T@\n' | sort)

once_output=$($MONITOR once)
grep -Fq 'battery capacity=92% status=Charging direction=charging voltage=4.184V raw_current=0.898A' <<<"$once_output"
grep -Fq 'usb online=1 present=1 type=DCP voltage=4.496V input_limit=1.000A' <<<"$once_output"
grep -Fq 'cpu policies=policy0:1416000:schedutil core_voltage=1.040V load1=0.25' <<<"$once_output"
grep -Fq 'gpu name=mali-gpu frequency_hz=420000000 governor=simple_ondemand runtime=active' <<<"$once_output"
grep -Fq 'display brightness=1250/2499 bl_power=0 wifi=up rx_bytes=123456 tx_bytes=654321' <<<"$once_output"
grep -Fq 'no wattage is calculated' <<<"$once_output"
if grep -Fq 'raw_power' <<<"$once_output"; then
	printf 'FAIL: 未校準 current_now 被換算成功率\n' >&2
	exit 1
fi

csv_output=$($MONITOR csv 1 1)
[[ $(wc -l <<<"$csv_output") == 2 ]]
[[ $(awk -F, 'NR == 1 { print NF }' <<<"$csv_output") == 27 ]]
[[ $(awk -F, 'NR == 2 { print NF }' <<<"$csv_output") == 27 ]]
grep -Fq '11111111-2222-3333-4444-555555555555,Charging,92,4184000,898000,RAW_UNCALIBRATED,charging' <<<"$csv_output"

watch_output=$($MONITOR watch 1 1)
[[ $(wc -l <<<"$watch_output") == 2 ]]

sources_output=$($MONITOR sources)
grep -Fq "battery.voltage_uv=$ROOTFS/sys/class/power_supply/battery/voltage_now" <<<"$sources_output"
grep -Fq 'warning.health=not-read-because-AXP717-health-read-clears-fault-bits' <<<"$sources_output"
if grep -Eq '/(health|uevent)$' <<<"$sources_output"; then
	printf 'FAIL: sources 暴露禁止輪詢的 health/uevent\n' >&2
	exit 1
fi

# Suspended GPU 的 cur_freq 可能只是最後一次值，不得冒充正在運作的頻率。
printf 'suspended\n' >"$ROOTFS/sys/class/devfreq/1800000.gpu/power/runtime_status"
suspended_output=$($MONITOR once)
grep -Fq 'gpu name=mali-gpu frequency_hz=NA governor=simple_ondemand runtime=suspended' <<<"$suspended_output"

# 缺值只影響該欄；不能變成 0，也不能沿用上一筆。
: >"$ROOTFS/sys/class/power_supply/battery/current_now"
missing_output=$($MONITOR once)
grep -Fq 'raw_current=NA' <<<"$missing_output"

# status 是方向的唯一可信來源；AXP717 raw current 的 sign 不得被程式翻轉。
printf 'Discharging\n' >"$ROOTFS/sys/class/power_supply/battery/status"
printf '%s\n' '-120000' >"$ROOTFS/sys/class/power_supply/battery/current_now"
discharge_output=$($MONITOR once)
grep -Fq 'status=Discharging direction=discharging voltage=4.184V raw_current=-0.120A' <<<"$discharge_output"

# attribute symlink 不可跟隨，即使目標可讀。
printf '7777777\n' >"$FIXTURE/outside-secret"
rm -f -- "$ROOTFS/sys/class/power_supply/battery/current_now"
ln -s "$FIXTURE/outside-secret" "$ROOTFS/sys/class/power_supply/battery/current_now"
escape_output=$($MONITOR once)
grep -Fq 'raw_current=NA' <<<"$escape_output"
if grep -Fq '7.777' <<<"$escape_output"; then
	printf 'FAIL: 跟隨 attribute symlink escape\n' >&2
	exit 1
fi

if $MONITOR watch 0 1 >/dev/null 2>&1; then
	printf 'FAIL: 接受 interval=0\n' >&2
	exit 1
fi
if $MONITOR csv NaN 1 >/dev/null 2>&1; then
	printf 'FAIL: 接受 interval=NaN\n' >&2
	exit 1
fi
if $MONITOR trend 1 2 >/dev/null 2>&1; then
	printf 'FAIL: trend 接受少於三個樣本\n' >&2
	exit 1
fi

after_hashes=$(find "$ROOTFS/sys" -type f -printf '%p\0' | sort -z | xargs -0 sha256sum)
after_mtimes=$(find "$ROOTFS/sys" -type f -printf '%p %T@\n' | sort)
# 上面測試本身刻意改過 fixture；重新取一個不變性基準，只覆蓋監控呼叫。
stable_hashes=$after_hashes
stable_mtimes=$after_mtimes
$MONITOR once >/dev/null
$MONITOR sources >/dev/null
[[ $(find "$ROOTFS/sys" -type f -printf '%p\0' | sort -z | xargs -0 sha256sum) == "$stable_hashes" ]]
[[ $(find "$ROOTFS/sys" -type f -printf '%p %T@\n' | sort) == "$stable_mtimes" ]]

[[ -n $before_hashes && -n $before_mtimes ]]
printf 'PASS power-monitor：Battery/USB/CPU/GPU/backlight/Wi-Fi、固定 CSV、缺值、方向、symlink、只讀契約\n'
