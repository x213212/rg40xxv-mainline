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
    "$ROOTFS/sys/devices/system/cpu/cpufreq/policy0" \
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
printf 'performance powersave schedutil' \
    >"$ROOTFS/sys/devices/system/cpu/cpufreq/policy0/scaling_available_governors"
printf 'schedutil' >"$ROOTFS/sys/devices/system/cpu/cpufreq/policy0/scaling_governor"
printf '480000 1008000 1416000' \
    >"$ROOTFS/sys/devices/system/cpu/cpufreq/policy0/scaling_available_frequencies"
printf '0 1 2 3' >"$ROOTFS/sys/devices/system/cpu/cpufreq/policy0/related_cpus"

export DEVICE_CONTROL_TESTING=1
export DEVICE_CONTROL_TEST_ROOT="$FIXTURE"
CTL="$ROOT/power-lock/power-lockctl"

# 單獨調背光只能依賴背光節點，不得因不相關 RGB/GPU 缺件失敗。
mv -- "$ROOTFS/sys/class/leds" "$ROOTFS/sys/class/leds.unavailable"
mv -- "$ROOTFS/sys/class/drm/card0/device/power/control" \
    "$ROOTFS/sys/class/drm/card0/device/power/control.unavailable"
POWER_LOCK_FAKE_NOW_MS=500 "$CTL" hardware brightness 25 >/dev/null
[[ $(<"$ROOTFS/sys/class/backlight/lcd-backlight/brightness") == 25 ]]
mv -- "$ROOTFS/sys/class/drm/card0/device/power/control.unavailable" \
    "$ROOTFS/sys/class/drm/card0/device/power/control"
mv -- "$ROOTFS/sys/class/leds.unavailable" "$ROOTFS/sys/class/leds"
printf '80' >"$ROOTFS/sys/class/backlight/lcd-backlight/brightness"

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
[[ $(<"$ROOTFS/sys/devices/system/cpu/cpufreq/policy0/scaling_governor") == powersave ]]
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
[[ $(<"$ROOTFS/sys/devices/system/cpu/cpufreq/policy0/scaling_governor") == schedutil ]]

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
[[ $(<"$FAIL_FIXTURE/rootfs/sys/devices/system/cpu/cpufreq/policy0/scaling_governor") == schedutil ]]
status_has 20110 '^mode=awake$'

# UI crash/restart recovery does not synthesize another power-key press.  It
# restores the saved outputs and leaves the new UI in a deterministic awake
# state; a second recovery is a no-op.
RECOVER_FIXTURE=$(mktemp -d /tmp/rg40xxv-device-control-test.powerrecover.XXXXXX)
trap 'rm -rf -- "$FIXTURE" "$LATE_FIXTURE" "$NEW_FIXTURE" "$FAIL_FIXTURE" "$RECOVER_FIXTURE"' EXIT
mkdir -p "$RECOVER_FIXTURE/rootfs"
cp -a "$ROOTFS/sys" "$RECOVER_FIXTURE/rootfs/sys"
mkdir -p "$RECOVER_FIXTURE/rootfs/run"
export DEVICE_CONTROL_TEST_ROOT="$RECOVER_FIXTURE"
at 30000 hardware screen-off >/dev/null
status_has 30001 '^mode=screen_off$'
[[ $(at 30002 hardware recover-awake) == ACTION_WAKE_RESTORED ]]
status_has 30003 '^mode=awake$'
[[ $(<"$RECOVER_FIXTURE/rootfs/sys/devices/system/cpu/cpufreq/policy0/scaling_governor") == schedutil ]]
[[ $(at 30004 hardware recover-awake) == ACTION_RECOVERY_NOT_NEEDED ]]

# CPU governor 還原暫時失敗不得把使用者困在黑屏；顯示先喚醒、
# powersave 快照保留，recover-awake 下次再精確還原。
WAKE_CPU_FIXTURE=$(mktemp -d /tmp/rg40xxv-device-control-test.powerwakecpu.XXXXXX)
trap 'rm -rf -- "$FIXTURE" "$LATE_FIXTURE" "$NEW_FIXTURE" "$FAIL_FIXTURE" "$RECOVER_FIXTURE" "$WAKE_CPU_FIXTURE"' EXIT
mkdir -p "$WAKE_CPU_FIXTURE/rootfs"
cp -a "$ROOTFS/sys" "$WAKE_CPU_FIXTURE/rootfs/sys"
mkdir -p "$WAKE_CPU_FIXTURE/rootfs/run"
export DEVICE_CONTROL_TEST_ROOT="$WAKE_CPU_FIXTURE"
at 40000 hardware screen-off >/dev/null
CPU_POLICY_TEST_FAIL_RESTORE_POLICY=policy0 \
    at 40001 hardware screen-on >/dev/null 2>&1
[[ $(<"$WAKE_CPU_FIXTURE/rootfs/sys/class/backlight/lcd-backlight/bl_power") == 0 ]]
[[ $(<"$WAKE_CPU_FIXTURE/rootfs/sys/devices/system/cpu/cpufreq/policy0/scaling_governor") == powersave ]]
[[ -f "$WAKE_CPU_FIXTURE/rootfs/run/rg40xxv/cpu-policy/sleep-governors.v1" ]]
status_has 40002 '^mode=lock_screen$'
[[ $(at 40003 hardware recover-awake) == ACTION_RECOVERY_NOT_NEEDED ]]
[[ $(<"$WAKE_CPU_FIXTURE/rootfs/sys/devices/system/cpu/cpufreq/policy0/scaling_governor") == schedutil ]]
[[ ! -e "$WAKE_CPU_FIXTURE/rootfs/run/rg40xxv/cpu-policy/sleep-governors.v1" ]]

printf 'PASS power-lock：command-scoped sysfs、短按鎖定／喚醒、CPU powersave 還原、三連按、timeout、可取消長按、runtime idle、crash recovery、rollback\n'
