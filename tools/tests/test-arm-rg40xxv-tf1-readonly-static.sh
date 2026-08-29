#!/usr/bin/env bash
# Host-only structural test.  It never opens or changes a block device.
set -euo pipefail
PATH=/usr/sbin:/usr/bin:/sbin:/bin
export PATH

SCRIPT=$(realpath -- "$0")
ROOT=$(CDPATH= cd -- "${SCRIPT%/*}/../.." && pwd -P)
HELPER=$ROOT/tools/arm-rg40xxv-tf1-readonly.sh

die()
{
	printf 'P8_READONLY_ARM_STATIC_TEST result=FAIL reason=%s device_write=NONE p7_write=NONE\n' \
		"$1" >&2
	exit 1
}

[[ -f $HELPER && ! -L $HELPER && -x $HELPER ]] || die helper-missing-or-not-executable
bash -n "$HELPER"

# Direct use must fail on dispatcher authorization before inspecting /dev/sdz.
set +e
DIRECT_OUTPUT=$(env -i PATH="$PATH" "$HELPER" /dev/sdz 2>&1)
DIRECT_RC=$?
set -e
[[ $DIRECT_RC -ne 0 ]] || die direct-invocation-was-accepted
grep -Fq 'reason=dispatcher-authorization-required' <<<"$DIRECT_OUTPUT" || \
	die direct-invocation-wrong-rejection

# This helper may change only RO flags.  Data-copy, mounting and RW-enabling
# command forms are structurally forbidden.
if grep -Eq 'blockdev[[:space:]]+--setrw|(^|[;&|])[[:space:]]*dd([[:space:]]|$)|(^|[;&|])[[:space:]]*mount([[:space:]]|$)' \
	"$HELPER"; then
	die forbidden-mutating-command
fi

for required in \
	'RG40XXV_P8_OPERATION_LOCK_FD' \
	'RG40XXV_P8_AUTH_MODE' \
	'RG40XXV_P8_AUTH_DEVICE' \
	'arm-readonly' \
	'EXPECTED_DISK_BYTES=62516101120' \
	'EXPECTED_GUID=PUT-YOUR-OWN-CARD-GPT-GUID-HERE' \
	'findmnt -rn -o MAJ:MIN,TARGET' \
	'/proc/self/mountinfo' \
	'/proc/swaps' \
	'/holders' \
	'DISKSEQ_FILE=$DISK_SYSFS/diskseq' \
	'whole-device-diskseq-stability' \
	'whole-device-sysfs-device-stability' \
	'exec {DISK_FD}<"$DEV"' \
	'blockdev --setro "/proc/$$/fd/$DISK_FD"' \
	'RO_RETRY_ACTIVE=1' \
	'P8_READONLY_ARM result=PASS' \
	'device_write=NONE p7_write=NONE'; do
	grep -Fq "$required" "$HELPER" || die "missing-required-guard:$required"
done

# Parent must be armed before the p1-p8 loop in the fail-safe function.
PARENT_LINE=$(grep -n 'blockdev --setro "/proc/$$/fd/$DISK_FD"' "$HELPER" | head -n 1 | cut -d: -f1)
PART_LINE=$(grep -n 'blockdev --setro "/proc/$$/fd/${PART_FD\[\$num\]}"' "$HELPER" | head -n 1 | cut -d: -f1)
[[ -n $PARENT_LINE && -n $PART_LINE && $PARENT_LINE -lt $PART_LINE ]] || \
	die parent-not-armed-first

# The failure retry must remain disabled throughout all device/GPT/layout
# validation and only turn on immediately before the intentional setro call.
ARM_LINE=$(grep -n '^RO_RETRY_ACTIVE=1$' "$HELPER" | cut -d: -f1)
LAYOUT_LINE=$(grep -n '^assert_exact_layout$' "$HELPER" | head -n 1 | cut -d: -f1)
CALL_LINE=$(grep -n '^retry_read_only || die' "$HELPER" | cut -d: -f1)
[[ -n $ARM_LINE && -n $LAYOUT_LINE && -n $CALL_LINE && \
	$ARM_LINE -gt $LAYOUT_LINE && $ARM_LINE -lt $CALL_LINE ]] || \
	die read-only-retry-enabled-before-preflight-complete

# Capture the kernel disk generation before pinning any descriptors, then
# recheck it once immediately before and once after the setro operation.
DISKSEQ_CAPTURE_LINE=$(grep -n '^DISKSEQ=$(cat -- "$DISKSEQ_FILE")' "$HELPER" | cut -d: -f1)
DISK_OPEN_LINE=$(grep -n '^exec {DISK_FD}<"$DEV"' "$HELPER" | cut -d: -f1)
mapfile -t STABILITY_CALL_LINES < <(
	grep -n '^validate_nodes_stable$' "$HELPER" | cut -d: -f1
)
[[ -n $DISKSEQ_CAPTURE_LINE && -n $DISK_OPEN_LINE && \
	$DISKSEQ_CAPTURE_LINE -lt $DISK_OPEN_LINE ]] || \
	die diskseq-not-captured-before-fd-pinning
[[ ${#STABILITY_CALL_LINES[@]} -eq 2 && \
	${STABILITY_CALL_LINES[0]} -lt $ARM_LINE && \
	${STABILITY_CALL_LINES[1]} -gt $CALL_LINE ]] || \
	die diskseq-not-rechecked-before-and-after-setro

printf 'P8_READONLY_ARM_STATIC_TEST result=PASS device_write=NONE p7_write=NONE\n'
