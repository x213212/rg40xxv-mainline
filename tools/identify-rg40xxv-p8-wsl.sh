#!/usr/bin/env bash
# Read-only TF1/p8 identity capture for RG40XX V under WSL.
#
# This is a private helper.  The public dispatcher must hold the shared p8
# operation lock and bind its authorization to both arguments.  The helper
# never mounts a filesystem, changes a block-device read-only flag, or writes
# any byte to the card.  Its only output file is a new, read-only host receipt.
set -euo pipefail
PATH=/usr/sbin:/usr/bin:/sbin:/bin
export PATH

SCRIPT=$(realpath -- "$0")
ROOT=$(CDPATH= cd -- "$(dirname -- "$SCRIPT")/.." && pwd -P)
IMAGE_REGISTRY="$ROOT/tools/rg40xxv-p8-images.tsv"
OPERATION_LOCK="$ROOT/reports/.rg40xxv-p8-operation.lock"

EXPECTED_DISK_BYTES=62516101120
EXPECTED_GUID=PUT-YOUR-OWN-CARD-GPT-GUID-HERE
P4_OFFSET=47332720640
P4_BYTES=67108864
P8_OFFSET=61890101248
P8_BYTES=67108864
P4_SHA=09bb1eee75a3ae1b2950c0886c0777abd1a48a266e045376a1b9e098894fc519

EXPECTED_LAYOUT='1 37748736 47244640256
2 47282388992 33554432
3 47315943424 16777216
4 47332720640 67108864
5 47399829504 7516192768
6 54916022272 4294967296
7 59210989568 2679111680
8 61890101248 67108864'

die()
{
	printf 'P8_IDENTIFY result=FAIL reason=%s device_write=NONE p7_write=NONE\n' \
		"$1" >&2
	exit 1
}

assert_eq()
{
	[[ $1 == "$2" ]] || die "$3-mismatch:actual=$1:expected=$2"
}

[[ $# == 2 ]] || die exactly-two-arguments-required
DEV=$1
RECEIPT_INPUT=$2
[[ $DEV =~ ^/dev/sd[a-z]$ ]] || die explicit-whole-wsl-device-required

# Reject direct invocation before inspecting the supplied block device.
AUTH_FD=${RG40XXV_P8_OPERATION_LOCK_FD:-}
AUTH_MODE=${RG40XXV_P8_AUTH_MODE:-}
AUTH_DEVICE=${RG40XXV_P8_AUTH_DEVICE:-}
AUTH_RECEIPT=${RG40XXV_P8_AUTH_RECEIPT:-}
AUTH_REGISTRY_SHA=${RG40XXV_P8_AUTH_REGISTRY_SHA256:-}
[[ $AUTH_FD =~ ^[0-9]+$ && -e /proc/$$/fd/$AUTH_FD ]] || \
	die dispatcher-authorization-required
[[ $(readlink -f "/proc/$$/fd/$AUTH_FD") == "$OPERATION_LOCK" ]] || \
	die dispatcher-lock-fd-mismatch
[[ -f $OPERATION_LOCK && ! -L $OPERATION_LOCK && \
	$(realpath -e -- "$OPERATION_LOCK") == "$OPERATION_LOCK" ]] || \
	die dispatcher-lock-file-invalid
flock -n "$AUTH_FD" || die dispatcher-lock-fd-not-owner
set +e
(
	exec 8<>"$OPERATION_LOCK" || exit 2
	flock -n 8
)
LOCK_PROBE_RC=$?
set -e
case $LOCK_PROBE_RC in
0) die dispatcher-lock-not-held ;;
1) ;;
*) die dispatcher-lock-probe-failed ;;
esac
[[ $AUTH_MODE == identify ]] || die dispatcher-mode-authorization-mismatch
[[ $AUTH_DEVICE == "$DEV" ]] || die dispatcher-device-authorization-mismatch

