#!/bin/bash
# Range-checked WSL p8 flasher for RG40XX V TF1.
# Mirrors every guard in tools/deploy-rg40xxv-p8-vblank-flush-v1.ps1:
#   exact disk identity, all 8 partition offsets/sizes, untouched stock p4,
#   current-p8 allowlist, pre-write backup, p8-range-only write,
#   full readback verification, p4 re-check, RO guard restored.
set -euo pipefail
PATH=/usr/sbin:/usr/bin:/sbin:/bin
export PATH

SCRIPT=$(realpath -- "$0")
ROOT=$(CDPATH= cd -- "$(dirname -- "$SCRIPT")/.." && pwd -P)
[[ $# == 4 ]] || {
  printf 'P8_FLASH result=FAIL reason=exactly-four-arguments-required\n' >&2
  exit 2
}
DEV=$4
DISK_NAME=$(basename -- "$DEV")
case $DISK_NAME in
	sd[a-z]) ;;
	*) printf 'unsupported RG40XXV_DEV: %s\n' "$DEV" >&2; exit 2 ;;
esac
P8DEV=${DEV}8
P8_NAME=${DISK_NAME}8
CANDIDATE_INPUT="${1:?candidate image path required}"
CANDIDATE=$CANDIDATE_INPUT
CANDIDATE_SHA="${2:?expected candidate sha256 required}"
BACKUP_INPUT="${3:?pre-write backup directory required}"
BACKUP_DIR=$(realpath -m -- "$BACKUP_INPUT") || {
  printf 'P8_FLASH result=FAIL reason=invalid-backup-path\n' >&2
  exit 2
}

EXPECTED_DISK_BYTES=62516101120
EXPECTED_GUID="PUT-YOUR-OWN-CARD-GPT-GUID-HERE"
P4_OFFSET=47332720640; P4_BYTES=67108864
P8_OFFSET=61890101248; P8_BYTES=67108864
P4_SHA="09bb1eee75a3ae1b2950c0886c0777abd1a48a266e045376a1b9e098894fc519"
IMAGE_REGISTRY="$ROOT/tools/rg40xxv-p8-images.tsv"
OPERATION_LOCK="$ROOT/reports/.rg40xxv-p8-operation.lock"
BACKUP_ROOT="$ROOT/backups"

for tool in awk basename blockdev cat cut date dd find findmnt flock grep ln \
  mkdir mktemp readlink realpath rm sfdisk sha256sum stat sync tee; do
  command -v "$tool" >/dev/null 2>&1 || {
    printf 'P8_FLASH result=FAIL reason=missing-tool:%s\n' "$tool" >&2
    exit 1
  }
done

AUTH_FD=${RG40XXV_P8_OPERATION_LOCK_FD:-}
AUTH_MODE=${RG40XXV_P8_AUTH_MODE:-}
AUTH_SHA=${RG40XXV_P8_AUTH_SHA256:-}
AUTH_DEVICE=${RG40XXV_P8_AUTH_DEVICE:-}
AUTH_BACKUP=${RG40XXV_P8_AUTH_BACKUP:-}
AUTH_CANDIDATE=${RG40XXV_P8_AUTH_CANDIDATE:-}
AUTH_REGISTRY_SHA=${RG40XXV_P8_AUTH_REGISTRY_SHA256:-}
AUTH_RUN=${RG40XXV_P8_AUTH_RUN:-}
AUTH_BUILD_LOCK_SHA=${RG40XXV_P8_AUTH_BUILD_LOCK_SHA256:-}
AUTH_FLASH_AUTH_SHA=${RG40XXV_P8_AUTH_FLASH_AUTHORIZATION_SHA256:-}
AUTH_OUTGOING_SHA=${RG40XXV_P8_AUTH_OUTGOING_SHA256:-}
AUTH_RESULT=${RG40XXV_P8_AUTH_RESULT:-}
AUTH_DISK_NODE_ID=${RG40XXV_P8_AUTH_DISK_NODE_ID:-}
AUTH_DISK_DEVICE_NUMBER=${RG40XXV_P8_AUTH_DISK_DEVICE_NUMBER:-}
AUTH_DISK_SYSFS_PATH=${RG40XXV_P8_AUTH_DISK_SYSFS_PATH:-}
AUTH_DISKSEQ=${RG40XXV_P8_AUTH_DISKSEQ:-}
AUTH_UNKNOWN_CURRENT_SHA=${RG40XXV_P8_AUTH_UNKNOWN_CURRENT_SHA256:-}
[[ $AUTH_FD =~ ^[0-9]+$ && -e /proc/$$/fd/$AUTH_FD ]] || {
  printf 'P8_FLASH result=FAIL reason=dispatcher-authorization-required\n' >&2
  exit 1
}
[[ -f $OPERATION_LOCK && ! -L $OPERATION_LOCK && \
   $(realpath -e -- "$OPERATION_LOCK") == "$OPERATION_LOCK" ]] || {
  printf 'P8_FLASH result=FAIL reason=dispatcher-lock-file-invalid\n' >&2
  exit 1
}
[[ $(readlink -f "/proc/$$/fd/$AUTH_FD") == "$OPERATION_LOCK" ]] || {
  printf 'P8_FLASH result=FAIL reason=dispatcher-lock-fd-mismatch\n' >&2
  exit 1
}
flock -n "$AUTH_FD" || {
  printf 'P8_FLASH result=FAIL reason=dispatcher-lock-fd-not-owner\n' >&2
  exit 1
}
set +e
(
  exec 8<>"$OPERATION_LOCK" || exit 2
  flock -n 8
)
LOCK_PROBE_RC=$?
set -e
case $LOCK_PROBE_RC in
  0) printf 'P8_FLASH result=FAIL reason=dispatcher-lock-not-held\n' >&2; exit 1 ;;
  1) ;;
  *) printf 'P8_FLASH result=FAIL reason=dispatcher-lock-probe-failed\n' >&2; exit 1 ;;
