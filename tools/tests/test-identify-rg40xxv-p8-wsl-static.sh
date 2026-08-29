#!/usr/bin/env bash
set -euo pipefail

# Host-only boundary checks for the private p8 identifier.  No real block
# device is opened and no operation receipt is published by this test.

SCRIPT=$(realpath -- "$0")
ROOT=$(CDPATH= cd -- "$(dirname -- "$SCRIPT")/../.." && pwd -P)
IDENTIFIER="$ROOT/tools/identify-rg40xxv-p8-wsl.sh"
LOCK="$ROOT/reports/.rg40xxv-p8-operation.lock"
FIXTURE=$(mktemp -d /tmp/rg40xxv-p8-identify-static.XXXXXX)

cleanup()
{
	rm -rf -- "$FIXTURE"
}
trap cleanup EXIT

die()
{
	printf 'P8_IDENTIFY_STATIC_TEST result=FAIL reason=%s device_write=NONE\n' \
		"$1" >&2
	exit 1
}

[[ -f $IDENTIFIER && ! -L $IDENTIFIER && -x $IDENTIFIER ]] || \
	die identifier-missing-or-not-executable
bash -n "$IDENTIFIER" || die identifier-shell-syntax

# Any helper capable of touching a block device is wrapped with a tripwire.
# Direct invocation must reject on dispatcher authorization before a tripwire
# can execute.
mkdir "$FIXTURE/bin"
for command_name in blockdev dd findmnt sfdisk; do
	printf '#!/usr/bin/env bash\nprintf "%%s\\n" "$0" >>"%s"\nexit 97\n' \
		"$FIXTURE/device-command-called" > "$FIXTURE/bin/$command_name"
	chmod 0755 "$FIXTURE/bin/$command_name"
done

set +e
env -i PATH="$FIXTURE/bin:$PATH" "$IDENTIFIER" /dev/sdz \
	"$ROOT/reports/forbidden-direct-receipt" \
	>"$FIXTURE/direct.out" 2>"$FIXTURE/direct.err"
direct_rc=$?
set -e
[[ $direct_rc -ne 0 ]] || die direct-invocation-accepted
grep -Fq 'reason=dispatcher-authorization-required' "$FIXTURE/direct.err" || \
	die direct-invocation-wrong-rejection
[[ ! -e $FIXTURE/device-command-called ]] || die direct-invocation-inspected-device
[[ ! -e $ROOT/reports/forbidden-direct-receipt ]] || \
	die direct-invocation-published-receipt

# Reuse a verifier's inherited operation lock when present; otherwise acquire
# the same persistent lock without truncating it.  Merely holding the lock is
# insufficient without dispatcher-bound mode, device and receipt authority.
inherited_lock_fd=${RG40XXV_P8_OPERATION_LOCK_FD:-}
if [[ $inherited_lock_fd =~ ^[0-9]+$ && \
	-e /proc/$$/fd/$inherited_lock_fd && \
	$(readlink -f -- "/proc/$$/fd/$inherited_lock_fd") == "$LOCK" ]]; then
	use_inherited_lock=1
else
	use_inherited_lock=0
fi
set +e
(
	if [[ $use_inherited_lock -eq 1 ]]; then
		test_lock_fd=$inherited_lock_fd
	else
		exec 7<>"$LOCK"
		flock 7
		test_lock_fd=7
	fi
	env -u RG40XXV_P8_AUTH_MODE -u RG40XXV_P8_AUTH_DEVICE \
		-u RG40XXV_P8_AUTH_RECEIPT -u RG40XXV_P8_AUTH_REGISTRY_SHA256 \
		RG40XXV_P8_OPERATION_LOCK_FD="$test_lock_fd" \
		PATH="$FIXTURE/bin:$PATH" "$IDENTIFIER" /dev/sdz \
		"$ROOT/reports/forbidden-lock-only-receipt"
) >"$FIXTURE/lock-only.out" 2>"$FIXTURE/lock-only.err"
lock_only_rc=$?
set -e
[[ $lock_only_rc -ne 0 ]] || die lock-only-invocation-accepted
grep -Fq 'reason=dispatcher-mode-authorization-mismatch' \
	"$FIXTURE/lock-only.err" || \
	die lock-only-wrong-rejection
[[ ! -e $FIXTURE/device-command-called ]] || die lock-only-invocation-inspected-device
[[ ! -e $ROOT/reports/forbidden-lock-only-receipt ]] || \
	die lock-only-invocation-published-receipt

# Even with a real inherited lock FD, authorization is argument-bound and
# fail-closed.  These cases still must not reach a mocked device command.
run_authorized_rejection()
{
	local expected_reason=$1 mode=$2 auth_device=$3 auth_receipt=$4
	local call_device=$5 call_receipt=$6 label=$7 rc
	set +e
	(
		if [[ $use_inherited_lock -eq 1 ]]; then
			test_lock_fd=$inherited_lock_fd
		else
			exec 7<>"$LOCK"
			flock 7
			test_lock_fd=7
		fi
		RG40XXV_P8_OPERATION_LOCK_FD="$test_lock_fd" \
		RG40XXV_P8_AUTH_MODE="$mode" \
		RG40XXV_P8_AUTH_DEVICE="$auth_device" \
		RG40XXV_P8_AUTH_RECEIPT="$auth_receipt" \
		PATH="$FIXTURE/bin:$PATH" \
			"$IDENTIFIER" "$call_device" "$call_receipt"
	) >"$FIXTURE/$label.out" 2>"$FIXTURE/$label.err"
	rc=$?
	set -e
	[[ $rc -ne 0 ]] || die "$label-accepted"
	grep -Fq "reason=$expected_reason" "$FIXTURE/$label.err" || \
		die "$label-wrong-rejection"
	[[ ! -e $FIXTURE/device-command-called ]] || die "$label-inspected-device"
}

