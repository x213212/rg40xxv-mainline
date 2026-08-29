#!/usr/bin/env bash
set -euo pipefail

TEST_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(cd -- "$TEST_DIR/.." && pwd)
FIXTURE=$(mktemp -d /tmp/rg40xxv-device-control-test.usb.XXXXXX)
trap 'rm -rf -- "$FIXTURE"' EXIT

mkdir -p \
    "$FIXTURE/configfs" \
    "$FIXTURE/sys/class/udc" \
    "$FIXTURE/sys/devices/platform/sunxi-test-udc"
ln -s ../../devices/platform/sunxi-test-udc \
    "$FIXTURE/sys/class/udc/sunxi-test-udc"

GOOD_CONFIG="$FIXTURE/config-good"
BAD_CONFIG="$FIXTURE/config-bad"
{
    printf '%s\n' \
        CONFIG_CONFIGFS_FS=y \
        CONFIG_USB_GADGET=y \
        CONFIG_USB_LIBCOMPOSITE=y \
        CONFIG_USB_CONFIGFS=y \
        CONFIG_USB_CONFIGFS_ACM=y \
        CONFIG_USB_CONFIGFS_RNDIS=y \
        CONFIG_USB_U_ETHER=y \
        CONFIG_USB_F_ACM=y \
        CONFIG_TTY=y \
        CONFIG_USB_DWC2=y
} >"$GOOD_CONFIG"
printf '%s\n' CONFIG_USB_GADGET=y >"$BAD_CONFIG"

"$ROOT/usb-debug/check-kernel-config.sh" "$GOOD_CONFIG" >/dev/null
if "$ROOT/usb-debug/check-kernel-config.sh" "$BAD_CONFIG" >/dev/null 2>&1; then
    printf 'FAIL: 缺少必要 config 時仍回報成功\n' >&2
    exit 1
fi

export DEVICE_CONTROL_TESTING=1
export DEVICE_CONTROL_TEST_ROOT="$FIXTURE"
"$ROOT/usb-debug/usb-debugctl" start >/dev/null

GADGET="$FIXTURE/configfs/usb_gadget/rg40xxv_debug"
[[ $(<"$GADGET/UDC") == sunxi-test-udc ]]
[[ -L "$GADGET/configs/c.1/rndis.usb0" ]]
[[ -L "$GADGET/configs/c.1/acm.GS0" ]]
[[ ! -d "$GADGET/functions/mass_storage.0" ]]
"$ROOT/usb-debug/usb-debugctl" status | grep -q '^mass_storage=absent$'

"$ROOT/usb-debug/usb-debugctl" start >/dev/null
"$ROOT/usb-debug/usb-debugctl" stop >/dev/null
[[ ! -s "$GADGET/UDC" ]]

# 正常 sysfs class symlink 只能落在同一 sysfs devices；外逃與過量列舉都 fail closed。
mkdir -p "$FIXTURE/outside-udc"
ln -s "$FIXTURE/outside-udc" "$FIXTURE/sys/class/udc/escape-udc"
if "$ROOT/usb-debug/usb-debugctl" start >/dev/null 2>&1; then
    printf 'FAIL: UDC class entry 外逃 sysfs devices 仍回報成功\n' >&2
    exit 1
fi
[[ ! -s "$GADGET/UDC" ]]
unlink "$FIXTURE/sys/class/udc/escape-udc"

for number in {1..8}; do
    mkdir -p "$FIXTURE/sys/devices/platform/noise-udc-$number"
    ln -s "../../devices/platform/noise-udc-$number" \
        "$FIXTURE/sys/class/udc/noise-udc-$number"
done
if "$ROOT/usb-debug/usb-debugctl" start >/dev/null 2>&1; then
    printf 'FAIL: UDC discovery 超過上限仍回報成功\n' >&2
    exit 1
fi
[[ ! -s "$GADGET/UDC" ]]

printf 'PASS usb-debug：config 檢查、RNDIS、ACM、冪等啟動、無 mass-storage、class symlink/escape/bounds、解除綁定\n'
