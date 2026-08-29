#!/usr/bin/env bash
# Private fail-closed helper for arming an exactly identified RG40XX V TF1
# reader device read-only.  It changes only the kernel block-device RO flags;
# it never mounts a filesystem and never reads or writes partition payloads.
set -euo pipefail
PATH=/usr/sbin:/usr/bin:/sbin:/bin
export PATH

SCRIPT=$(realpath -- "$0")
ROOT=$(CDPATH= cd -- "${SCRIPT%/*}/.." && pwd -P)
OPERATION_LOCK=$ROOT/reports/.rg40xxv-p8-operation.lock

EXPECTED_DISK_BYTES=62516101120
EXPECTED_GUID=PUT-YOUR-OWN-CARD-GPT-GUID-HERE
EXPECTED_LAYOUT='1 37748736 47244640256
2 47282388992 33554432
3 47315943424 16777216
4 47332720640 67108864
5 47399829504 7516192768
6 54916022272 4294967296
7 59210989568 2679111680
8 61890101248 67108864'

FAIL_REPORTED=0
RO_RETRY_ACTIVE=0

fail_without_device()
{
	FAIL_REPORTED=1
	printf 'P8_READONLY_ARM result=FAIL reason=%s device_write=NONE p7_write=NONE\n' \
		"$1" >&2
	exit 1
}

die()
{
	FAIL_REPORTED=1
	printf 'P8_READONLY_ARM result=FAIL reason=%s device_write=NONE p7_write=NONE\n' \
		"$1" >&2
	exit 1
}

[[ $# -eq 1 ]] || fail_without_device exactly-one-whole-device-required
DEV=$1
case $DEV in
	/dev/sd[a-z]) ;;
	*) fail_without_device unsupported-whole-device ;;
esac
DISK_NAME=${DEV##*/}

for tool in awk blockdev cat find findmnt flock readlink realpath sfdisk \
	sha256sum stat; do
	command -v "$tool" >/dev/null 2>&1 || \
		fail_without_device "missing-tool:$tool"
done

# The public dispatcher must both authorize this exact path and pass the
# already-held common p8 operation lock.  Reject before any device command.
AUTH_FD=${RG40XXV_P8_OPERATION_LOCK_FD:-}
AUTH_MODE=${RG40XXV_P8_AUTH_MODE:-}
AUTH_DEVICE=${RG40XXV_P8_AUTH_DEVICE:-}
[[ $AUTH_FD =~ ^[0-9]+$ && -e /proc/$$/fd/$AUTH_FD && \
	$AUTH_MODE == arm-readonly && $AUTH_DEVICE == "$DEV" ]] || \
	fail_without_device dispatcher-authorization-required
[[ -f $OPERATION_LOCK && ! -L $OPERATION_LOCK && \
	$(realpath -e -- "$OPERATION_LOCK") == "$OPERATION_LOCK" ]] || \
	fail_without_device dispatcher-lock-file-invalid
[[ $(readlink -f -- "/proc/$$/fd/$AUTH_FD") == "$OPERATION_LOCK" ]] || \
	fail_without_device dispatcher-lock-fd-mismatch
flock -n "$AUTH_FD" || fail_without_device dispatcher-lock-fd-not-owner
set +e
(
	exec 8<>"$OPERATION_LOCK" || exit 2
	flock -n 8
)
LOCK_PROBE_RC=$?
set -e
case $LOCK_PROBE_RC in
	0) fail_without_device dispatcher-lock-not-held ;;
	1) ;;
	*) fail_without_device dispatcher-lock-probe-failed ;;
esac

retry_read_only()
{
	local num failed=0
	blockdev --setro "/proc/$$/fd/$DISK_FD" >/dev/null 2>&1 || failed=1
	for num in 1 2 3 4 5 6 7 8; do
		blockdev --setro "/proc/$$/fd/${PART_FD[$num]}" >/dev/null 2>&1 || failed=1
	done
	[[ $(blockdev --getro "/proc/$$/fd/$DISK_FD" 2>/dev/null) == 1 ]] || failed=1
	for num in 1 2 3 4 5 6 7 8; do
		[[ $(blockdev --getro "/proc/$$/fd/${PART_FD[$num]}" 2>/dev/null) == 1 ]] || failed=1
	done
	[[ $failed -eq 0 ]]
}

