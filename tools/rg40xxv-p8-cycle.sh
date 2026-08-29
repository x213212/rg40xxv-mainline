#!/usr/bin/env bash
set -euo pipefail

# Internal p8 build/acceptance state machine.  This file never reads or writes
# a block device.  A future public dispatcher may create FLASH-MANIFEST.env,
# perform the guarded p8-only flash, then publish the documented flash receipt.

script=$(realpath -- "$0")
workspace=$(CDPATH= cd -- "$(dirname -- "$script")/.." && pwd -P)
profile_root=$workspace/lab/p8-profiles
report_root=$workspace/reports/p8-runs
operation_lock=$workspace/reports/.rg40xxv-p8-operation.lock
frozen_recovery_sha=6455c4d82d1594c03ab4f799c276da7249c6ffefd6fa22dd55b9077b1dfd4be3
banned_p8_sha=1c2366398314abb28785f13f69b733f9ce67129c729f523fbcd21cf996358941
p8_bytes=67108864
tf1_bytes=62516101120
tf1_guid=PUT-YOUR-OWN-CARD-GPT-GUID-HERE
stock_p4_sha=09bb1eee75a3ae1b2950c0886c0777abd1a48a266e045376a1b9e098894fc519

active_prepare=0
active_run=
declare -A KV=()

usage()
{
	cat >&2 <<EOF
usage: $0 prepare PROFILE
       $0 flash-preflight RUN
       $0 flash-complete RUN
       $0 observe RUN STEP P8_SHA256 PASS|FAIL
       $0 status RUN

RUN must be an exact run id; "latest" and paths are never accepted.
STEP order: first-cold, warm-1, warm-2, warm-3, cold-1, cold-2, cold-3.
This program never performs a device write.
EOF
	exit 2
}

die()
{
	printf 'P8_CYCLE result=FAIL reason=%s device_write=NONE p7_write=NONE\n' "$1" >&2
	exit 1
}

for tool in awk bash chmod cmp cp date dirname find flock grep mkdir mv \
	ln readlink realpath rm sha256sum stat tee wc; do
	command -v "$tool" >/dev/null 2>&1 || die "missing-tool:$tool"
done

valid_sha()
{
	[[ $1 =~ ^[0-9a-f]{64}$ ]]
}

