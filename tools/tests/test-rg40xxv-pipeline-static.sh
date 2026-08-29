#!/usr/bin/env bash
set -euo pipefail

# Host-only structural test for the single RG40XX V release entry point.  It
# never contacts the device, opens a block device, deploys, flashes or reboots.

script=$(realpath -- "$0")
workspace=$(CDPATH= cd -- "$(dirname -- "$script")/../.." && pwd -P)
entry=$workspace/tools/rg40xxv.sh
p7_cycle=$workspace/tools/rg40xxv-p7-cycle.sh
p7_deploy=$workspace/tools/deploy-rg40xxv-p7-artifact.sh
p7_accept=$workspace/tools/rg40xxv-p7-device-acceptance.sh
p7_receipt=$workspace/tools/rg40xxv-p7-source-receipt.sh
p7_ui=$workspace/tools/rg40xxv-p7-ui-pipeline.sh
p7_builder=$workspace/lab/deploy/rg40xxv-next-v1/build-release.sh
p7_verifier=$workspace/lab/deploy/rg40xxv-next-v1/rg40xxv-verify-next-release
p8_flasher=$workspace/tools/flash-rg40xxv-p8-wsl.sh
p8_verifier=$workspace/tools/verify-rg40xxv-p8-workspace.sh
p8_readonly_arm=$workspace/tools/arm-rg40xxv-tf1-readonly.sh
p8_identifier=$workspace/tools/identify-rg40xxv-p8-wsl.sh
p8_cycle=$workspace/tools/rg40xxv-p8-cycle.sh
frozen=$workspace/lab/candidates/rg40xxv-panel-adopt-closeflow-v9-noquiesce-dram1v2/rg40xxv-panel-adopt-closeflow-v9-noquiesce-dram1v2-persistent-legacy-p8.img
frozen_sha=6455c4d82d1594c03ab4f799c276da7249c6ffefd6fa22dd55b9077b1dfd4be3

die()
{
	printf 'RG40XXV_PIPELINE_STATIC_TEST result=FAIL reason=%s device_write=NONE\n' \
		"$1" >&2
	exit 1
}

for tool in awk bash cat chmod cut date find flock grep mkdir readlink realpath rm sed sha256sum stat \
	timeout; do
	command -v "$tool" >/dev/null 2>&1 || die "missing-tool:$tool"
done
for file in "$entry" "$p7_cycle" "$p7_deploy" "$p7_accept" \
	"$p7_receipt" "$p7_ui" "$p7_builder" "$p7_verifier" "$p8_flasher" \
	"$p8_verifier" "$p8_readonly_arm" "$p8_identifier" "$p8_cycle"; do
	[[ -f $file && ! -L $file && -x $file ]] || die "missing-executable:$file"
done
[[ -f $frozen && ! -L $frozen ]] || die frozen-p8-missing
[[ $(stat -c %s "$frozen") == 67108864 ]] || die frozen-p8-size
[[ $(sha256sum "$frozen" | awk '{print $1}') == "$frozen_sha" ]] || \
	die frozen-p8-sha

bash -n "$entry" "$p7_cycle" "$p7_deploy" "$p7_accept" "$p7_receipt" \
	"$p7_ui" "$p7_builder" "$p8_flasher" "$p8_verifier" "$p8_readonly_arm"
bash -n "$p8_identifier" "$p8_cycle"
sh -n "$p7_verifier"

grep -Fq 'build-lock-required' "$p7_builder" || die builder-build-lock-gate
grep -Fq 'p8_payload=NONE' "$p7_builder" || die builder-p8-payload-boundary
grep -Fq 'forbidden-boot-payload' "$p7_builder" || die builder-boot-payload-gate
grep -Fq 'p8-changed' "$p7_deploy" || die deploy-post-p8-gate
grep -Fq 'StrictHostKeyChecking=yes' "$p7_deploy" || die deploy-host-key-policy
grep -Fq 'StrictHostKeyChecking=yes' "$p7_accept" || die acceptance-host-key-policy
grep -Fq 'existing-archive-byte-conflict' "$p7_builder" || \
	die builder-archive-no-clobber-gate
grep -Fq 'release-publish-race' "$p7_builder" || die builder-release-race-gate
grep -Fq 'published-deploy-kit-tree-mismatch' "$p7_builder" || \
	die builder-kit-readback-gate
grep -Fq 'source-bytes-changed-during-package' "$p7_builder" || \
	die builder-source-readback-gate
grep -Fq 'exec "$p8_flasher" "$frozen_p8" "$frozen_p8_sha"' "$entry" || \
	die p8-guarded-delegation
grep -Fq '"$p8_verifier" --full' "$entry" || die p8-recovery-host-verifier
grep -Fq 'compgen -A variable RG40XXV_P8_AUTH_' "$entry" || \
	die p8-verifier-auth-environment-not-sanitized
