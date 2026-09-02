#!/usr/bin/env bash
set -euo pipefail

TEST_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(cd -- "$TEST_DIR/.." && pwd)
FIXTURE=$(mktemp -d /tmp/rg40xxv-device-control-test.ui-hardware.XXXXXX)
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
    "$LED_DEVICE_ROOT/led-rgb" \
    "$LED_DEVICE_ROOT/status-green" \
    "$LED_DEVICE_ROOT/power-red" \
    "$LED_DEVICE_ROOT/status-joystick" \
    "$LED_DEVICE_ROOT/power-stick" \
    "$ROOTFS/sys/class/drm/card0/device/power" \
    "$ROOTFS/sys/devices/system/cpu/cpufreq/policy0" \
    "$ROOTFS/run" \
    "$FIXTURE/configfs" \
    "$FIXTURE/sys/class/udc" \
    "$FIXTURE/sys/class/net" \
    "$FIXTURE/sys/devices/platform/sunxi-test-udc" \
    "$FIXTURE/sys/devices/virtual/net/usb0" \
    "$FIXTURE/dev" \
    "$FIXTURE/mock-bin"

ln -s ../../devices/platform/display/backlight/lcd-backlight \
    "$ROOTFS/sys/class/backlight/lcd-backlight"
ln -s ../../devices/platform/led-controller/leds/joystick-left-device \
    "$ROOTFS/sys/class/leds/joystick-left"
ln -s ../../devices/platform/led-controller/leds/joystick-right-device \
    "$ROOTFS/sys/class/leds/joystick-right"
ln -s ../../devices/platform/led-controller/leds/led-rgb \
    "$ROOTFS/sys/class/leds/rgb:kbd_backlight"
ln -s ../../devices/platform/led-controller/leds/status-green \
    "$ROOTFS/sys/class/leds/status:green"
ln -s ../../devices/platform/led-controller/leds/power-red \
    "$ROOTFS/sys/class/leds/power:red"
ln -s ../../devices/platform/led-controller/leds/status-joystick \
    "$ROOTFS/sys/class/leds/status:joystick"
ln -s ../../devices/platform/led-controller/leds/power-stick \
    "$ROOTFS/sys/class/leds/power:stick"
ln -s ../../devices/platform/sunxi-test-udc \
    "$FIXTURE/sys/class/udc/sunxi-test-udc"
ln -s ../../devices/virtual/net/usb0 "$FIXTURE/sys/class/net/usb0"
: >"$FIXTURE/dev/ttyGS0"

printf '80' >"$ROOTFS/sys/class/backlight/lcd-backlight/brightness"
printf '200' >"$ROOTFS/sys/class/backlight/lcd-backlight/max_brightness"
printf '5' >"$ROOTFS/sys/class/backlight/lcd-backlight/min_brightness"
printf '0' >"$ROOTFS/sys/class/backlight/lcd-backlight/bl_power"
for led in joystick-left joystick-right rgb:kbd_backlight; do
    printf '25' >"$ROOTFS/sys/class/leds/$led/brightness"
    printf '255' >"$ROOTFS/sys/class/leds/$led/max_brightness"
done
for led in status:green power:red status:joystick power:stick; do
    printf '99' >"$ROOTFS/sys/class/leds/$led/brightness"
    printf '255' >"$ROOTFS/sys/class/leds/$led/max_brightness"
done
printf 'on' >"$ROOTFS/sys/class/drm/card0/device/power/control"
printf 'schedutil' >"$ROOTFS/sys/devices/system/cpu/cpufreq/policy0/scaling_governor"
printf 'performance powersave schedutil' \
    >"$ROOTFS/sys/devices/system/cpu/cpufreq/policy0/scaling_available_governors"
printf '480000 1008000 1512000' \
    >"$ROOTFS/sys/devices/system/cpu/cpufreq/policy0/scaling_available_frequencies"
printf '0 1 2 3' >"$ROOTFS/sys/devices/system/cpu/cpufreq/policy0/related_cpus"

MOCK_SYSTEMCTL="$FIXTURE/mock-bin/systemctl"
{
    printf '#!/usr/bin/env bash\n'
    printf 'set -euo pipefail\n'
    printf '[[ ${DEVICE_CONTROL_TESTING:-0} == 1 ]]\n'
    printf '[[ $# == 1 && $1 == poweroff ]]\n'
    printf 'printf "%%s\\n" "$1" >>"$DEVICE_CONTROL_TEST_ROOT/rootfs/run/mock-systemctl.argv"\n'
} >"$MOCK_SYSTEMCTL"
chmod +x "$MOCK_SYSTEMCTL"