valid_relative_path()
{
	local value=$1
	[[ $value =~ ^[A-Za-z0-9._+/-]+$ && $value != /* && $value != . && \
		$value != .. && $value != ../* && $value != */../* && \
		$value != */.. && $value != *//* ]]
}

read_kv()
{
	local file=$1 line key value allowed_key
	shift
	local -A allowed=()
	KV=()
	[[ -f $file && ! -L $file ]] || die "kv-file-missing:$file"
	for allowed_key in "$@"; do allowed[$allowed_key]=1; done
	while IFS= read -r line || [[ -n $line ]]; do
		[[ $line =~ ^([a-z][a-z0-9_]*)=([A-Za-z0-9._/,+:-]+)$ ]] || \
			die "invalid-kv-line:$file"
		key=${BASH_REMATCH[1]}
		value=${BASH_REMATCH[2]}
		[[ ${allowed[$key]+yes} ]] || die "unknown-kv-key:$key"
		[[ ! ${KV[$key]+yes} ]] || die "duplicate-kv-key:$key"
		KV[$key]=$value
	done <"$file"
	[[ ${#KV[@]} == $# ]] || die "missing-kv-key:$file"
	for allowed_key in "$@"; do
		[[ ${KV[$allowed_key]+yes} ]] || die "missing-kv-key:$allowed_key"
	done
}

write_state()
{
	local run=$1 state=$2 temporary
	temporary=$run/.STATE.env.$$
	{
		printf 'schema=rg40xxv-p8-cycle-state-v1\n'
		printf 'run_id=%s\n' "${run##*/}"
		printf 'state=%s\n' "$state"
		printf 'updated_taipei=%s\n' "$(date '+%Y-%m-%dT%H:%M:%S%:z')"
		printf 'p7_write=NONE\n'
	} >"$temporary"
	chmod 0444 "$temporary"
	mv -f -- "$temporary" "$run/STATE.env"
}

read_state()
{
	local run=$1 state
	read_kv "$run/STATE.env" schema run_id state updated_taipei p7_write
	[[ ${KV[schema]} == rg40xxv-p8-cycle-state-v1 && \
		${KV[run_id]} == "${run##*/}" && ${KV[p7_write]} == NONE ]] || \
		die invalid-state-receipt
	state=${KV[state]}
	case $state in
	PREPARING|PREPARE_FAILED|PACKAGED_HOST_PASS|FLASH_AUTHORIZED|\
	FLASHED_READBACK_PASS|OBSERVED_FIRST_COLD_PASS|OBSERVED_WARM_1_PASS|\
	OBSERVED_WARM_2_PASS|OBSERVED_WARM_3_PASS|OBSERVED_COLD_1_PASS|\
	OBSERVED_COLD_2_PASS|ACCEPTED|RECOVERY_REQUIRED) ;;
	*) die "invalid-state:$state" ;;
	esac
	printf '%s' "$state"
}

require_state()
{
	local run=$1 expected=$2 actual
	actual=$(read_state "$run")
	[[ $actual == "$expected" ]] || die "invalid-state-transition:$actual:$expected"
}

on_exit()
{
	local rc=$?
	trap - EXIT
	if [[ $rc -ne 0 && $active_prepare == 1 && -n $active_run && -d $active_run ]]; then
		set +e
		write_state "$active_run" PREPARE_FAILED >/dev/null 2>&1
		set -e
	fi
	exit "$rc"
}
trap on_exit EXIT

require_operation_lock()
{
	local fd=${RG40XXV_P8_OPERATION_LOCK_FD:-} target
	[[ $fd =~ ^[0-9]+$ && -e /proc/$$/fd/$fd ]] || \
		die p8-operation-lock-inheritance-required
	[[ -f $operation_lock && ! -L $operation_lock ]] || die p8-operation-lock-invalid
	target=$(readlink -f "/proc/$$/fd/$fd") || die p8-operation-lock-fd
	[[ $target == "$operation_lock" ]] || die p8-operation-lock-fd-mismatch
	if (
		exec 7>"$operation_lock"
		flock -n 7
	); then
		die p8-operation-lock-not-held
	fi
}

resolve_run()
{
	local run_id=$1 run resolved
	[[ $run_id != latest && \
		$run_id =~ ^[0-9]{8}T[0-9]{6}[+-][0-9]{4}-[0-9]+$ ]] || \
		die invalid-run-id
	run=$report_root/$run_id
	[[ -d $run && ! -L $run ]] || die run-not-found
	resolved=$(realpath -e -- "$run") || die run-not-found
	[[ ${resolved%/*} == "$report_root" && ${resolved##*/} == "$run_id" ]] || \
		die run-outside-report-root
	printf '%s' "$resolved"
}

write_checksum()
{
	local file=$1 sidecar temporary
	sidecar=$file.sha256
	temporary=$file.sha256.$$
	[[ ! -e $sidecar && ! -L $sidecar ]] || die "no-clobber:${sidecar##*/}"
	(
		cd "${file%/*}"
		sha256sum "${file##*/}"
	) >"$temporary"
	chmod 0444 "$temporary"
	ln -- "$temporary" "$sidecar" || die "no-clobber:${sidecar##*/}"
	rm -f -- "$temporary"
}

verify_checksum()
{
	local file=$1 sidecar expected_name
	sidecar=$file.sha256
	expected_name=${file##*/}
	[[ -f $file && ! -L $file && -f $sidecar && ! -L $sidecar ]] || \
		die "receipt-missing:$expected_name"
	[[ $(stat -c %a "$file") == 444 && $(stat -c %a "$sidecar") == 444 ]] || \
		die "receipt-not-immutable:$expected_name"
	(
		cd "${file%/*}"
		sha256sum -c "${sidecar##*/}" >/dev/null
	) || die "receipt-sha-mismatch:$expected_name"
	[[ $(awk '{print $2}' "$sidecar") == "$expected_name" ]] || \
		die "receipt-name-mismatch:$expected_name"
}

profile_file()
{
	local profile_dir=$1 relative=$2 resolved
	valid_relative_path "$relative" || die invalid-profile-relative-path
	resolved=$(realpath -e -- "$profile_dir/$relative") || die profile-input-missing
	[[ $resolved == "$profile_dir"/* && -f $resolved && ! -L $resolved ]] || \
		die profile-input-outside-profile
	printf '%s' "$resolved"
}

validate_profile()
{
	local profile=$1 resolved spec change_file prepare_file
	[[ $profile != latest && $profile =~ ^[a-z0-9][a-z0-9._-]*$ ]] || \
		die invalid-profile-id
	resolved=$(realpath -e -- "$profile_root/$profile") || die profile-not-found
	[[ ${resolved%/*} == "$profile_root" && ${resolved##*/} == "$profile" && \
		-d $resolved && ! -L $resolved ]] || die profile-outside-profile-root
	[[ -z $(find "$resolved" -type l -print -quit) ]] || die profile-symlink-forbidden
	spec=$resolved/SPEC
	read_kv "$spec" schema profile_id base_p8_sha256 observable_variable_id \
		change_set change_set_sha256 prepare_script prepare_script_sha256
	[[ ${KV[schema]} == rg40xxv-p8-profile-v1 && ${KV[profile_id]} == "$profile" ]] || \
		die invalid-profile-spec
	valid_sha "${KV[base_p8_sha256]}" || die invalid-base-p8-sha256
	[[ ${KV[base_p8_sha256]} == "$frozen_recovery_sha" ]] || \
		die profile-base-must-be-frozen-v9
	[[ ${KV[observable_variable_id]} =~ ^[a-z0-9][a-z0-9._-]*$ ]] || \
		die invalid-observable-variable-id
	valid_sha "${KV[change_set_sha256]}" || die invalid-change-set-sha256
	valid_sha "${KV[prepare_script_sha256]}" || die invalid-prepare-script-sha256
	PROFILE_BASE_SHA=${KV[base_p8_sha256]}
	PROFILE_VARIABLE=${KV[observable_variable_id]}
	PROFILE_CHANGE_REL=${KV[change_set]}
	PROFILE_CHANGE_SHA=${KV[change_set_sha256]}
	PROFILE_SCRIPT_REL=${KV[prepare_script]}
	PROFILE_SCRIPT_SHA=${KV[prepare_script_sha256]}
	[[ $PROFILE_CHANGE_REL != "$PROFILE_SCRIPT_REL" ]] || die profile-input-role-collision
	change_file=$(profile_file "$resolved" "$PROFILE_CHANGE_REL")
	prepare_file=$(profile_file "$resolved" "$PROFILE_SCRIPT_REL")
	[[ -x $prepare_file ]] || die prepare-script-not-executable
	[[ $(sha256sum "$change_file" | awk '{print $1}') == "$PROFILE_CHANGE_SHA" ]] || \
		die change-set-sha256-mismatch
	[[ $(sha256sum "$prepare_file" | awk '{print $1}') == "$PROFILE_SCRIPT_SHA" ]] || \
		die prepare-script-sha256-mismatch
	PROFILE_ID=$profile
	PROFILE_DIR=$resolved
	PROFILE_SPEC=$spec
	PROFILE_SPEC_SHA=$(sha256sum "$spec" | awk '{print $1}')
	PROFILE_CHANGE_FILE=$change_file
	PROFILE_SCRIPT_FILE=$prepare_file
}

verify_artifact()
{
	local run=$1 relative=$2 expected_sha=$3 file resolved
	valid_relative_path "$relative" || die invalid-artifact-path
	file=$run/$relative
	resolved=$(realpath -e -- "$file") || die "artifact-missing:$relative"
	[[ $resolved == "$run"/* && -f $resolved && ! -L $resolved ]] || \
		die "artifact-outside-run:$relative"
	[[ $(sha256sum "$resolved" | awk '{print $1}') == "$expected_sha" ]] || \
		die "artifact-sha256-mismatch:$relative"
}

validate_staged()
{
	local run=$1 actual_count
	read_kv "$run/STAGED.env" schema image image_sha256 dtb dtb_sha256 \
		initramfs initramfs_sha256 selector selector_sha256 p8 p8_sha256 \
		p8_bytes reproducible_builds host_gate device_boot
	[[ ${KV[schema]} == rg40xxv-p8-staged-v1 && \
		${KV[image]} == artifact/Image && ${KV[dtb]} == artifact/board.dtb && \
		${KV[initramfs]} == artifact/initramfs_data.cpio && \
		${KV[selector]} == artifact/rg40xxv-boot-selector && \
		${KV[p8]} =~ ^artifact/([0-9a-f]{64})\.img$ && \
		${BASH_REMATCH[1]} == "${KV[p8_sha256]}" && \
		${KV[p8_bytes]} == "$p8_bytes" && \
		${KV[reproducible_builds]} == PASS && ${KV[host_gate]} == PASS && \
		${KV[device_boot]} == NOT_TESTED ]] || die invalid-staged-result
	for value in "${KV[image_sha256]}" "${KV[dtb_sha256]}" \
		"${KV[initramfs_sha256]}" "${KV[selector_sha256]}" "${KV[p8_sha256]}"; do
		valid_sha "$value" || die invalid-staged-sha256
	done
	[[ -d $run/artifact && ! -L $run/artifact && \
		-z $(find "$run/artifact" -type l -print -quit) ]] || die invalid-artifact-tree
	actual_count=$(find "$run/artifact" -type f -printf x | wc -c)
	[[ $actual_count == 5 ]] || die unexpected-artifact-count
	verify_artifact "$run" "${KV[image]}" "${KV[image_sha256]}"
	verify_artifact "$run" "${KV[dtb]}" "${KV[dtb_sha256]}"
	verify_artifact "$run" "${KV[initramfs]}" "${KV[initramfs_sha256]}"
	verify_artifact "$run" "${KV[selector]}" "${KV[selector_sha256]}"
	verify_artifact "$run" "${KV[p8]}" "${KV[p8_sha256]}"
	[[ $(stat -c %s "$run/${KV[p8]}") == "$p8_bytes" ]] || die p8-size-mismatch
	STAGED_IMAGE=${KV[image]}; STAGED_IMAGE_SHA=${KV[image_sha256]}
	STAGED_DTB=${KV[dtb]}; STAGED_DTB_SHA=${KV[dtb_sha256]}
	STAGED_INITRAMFS=${KV[initramfs]}; STAGED_INITRAMFS_SHA=${KV[initramfs_sha256]}
	STAGED_SELECTOR=${KV[selector]}; STAGED_SELECTOR_SHA=${KV[selector_sha256]}
	STAGED_P8=${KV[p8]}; STAGED_P8_SHA=${KV[p8_sha256]}
	STAGED_IMAGE_BYTES=$(stat -c %s "$run/$STAGED_IMAGE")
	STAGED_DTB_BYTES=$(stat -c %s "$run/$STAGED_DTB")
	STAGED_INITRAMFS_BYTES=$(stat -c %s "$run/$STAGED_INITRAMFS")
	STAGED_SELECTOR_BYTES=$(stat -c %s "$run/$STAGED_SELECTOR")
}

write_components()
{
	local run=$1 file temporary
	file=$run/COMPONENTS.sha256
	temporary=$run/.COMPONENTS.sha256.$$
	[[ ! -e $file && ! -L $file ]] || die no-clobber:COMPONENTS.sha256
	{
		printf '%s  %s\n' "$STAGED_IMAGE_SHA" "$STAGED_IMAGE"
		printf '%s  %s\n' "$STAGED_DTB_SHA" "$STAGED_DTB"
		printf '%s  %s\n' "$STAGED_INITRAMFS_SHA" "$STAGED_INITRAMFS"
		printf '%s  %s\n' "$STAGED_SELECTOR_SHA" "$STAGED_SELECTOR"
		printf '%s  %s\n' "$STAGED_P8_SHA" "$STAGED_P8"
	} >"$temporary"
	chmod 0444 "$temporary"
	ln -- "$temporary" "$file" || die no-clobber:COMPONENTS.sha256
	rm -f -- "$temporary"
}

write_build_lock()
{
	local run=$1 file temporary
	file=$run/BUILD-LOCK.env
	temporary=$run/.BUILD-LOCK.env.$$
	[[ ! -e $file && ! -L $file && ! -e $file.sha256 && ! -L $file.sha256 ]] || \
		die no-clobber:BUILD-LOCK.env
	{
		printf 'schema=rg40xxv-p8-build-lock-v1\n'
		printf 'run_id=%s\n' "${run##*/}"
		printf 'profile_id=%s\n' "$PROFILE_ID"
		printf 'base_p8_sha256=%s\n' "$PROFILE_BASE_SHA"
		printf 'observable_variable_id=%s\n' "$PROFILE_VARIABLE"
		printf 'spec_sha256=%s\n' "$PROFILE_SPEC_SHA"
		printf 'change_set_sha256=%s\n' "$PROFILE_CHANGE_SHA"
		printf 'prepare_script_sha256=%s\n' "$PROFILE_SCRIPT_SHA"
		printf 'image=%s\nimage_sha256=%s\nimage_bytes=%s\n' \
			"$STAGED_IMAGE" "$STAGED_IMAGE_SHA" "$STAGED_IMAGE_BYTES"
		printf 'dtb=%s\ndtb_sha256=%s\ndtb_bytes=%s\n' \
			"$STAGED_DTB" "$STAGED_DTB_SHA" "$STAGED_DTB_BYTES"
		printf 'initramfs=%s\ninitramfs_sha256=%s\ninitramfs_bytes=%s\n' \
			"$STAGED_INITRAMFS" "$STAGED_INITRAMFS_SHA" "$STAGED_INITRAMFS_BYTES"
		printf 'selector=%s\nselector_sha256=%s\nselector_bytes=%s\n' \
			"$STAGED_SELECTOR" "$STAGED_SELECTOR_SHA" "$STAGED_SELECTOR_BYTES"
		printf 'p8=%s\np8_sha256=%s\np8_bytes=%s\n' \
			"$STAGED_P8" "$STAGED_P8_SHA" "$p8_bytes"
		printf 'reproducible_builds=PASS\nhost_gate=PASS\ndevice_boot=NOT_TESTED\n'
	} >"$temporary"
	chmod 0444 "$temporary"
	ln -- "$temporary" "$file" || die no-clobber:BUILD-LOCK.env
	rm -f -- "$temporary"
	write_checksum "$file"
}

verify_build_lock()
{
	local run=$1 expected_components p8_name profile_id spec_sha change_sha script_sha
	verify_checksum "$run/BUILD-LOCK.env"
	read_kv "$run/BUILD-LOCK.env" schema run_id profile_id base_p8_sha256 \
		observable_variable_id spec_sha256 change_set_sha256 prepare_script_sha256 \
		image image_sha256 image_bytes dtb dtb_sha256 dtb_bytes initramfs \
		initramfs_sha256 initramfs_bytes selector selector_sha256 selector_bytes \
		p8 p8_sha256 p8_bytes reproducible_builds host_gate device_boot
	[[ ${KV[schema]} == rg40xxv-p8-build-lock-v1 && \
		${KV[run_id]} == "${run##*/}" && ${KV[p8_bytes]} == "$p8_bytes" && \
		${KV[reproducible_builds]} == PASS && ${KV[host_gate]} == PASS && \
		${KV[device_boot]} == NOT_TESTED ]] || die invalid-build-lock
	for key in base_p8_sha256 spec_sha256 change_set_sha256 prepare_script_sha256 \
		image_sha256 dtb_sha256 initramfs_sha256 selector_sha256 p8_sha256; do
		valid_sha "${KV[$key]}" || die "invalid-build-lock-sha:$key"
	done
	[[ ${KV[image]} == artifact/Image && ${KV[dtb]} == artifact/board.dtb && \
		${KV[initramfs]} == artifact/initramfs_data.cpio && \
		${KV[selector]} == artifact/rg40xxv-boot-selector && \
		${KV[p8]} == artifact/${KV[p8_sha256]}.img ]] || die invalid-build-lock-path
	for key in image_bytes dtb_bytes initramfs_bytes selector_bytes p8_bytes; do
		[[ ${KV[$key]} =~ ^[1-9][0-9]*$ ]] || die "invalid-build-lock-bytes:$key"
	done
	[[ -f $run/COMPONENTS.sha256 && ! -L $run/COMPONENTS.sha256 && \
		$(stat -c %a "$run/COMPONENTS.sha256") == 444 ]] || \
		die components-manifest-invalid
	expected_components=$(printf '%s  %s\n%s  %s\n%s  %s\n%s  %s\n%s  %s\n' \
		"${KV[image_sha256]}" "${KV[image]}" "${KV[dtb_sha256]}" "${KV[dtb]}" \
		"${KV[initramfs_sha256]}" "${KV[initramfs]}" \
		"${KV[selector_sha256]}" "${KV[selector]}" "${KV[p8_sha256]}" "${KV[p8]}")
	[[ $(<"$run/COMPONENTS.sha256") == "$expected_components" ]] || \
		die components-manifest-content
	(cd "$run" && sha256sum -c COMPONENTS.sha256 >/dev/null) || \
		die components-sha256-mismatch
	[[ $(stat -c %s "$run/${KV[image]}") == "${KV[image_bytes]}" && \
		$(stat -c %s "$run/${KV[dtb]}") == "${KV[dtb_bytes]}" && \
		$(stat -c %s "$run/${KV[initramfs]}") == "${KV[initramfs_bytes]}" && \
		$(stat -c %s "$run/${KV[selector]}") == "${KV[selector_bytes]}" && \
		$(stat -c %s "$run/${KV[p8]}") == "$p8_bytes" ]] || die component-size-mismatch
	LOCK_P8=${KV[p8]}
	LOCK_P8_SHA=${KV[p8_sha256]}
	LOCK_BUILD_SHA=$(awk '{print $1}' "$run/BUILD-LOCK.env.sha256")
	profile_id=${KV[profile_id]}; spec_sha=${KV[spec_sha256]}
	change_sha=${KV[change_set_sha256]}; script_sha=${KV[prepare_script_sha256]}
	p8_name=${LOCK_P8##*/}
	[[ $p8_name == "$LOCK_P8_SHA.img" ]] || die p8-name-sha-mismatch
	[[ -f $run/profile/SPEC && ! -L $run/profile/SPEC && \
		$(stat -c %a "$run/profile/SPEC") == 444 && \
		$(sha256sum "$run/profile/SPEC" | awk '{print $1}') == "$spec_sha" ]] || \
		die profile-spec-snapshot-mismatch
	read_kv "$run/profile/SPEC" schema profile_id base_p8_sha256 observable_variable_id \
		change_set change_set_sha256 prepare_script prepare_script_sha256
	[[ ${KV[schema]} == rg40xxv-p8-profile-v1 && ${KV[profile_id]} == "$profile_id" && \
		${KV[change_set_sha256]} == "$change_sha" && \
		${KV[prepare_script_sha256]} == "$script_sha" ]] || die profile-build-lock-mismatch
	verify_artifact "$run/profile" "${KV[change_set]}" "$change_sha"
	verify_artifact "$run/profile" "${KV[prepare_script]}" "$script_sha"
}

snapshot_profile()
{
	local run=$1 destination source relative
	mkdir -m 0755 -p "$run/profile"
	for relative in SPEC "$PROFILE_CHANGE_REL" "$PROFILE_SCRIPT_REL"; do
		case $relative in
		SPEC) source=$PROFILE_SPEC ;;
		"$PROFILE_CHANGE_REL") source=$PROFILE_CHANGE_FILE ;;
		"$PROFILE_SCRIPT_REL") source=$PROFILE_SCRIPT_FILE ;;
		esac
		destination=$run/profile/$relative
		mkdir -m 0755 -p "${destination%/*}"
		[[ ! -e $destination && ! -L $destination ]] || die profile-snapshot-collision
		cp -- "$source" "$destination"
	done
	chmod 0444 "$run/profile/SPEC" "$run/profile/$PROFILE_CHANGE_REL"
	chmod 0555 "$run/profile/$PROFILE_SCRIPT_REL"
	[[ $(sha256sum "$run/profile/SPEC" | awk '{print $1}') == "$PROFILE_SPEC_SHA" && \
		$(sha256sum "$run/profile/$PROFILE_CHANGE_REL" | awk '{print $1}') == \
			"$PROFILE_CHANGE_SHA" && \
		$(sha256sum "$run/profile/$PROFILE_SCRIPT_REL" | awk '{print $1}') == \
			"$PROFILE_SCRIPT_SHA" ]] || die profile-snapshot-sha256-mismatch
}

prepare_action()
{
	local profile=$1 run_id run rc
	require_operation_lock
	[[ -d $profile_root && ! -L $profile_root ]] || die profile-root-missing
	validate_profile "$profile"
	mkdir -p "$report_root"
	run_id=$(date '+%Y%m%dT%H%M%S%z')-$$
	run=$report_root/$run_id
	[[ ! -e $run && ! -L $run ]] || die run-id-collision
	mkdir -m 0755 "$run"
	mkdir -m 0755 "$run/logs" "$run/observations"
	active_prepare=1
	active_run=$run
	write_state "$run" PREPARING
	snapshot_profile "$run"
	printf 'P8_CYCLE action=prepare status=START run_id=%s profile=%s device_write=NONE\n' \
		"$run_id" "$profile"
	set +e
	(
		cd "$run/profile"
		"$run/profile/$PROFILE_SCRIPT_REL" "$run" "$run/profile/SPEC"
	) 2>&1 | tee "$run/logs/prepare.log"
	rc=${PIPESTATUS[0]}
	set -e
	[[ $rc == 0 ]] || die "prepare-script-failed:$rc"
	[[ $(sha256sum "$PROFILE_SPEC" | awk '{print $1}') == "$PROFILE_SPEC_SHA" && \
		$(sha256sum "$PROFILE_CHANGE_FILE" | awk '{print $1}') == "$PROFILE_CHANGE_SHA" && \
		$(sha256sum "$PROFILE_SCRIPT_FILE" | awk '{print $1}') == "$PROFILE_SCRIPT_SHA" ]] || \
		die profile-mutated-during-prepare
	[[ ! -e $run/BUILD-LOCK.env && ! -L $run/BUILD-LOCK.env && \
		! -e $run/COMPONENTS.sha256 && ! -L $run/COMPONENTS.sha256 && \
		! -e $run/FLASH-AUTHORIZATION.env && ! -L $run/FLASH-AUTHORIZATION.env ]] || \
		die prepare-script-protected-output
	require_state "$run" PREPARING
	validate_staged "$run"
	chmod 0444 "$run/STAGED.env" "$run/artifact/"*
	write_components "$run"
	write_build_lock "$run"
	verify_build_lock "$run"
	write_state "$run" PACKAGED_HOST_PASS
	active_prepare=0
	printf 'P8_CYCLE result=PASS action=prepare run_id=%s state=PACKAGED_HOST_PASS p8_sha256=%s device_boot=NOT_TESTED device_write=NONE p7_write=NONE\n' \
		"$run_id" "$LOCK_P8_SHA"
}

validate_identify_receipt()
{
	local relative=$1 expected_sha=$2 expected_device=$3 expected_outgoing=$4
	local receipt resolved
	valid_relative_path "$relative" || die invalid-identify-receipt-path
	[[ $relative == reports/* ]] || die identify-receipt-outside-reports
	receipt=$workspace/$relative
	resolved=$(realpath -e -- "$receipt") || die identify-receipt-missing
	[[ $resolved == "$workspace/reports/"* && -f $resolved && ! -L $resolved && \
		$(stat -c %a "$resolved") == 444 ]] || die identify-receipt-invalid
	valid_sha "$expected_sha" || die invalid-identify-receipt-sha256
	[[ $(sha256sum "$resolved" | awk '{print $1}') == "$expected_sha" ]] || \
		die identify-receipt-sha256-mismatch
	read_kv "$resolved" schema created_utc device disk_kernel_name disk_node_id \
		disk_device_number disk_sysfs_path diskseq disk_bytes logical_block_bytes \
		disk_read_only gpt_guid \
		gpt_dump_sha256 partition_count p1_offset p1_bytes p2_offset \
		p2_bytes p3_offset p3_bytes p4_offset p4_bytes p5_offset p5_bytes \
		p6_offset p6_bytes p7_offset p7_bytes p8_offset p8_bytes \
		all_partitions_unmounted p4_sha256 p8_sha256 p8_registry_outgoing_policy \
		p8_registry_target_policy p8_registry_identity image_registry_sha256 \
		device_write p7_write
	[[ ${KV[schema]} == rg40xxv-p8-identify-receipt-v2 && \
		${KV[device]} == "$expected_device" && ${KV[disk_kernel_name]} == "${expected_device##*/}" && \
		${KV[disk_node_id]} =~ ^[0-9a-f]+:[0-9a-f]+$ && \
		${KV[disk_device_number]} =~ ^[0-9]+:[0-9]+$ && \
		${KV[disk_sysfs_path]} == /sys/devices/* && \
		${KV[diskseq]} =~ ^[0-9]+$ && \
		${KV[disk_bytes]} == "$tf1_bytes" && ${KV[logical_block_bytes]} == 512 && \
		${KV[disk_read_only]} == 1 && \
		${KV[gpt_guid]} == "$tf1_guid" && ${KV[partition_count]} == 8 && \
		${KV[p4_sha256]} == "$stock_p4_sha" && ${KV[p8_sha256]} == "$expected_outgoing" && \
		${KV[p8_registry_outgoing_policy]} == KNOWN && \
		${KV[all_partitions_unmounted]} == PASS && ${KV[device_write]} == NONE && \
		${KV[p7_write]} == NONE ]] || die identify-receipt-identity-mismatch
	[[ ${KV[p1_offset]} == 37748736 && ${KV[p1_bytes]} == 47244640256 && \
		${KV[p2_offset]} == 47282388992 && ${KV[p2_bytes]} == 33554432 && \
		${KV[p3_offset]} == 47315943424 && ${KV[p3_bytes]} == 16777216 && \
		${KV[p4_offset]} == 47332720640 && ${KV[p4_bytes]} == 67108864 && \
		${KV[p5_offset]} == 47399829504 && ${KV[p5_bytes]} == 7516192768 && \
		${KV[p6_offset]} == 54916022272 && ${KV[p6_bytes]} == 4294967296 && \
		${KV[p7_offset]} == 59210989568 && ${KV[p7_bytes]} == 2679111680 && \
		${KV[p8_offset]} == 61890101248 && ${KV[p8_bytes]} == "$p8_bytes" ]] || \
		die identify-receipt-layout-mismatch
	valid_sha "${KV[p8_sha256]}" || die invalid-outgoing-p8-sha256
	valid_sha "${KV[gpt_dump_sha256]}" || die invalid-gpt-dump-sha256
	valid_sha "${KV[image_registry_sha256]}" || die invalid-image-registry-sha256
	RECEIPT_DISK_NODE_ID=${KV[disk_node_id]}
	RECEIPT_DISK_DEVICE_NUMBER=${KV[disk_device_number]}
	RECEIPT_DISK_SYSFS_PATH=${KV[disk_sysfs_path]}
	RECEIPT_DISKSEQ=${KV[diskseq]}
}

validate_flash_manifest()
{
	local run=$1 manifest device receipt receipt_sha outgoing disk_node_id
	local disk_device_number disk_sysfs_path diskseq
	manifest=$run/FLASH-MANIFEST.env
	read_kv "$manifest" schema run_id action build_lock_sha256 p8 p8_sha256 \
		p8_bytes device disk_node_id disk_device_number disk_sysfs_path diskseq \
		identify_receipt identify_receipt_sha256 outgoing_p8_sha256 p7_write device_write
	device=${KV[device]}; receipt=${KV[identify_receipt]}
	receipt_sha=${KV[identify_receipt_sha256]}; outgoing=${KV[outgoing_p8_sha256]}
	disk_node_id=${KV[disk_node_id]}; disk_device_number=${KV[disk_device_number]}
	disk_sysfs_path=${KV[disk_sysfs_path]}; diskseq=${KV[diskseq]}
	[[ ${KV[schema]} == rg40xxv-p8-flash-manifest-v2 && \
		${KV[run_id]} == "${run##*/}" && ${KV[action]} == FLASH_REQUESTED && \
		${KV[build_lock_sha256]} == "$LOCK_BUILD_SHA" && \
		${KV[p8]} == "$LOCK_P8" && ${KV[p8_sha256]} == "$LOCK_P8_SHA" && \
		${KV[p8_bytes]} == "$p8_bytes" && ${KV[p7_write]} == NONE && \
		${KV[device_write]} == NOT_PERFORMED ]] || die invalid-flash-manifest
	[[ $device =~ ^/dev/sd[a-z]$ ]] || die invalid-flash-manifest-device
	valid_sha "$outgoing" || die invalid-outgoing-p8-sha256
	validate_identify_receipt "$receipt" "$receipt_sha" "$device" "$outgoing"
	[[ $disk_node_id == "$RECEIPT_DISK_NODE_ID" && \
		$disk_device_number == "$RECEIPT_DISK_DEVICE_NUMBER" && \
		$disk_sysfs_path == "$RECEIPT_DISK_SYSFS_PATH" && \
		$diskseq == "$RECEIPT_DISKSEQ" ]] || die flash-manifest-device-binding-mismatch
	BOUND_DEVICE=$device
	BOUND_IDENTIFY_RECEIPT=$receipt
	BOUND_IDENTIFY_SHA=$receipt_sha
	BOUND_OUTGOING_SHA=$outgoing
	BOUND_DISK_NODE_ID=$disk_node_id
	BOUND_DISK_DEVICE_NUMBER=$disk_device_number
	BOUND_DISK_SYSFS_PATH=$disk_sysfs_path
	BOUND_DISKSEQ=$diskseq
	[[ $LOCK_P8_SHA != "$banned_p8_sha" ]] || die permanently-failed-p8-forbidden
}

verify_flash_authorization()
{
	local run=$1 manifest_sha disk_node_id disk_device_number disk_sysfs_path diskseq
	verify_checksum "$run/FLASH-MANIFEST.env"
	verify_checksum "$run/FLASH-AUTHORIZATION.env"
	manifest_sha=$(awk '{print $1}' "$run/FLASH-MANIFEST.env.sha256")
	read_kv "$run/FLASH-AUTHORIZATION.env" schema run_id build_lock_sha256 \
		flash_manifest_sha256 p8 p8_sha256 p8_bytes device disk_node_id \
		disk_device_number disk_sysfs_path diskseq identify_receipt \
		identify_receipt_sha256 outgoing_p8_sha256 authorization device_write p7_write
	disk_node_id=${KV[disk_node_id]}; disk_device_number=${KV[disk_device_number]}
	disk_sysfs_path=${KV[disk_sysfs_path]}; diskseq=${KV[diskseq]}
	[[ ${KV[schema]} == rg40xxv-p8-flash-authorization-v2 && \
		${KV[run_id]} == "${run##*/}" && ${KV[build_lock_sha256]} == "$LOCK_BUILD_SHA" && \
		${KV[flash_manifest_sha256]} == "$manifest_sha" && ${KV[p8]} == "$LOCK_P8" && \
		${KV[p8_sha256]} == "$LOCK_P8_SHA" && ${KV[p8_bytes]} == "$p8_bytes" && \
		${KV[authorization]} == HOST_PREFLIGHT_ONLY && ${KV[device_write]} == NONE && \
		${KV[p7_write]} == NONE ]] || die invalid-flash-authorization
	BOUND_DEVICE=${KV[device]}; BOUND_IDENTIFY_RECEIPT=${KV[identify_receipt]}
	BOUND_IDENTIFY_SHA=${KV[identify_receipt_sha256]}
	BOUND_OUTGOING_SHA=${KV[outgoing_p8_sha256]}
	[[ $BOUND_DEVICE =~ ^/dev/sd[a-z]$ ]] || die invalid-flash-authorization-device
	valid_sha "$BOUND_OUTGOING_SHA" || die invalid-outgoing-p8-sha256
	validate_identify_receipt "$BOUND_IDENTIFY_RECEIPT" "$BOUND_IDENTIFY_SHA" \
		"$BOUND_DEVICE" "$BOUND_OUTGOING_SHA"
	[[ $disk_node_id == "$RECEIPT_DISK_NODE_ID" && \
		$disk_device_number == "$RECEIPT_DISK_DEVICE_NUMBER" && \
		$disk_sysfs_path == "$RECEIPT_DISK_SYSFS_PATH" && \
		$diskseq == "$RECEIPT_DISKSEQ" ]] || die flash-authorization-device-binding-mismatch
	BOUND_DISK_NODE_ID=$disk_node_id
	BOUND_DISK_DEVICE_NUMBER=$disk_device_number
	BOUND_DISK_SYSFS_PATH=$disk_sysfs_path
	BOUND_DISKSEQ=$diskseq
	AUTH_DEVICE=$BOUND_DEVICE
	AUTH_OUTGOING_SHA=$BOUND_OUTGOING_SHA
}

flash_preflight_action()
{
	local run=$1 manifest_sha file temporary
	require_operation_lock
	require_state "$run" PACKAGED_HOST_PASS
	verify_build_lock "$run"
	[[ ! -e $run/FLASH-MANIFEST.env.sha256 && ! -L $run/FLASH-MANIFEST.env.sha256 && \
		! -e $run/FLASH-AUTHORIZATION.env && ! -L $run/FLASH-AUTHORIZATION.env ]] || \
		die no-clobber:flash-authorization
	validate_flash_manifest "$run"
	chmod 0444 "$run/FLASH-MANIFEST.env"
	write_checksum "$run/FLASH-MANIFEST.env"
	manifest_sha=$(awk '{print $1}' "$run/FLASH-MANIFEST.env.sha256")
	file=$run/FLASH-AUTHORIZATION.env
	temporary=$run/.FLASH-AUTHORIZATION.env.$$
	{
		printf 'schema=rg40xxv-p8-flash-authorization-v2\n'
		printf 'run_id=%s\n' "${run##*/}"
		printf 'build_lock_sha256=%s\n' "$LOCK_BUILD_SHA"
		printf 'flash_manifest_sha256=%s\n' "$manifest_sha"
		printf 'p8=%s\np8_sha256=%s\np8_bytes=%s\n' "$LOCK_P8" "$LOCK_P8_SHA" "$p8_bytes"
		printf 'device=%s\nidentify_receipt=%s\nidentify_receipt_sha256=%s\n' \
			"$BOUND_DEVICE" "$BOUND_IDENTIFY_RECEIPT" "$BOUND_IDENTIFY_SHA"
		printf 'disk_node_id=%s\ndisk_device_number=%s\n' \
			"$BOUND_DISK_NODE_ID" "$BOUND_DISK_DEVICE_NUMBER"
		printf 'disk_sysfs_path=%s\ndiskseq=%s\n' \
			"$BOUND_DISK_SYSFS_PATH" "$BOUND_DISKSEQ"
		printf 'outgoing_p8_sha256=%s\n' "$BOUND_OUTGOING_SHA"
		# This authorizes only the pinned host manifest for guarded integration;
		# it is neither a device write nor the user's current permission to write.
		printf 'authorization=HOST_PREFLIGHT_ONLY\ndevice_write=NONE\np7_write=NONE\n'
	} >"$temporary"
	chmod 0444 "$temporary"
	ln -- "$temporary" "$file" || die no-clobber:FLASH-AUTHORIZATION.env
	rm -f -- "$temporary"
	write_checksum "$file"
	verify_flash_authorization "$run"
	write_state "$run" FLASH_AUTHORIZED
	printf 'P8_CYCLE result=PASS action=flash-preflight run_id=%s state=FLASH_AUTHORIZED device=%s outgoing_p8_sha256=%s p8_sha256=%s device_write=NONE p7_write=NONE\n' \
		"${run##*/}" "$BOUND_DEVICE" "$BOUND_OUTGOING_SHA" "$LOCK_P8_SHA"
}

verify_flash_result()
{
	local run=$1
	verify_checksum "$run/FLASH-RESULT.env"
	read_kv "$run/FLASH-RESULT.env" schema run_id device disk_node_id \
		disk_device_number disk_sysfs_path diskseq outgoing_p8_sha256 p8_sha256 \
		readback_sha256 p8_bytes result write_mode p4 p7_write device_write
	[[ ${KV[schema]} == rg40xxv-p8-flash-result-v2 && \
		${KV[run_id]} == "${run##*/}" && ${KV[device]} == "$AUTH_DEVICE" && \
		${KV[disk_node_id]} == "$BOUND_DISK_NODE_ID" && \
		${KV[disk_device_number]} == "$BOUND_DISK_DEVICE_NUMBER" && \
		${KV[disk_sysfs_path]} == "$BOUND_DISK_SYSFS_PATH" && \
		${KV[diskseq]} == "$BOUND_DISKSEQ" && \
		${KV[outgoing_p8_sha256]} == "$AUTH_OUTGOING_SHA" && \
		${KV[p8_sha256]} == "$LOCK_P8_SHA" && \
		${KV[readback_sha256]} == "$LOCK_P8_SHA" && ${KV[p8_bytes]} == "$p8_bytes" && \
		${KV[result]} == PASS && ${KV[p4]} == UNCHANGED && ${KV[p7_write]} == NONE ]] || \
		die invalid-flash-result
	case ${KV[write_mode]}:${KV[device_write]} in
	P8_ONLY:P8_ONLY|ALREADY_COMPLETE:NONE) ;;
	*) die invalid-flash-result-write-mode ;;
	esac
}

