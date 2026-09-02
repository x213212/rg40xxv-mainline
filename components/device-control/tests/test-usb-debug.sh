#!/usr/bin/env bash
set -euo pipefail

TEST_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(cd -- "$TEST_DIR/.." && pwd)
FIXTURE=$(mktemp -d /tmp/rg40xxv-device-control-test.usb.XXXXXX)
trap 'rm -rf -- "$FIXTURE"' EXIT

mkdir -p \
    "$FIXTURE/configfs" \
    "$FIXTURE/sys/class/udc" \
    "$FIXTURE/sys/class/net" \
    "$FIXTURE/sys/devices/platform/sunxi-test-udc" \
    "$FIXTURE/sys/devices/virtual/net/usb0" \
    "$FIXTURE/dev"
ln -s ../../devices/platform/sunxi-test-udc \
    "$FIXTURE/sys/class/udc/sunxi-test-udc"
ln -s ../../devices/virtual/net/usb0 "$FIXTURE/sys/class/net/usb0"
: >"$FIXTURE/dev/ttyGS0"

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
        CONFIG_USB_U_SERIAL=y \
        CONFIG_USB_U_ETHER=y \
        CONFIG_USB_F_ACM=y \
        CONFIG_USB_F_RNDIS=y \
        CONFIG_USB_MUSB_HDRC=y \
        CONFIG_USB_MUSB_DUAL_ROLE=y \
        CONFIG_USB_MUSB_SUNXI=y \
        CONFIG_PHY_SUN4I_USB=y \
        CONFIG_USB_ROLE_SWITCH=y \
        CONFIG_EXTCON=y \
        CONFIG_TTY=y \
        CONFIG_USB_DWC2=y
} >"$GOOD_CONFIG"
printf '%s\n' CONFIG_USB_GADGET=y >"$BAD_CONFIG"

"$ROOT/usb-debug/check-kernel-config.sh" "$GOOD_CONFIG" >/dev/null
if "$ROOT/usb-debug/check-kernel-config.sh" "$BAD_CONFIG" >/dev/null 2>&1; then
    printf 'FAIL: 缺少必要 config 時仍回報成功\n' >&2
    exit 1
fi

# Generic configfs support is insufficient on this board.  The exact H700
# MUSB/PHY/role-switch path must remain built in or the gadget has no UDC to
# bind even though functions/rndis.usb0 and functions/acm.GS0 exist.
GENERIC_ONLY_CONFIG="$FIXTURE/config-generic-only"
grep -Ev '^CONFIG_(USB_MUSB_HDRC|USB_MUSB_DUAL_ROLE|USB_MUSB_SUNXI|PHY_SUN4I_USB|USB_ROLE_SWITCH|EXTCON)=' \
    "$GOOD_CONFIG" >"$GENERIC_ONLY_CONFIG"
if "$ROOT/usb-debug/check-kernel-config.sh" "$GENERIC_ONLY_CONFIG" >/dev/null 2>&1; then
    printf 'FAIL: 缺少 RG40XX V MUSB/PHY/role-switch 時仍回報成功\n' >&2
    exit 1
fi

export DEVICE_CONTROL_TESTING=1
export DEVICE_CONTROL_TEST_ROOT="$FIXTURE"
"$ROOT/usb-debug/usb-debugctl" start >/dev/null

GADGET="$FIXTURE/configfs/usb_gadget/rg40xxv_debug"
[[ $(<"$GADGET/UDC") == sunxi-test-udc ]]
[[ -L "$GADGET/configs/c.1/rndis.usb0" ]]
[[ -L "$GADGET/configs/c.1/acm.GS0" ]]
[[ $(readlink -- "$GADGET/configs/c.1/rndis.usb0") == "$GADGET/functions/rndis.usb0" ]]
[[ $(readlink -- "$GADGET/configs/c.1/acm.GS0") == "$GADGET/functions/acm.GS0" ]]
[[ $(readlink -- "$GADGET/os_desc/c.1") == "$GADGET/configs/c.1" ]]
[[ ! -d "$GADGET/functions/mass_storage.0" ]]
"$ROOT/usb-debug/usb-debugctl" status >"$FIXTURE/status.out"
grep -q '^mass_storage=absent$' "$FIXTURE/status.out"
grep -q '^usb0=ready$' "$FIXTURE/status.out"
grep -q '^ttyGS0=ready$' "$FIXTURE/status.out"