on_exit()
{
	local rc=$?
	trap - EXIT INT TERM HUP
	if [[ $RO_RETRY_ACTIVE -eq 1 ]]; then
		retry_read_only || true
	fi
	if [[ $rc -ne 0 && $FAIL_REPORTED -eq 0 ]]; then
		printf 'P8_READONLY_ARM result=FAIL reason=unexpected-error device_write=NONE p7_write=NONE\n' >&2
	fi
	exit "$rc"
}
trap on_exit EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP

assert_eq()
{
	[[ $1 == "$2" ]] || die "$3:actual=$1:expected=$2"
}

[[ -b $DEV && ! -L $DEV && $(readlink -f -- "$DEV") == "$DEV" ]] || \
	die whole-device-not-canonical-direct-block-node
[[ -d /sys/class/block/$DISK_NAME ]] || die whole-device-sysfs-node-missing
DISK_SYSFS=$(readlink -f -- "/sys/class/block/$DISK_NAME")
[[ -n $DISK_SYSFS && -d $DISK_SYSFS ]] || die whole-device-sysfs-path-invalid
DISK_DEV_DEC=$(cat -- "$DISK_SYSFS/dev") || die whole-device-sysfs-device-unreadable
[[ $DISK_DEV_DEC =~ ^[0-9]+:[0-9]+$ ]] || die whole-device-sysfs-device-invalid
DISKSEQ_FILE=$DISK_SYSFS/diskseq
[[ -r $DISKSEQ_FILE ]] || die whole-device-diskseq-unavailable
DISKSEQ=$(cat -- "$DISKSEQ_FILE") || die whole-device-diskseq-unreadable
[[ $DISKSEQ =~ ^[0-9]+$ ]] || die whole-device-diskseq-invalid
printf -v DISK_DEV_HEX '%x:%x' "${DISK_DEV_DEC%:*}" "${DISK_DEV_DEC#*:}"
assert_eq "$(stat -Lc '%t:%T' -- "$DEV")" "$DISK_DEV_HEX" \
	whole-device-node-sysfs-binding

declare -A PART_SYSFS PART_DEV_DEC PART_DEV_HEX PART_FD TARGET_DEV_DEC TARGET_DEV_HEX
TARGET_DEV_DEC[$DISK_DEV_DEC]=whole-device
TARGET_DEV_HEX[$DISK_DEV_HEX]=whole-device