grep -Fq 'p8-verifier-operation-lock-fd-invalid' "$entry" || \
	die p8-verifier-lock-fd-not-preserved
[[ $(grep -Fc '"$p8_verifier" --full' "$entry") == 1 ]] || \
	die p8-verifier-sanitizer-bypass-present
[[ $(grep -Ec '^[[:space:]]*p8_run_full_verifier$' "$entry") == 5 ]] || \
	die p8-verifier-call-not-routed-through-sanitizer
grep -Fq '"$p8_readonly_arm" "$device"' "$entry" || die p8-readonly-arm-delegation
grep -Fq '.rg40xxv-p8-operation.lock' "$p8_flasher" || die p8-operation-lock
grep -Fq 'schema=rg40xxv-p8-identify-receipt-v2' "$p8_identifier" || \
	die p8-identify-v2-device-binding
grep -Fq 'candidate mode refuses registry-unknown current p8' "$p8_flasher" || \
	die p8-candidate-unknown-current-gate
grep -Fq 'registry-unknown current p8 requires exact recovery confirmation' \
	"$p8_flasher" || die p8-recover-unknown-current-gate
for key in disk_node_id disk_device_number disk_sysfs_path diskseq; do
	grep -Fq "$key" "$p8_identifier" "$p8_cycle" "$p8_flasher" || \
		die "p8-device-binding-key-missing:$key"
done
candidate_unknown_line=$(grep -n -m1 \
	'candidate mode refuses registry-unknown current p8' "$p8_flasher" | cut -d: -f1)
recover_unknown_line=$(grep -n -m1 \
	'registry-unknown current p8 requires exact recovery confirmation' "$p8_flasher" | cut -d: -f1)
diskseq_binding_line=$(grep -n -m1 'authorized diskseq' "$p8_flasher" | cut -d: -f1)
setrw_line=$(grep -n -m1 'blockdev --setrw "$DEVICE_FD_PATH"' "$p8_flasher" | cut -d: -f1)
[[ $candidate_unknown_line -lt $setrw_line && $recover_unknown_line -lt $setrw_line && \
	$diskseq_binding_line -lt $setrw_line ]] || die p8-safety-gate-after-write-window
if grep -Fq 'flash-rg40xxv-p8-wsl.sh' "$p7_cycle" "$p7_deploy" "$p7_builder"; then
	die p7-references-p8-flasher
fi

"$entry" --help >/dev/null
if "$entry" p8 recover /dev/mmcblk0 >/dev/null 2>&1; then
	die p8-accepted-partition-or-non-wsl-device
fi
if "$entry" p8 recover /dev/sdz --confirm-current-sha not-a-sha >/dev/null 2>&1; then
	die p8-accepted-invalid-current-sha-confirmation
fi
if "$entry" p8 recover /dev/sdz --confirm-current-sha >/dev/null 2>&1; then
	die p8-accepted-missing-current-sha-confirmation
fi
if "$entry" p7 unknown >/dev/null 2>&1; then
	die p7-accepted-unknown-action
fi
if "$p7_ui" release >/dev/null 2>&1; then
	die ui-pipeline-still-publishes-release
fi

# Exercise the state and lock boundaries with disposable host-only fixtures.
# These paths contain no release payload and every tested command must reject
# before source gates, SSH, deployment, reboot or a block-device operation.
fixture_nonce=$(date +%s%N)$RANDOM
[[ $fixture_nonce =~ ^[0-9]+$ ]] || die fixture-nonce-invalid
fixture_id=20991231T235959+0800-$fixture_nonce
fixture_other_id=20991231T235958+0800-$fixture_nonce
fixture=$workspace/reports/p7-cycles/$fixture_id
fixture_other=$workspace/reports/p7-cycles/$fixture_other_id
receipt_out=/tmp/rg40xxv-p7-source-receipt.atomic-$fixture_nonce
lock_backup=$workspace/backups/rg40xxv-p8-lock-test-$fixture_nonce
cleanup_fixtures()
{
	rm -rf -- "$fixture" "$fixture_other" "$receipt_out" "$lock_backup"
	find /tmp -maxdepth 1 -type d \
		-name ".${receipt_out##*/}.tmp.*" -exec rm -rf -- {} + 2>/dev/null || true
}
trap cleanup_fixtures EXIT
mkdir -m 0755 -- "$fixture" "$fixture/logs" "$fixture_other"
cat >"$fixture/STATE.env" <<EOF
schema=rg40xxv-p7-cycle-state-v1
run_id=$fixture_id
state=FAILED
updated_taipei=2099-12-31T23:59:59+08:00
p8_write=NONE
EOF
if "$p7_cycle" deploy "$fixture_id" >"$fixture/reject.log" 2>&1; then
	die state-machine-accepted-failed-deploy
fi
grep -Fq 'reason=invalid-state-transition:FAILED' "$fixture/reject.log" || \
	die state-machine-wrong-failed-rejection