flash_complete_action()
{
	local run=$1
	require_operation_lock
	require_state "$run" FLASH_AUTHORIZED
	verify_build_lock "$run"
	verify_flash_authorization "$run"
	verify_flash_result "$run"
	write_state "$run" FLASHED_READBACK_PASS
	printf 'P8_CYCLE result=PASS action=flash-complete run_id=%s state=FLASHED_READBACK_PASS p8_sha256=%s readback_sha256=%s p4=UNCHANGED p7_write=NONE\n' \
		"${run##*/}" "$LOCK_P8_SHA" "$LOCK_P8_SHA"
}

step_number()
{
	case $1 in
	first-cold) printf 1 ;; warm-1) printf 2 ;; warm-2) printf 3 ;;
	warm-3) printf 4 ;; cold-1) printf 5 ;; cold-2) printf 6 ;; cold-3) printf 7 ;;
	*) return 1 ;;
	esac
}

expected_step()
{
	case $1 in
	FLASHED_READBACK_PASS) printf first-cold ;;
	OBSERVED_FIRST_COLD_PASS) printf warm-1 ;;
	OBSERVED_WARM_1_PASS) printf warm-2 ;;
	OBSERVED_WARM_2_PASS) printf warm-3 ;;
	OBSERVED_WARM_3_PASS) printf cold-1 ;;
	OBSERVED_COLD_1_PASS) printf cold-2 ;;
	OBSERVED_COLD_2_PASS) printf cold-3 ;;
	*) return 1 ;;
	esac
}

