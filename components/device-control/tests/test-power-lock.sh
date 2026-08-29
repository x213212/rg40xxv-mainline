#!/usr/bin/env bash
set -euo pipefail

TEST_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(cd -- "$TEST_DIR/.." && pwd)
FIXTURE=$(mktemp -d /tmp/rg40xxv-device-control-test.power.XXXXXX)
ROOTFS="$FIXTURE/rootfs"
BACKLIGHT_DEVICE="$ROOTFS/sys/devices/platform/display/backlight/lcd-backlight"
LED_DEVICE_ROOT="$ROOTFS/sys/devices/platform/led-controller/leds"
trap 'rm -rf -- "$FIXTURE"' EXIT

mkdir -p \
    "$ROOTFS/sys/class/backlight" \
    "$ROOTFS/sys/class/leds" \
    "$BACKLIGHT_DEVICE" \
    "$LED_DEVICE_ROOT/joystick-left-device" \
    "$LED_DEVICE_ROOT/joystick-right-device" \
    "$ROOTFS/sys/class/drm/card0/device/power" \
    "$ROOTFS/run"
ln -s ../../devices/platform/display/backlight/lcd-backlight \
    "$ROOTFS/sys/class/backlight/lcd-backlight"
ln -s ../../devices/platform/led-controller/leds/joystick-left-device \
    "$ROOTFS/sys/class/leds/joystick-left"
ln -s ../../devices/platform/led-controller/leds/joystick-right-device \
    "$ROOTFS/sys/class/leds/joystick-right"
printf '80' >"$ROOTFS/sys/class/backlight/lcd-backlight/brightness"
printf '100' >"$ROOTFS/sys/class/backlight/lcd-backlight/max_brightness"
printf '0' >"$ROOTFS/sys/class/backlight/lcd-backlight/bl_power"
printf '25' >"$ROOTFS/sys/class/leds/joystick-left/brightness"
printf '255' >"$ROOTFS/sys/class/leds/joystick-left/max_brightness"
printf '40' >"$ROOTFS/sys/class/leds/joystick-right/brightness"
printf '255' >"$ROOTFS/sys/class/leds/joystick-right/max_brightness"
printf 'on' >"$ROOTFS/sys/class/drm/card0/device/power/control"

export DEVICE_CONTROL_TESTING=1
export DEVICE_CONTROL_TEST_ROOT="$FIXTURE"
CTL="$ROOT/power-lock/power-lockctl"

at() {
    POWER_LOCK_FAKE_NOW_MS=$1 "$CTL" "${@:2}"
}

status_has() {
    local output
    output=$(at "$1" status)
    grep -q "$2" <<<"$output"
}

at 1000 event power-down >/dev/null
[[ $(at 1150 event power-up) == ACTION_SCREEN_OFF_LOCKED ]]
[[ $(<"$ROOTFS/sys/class/backlight/lcd-backlight/brightness") == 0 ]]
[[ $(<"$ROOTFS/sys/class/backlight/lcd-backlight/bl_power") == 4 ]]
[[ $(<"$ROOTFS/sys/class/leds/joystick-left/brightness") == 0 ]]
[[ $(<"$ROOTFS/sys/class/leds/joystick-right/brightness") == 0 ]]
[[ $(<"$ROOTFS/sys/class/drm/card0/device/power/control") == auto ]]
STATUS=$(at 1200 status)
grep -q '^mode=screen_off$' <<<"$STATUS"
grep -q '^ui=paused$' <<<"$STATUS"
grep -q '^suspend_to_ram=disabled$' <<<"$STATUS"

at 2000 event power-down >/dev/null
[[ $(at 2100 event power-up) == ACTION_SHOW_LOCK_SCREEN ]]
[[ $(<"$ROOTFS/sys/class/backlight/lcd-backlight/brightness") == 80 ]]
[[ $(<"$ROOTFS/sys/class/backlight/lcd-backlight/bl_power") == 0 ]]
[[ $(<"$ROOTFS/sys/class/leds/joystick-left/brightness") == 25 ]]
[[ $(<"$ROOTFS/sys/class/leds/joystick-right/brightness") == 40 ]]
[[ $(<"$ROOTFS/sys/class/drm/card0/device/power/control") == on ]]

[[ $(at 3000 event button A) == ACTION_UNLOCK_PROGRESS ]]
[[ $(at 3200 event button RESET) == NONE ]]
[[ $(at 3300 event button VOLUMEUP) == NONE ]]
[[ $(at 3400 event button A) == ACTION_UNLOCK_PROGRESS ]]
[[ $(at 3500 event button A) == ACTION_UNLOCKED ]]
status_has 3510 '^mode=awake$'