validate_partition()
{
	local num=$1 node=${DEV}$1 name=${DISK_NAME}$1 sysfs dev_dec dev_hex
	[[ -b $node && ! -L $node && $(readlink -f -- "$node") == "$node" ]] || \
		die "p${num}-not-canonical-direct-block-node"
	[[ -d /sys/class/block/$name ]] || die "p${num}-sysfs-node-missing"
	sysfs=$(readlink -f -- "/sys/class/block/$name")
	[[ ${sysfs%/*} == "$DISK_SYSFS" ]] || die "p${num}-sysfs-parent-mismatch"
	assert_eq "$(cat "/sys/class/block/$name/partition")" "$num" \
		"p${num}-partition-number"
	dev_dec=$(cat "/sys/class/block/$name/dev")
	printf -v dev_hex '%x:%x' "${dev_dec%:*}" "${dev_dec#*:}"
	assert_eq "$(stat -Lc '%t:%T' -- "$node")" "$dev_hex" \
		"p${num}-node-sysfs-binding"
	PART_SYSFS[$num]=$sysfs
	PART_DEV_DEC[$num]=$dev_dec
	PART_DEV_HEX[$num]=$dev_hex
	TARGET_DEV_DEC[$dev_dec]="p$num"
	TARGET_DEV_HEX[$dev_hex]="p$num"
}

for num in 1 2 3 4 5 6 7 8; do validate_partition "$num"; done

# Pin the already validated kernel block objects.  Every later ioctl and
# identity read uses these descriptors, so a /dev path cannot be substituted
# by a hot-unplug/replug race after validation.
exec {DISK_FD}<"$DEV" || die whole-device-open-failed
assert_eq "$(stat -Lc '%t:%T' -- "/proc/$$/fd/$DISK_FD")" "$DISK_DEV_HEX" \
	whole-device-fd-identity
for num in 1 2 3 4 5 6 7 8; do
	node=${DEV}$num
	exec {fd}<"$node" || die "p${num}-open-failed"
	PART_FD[$num]=$fd
	assert_eq "$(stat -Lc '%t:%T' -- "/proc/$$/fd/$fd")" \
		"${PART_DEV_HEX[$num]}" "p${num}-fd-identity"
done

validate_nodes_stable()
{
	local num node name sysfs dev_dec dev_hex diskseq
	[[ -b $DEV && ! -L $DEV && $(readlink -f -- "$DEV") == "$DEV" ]] || \
		die whole-device-binding-changed
	assert_eq "$(readlink -f -- "/sys/class/block/$DISK_NAME")" \
		"$DISK_SYSFS" whole-device-sysfs-path-stability
	assert_eq "$(stat -Lc '%t:%T' -- "$DEV")" "$DISK_DEV_HEX" \
		whole-device-node-identity-stability
	dev_dec=$(cat -- "$DISK_SYSFS/dev") || die whole-device-sysfs-device-unreadable
	[[ $dev_dec =~ ^[0-9]+:[0-9]+$ ]] || die whole-device-sysfs-device-invalid
	assert_eq "$dev_dec" "$DISK_DEV_DEC" whole-device-sysfs-device-stability
	diskseq=$(cat -- "$DISKSEQ_FILE") || die whole-device-diskseq-unreadable
	[[ $diskseq =~ ^[0-9]+$ ]] || die whole-device-diskseq-invalid
	assert_eq "$diskseq" "$DISKSEQ" whole-device-diskseq-stability
	for num in 1 2 3 4 5 6 7 8; do
		node=${DEV}$num
		name=${DISK_NAME}$num
		[[ -b $node && ! -L $node && $(readlink -f -- "$node") == "$node" ]] || \
			die "p${num}-binding-changed"
		sysfs=$(readlink -f -- "/sys/class/block/$name")
		assert_eq "$sysfs" "${PART_SYSFS[$num]}" "p${num}-sysfs-path-stability"
		dev_dec=$(cat "/sys/class/block/$name/dev")
		assert_eq "$dev_dec" "${PART_DEV_DEC[$num]}" "p${num}-sysfs-device-stability"
		printf -v dev_hex '%x:%x' "${dev_dec%:*}" "${dev_dec#*:}"
		assert_eq "$(stat -Lc '%t:%T' -- "$node")" "$dev_hex" \
			"p${num}-node-identity-stability"
	done
}

assert_eq "$(blockdev --getsize64 "/proc/$$/fd/$DISK_FD")" \
	"$EXPECTED_DISK_BYTES" disk-size
assert_eq "$(blockdev --getss "/proc/$$/fd/$DISK_FD")" 512 logical-sector-size

SFDISK_DUMP=$(sfdisk -d "/proc/$$/fd/$DISK_FD") || die gpt-dump-failed
SFDISK_SHA=$(printf '%s\n' "$SFDISK_DUMP" | sha256sum | awk '{print $1}')
GPT_LABEL=$(awk -F': ' '$1 == "label" {print tolower($2)}' <<<"$SFDISK_DUMP")
GPT_GUID=$(awk -F': ' '$1 == "label-id" {print toupper($2)}' <<<"$SFDISK_DUMP")
# sfdisk names entries after the path it was given.  Because this helper uses
# a pinned /proc/.../fd/N path, its dump uses ".../fd/Np1" rather than the
# original /dev/sdX1 spelling.  Count grammar-level partition records; exact
# p1-p8 identity and byte layout are independently verified through sysfs.
PARTITION_COUNT=$(awk '$2 == ":" {n++} END {print n+0}' <<<"$SFDISK_DUMP")
assert_eq "$GPT_LABEL" gpt gpt-label
assert_eq "$GPT_GUID" "$EXPECTED_GUID" gpt-guid
assert_eq "$PARTITION_COUNT" 8 partition-count

assert_exact_layout()
{
	local num expected_offset expected_bytes actual_offset actual_bytes
	while read -r num expected_offset expected_bytes; do
		[[ -n $num ]] || continue
		actual_offset=$(( $(cat "/sys/block/$DISK_NAME/$DISK_NAME$num/start") * 512 ))
		actual_bytes=$(( $(cat "/sys/block/$DISK_NAME/$DISK_NAME$num/size") * 512 ))
		assert_eq "$actual_offset" "$expected_offset" "p${num}-offset"
		assert_eq "$actual_bytes" "$expected_bytes" "p${num}-size"
	done <<<"$EXPECTED_LAYOUT"
}
assert_exact_layout

assert_no_active_consumers()
{
	local table devnum target swap_path swap_hex num holder

	# Check util-linux's resolved mount view by major:minor, not by a possibly
	# aliased /dev spelling.
	table=$(findmnt -rn -o MAJ:MIN,TARGET) || die findmnt-query-failed
	while read -r devnum target; do
		[[ -n $devnum ]] || continue
		[[ -z ${TARGET_DEV_DEC[$devnum]+present} ]] || \
			die "mounted-findmnt:${TARGET_DEV_DEC[$devnum]}:$target"
	done <<<"$table"

	# Independently inspect the kernel mount table's field 3 (major:minor).
	while read -r _ _ devnum _; do
		[[ -z ${TARGET_DEV_DEC[$devnum]+present} ]] || \
			die "mounted-mountinfo:${TARGET_DEV_DEC[$devnum]}"
	done </proc/self/mountinfo

	# A direct partition swap device may have no filesystem mount.
	while read -r swap_path _; do
		[[ $swap_path == Filename ]] && continue
		[[ -b $swap_path ]] || continue
		swap_hex=$(stat -Lc '%t:%T' -- "$swap_path") || die swap-device-stat-failed
		[[ -z ${TARGET_DEV_HEX[$swap_hex]+present} ]] || \
			die "active-swap:${TARGET_DEV_HEX[$swap_hex]}"
	done </proc/swaps

	for num in 0 1 2 3 4 5 6 7 8; do
		if [[ $num -eq 0 ]]; then
			holder=$(find "$DISK_SYSFS/holders" -mindepth 1 -maxdepth 1 -print -quit)
			[[ -z $holder ]] || die whole-device-has-holder
		else
			holder=$(find "${PART_SYSFS[$num]}/holders" -mindepth 1 -maxdepth 1 -print -quit)
			[[ -z $holder ]] || die "p${num}-has-holder"
		fi
	done
}
assert_no_active_consumers
validate_nodes_stable

# Arm the parent first, then attempt every partition even after an individual
# ioctl failure.  verify-all is part of the operation, not an advisory check.
# Only now, after exact identity/layout/consumer checks and FD pinning, enable
# the exit retry.  No earlier failure is allowed to issue an ioctl to DEV.
RO_RETRY_ACTIVE=1
retry_read_only || die read-only-arm-or-verification-failed

# Recheck identity, layout, GPT bytes and active consumers after the ioctls.
validate_nodes_stable
assert_eq "$(blockdev --getsize64 "/proc/$$/fd/$DISK_FD")" "$EXPECTED_DISK_BYTES" \
	post-arm-disk-size
POST_SFDISK_DUMP=$(sfdisk -d "/proc/$$/fd/$DISK_FD") || die post-arm-gpt-dump-failed
POST_SFDISK_SHA=$(printf '%s\n' "$POST_SFDISK_DUMP" | sha256sum | awk '{print $1}')
assert_eq "$POST_SFDISK_SHA" "$SFDISK_SHA" gpt-stability
assert_exact_layout
assert_no_active_consumers
assert_eq "$(blockdev --getro "/proc/$$/fd/$DISK_FD")" 1 parent-read-only
for num in 1 2 3 4 5 6 7 8; do
	assert_eq "$(blockdev --getro "/proc/$$/fd/${PART_FD[$num]}")" 1 \
		"p${num}-read-only"
done

RO_RETRY_ACTIVE=0
printf 'P8_READONLY_ARM result=PASS device=%s disk_bytes=%s gpt_guid=%s parent_ro=1 p1-p8_ro=1 device_write=NONE p7_write=NONE\n' \
	"$DEV" "$EXPECTED_DISK_BYTES" "$EXPECTED_GUID"