pass_state()
{
	case $1 in
	first-cold) printf OBSERVED_FIRST_COLD_PASS ;;
	warm-1) printf OBSERVED_WARM_1_PASS ;; warm-2) printf OBSERVED_WARM_2_PASS ;;
	warm-3) printf OBSERVED_WARM_3_PASS ;; cold-1) printf OBSERVED_COLD_1_PASS ;;
	cold-2) printf OBSERVED_COLD_2_PASS ;; cold-3) printf ACCEPTED ;;
	*) return 1 ;;
	esac
}

verify_observations()
{
	local run=$1 state=$2 current=FLASHED_READBACK_PASS step number file result next
	local seen=0 actual failure_step=
	local -a steps=(first-cold warm-1 warm-2 warm-3 cold-1 cold-2 cold-3)
	for step in "${steps[@]}"; do
		number=$(step_number "$step")
		printf -v file '%s/observations/%02d-%s.env' "$run" "$number" "$step"
		if [[ ! -e $file && ! -L $file ]]; then
			break
		fi
		verify_checksum "$file"
		read_kv "$file" schema run_id sequence step p8_sha256 result recorded_taipei
		[[ ${KV[schema]} == rg40xxv-p8-observation-v1 && \
			${KV[run_id]} == "${run##*/}" && ${KV[sequence]} == "$number" && \
			${KV[step]} == "$step" && ${KV[p8_sha256]} == "$LOCK_P8_SHA" && \
			${KV[result]} =~ ^(PASS|FAIL)$ ]] || die invalid-observation-record
		result=${KV[result]}
		seen=$((seen + 1))
		[[ $current != RECOVERY_REQUIRED ]] || die observation-after-failure
		if [[ $result == FAIL ]]; then
			current=RECOVERY_REQUIRED
			failure_step=$step
		else
			current=$(pass_state "$step")
		fi
	done
	actual=$(find "$run/observations" -maxdepth 1 -type f -name '*.env' -printf x | wc -c)
	[[ $actual == "$seen" ]] || die unexpected-observation-record
	[[ $current == "$state" ]] || die observation-state-mismatch
	if [[ $state == RECOVERY_REQUIRED ]]; then
		verify_checksum "$run/RECOVERY-REQUIRED.env"
		read_kv "$run/RECOVERY-REQUIRED.env" schema run_id failed_step \
			failed_p8_sha256 required_recovery_p8_sha256 recovery_required
		[[ ${KV[schema]} == rg40xxv-p8-recovery-required-v1 && \
			${KV[run_id]} == "${run##*/}" && ${KV[failed_step]} == "$failure_step" && \
			${KV[failed_p8_sha256]} == "$LOCK_P8_SHA" && \
			${KV[required_recovery_p8_sha256]} == "$frozen_recovery_sha" && \
			${KV[recovery_required]} == YES ]] || die invalid-recovery-required-receipt
	elif [[ $state == ACCEPTED ]]; then
		verify_checksum "$run/DEVICE-ACCEPTANCE.env"
		read_kv "$run/DEVICE-ACCEPTANCE.env" schema run_id p8_sha256 sequence device_boot
		[[ ${KV[schema]} == rg40xxv-p8-device-acceptance-v1 && \
			${KV[run_id]} == "${run##*/}" && ${KV[p8_sha256]} == "$LOCK_P8_SHA" && \
			${KV[sequence]} == first-cold,warm-1,warm-2,warm-3,cold-1,cold-2,cold-3 && \
			${KV[device_boot]} == PASS ]] || die invalid-device-acceptance-receipt
	fi
}

