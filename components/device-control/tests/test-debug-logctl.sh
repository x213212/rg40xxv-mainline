#!/usr/bin/env bash
set -euo pipefail

TEST_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(cd -- "$TEST_DIR/.." && pwd)
FIXTURE=$(mktemp -d /tmp/rg40xxv-device-control-test.logs.XXXXXX)
ROOTFS="$FIXTURE/rootfs"
trap 'rm -rf -- "$FIXTURE"' EXIT

mkdir -p \
    "$ROOTFS/var/log/rg40xxv/ui" \
    "$ROOTFS/var/log/rg40xxv/emulator" \
    "$ROOTFS/var/log/rg40xxv/boot" \
    "$ROOTFS/var/lib/rg40xxv/boot-logs" \
    "$ROOTFS/sys/fs/pstore" \
    "$ROOTFS/mnt/mmc/Saves" \
    "$ROOTFS/etc/NetworkManager/system-connections" \
    "$ROOTFS/etc/ssh"
printf 'ui frame error\n' >"$ROOTFS/var/log/rg40xxv/ui/ui.log"
printf 'emulator stayed intact\n' >"$ROOTFS/var/log/rg40xxv/emulator/core.log"
printf 'boot failed code=7\n' >"$ROOTFS/var/lib/rg40xxv/boot-logs/last-failure.log"
printf 'ramoops kernel panic\n' >"$ROOTFS/sys/fs/pstore/dmesg-ramoops-0"
printf 'SAVE-DATA\n' >"$ROOTFS/mnt/mmc/Saves/game.sav"
printf 'wifi-secret\n' >"$ROOTFS/etc/NetworkManager/system-connections/home.nmconnection"
printf 'ssh-private\n' >"$ROOTFS/etc/ssh/ssh_host_ed25519_key"

SAVE_HASH=$(sha256sum "$ROOTFS/mnt/mmc/Saves/game.sav")
WIFI_HASH=$(sha256sum "$ROOTFS/etc/NetworkManager/system-connections/home.nmconnection")
SSH_HASH=$(sha256sum "$ROOTFS/etc/ssh/ssh_host_ed25519_key")

export DEVICE_CONTROL_TESTING=1
export DEVICE_CONTROL_TEST_ROOT="$FIXTURE"
CTL="$ROOT/debug-logctl/debug-logctl"

STATUS_OUTPUT=$("$CTL" status all)
grep -q '^ui' <<<"$STATUS_OUTPUT"
ARCHIVE=$("$CTL" export ui)
[[ -f "$ARCHIVE" ]]
[[ $(stat -c '%a' "$ARCHIVE") == 600 ]]
tar -tzf "$ARCHIVE" >"$FIXTURE/export.list"
grep -q 'data/var/log/rg40xxv/ui/ui.log' "$FIXTURE/export.list"
if grep -Eq 'Saves|NetworkManager|ssh_host' "$FIXTURE/export.list"; then
    printf 'FAIL: 日誌匯出包含安全邊界外檔案\n' >&2
    exit 1
fi

"$CTL" clear ui >/dev/null
[[ ! -s "$ROOTFS/var/log/rg40xxv/ui/ui.log" ]]
grep -q 'stayed intact' "$ROOTFS/var/log/rg40xxv/emulator/core.log"

"$CTL" clear boot >/dev/null
RETAINED="$ROOTFS/var/lib/rg40xxv/debug-retained/last-boot-failure.tar.gz"
[[ -f "$RETAINED" ]]
tar -tzf "$RETAINED" >"$FIXTURE/retained.list"
grep -q 'pstore/dmesg-ramoops-0' "$FIXTURE/retained.list"
grep -q 'boot/last-failure.log' "$FIXTURE/retained.list"
grep -q 'ramoops kernel panic' "$ROOTFS/sys/fs/pstore/dmesg-ramoops-0"
[[ ! -s "$ROOTFS/var/lib/rg40xxv/boot-logs/last-failure.log" ]]

[[ $(sha256sum "$ROOTFS/mnt/mmc/Saves/game.sav") == "$SAVE_HASH" ]]
[[ $(sha256sum "$ROOTFS/etc/NetworkManager/system-connections/home.nmconnection") == "$WIFI_HASH" ]]
[[ $(sha256sum "$ROOTFS/etc/ssh/ssh_host_ed25519_key") == "$SSH_HASH" ]]

if "$CTL" export /tmp >/dev/null 2>&1; then
    printf 'FAIL: 接受任意匯出路徑／分類\n' >&2
    exit 1
fi

printf 'PASS debug-logctl：白名單匯出／清除、保留 last boot failure、敏感資料不變\n'