MOCK_VOLUME_CTL="$FIXTURE/mock-bin/rg40xxv-volume-ctl"
{
	printf '#!/usr/bin/env bash\n'
	printf 'set -euo pipefail\n'
	printf '[[ ${DEVICE_CONTROL_TESTING:-0} == 1 ]]\n'
	printf 'case "$*" in\n'
	printf '  "set "[0-9]|"set "[0-9][0-9]|"set 100"|up|down|mute-toggle) ;;\n'
	printf '  *) exit 2 ;;\n'
	printf 'esac\n'
	printf 'printf "%%s\\n" "$*" >>"$DEVICE_CONTROL_TEST_ROOT/rootfs/run/mock-volume.argv"\n'
} >"$MOCK_VOLUME_CTL"
chmod +x "$MOCK_VOLUME_CTL"

MOCK_NETWORK_CTL="$FIXTURE/mock-bin/rg40xxv-network-control"
{
	printf '#!/usr/bin/env bash\n'
	printf 'set -euo pipefail\n'
	printf '[[ ${DEVICE_CONTROL_TESTING:-0} == 1 ]]\n'
	printf 'case "${1:-}" in\n'
	printf '  status|recover|scan|disconnect) [[ $# == 1 ]] ;;\n'
	printf '  connect)\n'
	printf '    [[ $# == 2 && $2 =~ ^([[:xdigit:]]{2}:){5}[[:xdigit:]]{2}$ ]]\n'
	printf '    IFS= read -r secret\n'
	printf '    [[ $secret == fixture-network-secret ]]\n'
	printf '    printf "connect\\t%%s\\tsecret-bytes=%%s\\n" "$2" "${#secret}" >>"$DEVICE_CONTROL_TEST_ROOT/rootfs/run/mock-network.argv"\n'
	printf '    exit 0 ;;\n'
	printf '  forget) [[ $# == 2 && $2 =~ ^[[:xdigit:]]{8}-[[:xdigit:]]{4}-[1-5][[:xdigit:]]{3}-[89ABab][[:xdigit:]]{3}-[[:xdigit:]]{12}$ ]] ;;\n'
	printf '  hotspot) [[ $# == 2 && ( $2 == on || $2 == off ) ]] ;;\n'
	printf '  *) exit 2 ;;\n'
	printf 'esac\n'
	printf 'printf "%%s\\n" "$*" >>"$DEVICE_CONTROL_TEST_ROOT/rootfs/run/mock-network.argv"\n'
} >"$MOCK_NETWORK_CTL"
chmod +x "$MOCK_NETWORK_CTL"

MOCK_REBOOT_TARGET="$FIXTURE/mock-bin/rg40xxv-reboot-target"
{
	printf '#!/usr/bin/env bash\n'
	printf 'set -euo pipefail\n'
	printf '[[ ${DEVICE_CONTROL_TESTING:-0} == 1 ]]\n'
	printf '[[ $# == 1 && $1 == custom ]]\n'
	printf 'printf "%%s\\n" "$1" >>"$DEVICE_CONTROL_TEST_ROOT/rootfs/run/mock-reboot-target.argv"\n'
} >"$MOCK_REBOOT_TARGET"
chmod +x "$MOCK_REBOOT_TARGET"

export DEVICE_CONTROL_TESTING=1
export DEVICE_CONTROL_TEST_ROOT="$FIXTURE"
CTL="$ROOT/ui-hardwarectl/ui-hardwarectl"

expect_rejected() {
    if "$CTL" "$@" >/dev/null 2>&1; then
        printf 'FAIL: 應拒絕 argv：%s\n' "$*" >&2
        exit 1
    fi
}

CPU_BEFORE=$(sha256sum "$ROOTFS/sys/devices/system/cpu/cpufreq/policy0/"*)

"$CTL" brightness 0 >/dev/null
[[ $(<"$ROOTFS/sys/class/backlight/lcd-backlight/brightness") == 5 ]]
"$CTL" brightness 25 >/dev/null
[[ $(<"$ROOTFS/sys/class/backlight/lcd-backlight/brightness") == 50 ]]
[[ $(<"$ROOTFS/sys/class/backlight/lcd-backlight/bl_power") == 0 ]]