observe_action()
{
	local run=$1 step=$2 p8_sha=$3 result=$4 state expected number file temporary next
	require_operation_lock
	valid_sha "$p8_sha" || die invalid-observed-p8-sha256
	[[ $result =~ ^(PASS|FAIL)$ ]] || die invalid-observation-result
	state=$(read_state "$run")
	expected=$(expected_step "$state") || die "invalid-state-transition:$state:observe"
	[[ $step == "$expected" ]] || die "out-of-order-observation:$step:$expected"
	verify_build_lock "$run"
	verify_flash_authorization "$run"
	verify_flash_result "$run"
	verify_observations "$run" "$state"
	[[ $p8_sha == "$LOCK_P8_SHA" ]] || die observed-p8-sha256-mismatch
	number=$(step_number "$step") || die invalid-observation-step
	printf -v file '%s/observations/%02d-%s.env' "$run" "$number" "$step"
	[[ ! -e $file && ! -L $file && ! -e $file.sha256 && ! -L $file.sha256 ]] || \
		die no-clobber:observation
	temporary=$run/observations/.observation.$$
	{
		printf 'schema=rg40xxv-p8-observation-v1\n'
		printf 'run_id=%s\n' "${run##*/}"
		printf 'sequence=%s\nstep=%s\np8_sha256=%s\nresult=%s\n' \
			"$number" "$step" "$p8_sha" "$result"
		printf 'recorded_taipei=%s\n' "$(date '+%Y-%m-%dT%H:%M:%S%:z')"
	} >"$temporary"
	chmod 0444 "$temporary"
	ln -- "$temporary" "$file" || die no-clobber:observation
	rm -f -- "$temporary"
	write_checksum "$file"
	if [[ $result == FAIL ]]; then
		file=$run/RECOVERY-REQUIRED.env
		[[ ! -e $file && ! -L $file ]] || die no-clobber:RECOVERY-REQUIRED.env
		temporary=$run/.RECOVERY-REQUIRED.env.$$
		{
			printf 'schema=rg40xxv-p8-recovery-required-v1\nrun_id=%s\n' "${run##*/}"
			printf 'failed_step=%s\nfailed_p8_sha256=%s\n' "$step" "$p8_sha"
			printf 'required_recovery_p8_sha256=%s\nrecovery_required=YES\n' "$frozen_recovery_sha"
		} >"$temporary"
		chmod 0444 "$temporary"
		ln -- "$temporary" "$file" || die no-clobber:RECOVERY-REQUIRED.env
		rm -f -- "$temporary"
		write_checksum "$file"
		next=RECOVERY_REQUIRED
	else
		next=$(pass_state "$step")
		if [[ $next == ACCEPTED ]]; then
			file=$run/DEVICE-ACCEPTANCE.env
			[[ ! -e $file && ! -L $file ]] || die no-clobber:DEVICE-ACCEPTANCE.env
			temporary=$run/.DEVICE-ACCEPTANCE.env.$$
			{
				printf 'schema=rg40xxv-p8-device-acceptance-v1\nrun_id=%s\n' "${run##*/}"
				printf 'p8_sha256=%s\nsequence=first-cold,warm-1,warm-2,warm-3,cold-1,cold-2,cold-3\n' "$p8_sha"
				printf 'device_boot=PASS\n'
			} >"$temporary"
			chmod 0444 "$temporary"
			ln -- "$temporary" "$file" || die no-clobber:DEVICE-ACCEPTANCE.env
			rm -f -- "$temporary"
			write_checksum "$file"
		fi
	fi
	write_state "$run" "$next"
	printf 'P8_CYCLE result=PASS action=observe run_id=%s step=%s observation=%s state=%s p8_sha256=%s device_write=NONE p7_write=NONE\n' \
		"${run##*/}" "$step" "$result" "$next" "$p8_sha"
}