"$ROOT/usb-debug/usb-debugctl" start >/dev/null
"$ROOT/usb-debug/usb-debugctl" stop >/dev/null
[[ ! -s "$GADGET/UDC" ]]

# H700 MUSB may reject the two-function endpoint allocation.  The service must
# retry RNDIS-only instead of exposing generic exit 1 to the settings UI.
USB_DEBUG_TEST_FAIL_COMPOSITE_BIND=1 \
    "$ROOT/usb-debug/usb-debugctl" start >"$FIXTURE/fallback.out"
grep -Fq 'mode=rndis-only usb0=ready' "$FIXTURE/fallback.out"
[[ $(<"$GADGET/UDC") == sunxi-test-udc ]]
[[ -L "$GADGET/configs/c.1/rndis.usb0" ]]
[[ ! -e "$GADGET/configs/c.1/acm.GS0" ]]
[[ $(<"$GADGET/configs/c.1/strings/0x409/configuration") == \
    'RNDIS debug (CDC ACM unavailable)' ]]
"$ROOT/usb-debug/usb-debugctl" status >"$FIXTURE/status-fallback.out"
grep -q '^acm=disabled$' "$FIXTURE/status-fallback.out"
USB_DEBUG_TEST_FAIL_COMPOSITE_BIND=1 \
    "$ROOT/usb-debug/usb-debugctl" start >"$FIXTURE/fallback-again.out"
grep -Fq 'mode=rndis-only usb0=ready' "$FIXTURE/fallback-again.out"
"$ROOT/usb-debug/usb-debugctl" stop >/dev/null
[[ ! -s "$GADGET/UDC" ]]

# A later start must restore the full composite definition when the endpoint
# budget permits it; fallback must not permanently corrupt the configfs tree.
"$ROOT/usb-debug/usb-debugctl" start >/dev/null
[[ -L "$GADGET/configs/c.1/acm.GS0" ]]
"$ROOT/usb-debug/usb-debugctl" stop >/dev/null

# UDC attribute 寫入成功不等於核心已建立 usb0；缺少 net class
# evidence 時必須解除綁定並回報失敗。
unlink "$FIXTURE/sys/class/net/usb0"
if USB_DEBUG_TEST_USB0_WAIT_STEPS=1 \
    "$ROOT/usb-debug/usb-debugctl" start >/dev/null 2>&1; then
    printf 'FAIL: 缺少可驗證 usb0 時仍回報成功\n' >&2
    exit 1
fi
[[ ! -s "$GADGET/UDC" ]]
ln -s ../../devices/virtual/net/usb0 "$FIXTURE/sys/class/net/usb0"

# Reproduce the real device's interrupted first start: function instances and
# attributes exist, but no config/os_desc links were created.  A second start
# must complete that partial gadget instead of requiring a reboot or cleanup.
rm -f -- \
    "$GADGET/configs/c.1/rndis.usb0" \
    "$GADGET/configs/c.1/acm.GS0" \
    "$GADGET/os_desc/c.1"
[[ -d "$GADGET/functions/rndis.usb0" ]]
[[ -d "$GADGET/functions/acm.GS0" ]]
"$ROOT/usb-debug/usb-debugctl" start >/dev/null
[[ $(<"$GADGET/UDC") == sunxi-test-udc ]]
[[ -L "$GADGET/configs/c.1/rndis.usb0" ]]
[[ -L "$GADGET/configs/c.1/acm.GS0" ]]
[[ -L "$GADGET/os_desc/c.1" ]]
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

printf 'PASS usb-debug：config 檢查、RNDIS、ACM、MUSB RNDIS-only fallback、usb0 positive/negative readiness、冪等啟動、無 mass-storage、class symlink/escape/bounds、解除綁定\n'