# 不相關 helper 缺件不得令背光或 USB 命令一起失敗。
mv -- "$MOCK_VOLUME_CTL" "$MOCK_VOLUME_CTL.unavailable"
"$CTL" brightness 25 >/dev/null
[[ $(<"$ROOTFS/sys/class/backlight/lcd-backlight/brightness") == 50 ]]
"$CTL" usb-debug on >/dev/null
"$CTL" usb-debug off >/dev/null
if "$CTL" volume 55 >/dev/null 2>&1; then
    printf 'FAIL: volume helper 缺件時 volume 仍回報成功\n' >&2
    exit 1
fi
mv -- "$MOCK_VOLUME_CTL.unavailable" "$MOCK_VOLUME_CTL"

# 網路與重啟 helper 也必須 command-scoped；缺件不可拖垮其他控制。
mv -- "$MOCK_NETWORK_CTL" "$MOCK_NETWORK_CTL.unavailable"
"$CTL" brightness 25 >/dev/null
if "$CTL" network-status >/dev/null 2>&1; then
	printf 'FAIL: network helper 缺件時 network-status 仍回報成功\n' >&2
	exit 1
fi
mv -- "$MOCK_NETWORK_CTL.unavailable" "$MOCK_NETWORK_CTL"
mv -- "$MOCK_REBOOT_TARGET" "$MOCK_REBOOT_TARGET.unavailable"
"$CTL" brightness 25 >/dev/null
if "$CTL" reboot-custom >/dev/null 2>&1; then
	printf 'FAIL: reboot helper 缺件時 reboot-custom 仍回報成功\n' >&2
	exit 1
fi
mv -- "$MOCK_REBOOT_TARGET.unavailable" "$MOCK_REBOOT_TARGET"

"$CTL" joystick-rgb 0 >/dev/null
[[ $(<"$ROOTFS/sys/class/leds/joystick-left/brightness") == 0 ]]
[[ $(<"$ROOTFS/sys/class/leds/joystick-right/brightness") == 0 ]]
[[ $(<"$ROOTFS/sys/class/leds/rgb:kbd_backlight/brightness") == 0 ]]
"$CTL" joystick-rgb 40 >/dev/null
[[ $(<"$ROOTFS/sys/class/leds/joystick-left/brightness") == 102 ]]
[[ $(<"$ROOTFS/sys/class/leds/joystick-right/brightness") == 102 ]]
[[ $(<"$ROOTFS/sys/class/leds/rgb:kbd_backlight/brightness") == 102 ]]
[[ $(<"$ROOTFS/sys/class/leds/status:green/brightness") == 99 ]]
[[ $(<"$ROOTFS/sys/class/leds/power:red/brightness") == 99 ]]
[[ $(<"$ROOTFS/sys/class/leds/status:joystick/brightness") == 99 ]]
[[ $(<"$ROOTFS/sys/class/leds/power:stick/brightness") == 99 ]]

"$CTL" screen-off >/dev/null
[[ $(<"$ROOTFS/sys/class/backlight/lcd-backlight/brightness") == 0 ]]
[[ $(<"$ROOTFS/sys/class/backlight/lcd-backlight/bl_power") == 4 ]]
[[ $(<"$ROOTFS/sys/class/leds/joystick-left/brightness") == 0 ]]
[[ $(<"$ROOTFS/sys/class/leds/joystick-right/brightness") == 0 ]]
[[ $(<"$ROOTFS/sys/class/leds/rgb:kbd_backlight/brightness") == 0 ]]
[[ $(<"$ROOTFS/sys/class/leds/status:green/brightness") == 99 ]]
[[ $(<"$ROOTFS/sys/class/leds/power:red/brightness") == 99 ]]
[[ $(<"$ROOTFS/sys/class/drm/card0/device/power/control") == auto ]]
[[ $(<"$ROOTFS/sys/devices/system/cpu/cpufreq/policy0/scaling_governor") == powersave ]]
grep -q '^mode=screen_off$' "$ROOTFS/run/rg40xxv/power-lock/state.v1"
grep -q '^ui_mode=paused$' "$ROOTFS/run/rg40xxv/power-lock/state.v1"

