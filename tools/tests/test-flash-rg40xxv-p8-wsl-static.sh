#!/usr/bin/env bash
# Host-only structural test.  It never opens or changes a block device.
set -euo pipefail
PATH=/usr/sbin:/usr/bin:/sbin:/bin
export PATH

SCRIPT=$(realpath -- "$0")
ROOT=$(CDPATH= cd -- "${SCRIPT%/*}/../.." && pwd -P)
FLASHER=$ROOT/tools/flash-rg40xxv-p8-wsl.sh

die()
{
	printf 'P8_FLASH_STATIC_TEST result=FAIL reason=%s device_write=NONE p7_write=NONE\n' \
		"$1" >&2
	exit 1
}

[[ -f $FLASHER && ! -L $FLASHER && -x $FLASHER ]] || \
	die flasher-missing-or-not-executable
bash -n "$FLASHER"

# Direct use must reject on dispatcher authorization before resolving the
# nonexistent candidate or inspecting /dev/sdz.
set +e
DIRECT_OUTPUT=$(env -i PATH="$PATH" "$FLASHER" /tmp/rg40xxv-no-candidate.img \
	0000000000000000000000000000000000000000000000000000000000000000 \
	"$ROOT/backups/rg40xxv-p8-static-test-$$" /dev/sdz 2>&1)
DIRECT_RC=$?
set -e
[[ $DIRECT_RC -ne 0 ]] || die direct-invocation-was-accepted
grep -Fq 'reason=dispatcher-authorization-required' <<<"$DIRECT_OUTPUT" || \
	die direct-invocation-wrong-rejection

for required in \
	'AUTH_MODE =~ ^(recover|candidate)$' \
	'CANDIDATE_SEALED' \
	'path=ANONYMOUS' \
	'declare -A PART_NODE_ID=() PART_FD=()' \
	'exec {P8_WRITE_FD}<>"/proc/$$/fd/${PART_FD[8]}"' \
	'blockdev --flushbufs "$DEVICE_FD_PATH"' \
	'publish_flash_result P8_ONLY P8_ONLY' \
	'write_mode=%s' \
	'count=$(( P8_BYTES / 1048576 ))' \
	'p4=UNCHANGED' \
	'p7_write=NONE'; do
	grep -Fq "$required" "$FLASHER" || die "missing-required-guard:$required"
done

# Once identities are pinned, no RO/RW/flush/write operation may use a raw
# /dev path that can be rebound by hotplug.
if grep -Eq 'blockdev[[:space:]]+--(setro|setrw|flushbufs)[[:space:]]+"\$DEV"|blockdev[[:space:]]+--(setro|setrw)[[:space:]]+"\$P8DEV"|exec[[:space:]]+\{P8_WRITE_FD\}<>"\$P8DEV"' \
	"$FLASHER"; then
	die raw-device-mutating-path-present
fi

if grep -Eq '(^|[;&|])[[:space:]]*(fastboot|mount|umount)([[:space:]]|$)' "$FLASHER"; then
	die forbidden-device-command
fi

printf 'P8_FLASH_STATIC_TEST result=PASS pinned_parent_and_p1_p8=PASS sealed_candidate=PASS device_write=NONE p7_write=NONE\n'
