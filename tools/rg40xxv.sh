#!/usr/bin/env bash
set -euo pipefail
PATH=/usr/sbin:/usr/bin:/sbin:/bin
export PATH

# Single public entry point for RG40XX V release operations.  The p7 and p8
# namespaces are intentionally disjoint: p7 never accepts a block device and
# p8 never accepts a p7 release artifact.

script=$(realpath -- "$0")
workspace=$(CDPATH= cd -- "$(dirname -- "$script")/.." && pwd -P)
p7_cycle=$workspace/tools/rg40xxv-p7-cycle.sh
p8_verifier=$workspace/tools/verify-rg40xxv-p8-workspace.sh
p8_identifier=$workspace/tools/identify-rg40xxv-p8-wsl.sh
p8_readonly_arm=$workspace/tools/arm-rg40xxv-tf1-readonly.sh
p8_cycle=$workspace/tools/rg40xxv-p8-cycle.sh
p8_flasher=$workspace/tools/flash-rg40xxv-p8-wsl.sh
p8_image_registry=$workspace/tools/rg40xxv-p8-images.tsv
p8_operation_lock=$workspace/reports/.rg40xxv-p8-operation.lock
p8_backup_root=$workspace/backups
selftest=$workspace/tools/tests/test-rg40xxv-pipeline-static.sh
frozen_p8=$workspace/lab/candidates/rg40xxv-panel-adopt-closeflow-v9-noquiesce-dram1v2/rg40xxv-panel-adopt-closeflow-v9-noquiesce-dram1v2-persistent-legacy-p8.img
frozen_p8_sha=6455c4d82d1594c03ab4f799c276da7249c6ffefd6fa22dd55b9077b1dfd4be3
frozen_p8_bytes=67108864

usage()
{
	cat <<EOF
RG40XX V release entry point

Host-only status:
  $0 status
  $0 selftest

p7 userspace workflow (never writes p8):
  $0 p7 status
  $0 p7 host
  $0 p7 package [RUN|latest]
  $0 p7 deploy [RUN|latest]
  $0 p7 reboot [RUN|latest]
  $0 p7 accept [RUN|latest] [MODE]
  $0 p7 all

p8 boot/display workflow (never writes p7):
  $0 p8 verify
  $0 p8 prepare PROFILE
  $0 p8 status RUN
  $0 p8 arm-readonly WHOLE_TF1_DEVICE
  $0 p8 identify WHOLE_TF1_DEVICE [NEW_RECEIPT_FILE]
  $0 p8 flash RUN WHOLE_TF1_DEVICE NEW_BACKUP_DIRECTORY --confirm-sha FULL_SHA256
  $0 p8 recover WHOLE_TF1_DEVICE [NEW_BACKUP_DIRECTORY]
  $0 p8 recover WHOLE_TF1_DEVICE [NEW_BACKUP_DIRECTORY] --confirm-current-sha FULL_SHA256

P8 recovery is fixed to the frozen v9 image:
  SHA-256: $frozen_p8_sha

MODE is one of: preflight, ui, connectivity, runtime, performance, snapshot.
The p8 identify/recovery commands require an explicit whole-card device such
as /dev/sde.  Identify is strictly read-only.  Recovery cannot accept a
different target image or SHA-256.  A registry-unknown current p8 is accepted
only by recovery when its complete SHA-256 is explicitly confirmed.
EOF
}

die()
{
	printf 'RG40XXV_ENTRY result=FAIL reason=%s\n' "$1" >&2
	exit 1
}

require_executable()
{
	[[ -f $1 && ! -L $1 && -x $1 ]] || die "missing-executable:$1"
}

prepare_p8_operation_lock()
{
	local reports_real
	reports_real=$(realpath -e -- "$workspace/reports") || die p8-reports-path
	[[ $reports_real == "$workspace/reports" && -d $reports_real && \
		! -L $workspace/reports ]] || die p8-reports-path-invalid
	if [[ ! -e $p8_operation_lock && ! -L $p8_operation_lock ]]; then
		(umask 077; set -o noclobber; : >"$p8_operation_lock") 2>/dev/null || \
			die p8-operation-lock-create
	fi
	[[ -f $p8_operation_lock && ! -L $p8_operation_lock && \
		$(realpath -e -- "$p8_operation_lock") == "$p8_operation_lock" ]] || \
		die p8-operation-lock-invalid
	[[ $(stat -c '%u:%g:%h' -- "$p8_operation_lock") == 0:0:1 ]] || \
		die p8-operation-lock-owner-or-link-invalid
	chmod 0600 -- "$p8_operation_lock" || die p8-operation-lock-mode-repair
	[[ $(stat -c %a -- "$p8_operation_lock") == 600 ]] || \
		die p8-operation-lock-mode-invalid
}