[[ $(awk -F= '$1 == "state" {print $2}' "$fixture/STATE.env") == FAILED ]] || \
	die state-machine-overwrote-failed-state
sed -i 's/^state=FAILED$/state=PACKAGED/' "$fixture/STATE.env"
if "$p7_cycle" package "$fixture_id" >"$fixture/reject-package.log" 2>&1; then
	die state-machine-accepted-package-rerun
fi
grep -Fq 'reason=invalid-state-transition:PACKAGED' \
	"$fixture/reject-package.log" || die state-machine-wrong-package-rejection
[[ $(awk -F= '$1 == "state" {print $2}' "$fixture/STATE.env") == PACKAGED ]] || \
	die state-machine-overwrote-packaged-state

if "$p7_deploy" "$fixture/ARTIFACT.env" >"$fixture/direct.log" 2>&1; then
	die deploy-helper-accepted-without-cycle-lock
fi
grep -Fq 'reason=cycle-lock-not-inherited' "$fixture/direct.log" || \
	die deploy-helper-wrong-lock-rejection
printf 'dummy=yes\n' >"$fixture_other/ARTIFACT.env"
sha256sum "$fixture_other/ARTIFACT.env" >"$fixture_other/ARTIFACT.env.sha256"
chmod 0444 "$fixture_other/ARTIFACT.env" "$fixture_other/ARTIFACT.env.sha256"
set +e
(
	exec 8>"$workspace/reports/.rg40xxv-p7-cycle.lock"
	flock 8
	RG40XXV_P7_CYCLE_LOCK_FD=8 RG40XXV_P7_CYCLE_RUN="$fixture" \
		"$p7_deploy" "$fixture_other/ARTIFACT.env"
) >"$fixture/mismatch.log" 2>&1
mismatch_rc=$?
set -e
[[ $mismatch_rc -ne 0 ]] || die deploy-helper-accepted-cross-cycle-artifact
grep -Fq 'reason=artifact-cycle-mismatch' "$fixture/mismatch.log" || \
	die deploy-helper-wrong-cycle-rejection

set +e
timeout --signal=TERM --kill-after=1 0.05 \
	"$p7_receipt" "$receipt_out" >/dev/null 2>&1
receipt_rc=$?
set -e
[[ $receipt_rc -ne 0 ]] || die receipt-interrupt-did-not-interrupt
[[ ! -e $receipt_out && ! -L $receipt_out ]] || die receipt-left-partial-output
if find /tmp -maxdepth 1 -type d -name ".${receipt_out##*/}.tmp.*" \
	-print -quit | grep -q .; then
	die receipt-left-staging-directory
fi

# The low-level flasher is private: direct invocation must reject before it
# examines a candidate or device.  Clear inherited dispatcher authorization so
# this test remains valid when --full itself runs under the p8 operation lock.
set +e
env -i PATH="$PATH" "$p8_flasher" /tmp/nonexistent.img "$frozen_sha" \
	"$lock_backup" /dev/sdz >"$fixture/p8-lock.log" 2>&1
p8_lock_rc=$?
set -e
[[ $p8_lock_rc -ne 0 ]] || die p8-flasher-accepted-concurrent-operation
grep -Fq 'reason=dispatcher-authorization-required' "$fixture/p8-lock.log" || \
	die p8-flasher-accepted-direct-invocation

# Holding the common lock must make the public recovery entry reject before
# the expensive host verifier or any block-device inspection begins.
set +e
inherited_p8_fd=${RG40XXV_P8_OPERATION_LOCK_FD:-}
if [[ $inherited_p8_fd =~ ^[0-9]+$ && -e /proc/$$/fd/$inherited_p8_fd && \
	$(readlink -f "/proc/$$/fd/$inherited_p8_fd") == \
	"$workspace/reports/.rg40xxv-p8-operation.lock" ]]; then
	"$entry" p8 recover /dev/sdz "$lock_backup" \
		>"$fixture/p8-public-lock.log" 2>&1
	p8_public_lock_rc=$?
else
	(
		exec 7<>"$workspace/reports/.rg40xxv-p8-operation.lock"
		flock 7
		env -u RG40XXV_P8_OPERATION_LOCK_FD \
			"$entry" p8 recover /dev/sdz "$lock_backup"
	) >"$fixture/p8-public-lock.log" 2>&1
	p8_public_lock_rc=$?
fi
set -e
[[ $p8_public_lock_rc -ne 0 ]] || die p8-public-entry-accepted-concurrent-operation
grep -Fq 'reason=p8-operation-already-running' "$fixture/p8-public-lock.log" || \
	die p8-public-entry-wrong-lock-rejection

cleanup_fixtures
trap - EXIT

printf 'RG40XXV_PIPELINE_STATIC_TEST result=PASS frozen_p8_sha256=%s device_write=NONE p8_write=NONE\n' \
	"$frozen_sha"