"$CTL" screen-on >/dev/null
[[ $(<"$ROOTFS/sys/class/backlight/lcd-backlight/brightness") == 50 ]]
[[ $(<"$ROOTFS/sys/class/backlight/lcd-backlight/bl_power") == 0 ]]
[[ $(<"$ROOTFS/sys/class/leds/joystick-left/brightness") == 102 ]]
[[ $(<"$ROOTFS/sys/class/leds/joystick-right/brightness") == 102 ]]
[[ $(<"$ROOTFS/sys/class/leds/rgb:kbd_backlight/brightness") == 102 ]]
[[ $(<"$ROOTFS/sys/class/drm/card0/device/power/control") == on ]]
[[ $(<"$ROOTFS/sys/devices/system/cpu/cpufreq/policy0/scaling_governor") == schedutil ]]
grep -q '^mode=lock_screen$' "$ROOTFS/run/rg40xxv/power-lock/state.v1"
grep -q '^ui_mode=active$' "$ROOTFS/run/rg40xxv/power-lock/state.v1"

# UI 被 supervisor 重啟時，recover-awake 必須復原關屏 snapshot；正常
# awake 狀態重跑則必須是無副作用的 idempotent no-op。
"$CTL" screen-off >/dev/null
"$CTL" recover-awake >/dev/null
[[ $(<"$ROOTFS/sys/class/backlight/lcd-backlight/brightness") == 50 ]]
[[ $(<"$ROOTFS/sys/class/backlight/lcd-backlight/bl_power") == 0 ]]
[[ $(<"$ROOTFS/sys/class/drm/card0/device/power/control") == on ]]
[[ $(<"$ROOTFS/sys/devices/system/cpu/cpufreq/policy0/scaling_governor") == schedutil ]]
grep -q '^mode=awake$' "$ROOTFS/run/rg40xxv/power-lock/state.v1"
"$CTL" recover-awake >/dev/null
grep -q '^mode=awake$' "$ROOTFS/run/rg40xxv/power-lock/state.v1"
"$CTL" screen-off >/dev/null
"$CTL" screen-on >/dev/null
grep -q '^mode=lock_screen$' "$ROOTFS/run/rg40xxv/power-lock/state.v1"

# screen-off 的最後一步失敗時，GPU、RGB 與 bl_power 都必須回復。
if POWER_LOCK_TEST_FAIL_STEP=backlight_brightness "$CTL" screen-off >/dev/null 2>&1; then
    printf 'FAIL: screen-off 注入失敗仍回報成功\n' >&2
    exit 1
fi
[[ $(<"$ROOTFS/sys/class/backlight/lcd-backlight/brightness") == 50 ]]
[[ $(<"$ROOTFS/sys/class/backlight/lcd-backlight/bl_power") == 0 ]]
[[ $(<"$ROOTFS/sys/class/leds/joystick-left/brightness") == 102 ]]
[[ $(<"$ROOTFS/sys/class/leds/joystick-right/brightness") == 102 ]]
[[ $(<"$ROOTFS/sys/class/leds/rgb:kbd_backlight/brightness") == 102 ]]
[[ $(<"$ROOTFS/sys/class/drm/card0/device/power/control") == on ]]
[[ $(<"$ROOTFS/sys/devices/system/cpu/cpufreq/policy0/scaling_governor") == schedutil ]]
grep -q '^mode=lock_screen$' "$ROOTFS/run/rg40xxv/power-lock/state.v1"

# 第二顆 RGB 寫入失敗，第一顆已改值也必須 rollback。
if POWER_LOCK_TEST_FAIL_STEP=rgb_brightness POWER_LOCK_TEST_FAIL_AFTER=2 \
    "$CTL" joystick-rgb 10 >/dev/null 2>&1; then
    printf 'FAIL: RGB 部分寫入失敗仍回報成功\n' >&2
    exit 1
fi
[[ $(<"$ROOTFS/sys/class/leds/joystick-left/brightness") == 102 ]]
[[ $(<"$ROOTFS/sys/class/leds/joystick-right/brightness") == 102 ]]
[[ $(<"$ROOTFS/sys/class/leds/rgb:kbd_backlight/brightness") == 102 ]]

# class entry 只允許解析至同一 sysfs devices root；外逃連結必須 fail closed。
OUTSIDE_LED="$FIXTURE/outside-led"
mkdir -p "$OUTSIDE_LED"
printf '77' >"$OUTSIDE_LED/brightness"
printf '255' >"$OUTSIDE_LED/max_brightness"
ln -s "$OUTSIDE_LED" "$ROOTFS/sys/class/leds/joystick-escape"
if "$CTL" joystick-rgb 10 >/dev/null 2>&1; then
    printf 'FAIL: class entry 外逃 sysfs devices 仍回報成功\n' >&2
    exit 1
