#!/bin/sh
set -eu

[ "$#" -eq 1 ] || {
	printf 'usage: %s RELEASE_ROOT\n' "$0" >&2
	exit 2
}
root=$1
helper=$root/rootfs-overlay/usr/sbin/rg40xxv-bluetooth-control
hci_helper=$root/rootfs-overlay/usr/libexec/rg40xxv/rg40xxv-bluetooth-hci-ready
admission=$root/opt/rg40xxv/bluetooth/runtime/admission.env
service=$root/rootfs-overlay/usr/lib/systemd/system/rg40xxv-bluetooth-state.service
hci_service=$root/rootfs-overlay/usr/lib/systemd/system/rg40xxv-bluetooth-hci-ready.service
mount_unit=$root/rootfs-overlay/usr/lib/systemd/system/var-lib-bluetooth.mount
dropin=$root/rootfs-overlay/etc/systemd/system/bluetooth.service.d/20-rg40xxv-runtime.conf
ui_dropin=$root/rootfs-overlay/etc/systemd/system/rg40xxv-ui.service.d/30-bluetooth.conf
multi_user_dropin=$root/rootfs-overlay/etc/systemd/system/multi-user.target.d/30-bluetooth.conf
runtime_lock=$root/manifest/runtime-lock.env
firmware_license=$root/manifest/LICENCE.rtlwifi_firmware.txt

[ -f "$helper" ] && [ ! -L "$helper" ] && [ -x "$helper" ]
[ "$(stat -c %a "$helper")" = 755 ]
[ -f "$hci_helper" ] && [ ! -L "$hci_helper" ] && [ -x "$hci_helper" ]
[ "$(stat -c %a "$hci_helper")" = 755 ]
[ -f "$admission" ] && [ ! -L "$admission" ]
[ "$(wc -l <"$admission")" = 2 ]
grep -Fqx 'schema=rg40xxv-bluetooth-runtime-admission-v1' "$admission"
grep -Fqx 'status=PASS' "$admission"
grep -Fqx \
	'kernel_firmware_commit=8c7fac62c0d1c3b8915f596effc1ef6e95fd6b5f' \
	"$runtime_lock"
grep -Fqx 'device_validation=PENDING' "$runtime_lock"
sha256sum "$firmware_license" | grep -Fq \
	'a61351665b4f264f6c631364f85b907d8f8f41f8b369533ef4021765f9f3b62e'

grep -Fqx 'RequiresMountsFor=/mnt/data' "$service"
grep -Fqx 'Before=var-lib-bluetooth.mount bluetooth.service' "$service"
grep -Fqx \
	'ExecStart=/usr/bin/install -d -o root -g root -m 0700 /mnt/data/rg40xxv/state/bluetooth' \
	"$service"
grep -Fqx 'What=/mnt/data/rg40xxv/state/bluetooth' "$mount_unit"
grep -Fqx 'Where=/var/lib/bluetooth' "$mount_unit"
grep -Fqx 'Options=bind,nodev,nosuid,noexec' "$mount_unit"
grep -Fqx 'DefaultDependencies=no' "$mount_unit"
grep -Fqx 'After=local-fs.target rg40xxv-bluetooth-state.service' "$mount_unit"
grep -Fqx \
	'Requires=var-lib-bluetooth.mount rg40xxv-bluetooth-hci-ready.service' \
	"$dropin"
grep -Fqx \
	'After=var-lib-bluetooth.mount rg40xxv-bluetooth-hci-ready.service systemd-rfkill.service' \
	"$dropin"
grep -Fqx 'ExecStartPre=/usr/sbin/rfkill unblock bluetooth' "$dropin"
grep -Fqx 'Wants=bluetooth.service' "$ui_dropin"
grep -Fqx 'Wants=bluetooth.service' "$multi_user_dropin"
if grep -Eq '^[[:space:]]*(After|Requires|Requisite|BindsTo)=' "$ui_dropin" ||
	grep -Eiq 'network|udev|wait-online' "$ui_dropin"; then
	printf '%s\n' 'UI Bluetooth drop-in contains a first-frame blocker' >&2
	exit 1
fi

grep -Fqx 'Before=bluetooth.service' "$hci_service"
grep -Fqx \
	'ExecStart=/usr/libexec/rg40xxv/rg40xxv-bluetooth-hci-ready' \
	"$hci_service"
grep -Fqx 'TimeoutStartSec=12s' "$hci_service"
grep -Fqx 'RuntimeDirectory=rg40xxv/bluetooth' "$hci_service"
grep -Fqx \
	'ReadWritePaths=/sys/bus/serial/drivers/hci_uart_h5 /run/rg40xxv/bluetooth' \
	"$hci_service"
grep -Fqx 'ConditionPathExists=/sys/bus/serial/drivers/hci_uart_h5' \
	"$hci_service"
grep -Fqx \
	'FIRMWARE_SHA=3baa2eeaa43c959054687a67771e7435e73b2ff3e79dfb765121d8b7dc719391' \
	"$hci_helper"
grep -Fqx \
	'CONFIG_SHA=6ddeb15f23588053e00cb08d25588bd7cf98d60fa93d9478efcef4ae8064a7ac' \
	"$hci_helper"
grep -Fqx 'SERIAL_DEVICE=serial0-0' "$hci_helper"
grep -Fqx 'DRIVER_NAME=hci_uart_h5' "$hci_helper"

[ "$(readlink -- "$root/rootfs-overlay/usr/lib/systemd/system/dbus-org.bluez.service")" = \
	bluetooth.service ]

sha256sum "$root/rootfs-overlay/usr/lib/firmware/rtl_bt/rtl8821cs_fw.bin" |
	grep -Fq '3baa2eeaa43c959054687a67771e7435e73b2ff3e79dfb765121d8b7dc719391'
sha256sum "$root/rootfs-overlay/usr/lib/firmware/rtl_bt/rtl8821cs_config.bin" |
	grep -Fq '6ddeb15f23588053e00cb08d25588bd7cf98d60fa93d9478efcef4ae8064a7ac'

if strings "$helper" | grep -Eq 'RG40XXV_.*TEST|DEVICE_CONTROL_TESTING'; then
	printf '%s\n' 'production helper contains a test injection hook' >&2
	exit 1
fi
printf 'BLUETOOTH_PAYLOAD_TEST PASS\n'