authorized_receipt="$ROOT/reports/forbidden-authorized-receipt"
run_authorized_rejection dispatcher-mode-authorization-mismatch recover \
	/dev/sdz "$authorized_receipt" /dev/sdz "$authorized_receipt" wrong-mode
run_authorized_rejection dispatcher-device-authorization-mismatch identify \
	/dev/sdy "$authorized_receipt" /dev/sdz "$authorized_receipt" wrong-device
run_authorized_rejection dispatcher-receipt-authorization-mismatch identify \
	/dev/sdz "$ROOT/reports/a-different-receipt" /dev/sdz \
	"$authorized_receipt" wrong-receipt
run_authorized_rejection target-is-not-a-direct-block-device identify \
	/dev/sdz "$authorized_receipt" /dev/sdz "$authorized_receipt" no-block-device
outside_receipt="$FIXTURE/outside-receipt"
run_authorized_rejection receipt-outside-reports identify /dev/sdz \
	"$outside_receipt" /dev/sdz "$outside_receipt" outside-receipt
[[ ! -e $authorized_receipt && ! -e $outside_receipt ]] || \
	die rejected-authorization-published-receipt

# Static invariants complement the early-rejection tripwires.  There may be
# read-only blockdev/dd operations, but never RO mutation, output dd, mount,
# filesystem creation, or a device path chosen implicitly.
grep -Fq 'EXPECTED_DISK_BYTES=62516101120' "$IDENTIFIER" || die disk-size-pin-missing
grep -Fq 'EXPECTED_GUID=PUT-YOUR-OWN-CARD-GPT-GUID-HERE' "$IDENTIFIER" || \
	die gpt-guid-pin-missing
grep -Fq 'P4_SHA=09bb1eee75a3ae1b2950c0886c0777abd1a48a266e045376a1b9e098894fc519' \
	"$IDENTIFIER" || die p4-sha-pin-missing
grep -Fq 'P8_OUTGOING_POLICY=UNKNOWN' "$IDENTIFIER" || \
	die p8-unknown-outgoing-policy-missing
grep -Fq 'P8_POLICY=UNREGISTERED' "$IDENTIFIER" || \
	die p8-unregistered-target-policy-missing
grep -Fq 'P8_IDENTITY=UNREGISTERED' "$IDENTIFIER" || \
	die p8-unregistered-identity-missing
grep -Fq 'disk-read-only-state' "$IDENTIFIER" || die disk-ro-gate-missing
grep -Fq 'all_partitions_unmounted=PASS' "$IDENTIFIER" || die mount-gate-missing
grep -Fq 'declare -A PART_NODE_ID=() PART_FD=()' "$IDENTIFIER" || \
	die partition-fd-pinning-missing
grep -Fq 'DISKSEQ_FILE=/sys/class/block/$DISK_NAME/diskseq' "$IDENTIFIER" || \
	die diskseq-stability-gate-missing
grep -Fq 'sfdisk -d "$DEVICE_FD_PATH"' "$IDENTIFIER" || \
	die pinned-gpt-read-missing
grep -Fq 'blockdev --getro "/proc/$$/fd/${PART_FD[$num]}"' "$IDENTIFIER" || \
	die pinned-partition-ro-read-missing
for layout_row in \
	'1 37748736 47244640256' \
	'2 47282388992 33554432' \
	'3 47315943424 16777216' \
	'4 47332720640 67108864' \
	'5 47399829504 7516192768' \
	'6 54916022272 4294967296' \
	'7 59210989568 2679111680' \
	'8 61890101248 67108864'; do
	grep -Fq "$layout_row" "$IDENTIFIER" || die exact-layout-pin-missing
done
grep -Fq 'chmod 0444 "$RECEIPT_TMP"' "$IDENTIFIER" || die immutable-receipt-mode-missing
grep -Fq 'ln -- "$RECEIPT_TMP" "$RECEIPT"' "$IDENTIFIER" || \
	die no-clobber-receipt-publication-missing
if grep -Eq 'blockdev[[:space:]]+--set(ro|rw)' "$IDENTIFIER"; then
	die block-device-mode-mutation-present
fi
if grep -Eq 'blockdev[[:space:]]+--(getsize64|getro)[[:space:]]+"\$DEV"|sfdisk[[:space:]]+-d[[:space:]]+"\$DEV"' \
	"$IDENTIFIER"; then
	die raw-device-evidence-read-present
fi
if grep -Eq '(^|[[:space:]])dd[[:space:]][^\n]*[[:space:]]of=' "$IDENTIFIER"; then
	die device-write-dd-present
fi
if grep -Eq '(^|[[:space:]])(mount|umount|mkfs)([[:space:]]|$)' "$IDENTIFIER"; then
	die filesystem-mutation-present
fi

printf 'P8_IDENTIFY_STATIC_TEST result=PASS direct_invocation=REJECTED lock_only=REJECTED device_commands=NOT_CALLED device_write=NONE\n'