esac
[[ $AUTH_MODE =~ ^(recover|candidate)$ && $AUTH_SHA == "$CANDIDATE_SHA" && \
   $AUTH_DEVICE == "$DEV" && $AUTH_BACKUP == "$BACKUP_DIR" ]] || {
  printf 'P8_FLASH result=FAIL reason=dispatcher-target-authorization-mismatch\n' >&2
  exit 1
}
[[ $AUTH_DISK_NODE_ID =~ ^[0-9a-f]+:[0-9a-f]+$ && \
   $AUTH_DISK_DEVICE_NUMBER =~ ^[0-9]+:[0-9]+$ && \
   $AUTH_DISK_SYSFS_PATH == /sys/devices/* && $AUTH_DISKSEQ =~ ^[0-9]+$ ]] || {
  printf 'P8_FLASH result=FAIL reason=dispatcher-device-binding-invalid\n' >&2
  exit 1
}
case $AUTH_MODE in
  candidate)
    [[ -z $AUTH_UNKNOWN_CURRENT_SHA ]] || {
      printf 'P8_FLASH result=FAIL reason=candidate-unknown-current-override-forbidden\n' >&2
      exit 1
    }
    ;;
  recover)
    [[ -z $AUTH_UNKNOWN_CURRENT_SHA || \
       $AUTH_UNKNOWN_CURRENT_SHA =~ ^[0-9a-f]{64}$ ]] || {
      printf 'P8_FLASH result=FAIL reason=recovery-unknown-current-confirmation-invalid\n' >&2
      exit 1
    }
    ;;
esac
CANDIDATE=$(realpath -e -- "$CANDIDATE_INPUT") || {
  printf 'P8_FLASH result=FAIL reason=invalid-candidate-path\n' >&2
  exit 2
}
[[ $AUTH_CANDIDATE == "$CANDIDATE" && $CANDIDATE_INPUT == "$CANDIDATE" ]] || {
  printf 'P8_FLASH result=FAIL reason=dispatcher-candidate-authorization-mismatch\n' >&2
  exit 1
}

auth_kv()
{
  local file=$1 key=$2
  awk -F= -v wanted="$key" '
    $1 == wanted { value = substr($0, length($1) + 2); count++ }
    END { if (count != 1) exit 1; print value }
  ' "$file"
}

if [[ $AUTH_MODE == candidate ]]; then
  AUTH_RUN_ID=${AUTH_RUN##*/}
  [[ ${AUTH_RUN%/*} == "$ROOT/reports/p8-runs" && \
     $AUTH_RUN_ID =~ ^[0-9]{8}T[0-9]{6}[+-][0-9]{4}-[0-9]+$ && \
     -d $AUTH_RUN && ! -L $AUTH_RUN && $(realpath -e -- "$AUTH_RUN") == "$AUTH_RUN" ]] || {
    printf 'P8_FLASH result=FAIL reason=candidate-run-authorization-invalid\n' >&2
    exit 1
  }
  BUILD_LOCK=$AUTH_RUN/BUILD-LOCK.env
  FLASH_AUTH=$AUTH_RUN/FLASH-AUTHORIZATION.env
  [[ -f $BUILD_LOCK && ! -L $BUILD_LOCK && -f $BUILD_LOCK.sha256 && \
     ! -L $BUILD_LOCK.sha256 && -f $FLASH_AUTH && ! -L $FLASH_AUTH && \
     -f $FLASH_AUTH.sha256 && ! -L $FLASH_AUTH.sha256 && \
     $(stat -c %a -- "$BUILD_LOCK") == 444 && \
     $(stat -c %a -- "$BUILD_LOCK.sha256") == 444 && \
     $(stat -c %a -- "$FLASH_AUTH") == 444 && \
     $(stat -c %a -- "$FLASH_AUTH.sha256") == 444 ]] || {
    printf 'P8_FLASH result=FAIL reason=candidate-run-receipts-invalid\n' >&2
    exit 1
  }
  [[ $AUTH_BUILD_LOCK_SHA =~ ^[0-9a-f]{64}$ && \
     $AUTH_FLASH_AUTH_SHA =~ ^[0-9a-f]{64}$ && \
     $AUTH_OUTGOING_SHA =~ ^[0-9a-f]{64}$ && \
     $(sha256sum "$BUILD_LOCK" | cut -d' ' -f1) == "$AUTH_BUILD_LOCK_SHA" && \
     $(sha256sum "$FLASH_AUTH" | cut -d' ' -f1) == "$AUTH_FLASH_AUTH_SHA" && \
     $(auth_kv "$BUILD_LOCK" run_id) == "${AUTH_RUN##*/}" && \
     $(auth_kv "$BUILD_LOCK" p8_sha256) == "$CANDIDATE_SHA" && \
     $(auth_kv "$FLASH_AUTH" schema) == rg40xxv-p8-flash-authorization-v2 && \
     $(auth_kv "$FLASH_AUTH" run_id) == "${AUTH_RUN##*/}" && \
     $(auth_kv "$FLASH_AUTH" build_lock_sha256) == "$AUTH_BUILD_LOCK_SHA" && \
     $(auth_kv "$FLASH_AUTH" p8_sha256) == "$CANDIDATE_SHA" && \
     $(auth_kv "$FLASH_AUTH" device) == "$DEV" && \
     $(auth_kv "$FLASH_AUTH" disk_node_id) == "$AUTH_DISK_NODE_ID" && \
     $(auth_kv "$FLASH_AUTH" disk_device_number) == "$AUTH_DISK_DEVICE_NUMBER" && \
     $(auth_kv "$FLASH_AUTH" disk_sysfs_path) == "$AUTH_DISK_SYSFS_PATH" && \
     $(auth_kv "$FLASH_AUTH" diskseq) == "$AUTH_DISKSEQ" && \
     $(auth_kv "$FLASH_AUTH" outgoing_p8_sha256) == "$AUTH_OUTGOING_SHA" ]] || {
    printf 'P8_FLASH result=FAIL reason=candidate-run-binding-mismatch\n' >&2
    exit 1
  }
  (cd "$AUTH_RUN" && sha256sum -c BUILD-LOCK.env.sha256 >/dev/null && \
    sha256sum -c FLASH-AUTHORIZATION.env.sha256 >/dev/null) || {
    printf 'P8_FLASH result=FAIL reason=candidate-run-sidecar-mismatch\n' >&2
    exit 1
  }
  [[ $AUTH_RESULT == "$AUTH_RUN/FLASH-RESULT.env" && \
     ! -e $AUTH_RESULT && ! -L $AUTH_RESULT && \
     ! -e $AUTH_RESULT.sha256 && ! -L $AUTH_RESULT.sha256 ]] || {
    printf 'P8_FLASH result=FAIL reason=candidate-result-path-invalid\n' >&2
    exit 1
  }
fi

[[ -d $BACKUP_ROOT && ! -L $BACKUP_ROOT && \
   $(realpath -e -- "$BACKUP_ROOT") == "$BACKUP_ROOT" ]] || {
  printf 'P8_FLASH result=FAIL reason=backup-root-invalid\n' >&2
  exit 1
}
BACKUP_MOUNT_ID=$(findmnt -nro ID -T "$BACKUP_ROOT") || {
  printf 'P8_FLASH result=FAIL reason=backup-root-mount-read-failed\n' >&2
  exit 1
}
ROOT_MOUNT_ID=$(findmnt -nro ID -T "$ROOT") || {
  printf 'P8_FLASH result=FAIL reason=workspace-mount-read-failed\n' >&2
  exit 1
}
[[ -n $BACKUP_MOUNT_ID && $BACKUP_MOUNT_ID == "$ROOT_MOUNT_ID" && \
   $(stat -c %d -- "$BACKUP_ROOT") == $(stat -c %d -- "$ROOT") ]] || {
  printf 'P8_FLASH result=FAIL reason=backup-root-not-on-workspace-filesystem\n' >&2
  exit 1
}
[[ $BACKUP_INPUT == "$BACKUP_DIR" && ${BACKUP_DIR%/*} == "$BACKUP_ROOT" && \
   ${BACKUP_DIR##*/} =~ ^[A-Za-z0-9._+-]+$ ]] || {
  printf 'P8_FLASH result=FAIL reason=backup-outside-host-root\n' >&2
  exit 1
}
[[ ! -e $BACKUP_DIR && ! -L $BACKUP_DIR ]] || {
  printf 'P8_FLASH result=FAIL reason=backup-directory-must-be-new\n' >&2
  exit 1
}
unset LOG
LOG=$BACKUP_DIR/flash.log

# Exact p1..p8 layout, in bytes: "number offset size"
EXPECTED_LAYOUT="1 37748736 47244640256
2 47282388992 33554432
3 47315943424 16777216
4 47332720640 67108864
5 47399829504 7516192768
6 54916022272 4294967296
7 59210989568 2679111680
8 61890101248 67108864"

umask 077
mkdir -- "$BACKUP_DIR"
[[ -d $BACKUP_DIR && ! -L $BACKUP_DIR && \
   $(stat -c %d -- "$BACKUP_DIR") == $(stat -c %d -- "$ROOT") ]] || {
  printf 'P8_FLASH result=FAIL reason=backup-directory-host-verification-failed\n' >&2
  exit 1
}
log() { printf '%s %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$*" | tee -a "$LOG"; }
die() { log "FAIL $*"; exit 1; }
assert_eq() { [[ "$1" == "$2" ]] || die "$3 mismatch: actual=$1 expected=$2"; }

publish_flash_result()
{
  local write_mode=$1 device_write=$2 readback=$3 result_sha
  [[ $AUTH_MODE == candidate ]] || die "flash result publication outside candidate mode"
  [[ ! -e $AUTH_RESULT && ! -L $AUTH_RESULT && \
     ! -e $AUTH_RESULT.sha256 && ! -L $AUTH_RESULT.sha256 ]] || \
    die "flash result destination already exists"
  RESULT_TMP=$(mktemp "$AUTH_RUN/.FLASH-RESULT.env.tmp.XXXXXX") || \
    die "flash result temp create failed"
  RESULT_SIDE_TMP=$(mktemp "$AUTH_RUN/.FLASH-RESULT.env.sha256.tmp.XXXXXX") || {
    rm -f -- "$RESULT_TMP"
    RESULT_TMP=
    die "flash result sidecar temp create failed"
  }
  {
    printf 'schema=rg40xxv-p8-flash-result-v2\n'
    printf 'run_id=%s\ndevice=%s\n' "${AUTH_RUN##*/}" "$DEV"
    printf 'disk_node_id=%s\ndisk_device_number=%s\n' \
      "$DEV_NODE_ID" "$disk_sysfs_decimal"
    printf 'disk_sysfs_path=%s\ndiskseq=%s\n' "$DISK_SYSFS" "$DISKSEQ"
    printf 'outgoing_p8_sha256=%s\np8_sha256=%s\n' \
      "$AUTH_OUTGOING_SHA" "$CANDIDATE_SHA"
    printf 'readback_sha256=%s\np8_bytes=%s\n' "$readback" "$P8_BYTES"
    printf 'result=PASS\nwrite_mode=%s\np4=UNCHANGED\n' "$write_mode"
    printf 'p7_write=NONE\ndevice_write=%s\n' "$device_write"
  } >"$RESULT_TMP"
  chmod 0444 "$RESULT_TMP"
  result_sha=$(sha256sum "$RESULT_TMP" | cut -d' ' -f1)
  printf '%s  FLASH-RESULT.env\n' "$result_sha" >"$RESULT_SIDE_TMP"
  chmod 0444 "$RESULT_SIDE_TMP"
  ln -- "$RESULT_TMP" "$AUTH_RESULT" || die "flash result publish conflict"
  if ! ln -- "$RESULT_SIDE_TMP" "$AUTH_RESULT.sha256"; then
    rm -f -- "$AUTH_RESULT"
    die "flash result sidecar publish conflict"
  fi
  rm -f -- "$RESULT_TMP" "$RESULT_SIDE_TMP"
  RESULT_TMP=
  RESULT_SIDE_TMP=
  (cd "$AUTH_RUN" && sha256sum -c FLASH-RESULT.env.sha256 >/dev/null) ||
    die "published flash result SHA256 mismatch"
  log "FLASH_RESULT_OK path=$AUTH_RESULT write_mode=$write_mode device_write=$device_write"
}

RO_GUARD_ACTIVE=0
P8_WRITE_FD_OPEN=0
BACKUP_TMP=
CANDIDATE_STAGE=
RESULT_TMP=
RESULT_SIDE_TMP=
restore_read_only()
{
  local num failed=0
  # Restore the broad parent barrier first, then attempt every partition even
  # if an individual ioctl fails. Never short-circuit while anything may be RW.
  blockdev --setro "$DEVICE_FD_PATH" || failed=1
  for num in 1 2 3 4 5 6 7 8; do
    blockdev --setro "/proc/$$/fd/${PART_FD[$num]}" || failed=1
  done
  [[ $(blockdev --getro "$DEVICE_FD_PATH") == 1 ]] || failed=1
  for num in 1 2 3 4 5 6 7 8; do
    [[ $(blockdev --getro "/proc/$$/fd/${PART_FD[$num]}") == 1 ]] || failed=1
  done
  [[ $failed == 0 ]] || return 1
  RO_GUARD_ACTIVE=0
  log "RO_GUARD parent=1 p1-p8=1"
}

restore_read_only_on_exit()
{
  local rc=$?

  trap - EXIT
  if [[ $P8_WRITE_FD_OPEN == 1 ]]; then
    exec {P8_WRITE_FD}>&-
    P8_WRITE_FD_OPEN=0
  fi
  if [[ -n $BACKUP_TMP ]]; then
    rm -f -- "$BACKUP_TMP"
    BACKUP_TMP=
  fi
  if [[ -n $CANDIDATE_STAGE ]]; then
    rm -f -- "$CANDIDATE_STAGE"
    CANDIDATE_STAGE=
  fi
  [[ -z $RESULT_TMP ]] || rm -f -- "$RESULT_TMP"
  [[ -z $RESULT_SIDE_TMP ]] || rm -f -- "$RESULT_SIDE_TMP"
  if [[ $RO_GUARD_ACTIVE == 1 ]]; then
    if ! restore_read_only; then
      log "FAIL read-only guard could not be restored"
      rc=1
    fi
  fi
  exit "$rc"
}
trap restore_read_only_on_exit EXIT

# $2/$3 are raw BYTE offsets/lengths; skip_bytes/count_bytes take bytes, not blocks.
range_sha() { dd if="$1" bs=1M skip="$2" count="$3" \
  iflag=skip_bytes,count_bytes status=none | sha256sum | cut -d' ' -f1; }

log "PRECHECK dev=$DEV candidate=$CANDIDATE"

# Keep recognition of an outgoing image separate from authorization to deploy
# it.  A permanently failed image must remain recognizable so recovery can
# back it up, while its BANNED target policy prevents it becoming a candidate.
[[ -f $IMAGE_REGISTRY && ! -L $IMAGE_REGISTRY ]] || die "image registry missing"
exec {REGISTRY_FD}<"$IMAGE_REGISTRY"
REGISTRY_FD_PATH=/proc/$$/fd/$REGISTRY_FD
REGISTRY_ID=$(stat -Lc '%d:%i:%s' "$REGISTRY_FD_PATH")
REGISTRY_SHA=$(sha256sum "$REGISTRY_FD_PATH" | cut -d' ' -f1)
assert_eq "$REGISTRY_SHA" "$AUTH_REGISTRY_SHA" "authorized image registry SHA256"
registry_rows=0
declare -A registry_seen=()
while IFS=$'\t' read -r image_sha outgoing_policy target_policy identity extra; do
  [[ -z $image_sha || $image_sha == \#* ]] && continue
  [[ -z ${extra:-} ]] || die "invalid image registry field count"
  [[ $image_sha =~ ^[0-9a-f]{64}$ ]] || die "invalid image registry SHA256"
  [[ $outgoing_policy == KNOWN ]] || die "invalid outgoing policy for $image_sha"
  [[ $target_policy =~ ^(FROZEN|RECOVERY_ONLY|HOST_ONLY|RETIRED|BANNED)$ ]] ||
    die "invalid target policy for $image_sha"
  [[ $identity =~ ^[A-Za-z0-9._-]+$ ]] || die "invalid image identity for $image_sha"
  [[ -z ${registry_seen[$image_sha]:-} ]] || die "duplicate image registry SHA256: $image_sha"
  registry_seen[$image_sha]="$target_policy:$identity"
  registry_rows=$((registry_rows + 1))
done < "$REGISTRY_FD_PATH"
assert_eq "$(sha256sum "$REGISTRY_FD_PATH" | cut -d' ' -f1)" "$REGISTRY_SHA" \
  "image registry post-parse SHA256"
[[ $registry_rows -gt 0 ]] || die "empty image registry"
log "IMAGE_REGISTRY_OK rows=$registry_rows"
case $AUTH_MODE in
recover)
  [[ ${registry_seen[$CANDIDATE_SHA]:-} == FROZEN:* ]] ||
    die "recovery target is not registry policy FROZEN (sha256=$CANDIDATE_SHA)"
  ;;
candidate)
  [[ ${registry_seen[$CANDIDATE_SHA]:-} == HOST_ONLY:* ]] ||
    die "candidate target is not registry policy HOST_ONLY (sha256=$CANDIDATE_SHA)"
  ;;
esac

# --- candidate image ---
[[ -f "$CANDIDATE" && ! -L "$CANDIDATE" ]] ||
  die "candidate image missing, non-regular or symlinked"
exec {CANDIDATE_FD}<"$CANDIDATE"
CANDIDATE_FD_PATH=/proc/$$/fd/$CANDIDATE_FD
CANDIDATE_ID=$(stat -Lc '%d:%i:%s' "$CANDIDATE_FD_PATH")
assert_eq "$(stat -Lc %s "$CANDIDATE_FD_PATH")" "$P8_BYTES" "candidate size"
CAND_ACTUAL=$(sha256sum "$CANDIDATE_FD_PATH" | cut -d' ' -f1)
assert_eq "$CAND_ACTUAL" "$CANDIDATE_SHA" "candidate SHA256"
log "CANDIDATE_OK bytes=$P8_BYTES sha256=$CAND_ACTUAL inode=$CANDIDATE_ID"

# Seal the exact candidate bytes into an anonymous, private inode before any
# device work.  A concurrent rebuild can mutate the public source path, but it
# cannot replace or write this unlinked FD.  The sealed copy is independently
# size- and hash-verified before becoming the sole write source.
CANDIDATE_STAGE=$(mktemp "$BACKUP_DIR/.candidate-stage.XXXXXX") || \
  die "candidate stage create failed"
dd if="$CANDIDATE_FD_PATH" of="$CANDIDATE_STAGE" bs=1M \
  count=$(( P8_BYTES / 1048576 )) conv=fsync status=none
assert_eq "$(stat -Lc %s "$CANDIDATE_STAGE")" "$P8_BYTES" \
  "sealed candidate size"
assert_eq "$(sha256sum "$CANDIDATE_STAGE" | cut -d' ' -f1)" \
  "$CANDIDATE_SHA" "sealed candidate SHA256"
exec {SEALED_CANDIDATE_FD}<"$CANDIDATE_STAGE"
SEALED_CANDIDATE_FD_PATH=/proc/$$/fd/$SEALED_CANDIDATE_FD
assert_eq "$(sha256sum "$SEALED_CANDIDATE_FD_PATH" | cut -d' ' -f1)" \
  "$CANDIDATE_SHA" "opened sealed candidate SHA256"
rm -f -- "$CANDIDATE_STAGE"
CANDIDATE_STAGE=
exec {CANDIDATE_FD}<&-
CANDIDATE_FD=$SEALED_CANDIDATE_FD
CANDIDATE_FD_PATH=$SEALED_CANDIDATE_FD_PATH
CANDIDATE_ID=$(stat -Lc '%d:%i:%s' "$CANDIDATE_FD_PATH")
log "CANDIDATE_SEALED sha256=$CANDIDATE_SHA inode=$CANDIDATE_ID path=ANONYMOUS"

# --- card identity ---
[[ -b $DEV && ! -L $DEV && $(readlink -f -- "$DEV") == "$DEV" ]] ||
  die "whole device is not a canonical direct block node: $DEV"
[[ -d /sys/class/block/$DISK_NAME ]] || die "disk sysfs node missing"
DISK_SYSFS=$(readlink -f -- "/sys/class/block/$DISK_NAME")
DEV_NODE_ID=$(stat -Lc '%t:%T' "$DEV")
disk_sysfs_decimal=$(cat "/sys/class/block/$DISK_NAME/dev")
[[ $disk_sysfs_decimal =~ ^[0-9]+:[0-9]+$ ]] || die "disk sysfs device number invalid"
printf -v disk_sysfs_hex '%x:%x' "${disk_sysfs_decimal%:*}" "${disk_sysfs_decimal#*:}"
assert_eq "$DEV_NODE_ID" "$disk_sysfs_hex" "whole-device sysfs binding"
DISKSEQ_FILE=/sys/class/block/$DISK_NAME/diskseq
[[ -r $DISKSEQ_FILE ]] || die "diskseq unavailable"
DISKSEQ=$(cat "$DISKSEQ_FILE") || die "diskseq read failed"
[[ $DISKSEQ =~ ^[0-9]+$ ]] || die "diskseq invalid"
assert_eq "$DEV_NODE_ID" "$AUTH_DISK_NODE_ID" "authorized disk node identity"
assert_eq "$disk_sysfs_decimal" "$AUTH_DISK_DEVICE_NUMBER" \
  "authorized disk device number"
assert_eq "$DISK_SYSFS" "$AUTH_DISK_SYSFS_PATH" "authorized disk sysfs path"
assert_eq "$DISKSEQ" "$AUTH_DISKSEQ" "authorized diskseq"

declare -A PART_NODE_ID=() PART_FD=()
validate_partition_node()
{
  local num=$1 node=${DEV}$1 name=${DISK_NAME}$1 sysfs decimal expected_hex
  [[ -b $node && ! -L $node && $(readlink -f -- "$node") == "$node" ]] ||
    die "p$num is not a canonical direct block node: $node"
  [[ -d /sys/class/block/$name ]] || die "p$num sysfs node missing"
  sysfs=$(readlink -f -- "/sys/class/block/$name")
  [[ ${sysfs%/*} == "$DISK_SYSFS" ]] || die "p$num sysfs parent mismatch"
  assert_eq "$(cat "/sys/class/block/$name/partition")" "$num" \
    "p$num partition number"
  decimal=$(cat "/sys/class/block/$name/dev")
  printf -v expected_hex '%x:%x' "${decimal%:*}" "${decimal#*:}"
  assert_eq "$(stat -Lc '%t:%T' "$node")" "$expected_hex" \
    "p$num node sysfs binding"
  if [[ -n ${PART_NODE_ID[$num]+present} ]]; then
    assert_eq "$(stat -Lc '%t:%T' "$node")" "${PART_NODE_ID[$num]}" \
      "p$num stable node identity"
  else
    PART_NODE_ID[$num]=$(stat -Lc '%t:%T' "$node")
  fi
}

validate_all_device_nodes()
{
  local num
  [[ -b $DEV && ! -L $DEV && $(readlink -f -- "$DEV") == "$DEV" ]] ||
    die "whole device binding changed"
  assert_eq "$(stat -Lc '%t:%T' "$DEV")" "$DEV_NODE_ID" \
    "stable whole-device identity"
  assert_eq "$(readlink -f -- "/sys/class/block/$DISK_NAME")" "$DISK_SYSFS" \
    "stable whole-device sysfs path"
  assert_eq "$(cat "/sys/class/block/$DISK_NAME/dev")" "$disk_sysfs_decimal" \
    "stable whole-device number"
  assert_eq "$(cat "$DISKSEQ_FILE")" "$DISKSEQ" "stable diskseq"
  for num in 1 2 3 4 5 6 7 8; do validate_partition_node "$num"; done
}

validate_all_device_nodes
P8_NODE_ID=${PART_NODE_ID[8]}
exec {DEVICE_FD}<"$DEV"
DEVICE_FD_PATH=/proc/$$/fd/$DEVICE_FD
assert_eq "$(stat -Lc '%t:%T' "$DEVICE_FD_PATH")" "$DEV_NODE_ID" "open disk identity"
for num in 1 2 3 4 5 6 7 8; do
  exec {fd}<"${DEV}${num}"
  PART_FD[$num]=$fd
  assert_eq "$(stat -Lc '%t:%T' "/proc/$$/fd/$fd")" \
    "${PART_NODE_ID[$num]}" "open p$num identity"
done
assert_eq "$(blockdev --getsize64 "/proc/$$/fd/${PART_FD[8]}")" \
  "$P8_BYTES" "p8 block size"

assert_eq "$(blockdev --getsize64 "$DEVICE_FD_PATH")" "$EXPECTED_DISK_BYTES" "disk size"
SFDISK_DUMP=$(sfdisk -d "$DEVICE_FD_PATH") || die "GPT dump failed"
SFDISK_SHA=$(printf '%s\n' "$SFDISK_DUMP" | sha256sum | cut -d' ' -f1)
GUID=$(awk -F': ' '/^label-id/{print toupper($2)}' <<<"$SFDISK_DUMP")
assert_eq "$GUID" "$EXPECTED_GUID" "GPT disk GUID"
PARTS=$(awk '$2 == ":" {n++} END {print n+0}' <<<"$SFDISK_DUMP")
assert_eq "$PARTS" "8" "partition count"

assert_exact_layout()
{
  local num off size a_off a_size
  while read -r num off size; do
    [[ -z $num ]] && continue
    a_off=$(( $(cat "/sys/block/$DISK_NAME/$DISK_NAME$num/start") * 512 ))
    a_size=$(( $(cat "/sys/block/$DISK_NAME/$DISK_NAME$num/size") * 512 ))
    assert_eq "$a_off"  "$off"  "p$num offset"
    assert_eq "$a_size" "$size" "p$num size"
  done <<< "$EXPECTED_LAYOUT"
}

assert_disk_metadata_stable()
{
  local current_dump current_sha
  validate_all_device_nodes
  assert_eq "$(stat -Lc '%t:%T' "$DEVICE_FD_PATH")" "$DEV_NODE_ID" \
    "open whole-device identity"
  assert_eq "$(blockdev --getsize64 "$DEVICE_FD_PATH")" "$EXPECTED_DISK_BYTES" \
    "stable disk size"
  current_dump=$(sfdisk -d "$DEVICE_FD_PATH") || die "stable GPT dump failed"
  current_sha=$(printf '%s\n' "$current_dump" | sha256sum | cut -d' ' -f1)
  assert_eq "$current_sha" "$SFDISK_SHA" "stable GPT dump SHA256"
  assert_exact_layout
}

assert_exact_layout
validate_all_device_nodes
log "LAYOUT_OK p1-p8 offsets_and_sizes=EXACT"

# --- nothing from this card may be mounted ---
assert_unmounted()
{
  local node=$1 output rc name devnum
  set +e
  output=$(findmnt -rn -S "$node" -o TARGET 2>/dev/null)
  rc=$?
  set -e
  case $rc in
    0) die "mounted device: $node target=${output:-unknown}" ;;
    1) ;;
    *) die "mount state read failed: $node rc=$rc" ;;
  esac
  name=$(basename -- "$node")
  [[ -r /sys/class/block/$name/dev ]] || die "mountinfo device number missing: $name"
  devnum=$(cat "/sys/class/block/$name/dev")
  set +e
  awk -v wanted="$devnum" '$3 == wanted { found = 1 } END { exit(found ? 0 : 1) }' \
    /proc/self/mountinfo
  rc=$?
  set -e
  case $rc in
    0) die "mounted device number: $node dev=$devnum" ;;
    1) ;;
    *) die "mountinfo parse failed: $node rc=$rc" ;;
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
    resolved=$(readlink -f -- "$source") || die "swap source cannot be resolved: $source"
    case $resolved in
      "$DEV"|"${DEV}"[1-8]) die "TF1 node is active swap: $resolved" ;;
    esac
  done </proc/swaps
}

assert_no_holders()
{
  local num name holder
  for num in '' 1 2 3 4 5 6 7 8; do
    name=$DISK_NAME$num
    holder=$(find "/sys/class/block/$name/holders" -mindepth 1 -maxdepth 1 \
      -print -quit 2>/dev/null) || die "holder state read failed: $name"
    [[ -z $holder ]] || die "TF1 node has an active holder: $name"
  done
}

assert_card_not_in_use()
{
  validate_all_device_nodes
  assert_all_unmounted
  assert_not_swap
  assert_no_holders
}

assert_all_read_only()
{
  local num
  assert_eq "$(blockdev --getro "$DEVICE_FD_PATH")" "1" \
    "whole-device read-only entry guard"
  for num in 1 2 3 4 5 6 7 8; do
    assert_eq "$(blockdev --getro "/proc/$$/fd/${PART_FD[$num]}")" "1" \
      "p$num read-only entry guard"
  done
}

assert_card_not_in_use
assert_all_read_only

# --- stock recovery must be untouched ---
P4_NOW=$(range_sha "$DEVICE_FD_PATH" "$P4_OFFSET" "$P4_BYTES")
assert_eq "$P4_NOW" "$P4_SHA" "p4 SHA256"
log "P4_OK sha256=$P4_NOW"

# --- current p8 provenance ---
CUR_P8=$(range_sha "$DEVICE_FD_PATH" "$P8_OFFSET" "$P8_BYTES")
log "CURRENT_P8 sha256=$CUR_P8"
case $AUTH_MODE in
candidate)
  [[ -n ${registry_seen[$CUR_P8]:-} ]] || \
    die "candidate mode refuses registry-unknown current p8 (sha256=$CUR_P8)"
  [[ $CUR_P8 == "$AUTH_OUTGOING_SHA" || $CUR_P8 == "$CANDIDATE_SHA" ]] || \
    die "current p8 differs from bound identify receipt (actual=$CUR_P8 authorized=$AUTH_OUTGOING_SHA)"
  ;;
recover)
  if [[ -n ${registry_seen[$CUR_P8]:-} ]]; then
    [[ -z $AUTH_UNKNOWN_CURRENT_SHA ]] || \
      die "unknown-current confirmation supplied for registry-known p8 (sha256=$CUR_P8)"
  else
    [[ -n $AUTH_UNKNOWN_CURRENT_SHA && $AUTH_UNKNOWN_CURRENT_SHA == "$CUR_P8" ]] || \
      die "registry-unknown current p8 requires exact recovery confirmation (actual=$CUR_P8)"
    log "CURRENT_P8_UNKNOWN_RECOVERY_CONFIRMED sha256=$CUR_P8"
  fi
  ;;
esac
if [[ "$CUR_P8" == "$CANDIDATE_SHA" ]]; then
  assert_disk_metadata_stable
  assert_card_not_in_use
  assert_all_read_only
  assert_eq "$(stat -Lc '%d:%i:%s' "$IMAGE_REGISTRY")" "$REGISTRY_ID" \
    "image registry node before already-complete result"
  assert_eq "$(sha256sum "$REGISTRY_FD_PATH" | cut -d' ' -f1)" "$REGISTRY_SHA" \
    "image registry before already-complete result"
  restore_read_only || die "read-only guard could not be set"
  if [[ $AUTH_MODE == candidate ]]; then
    publish_flash_result ALREADY_COMPLETE NONE "$CUR_P8"
    PREVIOUS_FOR_LOG=$AUTH_OUTGOING_SHA
  else
    PREVIOUS_FOR_LOG=$CUR_P8
  fi
  log "DEPLOY_PASS mode=already-complete p8_sha256=$CUR_P8 previous=$PREVIOUS_FOR_LOG p4=UNCHANGED"
  log "DONE_SAFE_TO_EJECT"
  exit 0
fi
if [[ -n ${registry_seen[$CUR_P8]:-} ]]; then
  log "CURRENT_P8_RECOGNIZED policy=${registry_seen[$CUR_P8]}"
else
  log "CURRENT_P8_RECOGNIZED policy=RECOVER_ONLY_EXPLICIT_UNKNOWN"
fi

# --- reversible: keep the outgoing p8 ---
PRE="$BACKUP_DIR/p8-before-$CUR_P8.img"
[[ ! -e $PRE && ! -L $PRE ]] || die "pre-write backup destination exists"
BACKUP_TMP=$(mktemp "$BACKUP_DIR/.p8-before.tmp.XXXXXX") || \
  die "pre-write backup temp create failed"
dd if="$DEVICE_FD_PATH" of="$BACKUP_TMP" bs=1M skip="$P8_OFFSET" count="$P8_BYTES" \
   iflag=skip_bytes,count_bytes conv=fsync status=none
PRE_SHA=$(sha256sum "$BACKUP_TMP" | cut -d' ' -f1)
assert_eq "$PRE_SHA" "$CUR_P8" "pre-write backup SHA256"
chmod 0444 "$BACKUP_TMP" || die "pre-write backup chmod failed"
ln -- "$BACKUP_TMP" "$PRE" || die "pre-write backup publish conflict"
sync -f "$PRE"
rm -f -- "$BACKUP_TMP"
BACKUP_TMP=
log "PREWRITE_BACKUP_OK path=$PRE sha256=$PRE_SHA"
assert_card_not_in_use
assert_all_read_only
assert_disk_metadata_stable
assert_eq "$(sha256sum "$CANDIDATE_FD_PATH" | cut -d' ' -f1)" \
  "$CANDIDATE_SHA" "candidate SHA256 immediately before write window"

# --- write only the 64 MiB p8 range ---
RO_WAS=$(blockdev --getro "$DEVICE_FD_PATH")
assert_eq "$RO_WAS" "1" "whole-device pre-write read-only guard"
log "RO_GUARD was=$RO_WAS -> clearing for p8 write only"
RO_GUARD_ACTIVE=1
trap restore_read_only_on_exit EXIT
for num in 1 2 3 4 5 6 7; do
  blockdev --setro "/proc/$$/fd/${PART_FD[$num]}"
done
blockdev --setrw "$DEVICE_FD_PATH"
for num in 1 2 3 4 5 6 7; do
  blockdev --setro "/proc/$$/fd/${PART_FD[$num]}"
done
blockdev --setrw "/proc/$$/fd/${PART_FD[8]}"
assert_eq "$(blockdev --getro "$DEVICE_FD_PATH")" "0" \
  "whole-device write-window guard"
for num in 1 2 3 4 5 6 7; do
  assert_eq "$(blockdev --getro "/proc/$$/fd/${PART_FD[$num]}")" "1" \
    "p$num protected during p8 write"
done
assert_eq "$(blockdev --getro "/proc/$$/fd/${PART_FD[8]}")" "0" \
  "p8 write-window guard"
assert_eq "$(stat -Lc '%t:%T' "$DEVICE_FD_PATH")" "$DEV_NODE_ID" \
  "disk identity before write"
assert_eq "$(stat -Lc '%t:%T' "/proc/$$/fd/${PART_FD[8]}")" "$P8_NODE_ID" \
  "p8 identity before write"
assert_card_not_in_use
exec {P8_WRITE_FD}<>"/proc/$$/fd/${PART_FD[8]}"
P8_WRITE_FD_OPEN=1
P8_WRITE_FD_PATH=/proc/$$/fd/$P8_WRITE_FD
assert_eq "$(stat -Lc '%t:%T' "$P8_WRITE_FD_PATH")" "$P8_NODE_ID" \
  "open p8 identity"
assert_eq "$(blockdev --getsize64 "$P8_WRITE_FD_PATH")" "$P8_BYTES" \
  "open p8 size"
assert_card_not_in_use

log "Writing exactly 64 MiB to p8 ($P8DEV); no other byte range is targeted"
assert_eq "$(stat -Lc '%d:%i:%s' "$CANDIDATE_FD_PATH")" "$CANDIDATE_ID" \
  "candidate open-file identity"
assert_eq "$(sha256sum "$CANDIDATE_FD_PATH" | cut -d' ' -f1)" \
  "$CANDIDATE_SHA" "candidate SHA256 at final write boundary"
dd if="$CANDIDATE_FD_PATH" of="$P8_WRITE_FD_PATH" bs=1M \
  count=$(( P8_BYTES / 1048576 )) conv=fsync,notrunc status=none
exec {P8_WRITE_FD}>&-
P8_WRITE_FD_OPEN=0
blockdev --flushbufs "$DEVICE_FD_PATH"

# --- verify from the same pinned block device after a checked BLKFLSBUF ---
assert_eq "$(stat -Lc '%t:%T' "$DEVICE_FD_PATH")" "$DEV_NODE_ID" \
  "disk identity after write"
assert_card_not_in_use
assert_disk_metadata_stable
READBACK=$(range_sha "$DEVICE_FD_PATH" "$P8_OFFSET" "$P8_BYTES")
assert_eq "$READBACK" "$CANDIDATE_SHA" "p8 readback SHA256"
P4_AFTER=$(range_sha "$DEVICE_FD_PATH" "$P4_OFFSET" "$P4_BYTES")
assert_eq "$P4_AFTER" "$P4_SHA" "p4 post-write SHA256"
assert_card_not_in_use
assert_disk_metadata_stable
assert_eq "$(stat -Lc '%d:%i:%s' "$IMAGE_REGISTRY")" "$REGISTRY_ID" \
  "image registry node before success"
assert_eq "$(sha256sum "$REGISTRY_FD_PATH" | cut -d' ' -f1)" "$REGISTRY_SHA" \
  "image registry before success"
restore_read_only || die "read-only guard could not be restored"
trap - EXIT
if [[ $AUTH_MODE == candidate ]]; then
  publish_flash_result P8_ONLY P8_ONLY "$READBACK"
fi
log "DEPLOY_PASS p8_sha256=$READBACK previous=$CUR_P8 p4=UNCHANGED"
log "DONE_SAFE_TO_EJECT"