RECEIPT=$(realpath -m -- "$RECEIPT_INPUT") || die invalid-receipt-path
[[ $AUTH_RECEIPT == "$RECEIPT" ]] || \
	die dispatcher-receipt-authorization-mismatch

for tool in awk basename blockdev cat chmod cut date dd dirname find findmnt \
	flock grep ln mktemp readlink realpath rm sfdisk sha256sum stat; do
	command -v "$tool" >/dev/null 2>&1 || die "missing-tool:$tool"
done

# Receipts are atomic, no-clobber publications under the host reports tree.
# Mode 0444 is an operational guard, not a claim of cryptographic immutability;
# every consumer must rehash the receipt.
REPORTS_REAL=$(realpath -e -- "$ROOT/reports") || die reports-path-unreadable
RECEIPT_PARENT_INPUT=$(dirname -- "$RECEIPT")
RECEIPT_PARENT=$(realpath -e -- "$RECEIPT_PARENT_INPUT") || \
	die receipt-parent-must-exist
[[ -d $RECEIPT_PARENT && ! -L $RECEIPT_PARENT_INPUT ]] || \
	die receipt-parent-invalid
[[ $RECEIPT_PARENT == "$REPORTS_REAL" || \
	$RECEIPT_PARENT == "$REPORTS_REAL/"* ]] || die receipt-outside-reports
REPORTS_MOUNT_ID=$(findmnt -nro ID -T "$REPORTS_REAL") || die reports-mount-read-failed
RECEIPT_MOUNT_ID=$(findmnt -nro ID -T "$RECEIPT_PARENT") || \
	die receipt-mount-read-failed
[[ -n $REPORTS_MOUNT_ID && $RECEIPT_MOUNT_ID == "$REPORTS_MOUNT_ID" && \
	$(stat -c %d -- "$RECEIPT_PARENT") == $(stat -c %d -- "$REPORTS_REAL") ]] || \
	die receipt-parent-not-on-reports-filesystem
RECEIPT_NAME=$(basename -- "$RECEIPT")
[[ $RECEIPT_NAME != . && $RECEIPT_NAME != .. && -n $RECEIPT_NAME ]] || \
	die invalid-receipt-name
RECEIPT="$RECEIPT_PARENT/$RECEIPT_NAME"
[[ ! -e $RECEIPT && ! -L $RECEIPT ]] || die receipt-must-be-new

[[ -b $DEV && ! -L $DEV ]] || die target-is-not-a-direct-block-device
[[ $(readlink -f -- "$DEV") == "$DEV" ]] || die target-device-path-not-canonical
DISK_NAME=$(basename -- "$DEV")
[[ -d /sys/class/block/$DISK_NAME ]] || die disk-sysfs-missing
DISK_SYSFS=$(readlink -f -- "/sys/class/block/$DISK_NAME") || die disk-sysfs-read-failed
DISKSEQ_FILE=/sys/class/block/$DISK_NAME/diskseq
[[ -r $DISKSEQ_FILE ]] || die diskseq-unavailable
DISKSEQ=$(cat "$DISKSEQ_FILE") || die diskseq-read-failed
[[ $DISKSEQ =~ ^[0-9]+$ ]] || die diskseq-invalid
DEV_NODE_ID=$(stat -Lc '%t:%T' "$DEV") || die disk-node-id-read-failed
disk_sysfs_decimal=$(cat "/sys/class/block/$DISK_NAME/dev") || die disk-sysfs-dev-read-failed
[[ $disk_sysfs_decimal =~ ^[0-9]+:[0-9]+$ ]] || die disk-sysfs-dev-invalid
printf -v disk_sysfs_hex '%x:%x' "${disk_sysfs_decimal%:*}" "${disk_sysfs_decimal#*:}"
assert_eq "$DEV_NODE_ID" "$disk_sysfs_hex" whole-device-sysfs-binding
exec {DEVICE_FD}<"$DEV"
DEVICE_FD_PATH=/proc/$$/fd/$DEVICE_FD
assert_eq "$(stat -Lc '%t:%T' "$DEVICE_FD_PATH")" "$DEV_NODE_ID" \
	open-whole-device-identity