status_action()
{
	local run=$1 state next=NONE
	state=$(read_state "$run")
	case $state in
	PACKAGED_HOST_PASS|FLASH_AUTHORIZED|FLASHED_READBACK_PASS|OBSERVED_*|ACCEPTED|RECOVERY_REQUIRED)
		verify_build_lock "$run"
		;;
	esac
	case $state in
	FLASH_AUTHORIZED|FLASHED_READBACK_PASS|OBSERVED_*|ACCEPTED|RECOVERY_REQUIRED)
		verify_flash_authorization "$run"
		;;
	esac
	case $state in
	FLASHED_READBACK_PASS|OBSERVED_*|ACCEPTED|RECOVERY_REQUIRED)
		verify_flash_result "$run"
		verify_observations "$run" "$state"
		;;
	esac
	case $state in
	PACKAGED_HOST_PASS) next=flash-preflight ;;
	FLASH_AUTHORIZED) next=external-guarded-flash ;;
	FLASHED_READBACK_PASS|OBSERVED_*) next=$(expected_step "$state") ;;
	RECOVERY_REQUIRED) next=recover-frozen-v9 ;;
	esac
	printf 'P8_CYCLE result=PASS action=status run_id=%s state=%s next=%s device_write=NONE p7_write=NONE\n' \
		"${run##*/}" "$state" "$next"
}

case ${1:-} in
prepare)
	[[ $# == 2 ]] || usage
	prepare_action "$2"
	;;
flash-preflight)
	[[ $# == 2 ]] || usage
	run=$(resolve_run "$2")
	flash_preflight_action "$run"
	;;
flash-complete)
	[[ $# == 2 ]] || usage
	run=$(resolve_run "$2")
	flash_complete_action "$run"
	;;
observe)
	[[ $# == 5 ]] || usage
	run=$(resolve_run "$2")
	observe_action "$run" "$3" "$4" "$5"
	;;
status)
	[[ $# == 2 ]] || usage
	run=$(resolve_run "$2")
	status_action "$run"
	;;
-h|--help|help|'') usage ;;
*) die "unknown-action:${1:-missing}" ;;
esac