p8_run_full_verifier()
{
	local lock_fd=${RG40XXV_P8_OPERATION_LOCK_FD:-} auth_name
	[[ $lock_fd =~ ^[0-9]+$ && -e /proc/$$/fd/$lock_fd && \
		$(readlink -f -- "/proc/$$/fd/$lock_fd") == "$p8_operation_lock" ]] || \
		die p8-verifier-operation-lock-fd-invalid
	(
		# A verifier must never inherit a device/action capability from the
		# surrounding identify, flash or recovery transaction.  Keep only the
		# canonical operation-lock FD so nested lock-boundary tests remain valid.
		while IFS= read -r auth_name; do
			unset "$auth_name"
		done < <(compgen -A variable RG40XXV_P8_AUTH_)
		exec "$p8_verifier" --full
	)
}

p8_kv_exact()
{
	local file=$1 key=$2
	awk -F= -v wanted="$key" '
		$1 == wanted { value = substr($0, length($1) + 2); count++ }
		END { if (count != 1) exit 1; print value }
	' "$file"
}

p8_capture_device_binding()
{
	local device=$1 name sysfs decimal expected_hex diskseq_file
	[[ $device =~ ^/dev/sd[a-z]$ && -b $device && ! -L $device && \
		$(readlink -f -- "$device") == "$device" ]] || die p8-device-binding-invalid
	name=${device##*/}
	[[ -d /sys/class/block/$name ]] || die p8-device-sysfs-missing
	sysfs=$(readlink -f -- "/sys/class/block/$name") || die p8-device-sysfs-read
	[[ $sysfs == /sys/devices/* ]] || die p8-device-sysfs-invalid
	decimal=$(cat "/sys/class/block/$name/dev") || die p8-device-number-read
	[[ $decimal =~ ^[0-9]+:[0-9]+$ ]] || die p8-device-number-invalid
	printf -v expected_hex '%x:%x' "${decimal%:*}" "${decimal#*:}"
	P8_BOUND_DISK_NODE_ID=$(stat -Lc '%t:%T' "$device") || die p8-device-node-id-read
	[[ $P8_BOUND_DISK_NODE_ID == "$expected_hex" ]] || die p8-device-node-sysfs-mismatch
	diskseq_file=/sys/class/block/$name/diskseq
	[[ -r $diskseq_file ]] || die p8-device-diskseq-unavailable
	P8_BOUND_DISKSEQ=$(cat "$diskseq_file") || die p8-device-diskseq-read
	[[ $P8_BOUND_DISKSEQ =~ ^[0-9]+$ ]] || die p8-device-diskseq-invalid
	P8_BOUND_DISK_DEVICE_NUMBER=$decimal
	P8_BOUND_DISK_SYSFS_PATH=$sysfs
}

p8_resolve_run()
{
	local run_id=$1 run
	[[ $run_id != latest && \
		$run_id =~ ^[0-9]{8}T[0-9]{6}[+-][0-9]{4}-[0-9]+$ ]] || \
		die p8-invalid-run-id
	run=$workspace/reports/p8-runs/$run_id
	[[ -d $run && ! -L $run && $(realpath -e -- "$run") == "$run" ]] || \
		die p8-run-invalid
	printf '%s' "$run"
}

p8_verify_receipt_sidecar()
{
	local file=$1 expected_name=${1##*/}
	[[ -f $file && ! -L $file && -f $file.sha256 && ! -L $file.sha256 && \
		$(stat -c %a -- "$file") == 444 && \
		$(stat -c %a -- "$file.sha256") == 444 ]] || die p8-receipt-invalid
	(
		cd "${file%/*}"
		sha256sum -c "${file##*/}.sha256" >/dev/null
	) || die p8-receipt-sha256-mismatch
	[[ $(awk '{print $2}' "$file.sha256") == "$expected_name" ]] || \
		die p8-receipt-sidecar-name-mismatch
}

p8_register_host_candidate()
{
	local run_id=$1 candidate_sha=$2 count existing temp before_sha identity
	[[ $candidate_sha =~ ^[0-9a-f]{64}$ ]] || die p8-candidate-sha-invalid
	[[ -f $p8_image_registry && ! -L $p8_image_registry && \
		$(realpath -e -- "$p8_image_registry") == "$p8_image_registry" ]] || \
		die p8-image-registry-invalid
	count=$(awk -F '\t' -v sha="$candidate_sha" '$1 == sha {n++} END {print n+0}' \
		"$p8_image_registry")
	if [[ $count == 1 ]]; then
		existing=$(awk -F '\t' -v sha="$candidate_sha" \
			'$1 == sha {print $2 "\t" $3 "\t" $4}' "$p8_image_registry")
		[[ $existing == $'KNOWN\tHOST_ONLY\t'* ]] || \
			die p8-candidate-registry-policy-conflict
		return
	fi
	[[ $count == 0 ]] || die p8-candidate-registry-duplicate
	identity=candidate-${run_id//+/_}
	[[ $identity =~ ^[A-Za-z0-9._-]+$ ]] || die p8-candidate-registry-identity
	before_sha=$(sha256sum "$p8_image_registry" | awk '{print $1}') || \
		die p8-image-registry-sha-read
	temp=$(mktemp "$workspace/tools/.rg40xxv-p8-images.tsv.XXXXXX") || \
		die p8-image-registry-temp
	cp -- "$p8_image_registry" "$temp"
	printf '%s\tKNOWN\tHOST_ONLY\t%s\n' "$candidate_sha" "$identity" >>"$temp"
	chmod 0644 "$temp"
	[[ $(sha256sum "$p8_image_registry" | awk '{print $1}') == "$before_sha" ]] || {
		rm -f -- "$temp"
		die p8-image-registry-changed-during-update
	}
	mv -- "$temp" "$p8_image_registry"
	[[ $(awk -F '\t' -v sha="$candidate_sha" '$1 == sha && $2 == "KNOWN" && \
		$3 == "HOST_ONLY" {n++} END {print n+0}' "$p8_image_registry") == 1 ]] || \
		die p8-candidate-registry-publish-failed
}

p8_publish_flash_manifest()
{
	local run=$1 device=$2 receipt=$3 outgoing=$4 lock_sha p8_rel p8_sha
	local receipt_rel receipt_sha temporary manifest disk_node_id
	local disk_device_number disk_sysfs_path diskseq
	p8_verify_receipt_sidecar "$run/BUILD-LOCK.env"
	lock_sha=$(sha256sum "$run/BUILD-LOCK.env" | awk '{print $1}')
	p8_rel=$(p8_kv_exact "$run/BUILD-LOCK.env" p8) || die p8-build-lock-p8
	p8_sha=$(p8_kv_exact "$run/BUILD-LOCK.env" p8_sha256) || die p8-build-lock-p8-sha
	receipt_rel=${receipt#"$workspace/"}
	[[ $receipt_rel == reports/* && $receipt != "$receipt_rel" ]] || \
		die p8-identify-receipt-relative-path
	receipt_sha=$(sha256sum "$receipt" | awk '{print $1}') || \
		die p8-identify-receipt-sha-read
	disk_node_id=$(p8_kv_exact "$receipt" disk_node_id) || \
		die p8-identify-disk-node-id-read
	disk_device_number=$(p8_kv_exact "$receipt" disk_device_number) || \
		die p8-identify-disk-device-number-read
	disk_sysfs_path=$(p8_kv_exact "$receipt" disk_sysfs_path) || \
		die p8-identify-disk-sysfs-path-read
	diskseq=$(p8_kv_exact "$receipt" diskseq) || die p8-identify-diskseq-read
	[[ $disk_node_id =~ ^[0-9a-f]+:[0-9a-f]+$ && \
		$disk_device_number =~ ^[0-9]+:[0-9]+$ && \
		$disk_sysfs_path == /sys/devices/* && $diskseq =~ ^[0-9]+$ ]] || \
		die p8-identify-device-binding-invalid
	manifest=$run/FLASH-MANIFEST.env
	[[ ! -e $manifest && ! -L $manifest ]] || die p8-flash-manifest-exists
	temporary=$(mktemp "$run/.FLASH-MANIFEST.env.tmp.XXXXXX") || \
		die p8-flash-manifest-temp
	{
		printf 'schema=rg40xxv-p8-flash-manifest-v2\nrun_id=%s\n' "${run##*/}"
		printf 'action=FLASH_REQUESTED\nbuild_lock_sha256=%s\n' "$lock_sha"
		printf 'p8=%s\np8_sha256=%s\np8_bytes=%s\n' \
			"$p8_rel" "$p8_sha" "$frozen_p8_bytes"
		printf 'device=%s\nidentify_receipt=%s\nidentify_receipt_sha256=%s\n' \
			"$device" "$receipt_rel" "$receipt_sha"
		printf 'disk_node_id=%s\ndisk_device_number=%s\n' \
			"$disk_node_id" "$disk_device_number"
		printf 'disk_sysfs_path=%s\ndiskseq=%s\n' "$disk_sysfs_path" "$diskseq"
		printf 'outgoing_p8_sha256=%s\np7_write=NONE\ndevice_write=NOT_PERFORMED\n' \
			"$outgoing"
	} >"$temporary"
	chmod 0444 "$temporary"
	ln -- "$temporary" "$manifest" || {
		rm -f -- "$temporary"
		die p8-flash-manifest-publish-conflict
	}
	rm -f -- "$temporary"
}

dispatch_p7()
{
	local action=${1:-}
	require_executable "$p7_cycle"
	case $action in
	status|host|package|deploy|reboot|accept|all)
		printf 'RG40XXV_ENTRY scope=p7 action=%s p8_write=NONE\n' "$action"
		exec "$p7_cycle" "$@"
		;;
	-h|--help|help|'')
		usage
		;;
	*)
		die "unknown-p7-action:$action"
		;;
	esac
}

dispatch_p8()
{
	local action=${1:-} device receipt backup actual_sha actual_bytes actual_registry_sha
	local run_id run state confirm_sha candidate_rel candidate candidate_sha lock_sha
	local outgoing flash_auth_sha receipt_rel receipt_registry_sha registry_row
	local unknown_current_sha= bound_disk_node_id bound_disk_device_number
	local bound_disk_sysfs_path bound_diskseq
	case $action in
	verify)
		[[ $# == 1 ]] || die p8-verify-arguments
		require_executable "$p8_verifier"
		prepare_p8_operation_lock
		exec 9<>"$p8_operation_lock"
		flock -n 9 || die p8-operation-already-running
		export RG40XXV_P8_OPERATION_LOCK_FD=9
		printf 'RG40XXV_ENTRY scope=p8 action=verify device_write=NONE\n'
		p8_run_full_verifier
		;;
	prepare)
		[[ $# == 2 ]] || die p8-prepare-arguments
		require_executable "$p8_cycle"
		prepare_p8_operation_lock
		exec 9<>"$p8_operation_lock"
		flock -n 9 || die p8-operation-already-running
		export RG40XXV_P8_OPERATION_LOCK_FD=9
		require_executable "$p8_verifier"
		p8_run_full_verifier
		printf 'RG40XXV_ENTRY scope=p8 action=prepare profile=%s device_write=NONE p7_write=NONE\n' "$2"
		exec "$p8_cycle" prepare "$2"
		;;
	status)
		[[ $# == 2 ]] || die p8-status-arguments
		require_executable "$p8_cycle"
		prepare_p8_operation_lock
		exec 9<>"$p8_operation_lock"
		flock -n 9 || die p8-operation-already-running
		export RG40XXV_P8_OPERATION_LOCK_FD=9
		printf 'RG40XXV_ENTRY scope=p8 action=status run_id=%s device_write=NONE p7_write=NONE\n' "$2"
		exec "$p8_cycle" status "$2"
		;;
	arm-readonly)
		[[ $# == 2 ]] || die p8-arm-readonly-arguments
		device=$2
		[[ $device =~ ^/dev/sd[a-z]$ ]] || die p8-arm-readonly-whole-device-required
		require_executable "$p8_readonly_arm"
		command -v flock >/dev/null 2>&1 || die missing-flock
		prepare_p8_operation_lock
		exec 9<>"$p8_operation_lock"
		flock -n 9 || die p8-operation-already-running
		export RG40XXV_P8_OPERATION_LOCK_FD=9
		export RG40XXV_P8_AUTH_MODE=arm-readonly
		export RG40XXV_P8_AUTH_DEVICE=$device
		printf 'RG40XXV_ENTRY scope=p8 action=arm-readonly device=%s device_write=NONE p7_write=NONE\n' \
			"$device"
		exec "$p8_readonly_arm" "$device"
		;;
	identify)
		[[ $# -ge 2 && $# -le 3 ]] || die p8-identify-arguments
		device=$2
		[[ $device =~ ^/dev/sd[a-z]$ ]] || die p8-identify-whole-device-required
		receipt=${3:-$workspace/reports/rg40xxv-p8-identify-$(date '+%Y%m%dT%H%M%S%z')-$$.receipt}
		receipt=$(realpath -m -- "$receipt") || die p8-identify-receipt-path
		[[ ! -e $receipt && ! -L $receipt ]] || die p8-identify-receipt-must-be-new
		require_executable "$p8_identifier"
		command -v flock >/dev/null 2>&1 || die missing-flock
		prepare_p8_operation_lock
		exec 9<>"$p8_operation_lock"
		flock -n 9 || die p8-operation-already-running
		export RG40XXV_P8_OPERATION_LOCK_FD=9
		export RG40XXV_P8_AUTH_MODE=identify
		export RG40XXV_P8_AUTH_DEVICE=$device
		export RG40XXV_P8_AUTH_RECEIPT=$receipt
		require_executable "$p8_verifier"
		p8_run_full_verifier
		[[ -f $p8_image_registry && ! -L $p8_image_registry ]] || die p8-image-registry-missing
		actual_registry_sha=$(sha256sum "$p8_image_registry" | awk '{print $1}') || \
			die p8-image-registry-sha-read
		export RG40XXV_P8_AUTH_REGISTRY_SHA256=$actual_registry_sha
		printf 'RG40XXV_ENTRY scope=p8 action=identify device=%s receipt=%s device_write=NONE p7_write=NONE\n' \
			"$device" "$receipt"
		exec "$p8_identifier" "$device" "$receipt"
		;;
	flash)
		[[ $# == 6 && $5 == --confirm-sha ]] || die p8-flash-arguments
		run_id=$2
		device=$3
		backup=$4
		confirm_sha=$6
		[[ $device =~ ^/dev/sd[a-z]$ ]] || die p8-flash-whole-device-required
		[[ $confirm_sha =~ ^[0-9a-f]{64}$ ]] || die p8-flash-confirm-sha-invalid
		run=$(p8_resolve_run "$run_id")
		[[ -d $p8_backup_root && ! -L $p8_backup_root && \
			$(realpath -e -- "$p8_backup_root") == "$p8_backup_root" ]] || \
			die p8-backup-root-invalid
		backup=$(realpath -m -- "$backup") || die p8-flash-backup-path
		[[ ${backup%/*} == "$p8_backup_root" && \
			${backup##*/} =~ ^[A-Za-z0-9._+-]+$ && \
			! -e $backup && ! -L $backup ]] || die p8-flash-backup-must-be-new
		command -v flock >/dev/null 2>&1 || die missing-flock
		prepare_p8_operation_lock
		exec 9<>"$p8_operation_lock"
		flock -n 9 || die p8-operation-already-running
		export RG40XXV_P8_OPERATION_LOCK_FD=9

		# First device action after acquiring the common lock: exact TF1
		# identity/layout/consumer validation followed by parent+p1-p8 RO.
		require_executable "$p8_readonly_arm"
		export RG40XXV_P8_AUTH_MODE=arm-readonly
		export RG40XXV_P8_AUTH_DEVICE=$device
		"$p8_readonly_arm" "$device"

		require_executable "$p8_verifier"
		p8_run_full_verifier
		require_executable "$p8_cycle"
		"$p8_cycle" status "$run_id"
		state=$(p8_kv_exact "$run/STATE.env" state) || die p8-run-state-read
		p8_verify_receipt_sidecar "$run/BUILD-LOCK.env"
		candidate_rel=$(p8_kv_exact "$run/BUILD-LOCK.env" p8) || \
			die p8-build-lock-p8
		candidate_sha=$(p8_kv_exact "$run/BUILD-LOCK.env" p8_sha256) || \
			die p8-build-lock-p8-sha
		[[ $candidate_sha == "$confirm_sha" ]] || die p8-flash-confirm-sha-mismatch
		candidate=$(realpath -e -- "$run/$candidate_rel") || die p8-candidate-missing
		[[ $candidate == "$run/artifact/$candidate_sha.img" && \
			-f $candidate && ! -L $candidate && \
			$(stat -c %s -- "$candidate") == "$frozen_p8_bytes" && \
			$(sha256sum "$candidate" | awk '{print $1}') == "$candidate_sha" ]] || \
			die p8-candidate-build-lock-mismatch

		case $state in
		PACKAGED_HOST_PASS)
			# Registration means only "recognizable outgoing for recovery";
			# HOST_ONLY is not an acceptance claim.
			p8_register_host_candidate "$run_id" "$candidate_sha"
			actual_registry_sha=$(sha256sum "$p8_image_registry" | awk '{print $1}') || \
				die p8-image-registry-sha-read
			receipt=$workspace/reports/rg40xxv-p8-identify-${run_id//+/_}-$(date '+%Y%m%dT%H%M%S%z')-$$.receipt
			[[ ! -e $receipt && ! -L $receipt ]] || die p8-identify-receipt-must-be-new
			require_executable "$p8_identifier"
			export RG40XXV_P8_AUTH_MODE=identify
			export RG40XXV_P8_AUTH_DEVICE=$device
			export RG40XXV_P8_AUTH_RECEIPT=$receipt
			export RG40XXV_P8_AUTH_REGISTRY_SHA256=$actual_registry_sha
			"$p8_identifier" "$device" "$receipt"
			outgoing=$(p8_kv_exact "$receipt" p8_sha256) || \
				die p8-identify-outgoing-sha-read
			[[ $outgoing =~ ^[0-9a-f]{64}$ ]] || die p8-identify-outgoing-sha-invalid
			[[ $(p8_kv_exact "$receipt" p8_registry_outgoing_policy) == KNOWN ]] || \
				die p8-candidate-current-p8-unknown
			p8_publish_flash_manifest "$run" "$device" "$receipt" "$outgoing"
			"$p8_cycle" flash-preflight "$run_id"
			;;
		FLASH_AUTHORIZED)
			p8_verify_receipt_sidecar "$run/FLASH-AUTHORIZATION.env"
			[[ $(p8_kv_exact "$run/FLASH-AUTHORIZATION.env" device) == "$device" ]] || \
				die p8-authorized-device-mismatch
			outgoing=$(p8_kv_exact "$run/FLASH-AUTHORIZATION.env" outgoing_p8_sha256) || \
				die p8-authorized-outgoing-sha-read
			if [[ -f $run/FLASH-RESULT.env && -f $run/FLASH-RESULT.env.sha256 ]]; then
				"$p8_cycle" flash-complete "$run_id"
				printf 'RG40XXV_ENTRY scope=p8 action=flash run_id=%s state=FLASHED_READBACK_PASS p8_sha256=%s resumed=RESULT_ONLY p7_write=NONE\n' \
					"$run_id" "$candidate_sha"
				return
			fi
			registry_row=$(awk -F '\t' -v sha="$candidate_sha" \
				'$1 == sha {print $2 "\t" $3}' "$p8_image_registry")
			[[ $registry_row == $'KNOWN\tHOST_ONLY' ]] || \
				die p8-authorized-candidate-registry-missing
			actual_registry_sha=$(sha256sum "$p8_image_registry" | awk '{print $1}') || \
				die p8-image-registry-sha-read
			receipt_rel=$(p8_kv_exact "$run/FLASH-AUTHORIZATION.env" identify_receipt) || \
				die p8-authorized-identify-receipt-read
			receipt=$workspace/$receipt_rel
			receipt_registry_sha=$(p8_kv_exact "$receipt" image_registry_sha256) || \
				die p8-identify-registry-sha-read
			[[ $receipt_registry_sha == "$actual_registry_sha" ]] || \
				die p8-image-registry-changed-after-authorization
			;;
		*) die "p8-flash-invalid-state:$state" ;;
		esac

		p8_verify_receipt_sidecar "$run/FLASH-AUTHORIZATION.env"
		lock_sha=$(sha256sum "$run/BUILD-LOCK.env" | awk '{print $1}')
		flash_auth_sha=$(sha256sum "$run/FLASH-AUTHORIZATION.env" | awk '{print $1}')
		actual_registry_sha=$(sha256sum "$p8_image_registry" | awk '{print $1}')
		bound_disk_node_id=$(p8_kv_exact "$run/FLASH-AUTHORIZATION.env" disk_node_id) || \
			die p8-authorized-disk-node-id-read
		bound_disk_device_number=$(p8_kv_exact "$run/FLASH-AUTHORIZATION.env" disk_device_number) || \
			die p8-authorized-disk-device-number-read
		bound_disk_sysfs_path=$(p8_kv_exact "$run/FLASH-AUTHORIZATION.env" disk_sysfs_path) || \
			die p8-authorized-disk-sysfs-path-read
		bound_diskseq=$(p8_kv_exact "$run/FLASH-AUTHORIZATION.env" diskseq) || \
			die p8-authorized-diskseq-read
		export RG40XXV_P8_AUTH_MODE=candidate
		export RG40XXV_P8_AUTH_SHA256=$candidate_sha
		export RG40XXV_P8_AUTH_DEVICE=$device
		export RG40XXV_P8_AUTH_BACKUP=$backup
		export RG40XXV_P8_AUTH_CANDIDATE=$candidate
		export RG40XXV_P8_AUTH_REGISTRY_SHA256=$actual_registry_sha
		export RG40XXV_P8_AUTH_RUN=$run
		export RG40XXV_P8_AUTH_BUILD_LOCK_SHA256=$lock_sha
		export RG40XXV_P8_AUTH_FLASH_AUTHORIZATION_SHA256=$flash_auth_sha
		export RG40XXV_P8_AUTH_OUTGOING_SHA256=$outgoing
		export RG40XXV_P8_AUTH_RESULT=$run/FLASH-RESULT.env
		export RG40XXV_P8_AUTH_DISK_NODE_ID=$bound_disk_node_id
		export RG40XXV_P8_AUTH_DISK_DEVICE_NUMBER=$bound_disk_device_number
		export RG40XXV_P8_AUTH_DISK_SYSFS_PATH=$bound_disk_sysfs_path
		export RG40XXV_P8_AUTH_DISKSEQ=$bound_diskseq
		export RG40XXV_P8_AUTH_UNKNOWN_CURRENT_SHA256=
		require_executable "$p8_flasher"
		printf 'RG40XXV_ENTRY scope=p8 action=flash run_id=%s device=%s p8_sha256=%s backup=%s p7_write=NONE\n' \
			"$run_id" "$device" "$candidate_sha" "$backup"
		"$p8_flasher" "$candidate" "$candidate_sha" "$backup" "$device"
		"$p8_cycle" flash-complete "$run_id"
		printf 'RG40XXV_ENTRY result=PASS scope=p8 action=flash run_id=%s state=FLASHED_READBACK_PASS p8_sha256=%s p7_write=NONE\n' \
			"$run_id" "$candidate_sha"
		;;
		recover)
		case $# in
		2) backup= ;;
		3)
			[[ $3 != --* ]] || die p8-recover-arguments
			backup=$3
			;;
		4)
			[[ $3 == --confirm-current-sha ]] || die p8-recover-arguments
			backup=
			unknown_current_sha=$4
			;;
		5)
			[[ $3 != --* ]] || die p8-recover-arguments
			backup=$3
			[[ $4 == --confirm-current-sha ]] || die p8-recover-arguments
			unknown_current_sha=$5
			;;
		*) die p8-recover-arguments ;;
		esac
		device=$2
		[[ $device =~ ^/dev/sd[a-z]$ ]] || die p8-recover-whole-device-required
		[[ -z $unknown_current_sha || $unknown_current_sha =~ ^[0-9a-f]{64}$ ]] || \
			die p8-recover-confirm-current-sha-invalid
		[[ -d $p8_backup_root && ! -L $p8_backup_root ]] || die p8-backup-root-invalid
		[[ $(realpath -e -- "$p8_backup_root") == "$p8_backup_root" ]] || \
			die p8-backup-root-not-canonical
		backup=${backup:-$p8_backup_root/rg40xxv-p8-v9-recovery-$(date '+%Y%m%dT%H%M%S%z')-$$}
		backup=$(realpath -m -- "$backup") || die p8-recover-backup-path
		[[ ${backup%/*} == "$p8_backup_root" ]] || die p8-recover-backup-outside-host-root
		[[ ${backup##*/} =~ ^[A-Za-z0-9._+-]+$ ]] || die p8-recover-backup-name
		[[ ! -e $backup && ! -L $backup ]] || die p8-recover-backup-must-be-new
		command -v flock >/dev/null 2>&1 || die missing-flock
		prepare_p8_operation_lock
		exec 9<>"$p8_operation_lock"
		flock -n 9 || die p8-operation-already-running
		export RG40XXV_P8_OPERATION_LOCK_FD=9
		# A freshly attached USB/WSL disk commonly starts writable.  Under the
		# same operation lock, fully identify it and arm parent+p1-p8 read-only
		# before any artifact hashing/build verification.  The flasher then
		# repeats all guards and opens only p8 for its bounded RW window.
		require_executable "$p8_readonly_arm"
		export RG40XXV_P8_AUTH_MODE=arm-readonly
		export RG40XXV_P8_AUTH_DEVICE=$device
		"$p8_readonly_arm" "$device"
		p8_capture_device_binding "$device"
		require_executable "$p8_flasher"
		[[ -f $frozen_p8 && ! -L $frozen_p8 ]] || die frozen-p8-missing
		actual_bytes=$(stat -c %s -- "$frozen_p8") || die frozen-p8-stat
		[[ $actual_bytes == "$frozen_p8_bytes" ]] || die frozen-p8-size
		actual_sha=$(sha256sum "$frozen_p8" | awk '{print $1}') || \
			die frozen-p8-sha-read
		[[ $actual_sha == "$frozen_p8_sha" ]] || die frozen-p8-sha-mismatch
		require_executable "$p8_verifier"
		p8_run_full_verifier
		[[ -f $p8_image_registry && ! -L $p8_image_registry ]] || die p8-image-registry-missing
		actual_registry_sha=$(sha256sum "$p8_image_registry" | awk '{print $1}') || \
			die p8-image-registry-sha-read
		export RG40XXV_P8_AUTH_MODE=recover
		export RG40XXV_P8_AUTH_SHA256=$frozen_p8_sha
		export RG40XXV_P8_AUTH_DEVICE=$device
		export RG40XXV_P8_AUTH_BACKUP=$backup
		export RG40XXV_P8_AUTH_CANDIDATE=$frozen_p8
		export RG40XXV_P8_AUTH_REGISTRY_SHA256=$actual_registry_sha
		export RG40XXV_P8_AUTH_DISK_NODE_ID=$P8_BOUND_DISK_NODE_ID
		export RG40XXV_P8_AUTH_DISK_DEVICE_NUMBER=$P8_BOUND_DISK_DEVICE_NUMBER
		export RG40XXV_P8_AUTH_DISK_SYSFS_PATH=$P8_BOUND_DISK_SYSFS_PATH
		export RG40XXV_P8_AUTH_DISKSEQ=$P8_BOUND_DISKSEQ
		export RG40XXV_P8_AUTH_UNKNOWN_CURRENT_SHA256=$unknown_current_sha
		printf 'RG40XXV_ENTRY scope=p8 action=recover device=%s image_sha256=%s backup=%s p7_write=NONE\n' \
			"$device" "$frozen_p8_sha" "$backup"
		exec "$p8_flasher" "$frozen_p8" "$frozen_p8_sha" "$backup" "$device"
		;;
	-h|--help|help|'')
		usage
		;;
	*)
		die "unknown-p8-action:$action"
		;;
	esac
}

case ${1:-} in
status)
	[[ $# == 1 ]] || die status-arguments
	require_executable "$p7_cycle"
	printf 'RG40XXV_ENTRY scope=host action=status device_write=NONE\n'
	exec "$p7_cycle" status
	;;
selftest)
	[[ $# == 1 ]] || die selftest-arguments
	require_executable "$selftest"
	printf 'RG40XXV_ENTRY scope=host action=selftest device_write=NONE\n'
	exec "$selftest"
	;;
p7)
	shift
	dispatch_p7 "$@"
	;;
p8)
	shift
	dispatch_p8 "$@"
	;;
-h|--help|help|'')
	usage
	;;
*)
	die "unknown-scope:${1:-missing}"
	;;
esac