declare -A PART_NODE_ID=() PART_FD=()
validate_partition_node()
{
	local num=$1 node=${DEV}$1 name=${DISK_NAME}$1 sysfs decimal expected_hex
	[[ -b $node && ! -L $node && $(readlink -f -- "$node") == "$node" ]] || \
		die "partition-block-device-invalid:p$num"
	[[ -d /sys/class/block/$name ]] || die "partition-sysfs-missing:p$num"
	sysfs=$(readlink -f -- "/sys/class/block/$name") || \
		die "partition-sysfs-read-failed:p$num"
	[[ ${sysfs%/*} == "$DISK_SYSFS" ]] || die "partition-parent-mismatch:p$num"
	assert_eq "$(cat "/sys/class/block/$name/partition")" "$num" \
		"partition-p$num-number"
	decimal=$(cat "/sys/class/block/$name/dev") || die "partition-dev-read-failed:p$num"
	printf -v expected_hex '%x:%x' "${decimal%:*}" "${decimal#*:}"
	assert_eq "$(stat -Lc '%t:%T' "$node")" "$expected_hex" \
		"partition-p$num-node-binding"
	if [[ -n ${PART_NODE_ID[$num]+present} ]]; then
		assert_eq "$(stat -Lc '%t:%T' "$node")" "${PART_NODE_ID[$num]}" \
			"partition-p$num-stable-node"
	else
		PART_NODE_ID[$num]=$(stat -Lc '%t:%T' "$node")
	fi
}

validate_all_device_nodes()
{
	local num
	[[ -b $DEV && ! -L $DEV && $(readlink -f -- "$DEV") == "$DEV" ]] || \
		die whole-device-binding-changed
	assert_eq "$(stat -Lc '%t:%T' "$DEV")" "$DEV_NODE_ID" stable-whole-device-node
	assert_eq "$(readlink -f -- "/sys/class/block/$DISK_NAME")" "$DISK_SYSFS" \
		stable-whole-device-sysfs
	for num in 1 2 3 4 5 6 7 8; do validate_partition_node "$num"; done
}

validate_all_device_nodes
for num in 1 2 3 4 5 6 7 8; do
	exec {fd}<"${DEV}${num}" || die "partition-open-failed:p$num"
	PART_FD[$num]=$fd
	assert_eq "$(stat -Lc '%t:%T' "/proc/$$/fd/$fd")" \
		"${PART_NODE_ID[$num]}" "partition-p$num-open-identity"
done

[[ -f $IMAGE_REGISTRY && ! -L $IMAGE_REGISTRY ]] || die image-registry-missing
exec {REGISTRY_FD}<"$IMAGE_REGISTRY"
REGISTRY_FD_PATH=/proc/$$/fd/$REGISTRY_FD
REGISTRY_ID=$(stat -Lc '%d:%i:%s' "$REGISTRY_FD_PATH")
REGISTRY_SHA=$(sha256sum "$REGISTRY_FD_PATH" | cut -d' ' -f1) || \
	die image-registry-sha-read
assert_eq "$REGISTRY_SHA" "$AUTH_REGISTRY_SHA" authorized-image-registry-sha256
registry_rows=0
declare -A registry_policy=()
declare -A registry_identity=()
while IFS=$'\t' read -r image_sha outgoing_policy target_policy identity extra; do
	[[ -z $image_sha || $image_sha == \#* ]] && continue
	[[ -z ${extra:-} ]] || die invalid-image-registry-field-count
	[[ $image_sha =~ ^[0-9a-f]{64}$ ]] || die invalid-image-registry-sha256
	[[ $outgoing_policy == KNOWN ]] || die "invalid-outgoing-policy:$image_sha"
	[[ $target_policy =~ ^(FROZEN|RECOVERY_ONLY|HOST_ONLY|RETIRED|BANNED)$ ]] ||
		die "invalid-target-policy:$image_sha"
	[[ $identity =~ ^[A-Za-z0-9._-]+$ ]] || die "invalid-image-identity:$image_sha"
	[[ -z ${registry_policy[$image_sha]+present} ]] || \
		die "duplicate-image-registry-sha256:$image_sha"
	registry_policy[$image_sha]=$target_policy
	registry_identity[$image_sha]=$identity
	registry_rows=$((registry_rows + 1))
done < "$REGISTRY_FD_PATH"
[[ $registry_rows -gt 0 ]] || die empty-image-registry

DISK_BYTES=$(blockdev --getsize64 "$DEVICE_FD_PATH") || die disk-size-read-failed
assert_eq "$DISK_BYTES" "$EXPECTED_DISK_BYTES" disk-size
LOGICAL_BLOCK_BYTES=$(cat "/sys/class/block/$DISK_NAME/queue/logical_block_size") || \
	die logical-block-size-read-failed
assert_eq "$LOGICAL_BLOCK_BYTES" 512 logical-block-size
DISK_RO=$(blockdev --getro "$DEVICE_FD_PATH") || die disk-read-only-state-read-failed
assert_eq "$DISK_RO" 1 disk-read-only-state

SFDISK_DUMP=$(sfdisk -d "$DEVICE_FD_PATH") || die gpt-dump-failed
SFDISK_SHA=$(printf '%s\n' "$SFDISK_DUMP" | sha256sum | cut -d' ' -f1)
GUID=$(awk -F': *' '/^label-id:/{print toupper($2); exit}' <<<"$SFDISK_DUMP")
assert_eq "$GUID" "$EXPECTED_GUID" gpt-guid
PARTS=$(awk '$2 == ":" {n++} END {print n+0}' <<<"$SFDISK_DUMP")
assert_eq "$PARTS" 8 partition-count

while read -r num expected_offset expected_size; do
	[[ -n $num ]] || continue
	PART_DEV=${DEV}${num}
	START_FILE=/sys/block/$DISK_NAME/$DISK_NAME$num/start
	SIZE_FILE=/sys/block/$DISK_NAME/$DISK_NAME$num/size
	[[ -r $START_FILE && -r $SIZE_FILE ]] || die "partition-sysfs-missing:p$num"
	start_sectors=$(cat "$START_FILE") || die "partition-start-read-failed:p$num"
	size_sectors=$(cat "$SIZE_FILE") || die "partition-size-read-failed:p$num"
	[[ $start_sectors =~ ^[0-9]+$ && $size_sectors =~ ^[0-9]+$ ]] || \
		die "partition-sysfs-not-numeric:p$num"
	actual_offset=$((start_sectors * 512))
	actual_size=$((size_sectors * 512))
	assert_eq "$actual_offset" "$expected_offset" "partition-p$num-offset"
	assert_eq "$actual_size" "$expected_size" "partition-p$num-size"
done <<< "$EXPECTED_LAYOUT"
validate_all_device_nodes

assert_unmounted()
{
	local node=$1 output rc name devnum
	set +e
	output=$(findmnt -rn -S "$node" -o TARGET 2>/dev/null)
	rc=$?
	set -e
	case $rc in
	0) die "mounted-device:$node:${output:-unknown}" ;;
	1) ;;
	*) die "mount-state-read-failed:$node:rc=$rc" ;;
	esac
	name=$(basename -- "$node")
	[[ -r /sys/class/block/$name/dev ]] || die "mountinfo-device-number-missing:$name"
	devnum=$(cat "/sys/class/block/$name/dev")
	set +e
	awk -v wanted="$devnum" '$3 == wanted { found = 1 } END { exit(found ? 0 : 1) }' \
		/proc/self/mountinfo
	rc=$?
	set -e
	case $rc in
	0) die "mounted-device-number:$node:$devnum" ;;
	1) ;;
	*) die "mountinfo-parse-failed:$node:rc=$rc" ;;
	esac
}

assert_all_unmounted()
{
	local num
	assert_unmounted "$DEV"
	for num in 1 2 3 4 5 6 7 8; do
		assert_unmounted "${DEV}${num}"
	done
}

assert_not_swap()
{
	local source resolved rest
	while read -r source rest; do
		[[ $source == Filename ]] && continue
		resolved=$(readlink -f -- "$source") || die "swap-source-unresolved:$source"
		case $resolved in
		"$DEV"|"${DEV}"[1-8]) die "tf1-node-is-swap:$resolved" ;;
		esac
	done </proc/swaps
}

assert_no_holders()
{
	local num name holder
	for num in '' 1 2 3 4 5 6 7 8; do
		name=$DISK_NAME$num
		holder=$(find "/sys/class/block/$name/holders" -mindepth 1 -maxdepth 1 \
			-print -quit 2>/dev/null) || die "holder-state-read-failed:$name"
		[[ -z $holder ]] || die "tf1-node-has-holder:$name"
	done
}

assert_all_read_only()
{
	local num
	assert_eq "$(blockdev --getro "$DEVICE_FD_PATH")" 1 disk-read-only-state
	for num in 1 2 3 4 5 6 7 8; do
		assert_eq "$(blockdev --getro "/proc/$$/fd/${PART_FD[$num]}")" 1 \
			"partition-p$num-read-only-state"
	done
}

assert_card_stable_and_idle()
{
	local current_dump current_sha
	validate_all_device_nodes
	assert_eq "$(stat -Lc '%t:%T' "$DEVICE_FD_PATH")" "$DEV_NODE_ID" \
		open-whole-device-stable
	assert_eq "$(cat "$DISKSEQ_FILE")" "$DISKSEQ" stable-diskseq
	for num in 1 2 3 4 5 6 7 8; do
		assert_eq "$(stat -Lc '%t:%T' "/proc/$$/fd/${PART_FD[$num]}")" \
			"${PART_NODE_ID[$num]}" "partition-p$num-open-stable"
	done
	assert_eq "$(blockdev --getsize64 "$DEVICE_FD_PATH")" \
		"$EXPECTED_DISK_BYTES" stable-disk-size
	assert_all_read_only
	assert_all_unmounted
	assert_not_swap
	assert_no_holders
	current_dump=$(sfdisk -d "$DEVICE_FD_PATH") || die stable-gpt-dump-failed
	current_sha=$(printf '%s\n' "$current_dump" | sha256sum | cut -d' ' -f1)
	assert_eq "$current_sha" "$SFDISK_SHA" stable-gpt-dump-sha256
}

range_sha()
{
	local source=$1 offset=$2 length=$3 digest_line
	digest_line=$(dd if="$source" bs=1M skip="$offset" count="$length" \
		iflag=skip_bytes,count_bytes status=none | sha256sum) || \
		die range-sha-read-failed
	printf '%s\n' "${digest_line%% *}"
}

assert_card_stable_and_idle
P4_NOW=$(range_sha "$DEVICE_FD_PATH" "$P4_OFFSET" "$P4_BYTES")
assert_eq "$P4_NOW" "$P4_SHA" p4-sha256
P8_NOW=$(range_sha "$DEVICE_FD_PATH" "$P8_OFFSET" "$P8_BYTES")
[[ $P8_NOW =~ ^[0-9a-f]{64}$ ]] || die invalid-p8-sha256
if [[ -n ${registry_policy[$P8_NOW]+present} ]]; then
	P8_OUTGOING_POLICY=KNOWN
	P8_POLICY=${registry_policy[$P8_NOW]}
	P8_IDENTITY=${registry_identity[$P8_NOW]}
else
	# A read-only identify must still be able to name a partially written p8 so
	# the operator can explicitly authorize frozen recovery.  This is evidence,
	# not permission: candidate manifest validation requires KNOWN.
	P8_OUTGOING_POLICY=UNKNOWN
	P8_POLICY=UNREGISTERED
	P8_IDENTITY=UNREGISTERED
fi

# Recheck every non-mutating guard after the full reads, so a hot-unplug,
# mount, RO-state change, or registry replacement cannot publish a receipt.
assert_card_stable_and_idle
assert_eq "$(stat -Lc '%d:%i:%s' "$IMAGE_REGISTRY")" "$REGISTRY_ID" \
	image-registry-node-postread
assert_eq "$(sha256sum "$REGISTRY_FD_PATH" | cut -d' ' -f1)" \
	"$REGISTRY_SHA" image-registry-postread-sha256

umask 077
RECEIPT_TMP=$(mktemp "$RECEIPT_PARENT/.${RECEIPT_NAME}.tmp.XXXXXX") || \
	die receipt-temp-create-failed
cleanup_receipt_tmp()
{
	rm -f -- "${RECEIPT_TMP:-}"
}
trap cleanup_receipt_tmp EXIT

{
	printf 'schema=rg40xxv-p8-identify-receipt-v2\n'
	printf 'created_utc=%s\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
	printf 'device=%s\n' "$DEV"
	printf 'disk_kernel_name=%s\n' "$DISK_NAME"
	printf 'disk_node_id=%s\n' "$DEV_NODE_ID"
	printf 'disk_device_number=%s\n' "$disk_sysfs_decimal"
	printf 'disk_sysfs_path=%s\n' "$DISK_SYSFS"
	printf 'diskseq=%s\n' "$DISKSEQ"
	printf 'disk_bytes=%s\n' "$DISK_BYTES"
	printf 'logical_block_bytes=%s\n' "$LOGICAL_BLOCK_BYTES"
	printf 'disk_read_only=1\n'
	printf 'gpt_guid=%s\n' "$GUID"
	printf 'gpt_dump_sha256=%s\n' "$SFDISK_SHA"
	printf 'partition_count=8\n'
	while read -r num offset size; do
		[[ -n $num ]] || continue
		printf 'p%s_offset=%s\n' "$num" "$offset"
		printf 'p%s_bytes=%s\n' "$num" "$size"
	done <<< "$EXPECTED_LAYOUT"
	printf 'all_partitions_unmounted=PASS\n'
	printf 'p4_sha256=%s\n' "$P4_NOW"
	printf 'p8_sha256=%s\n' "$P8_NOW"
	printf 'p8_registry_outgoing_policy=%s\n' "$P8_OUTGOING_POLICY"
	printf 'p8_registry_target_policy=%s\n' "$P8_POLICY"
	printf 'p8_registry_identity=%s\n' "$P8_IDENTITY"
	printf 'image_registry_sha256=%s\n' "$REGISTRY_SHA"
	printf 'device_write=NONE\n'
	printf 'p7_write=NONE\n'
} > "$RECEIPT_TMP"
chmod 0444 "$RECEIPT_TMP" || die receipt-chmod-failed
# A hard-link publication is atomic and cannot replace an existing receipt.
ln -- "$RECEIPT_TMP" "$RECEIPT" || die receipt-publish-conflict
rm -f -- "$RECEIPT_TMP"
RECEIPT_TMP=
trap - EXIT

assert_eq "$(stat -c %a -- "$RECEIPT")" 444 receipt-mode
RECEIPT_SHA=$(sha256sum "$RECEIPT" | cut -d' ' -f1) || \
	die receipt-sha256-read-failed
printf 'P8_IDENTIFY result=PASS device=%s disk_bytes=%s gpt_guid=%s p4_sha256=%s p8_sha256=%s p8_outgoing_policy=%s p8_policy=%s p8_identity=%s receipt=%s receipt_sha256=%s device_write=NONE p7_write=NONE\n' \
	"$DEV" "$DISK_BYTES" "$GUID" "$P4_NOW" "$P8_NOW" \
	"$P8_OUTGOING_POLICY" "$P8_POLICY" "$P8_IDENTITY" "$RECEIPT" "$RECEIPT_SHA"