fi
[[ $(<"$OUTSIDE_LED/brightness") == 77 ]]
[[ $(<"$ROOTFS/sys/class/leds/joystick-left/brightness") == 102 ]]
unlink "$ROOTFS/sys/class/leds/joystick-escape"

# class target 合法也不得接受 symlink attribute。
mkdir -p "$ROOTFS/sys/devices/platform/led-controller/leds/joystick-attr-escape"
printf '255' >"$ROOTFS/sys/devices/platform/led-controller/leds/joystick-attr-escape/max_brightness"
printf '66' >"$FIXTURE/outside-led-brightness"
ln -s "$FIXTURE/outside-led-brightness" \
    "$ROOTFS/sys/devices/platform/led-controller/leds/joystick-attr-escape/brightness"
ln -s ../../devices/platform/led-controller/leds/joystick-attr-escape \
    "$ROOTFS/sys/class/leds/joystick-attr-escape"
if "$CTL" joystick-rgb 10 >/dev/null 2>&1; then
    printf 'FAIL: symlink RGB attribute 仍回報成功\n' >&2
    exit 1
fi
[[ $(<"$FIXTURE/outside-led-brightness") == 66 ]]
[[ $(<"$ROOTFS/sys/class/leds/joystick-left/brightness") == 102 ]]
unlink "$ROOTFS/sys/class/leds/joystick-attr-escape"
unlink "$ROOTFS/sys/devices/platform/led-controller/leds/joystick-attr-escape/brightness"
rm -f -- "$ROOTFS/sys/devices/platform/led-controller/leds/joystick-attr-escape/max_brightness"
rmdir "$ROOTFS/sys/devices/platform/led-controller/leds/joystick-attr-escape"

"$CTL" joystick-rgb 10 >/dev/null
[[ $(<"$OUTSIDE_LED/brightness") == 77 ]]
[[ $(<"$ROOTFS/sys/class/leds/joystick-left/brightness") == 26 ]]
[[ $(<"$ROOTFS/sys/class/leds/joystick-right/brightness") == 26 ]]
[[ $(<"$ROOTFS/sys/class/leds/rgb:kbd_backlight/brightness") == 26 ]]
[[ $(<"$ROOTFS/sys/class/leds/status:green/brightness") == 99 ]]
[[ $(<"$ROOTFS/sys/class/leds/power:red/brightness") == 99 ]]
[[ $(<"$ROOTFS/sys/class/leds/status:joystick/brightness") == 99 ]]
[[ $(<"$ROOTFS/sys/class/leds/power:stick/brightness") == 99 ]]

# min_brightness 缺少時 brightness 0 固定 clamp 到 1；symlink min 拒絕。
unlink "$ROOTFS/sys/class/backlight/lcd-backlight/min_brightness"
"$CTL" brightness 0 >/dev/null
[[ $(<"$ROOTFS/sys/class/backlight/lcd-backlight/brightness") == 1 ]]
printf '9' >"$FIXTURE/outside-min-brightness"
ln -s "$FIXTURE/outside-min-brightness" \
    "$ROOTFS/sys/class/backlight/lcd-backlight/min_brightness"
if "$CTL" brightness 0 >/dev/null 2>&1; then
    printf 'FAIL: symlink min_brightness 未被拒絕\n' >&2
    exit 1
fi
[[ $(<"$ROOTFS/sys/class/backlight/lcd-backlight/brightness") == 1 ]]
unlink "$ROOTFS/sys/class/backlight/lcd-backlight/min_brightness"
printf '5' >"$ROOTFS/sys/class/backlight/lcd-backlight/min_brightness"

# 超過 bounded LED discovery 上限時，在任何寫入前 fail closed。
for number in {1..62}; do
    mkdir -p "$ROOTFS/sys/devices/platform/noise-leds/noise-$number"
    ln -s "../../devices/platform/noise-leds/noise-$number" \
        "$ROOTFS/sys/class/leds/noise-$number"
done
if "$CTL" joystick-rgb 20 >/dev/null 2>&1; then
    printf 'FAIL: LED discovery 超過上限仍回報成功\n' >&2
    exit 1
fi
[[ $(<"$ROOTFS/sys/class/leds/joystick-left/brightness") == 26 ]]
[[ $(<"$ROOTFS/sys/class/leds/joystick-right/brightness") == 26 ]]
[[ $(<"$ROOTFS/sys/class/leds/rgb:kbd_backlight/brightness") == 26 ]]
for number in {1..62}; do
    unlink "$ROOTFS/sys/class/leds/noise-$number"
    rmdir "$ROOTFS/sys/devices/platform/noise-leds/noise-$number"