# 不同按鍵與 timeout 都會重新從 1 計算。
at 4000 event power-down >/dev/null
at 4100 event power-up >/dev/null
at 4200 event power-down >/dev/null
at 4300 event power-up >/dev/null
at 4400 event button A >/dev/null
at 4500 event button B >/dev/null
at 4600 event button A >/dev/null
at 6201 event button A >/dev/null
STATUS=$(at 6202 status)
grep -q '^mode=lock_screen$' <<<"$STATUS"
grep -q '^unlock_count=1$' <<<"$STATUS"

# 約 3 秒長按：2.5 秒出確認；B 取消後直到 power-up 都不再觸發。
at 7000 event power-down >/dev/null
[[ $(at 9499 tick) == NONE ]]
[[ $(at 9500 tick) == ACTION_SHOW_SHUTDOWN_CONFIRM ]]
[[ $(at 9600 event button B) == ACTION_CANCEL_SHUTDOWN ]]
[[ $(at 9700 tick) == NONE ]]
at 9800 event power-up >/dev/null
status_has 9810 '^mode=lock_screen$'

# 持續按滿 3 秒只回傳 orderly shutdown action，不直接關閉 host。
at 10000 event power-down >/dev/null
at 12500 tick >/dev/null
[[ $(at 13000 tick) == ACTION_REQUEST_ORDERLY_SHUTDOWN ]]
status_has 13001 '^mode=shutdown_requested$'

# timer 即使晚到（第一個 tick 已超過 3 秒），仍先顯示確認並保留取消窗。
LATE_FIXTURE=$(mktemp -d /tmp/rg40xxv-device-control-test.powerlate.XXXXXX)
mkdir -p "$LATE_FIXTURE/rootfs"
cp -a "$ROOTFS/sys" "$LATE_FIXTURE/rootfs/sys"
mkdir -p "$LATE_FIXTURE/rootfs/run"
export DEVICE_CONTROL_TEST_ROOT="$LATE_FIXTURE"
at 13100 event power-down >/dev/null
[[ $(at 16100 tick) == ACTION_SHOW_SHUTDOWN_CONFIRM ]]
[[ $(at 16200 event button B) == ACTION_CANCEL_SHUTDOWN ]]
at 16300 event power-up >/dev/null
status_has 16301 '^mode=awake$'

# 關閉鎖定時，短按仍省電，但 state/API 是 light_sleep，喚醒直接 awake。
NEW_FIXTURE=$(mktemp -d /tmp/rg40xxv-device-control-test.powernolock.XXXXXX)
mkdir -p "$NEW_FIXTURE/rootfs"
cp -a "$ROOTFS/sys" "$NEW_FIXTURE/rootfs/sys"
mkdir -p "$NEW_FIXTURE/rootfs/run"
export DEVICE_CONTROL_TEST_ROOT="$NEW_FIXTURE"
at 14000 config set lock-enabled false >/dev/null
[[ $(at 14001 config get lock-enabled) == false ]]
at 14100 event power-down >/dev/null
[[ $(at 14200 event power-up) == ACTION_SCREEN_OFF_LIGHT_SLEEP ]]
status_has 14201 '^mode=light_sleep$'
status_has 14202 '^locked=no$'
at 14300 event power-down >/dev/null
[[ $(at 14400 event power-up) == ACTION_WAKE_RESTORED ]]
status_has 14401 '^mode=awake$'
[[ $(<"$NEW_FIXTURE/rootfs/sys/class/backlight/lcd-backlight/brightness") == 80 ]]

# 新 fixture 驗證寫入失敗會 rollback，維持 awake 與原亮度。
FAIL_FIXTURE=$(mktemp -d /tmp/rg40xxv-device-control-test.powerfail.XXXXXX)
trap 'rm -rf -- "$FIXTURE" "$LATE_FIXTURE" "$NEW_FIXTURE" "$FAIL_FIXTURE"' EXIT
mkdir -p "$FAIL_FIXTURE/rootfs"
cp -a "$ROOTFS/sys" "$FAIL_FIXTURE/rootfs/sys"
mkdir -p "$FAIL_FIXTURE/rootfs/run"
export DEVICE_CONTROL_TEST_ROOT="$FAIL_FIXTURE"
at 20000 event power-down >/dev/null
if POWER_LOCK_TEST_FAIL_STEP=backlight_brightness POWER_LOCK_FAKE_NOW_MS=20100 \
    "$CTL" event power-up >/dev/null 2>&1; then
    printf 'FAIL: 注入背光寫入失敗時仍回報成功\n' >&2
    exit 1
fi
[[ $(<"$FAIL_FIXTURE/rootfs/sys/class/backlight/lcd-backlight/brightness") == 80 ]]
[[ $(<"$FAIL_FIXTURE/rootfs/sys/class/leds/joystick-left/brightness") == 25 ]]
status_has 20110 '^mode=awake$'

printf 'PASS power-lock：短按鎖定／喚醒、三連按、timeout、可取消長按、runtime idle、rollback\n'
