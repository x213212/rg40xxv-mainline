#!/usr/bin/env bash
# Walk one deployment from a built kernel to a device that has booted it.
#
# This is a guided wrapper, not an automation: two steps need a human and the
# script stops at both. It never writes to a card by itself -- it calls the
# guarded flasher, which enforces disk identity, partition layout, an allowlist
# for the p8 currently on the card, a pre-write backup and a full readback.
#
#   tools/deploy.sh <device> <backup-dir> [candidate.img]
#
# Nothing here proceeds past a failure. If a stage fails, stop and read it.

set -euo pipefail

REPO_ROOT=$(CDPATH= cd -- "$(dirname -- "$(realpath -- "$0")")/.." && pwd -P)
DEVICE=${1:?device required, e.g. /dev/sdX}
BACKUP_DIR=${2:?backup directory required}
CANDIDATE=${3:-$REPO_ROOT/out/candidate-p8.img}

say() { printf '\n=== %s ===\n' "$1"; }
halt() { printf '\n!! %s\n' "$1" >&2; exit 1; }

confirm() {
    printf '\n%s\n' "$1"
    read -r -p 'Type exactly "yes" to continue: ' reply
    [ "$reply" = "yes" ] || halt "stopped by operator"
}

say "0. Preconditions"
[ -b "$DEVICE" ] || halt "$DEVICE is not a block device"
command -v sha256sum >/dev/null || halt "sha256sum missing"
[ -f "$CANDIDATE" ] || halt "candidate image not found: $CANDIDATE (run 'make all' first)"
mkdir -p "$BACKUP_DIR"

say "1. Identify the card (read-only)"
"$REPO_ROOT/tools/identify-rg40xxv-p8-wsl.sh" "$DEVICE" \
    || halt "identification failed: the card is not the one the guards expect"

say "2. Back up every partition"
# The stock image is not in this repository. This backup is the only way back.
if [ -n "$(ls -A "$BACKUP_DIR" 2>/dev/null)" ]; then
    printf 'backup directory is not empty, reusing: %s\n' "$BACKUP_DIR"
else
    "$REPO_ROOT/tools/collect_rg40xxv_readonly.sh" "$DEVICE" "$BACKUP_DIR" \
        || halt "backup failed -- do not continue without one"
fi
confirm "Confirm the backup in $BACKUP_DIR is complete, verified, and copied somewhere off this machine."

say "3. Audit the card (read-only)"
"$REPO_ROOT/tools/arm-rg40xxv-tf1-readonly.sh" "$DEVICE" \
    || halt "read-only audit failed"

say "4. Candidate digest"
CANDIDATE_SHA=$(sha256sum "$CANDIDATE" | cut -d' ' -f1)
printf 'candidate: %s\nsha256:    %s\n' "$CANDIDATE" "$CANDIDATE_SHA"

confirm "About to write p8 (64 MiB) on $DEVICE. Every other partition is left alone."

say "5. Write p8 and read it back"
"$REPO_ROOT/tools/flash-rg40xxv-p8-wsl.sh" \
    "$CANDIDATE" "$CANDIDATE_SHA" "$BACKUP_DIR" "$DEVICE" \
    || halt "flash failed -- the script reports whether anything was written"

say "6. Boot the device"
cat <<'MANUAL'
The host side is done. The device has to be power-cycled by hand:

  1. Eject the card safely and put it back in the device.
  2. Power the device off completely -- a reboot from a half-running system is
     not the same test, and this port's init path is not fixed yet.
  3. Power on and watch the first frames. A dark screen usually means the wrong
     panel dtb, not a brick.

If it does not come up, restore the backup from step 2. That is the plan.
MANUAL
printf '\nDEPLOY result=HOST_PASS device_boot=NOT_TESTED sha256=%s\n' "$CANDIDATE_SHA"