done

MARKER="$FIXTURE/argv-injection"
expect_rejected brightness 101
expect_rejected brightness -1
expect_rejected brightness "50;touch $MARKER"
expect_rejected joystick-rgb '$(touch /tmp/never-run)'
expect_rejected volume 101
expect_rejected volume -1
expect_rejected volume "50;touch $MARKER"
expect_rejected volume-up extra
expect_rejected mute-toggle now
expect_rejected usb-debug 'on;id'
expect_rejected network-status now
expect_rejected network-wifi-recover now
expect_rejected network-wifi-scan now
expect_rejected network-wifi-connect 'AA:BB:CC:DD:EE:FF;id'
expect_rejected network-wifi-forget '../../../../etc/shadow'
expect_rejected network-hotspot 'on;id'
expect_rejected reboot-custom stock
expect_rejected reboot-custom fastboot
expect_rejected reboot-custom poweroff
expect_rejected screen-off extra
expect_rejected unknown-command
[[ ! -e $MARKER ]]

"$CTL" volume 0 >/dev/null
"$CTL" volume 55 >/dev/null
"$CTL" volume-up >/dev/null
"$CTL" volume-down >/dev/null
"$CTL" mute-toggle >/dev/null
[[ $(<"$ROOTFS/run/mock-volume.argv") == $'set 0\nset 55\nup\ndown\nmute-toggle' ]]

"$CTL" network-status >/dev/null
"$CTL" network-wifi-recover >/dev/null
"$CTL" network-wifi-scan >/dev/null
printf '%s\n' fixture-network-secret | \
	"$CTL" network-wifi-connect aa:bb:cc:dd:ee:ff >/dev/null
"$CTL" network-wifi-disconnect >/dev/null
"$CTL" network-wifi-forget 550e8400-e29b-41d4-a716-446655440000 >/dev/null
"$CTL" network-hotspot on >/dev/null
"$CTL" network-hotspot off >/dev/null
[[ $(<"$ROOTFS/run/mock-network.argv") == \
	$'status\nrecover\nscan\nconnect\tAA:BB:CC:DD:EE:FF\tsecret-bytes=22\ndisconnect\nforget 550e8400-e29b-41d4-a716-446655440000\nhotspot on\nhotspot off' ]]
! grep -Fq fixture-network-secret "$ROOTFS/run/mock-network.argv"

"$CTL" reboot-custom >/dev/null
[[ $(<"$ROOTFS/run/mock-reboot-target.argv") == custom ]]
! grep -Eq 'stock|fastboot|poweroff' "$ROOTFS/run/mock-reboot-target.argv"

"$CTL" usb-debug on >/dev/null
GADGET="$FIXTURE/configfs/usb_gadget/rg40xxv_debug"
[[ $(<"$GADGET/UDC") == sunxi-test-udc ]]
[[ -L "$GADGET/configs/c.1/rndis.usb0" ]]
[[ -L "$GADGET/configs/c.1/acm.GS0" ]]
[[ ! -d "$GADGET/functions/mass_storage.0" ]]
"$CTL" usb-debug off >/dev/null
[[ ! -s "$GADGET/UDC" ]]

mkdir -p "$GADGET/functions/mass_storage.0"
if "$CTL" usb-debug on >/dev/null 2>&1; then
    printf 'FAIL: 預存 mass-storage function 時仍綁定 USB debug\n' >&2
    exit 1
fi
[[ ! -s "$GADGET/UDC" ]]
rmdir "$GADGET/functions/mass_storage.0"

"$CTL" orderly-shutdown
[[ $(<"$ROOTFS/run/mock-systemctl.argv") == poweroff ]]
expect_rejected orderly-shutdown now
[[ $(wc -l <"$ROOTFS/run/mock-systemctl.argv") == 1 ]]

CPU_AFTER=$(sha256sum "$ROOTFS/sys/devices/system/cpu/cpufreq/policy0/"*)
[[ $CPU_BEFORE == "$CPU_AFTER" ]]
[[ $(<"$ROOTFS/sys/devices/system/cpu/cpufreq/policy0/scaling_available_frequencies") == \
    '480000 1008000 1512000' ]]

printf 'PASS ui-hardwarectl：command-scoped helper、allowlist、安全背光 minimum、RGB 0、volume/network 固定 argv、密碼不進 argv、reboot 固定 custom、screen+CPU rollback、no-symlink/bounds、USB 無 mass-storage、mock poweroff\n'
