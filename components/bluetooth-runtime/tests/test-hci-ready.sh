#!/usr/bin/env bash
set -euo pipefail

project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
workspace=$(CDPATH= cd -- "$project/../../../.." && pwd -P)
helper=$project/src/rg40xxv-bluetooth-hci-ready
firmware_source=$workspace/build-inputs/rg40xxv-test1-firmware/rtl_bt

fail()
{
	printf 'BLUETOOTH_HCI_READY_TEST result=FAIL reason=%s\n' "$1" >&2
	exit 1
}

for tool in awk chmod grep install mktemp realpath sha256sum stat; do
	command -v "$tool" >/dev/null 2>&1 || fail "missing-tool:$tool"
done
[[ -x $helper && ! -L $helper ]] || fail helper

temporary=$(mktemp -d /tmp/rg40xxv-bluetooth-hci-test.XXXXXX)
trap 'rm -rf -- "$temporary"' EXIT HUP INT TERM
rootfs=$temporary/rootfs
driver=$temporary/sys/bus/serial/drivers/hci_uart_h5
state=$temporary/state
mock_bin=$temporary/mock-bin

install -d -m 0755 \
	"$rootfs/usr/lib/firmware/rtl_bt" "$rootfs/run" \
	"$driver" "$state" "$mock_bin"
install -m 0644 "$firmware_source/rtl8821cs_fw.bin" \
	"$rootfs/usr/lib/firmware/rtl_bt/rtl8821cs_fw.bin"
install -m 0644 "$firmware_source/rtl8821cs_config.bin" \
	"$rootfs/usr/lib/firmware/rtl_bt/rtl8821cs_config.bin"
: >"$driver/bind"
: >"$driver/unbind"
chmod 0600 "$driver/bind" "$driver/unbind"

cat >"$mock_bin/hciconfig" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

[[ ${MOCK_STATE_ROOT:-} == /tmp/rg40xxv-bluetooth-hci-test.*/state ]] || exit 90
root=${MOCK_STATE_ROOT%/state}
driver=$root/sys/bus/serial/drivers/hci_uart_h5

if [[ -s $driver/bind ]] && grep -Fqx serial0-0 "$driver/bind"; then
	: >"$MOCK_STATE_ROOT/bound"
fi
if [[ ${1:-} == hci0 && ${2:-} == up ]]; then
	[[ -f $MOCK_STATE_ROOT/bound ]] || exit 1
	: >"$MOCK_STATE_ROOT/up"
	exit 0
fi
[[ $# == 1 && $1 == hci0 ]] || exit 2

if [[ -f $MOCK_STATE_ROOT/bound ]]; then
	flags='DOWN'
	[[ -f $MOCK_STATE_ROOT/up ]] && flags='UP RUNNING'
	cat <<OUT
hci0:   Type: Primary  Bus: UART
        BD Address: 10:22:33:44:55:66  ACL MTU: 1021:8  SCO MTU: 255:12
        $flags
OUT
else
	cat <<OUT
hci0:   Type: Primary  Bus: UART
        BD Address: 00:00:00:00:00:00  ACL MTU: 0:0  SCO MTU: 0:0
        DOWN
OUT
fi
EOF
chmod 0755 "$mock_bin/hciconfig"

run_helper()
{
	MOCK_STATE_ROOT=$state RG40XXV_BT_HCI_TESTING=1 \
		RG40XXV_BT_HCI_TEST_ROOT=$temporary "$helper"
}

output=$(run_helper)
grep -Fq \
	'BLUETOOTH_HCI_READY result=PASS address=10:22:33:44:55:66 driver=hci_uart_h5 device=serial0-0' \
	<<<"$output" || fail initial-pass
[[ $(<"$driver/unbind") == serial0-0 ]] || fail initial-unbind
[[ $(<"$driver/bind") == serial0-0 ]] || fail initial-bind
status=$rootfs/run/rg40xxv/bluetooth/hci-ready.v1
[[ -f $status && ! -L $status ]] || fail status-file
grep -Fqx 'RG40XXV_BLUETOOTH_HCI_READY 1' "$status" || fail status-schema
grep -Fqx 'state=READY' "$status" || fail status-state
grep -Fqx 'address=10:22:33:44:55:66' "$status" || fail status-address
grep -Fqx \
	'firmware_sha256=3baa2eeaa43c959054687a67771e7435e73b2ff3e79dfb765121d8b7dc719391' \
	"$status" || fail status-firmware
grep -Fqx \
	'config_sha256=6ddeb15f23588053e00cb08d25588bd7cf98d60fa93d9478efcef4ae8064a7ac' \
	"$status" || fail status-config

# A healthy second invocation must only ensure the adapter is up; it must not
# disturb the UART transport again.
: >"$driver/bind"
: >"$driver/unbind"
run_helper >/dev/null
[[ ! -s $driver/bind && ! -s $driver/unbind ]] || fail healthy-rebind

# Firmware integrity is checked before any sysfs write.
printf X >>"$rootfs/usr/lib/firmware/rtl_bt/rtl8821cs_fw.bin"
if run_helper >"$temporary/corrupt.out" 2>&1; then
	fail corrupt-firmware-accepted
fi
grep -Fq 'reason=firmware-sha' "$temporary/corrupt.out" || fail corrupt-reason
[[ ! -s $driver/bind && ! -s $driver/unbind ]] || fail corrupt-wrote-driver

# Reject redirected driver attributes instead of following an attacker-chosen
# path from a privileged oneshot service.
install -m 0644 "$firmware_source/rtl8821cs_fw.bin" \
	"$rootfs/usr/lib/firmware/rtl_bt/rtl8821cs_fw.bin"
rm -f -- "$driver/bind"
: >"$temporary/redirect-target"
ln -s "$temporary/redirect-target" "$driver/bind"
if run_helper >"$temporary/symlink.out" 2>&1; then
	fail symlink-bind-accepted
fi
grep -Fq 'reason=unsafe-driver-bind' "$temporary/symlink.out" || \
	fail symlink-reason
[[ ! -s $temporary/redirect-target ]] || fail symlink-followed

printf 'BLUETOOTH_HCI_READY_TEST result=PASS\n'
