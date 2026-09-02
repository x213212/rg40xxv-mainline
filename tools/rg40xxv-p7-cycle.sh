#!/usr/bin/env bash
set -euo pipefail

# Internal p7 state-machine.  Use tools/rg40xxv.sh as the public entry point.
# A cycle is the only producer of BUILD-LOCK.env and ARTIFACT.env.

script=$(realpath -- "$0")
workspace=$(CDPATH= cd -- "$(dirname -- "$script")/.." && pwd -P)
candidate=$workspace/lab/candidates/rg40xxv-next-v1-src
project=$workspace/lab/deploy/rg40xxv-next-v1
builder=$project/build-release.sh
report_root=$workspace/reports/p7-cycles
source_receipt_tool=$workspace/tools/rg40xxv-p7-source-receipt.sh
ui_runner=$workspace/tools/rg40xxv-p7-ui-pipeline.sh
component_runner=$workspace/tools/run-p7-component-host-gate.sh
deployer=$workspace/tools/deploy-rg40xxv-p7-artifact.sh
acceptance=$workspace/tools/rg40xxv-p7-device-acceptance.sh
frozen_p8_sha=6455c4d82d1594c03ab4f799c276da7249c6ffefd6fa22dd55b9077b1dfd4be3
frozen_p8=$workspace/lab/candidates/rg40xxv-panel-adopt-closeflow-v9-noquiesce-dram1v2/rg40xxv-panel-adopt-closeflow-v9-noquiesce-dram1v2-persistent-legacy-p8.img
device=${RG40XXV_DEVICE:-PUT-YOUR-OWN-DEVICE-HERE}
password=${RG40XXV_PASSWORD:-}

# These scripts talk to a handheld over SSH. The address and the password are
# whoever is running them, so there is no useful default: set RG40XXV_DEVICE and
# RG40XXV_PASSWORD. The refusal below is the guard working, not a bug.
case "$device" in
PUT-YOUR-OWN-DEVICE-HERE)
	printf '%s\n' 'set RG40XXV_DEVICE=user@host for your own device' >&2
	exit 2 ;;
esac
[ -n "$password" ] || { printf '%s\n' 'set RG40XXV_PASSWORD' >&2; exit 2; }
known_hosts=${RG40XXV_KNOWN_HOSTS:-$workspace/firmware/live/known_hosts}

cycle_lock_acquired=0
host_lock_acquired=0
active_run=
active_phase=none
last_error_reason=command-failed

usage()
{
	cat >&2 <<EOF
usage: $0 host
       $0 package [RUN|latest]
       $0 deploy [RUN|latest]
       $0 reboot [RUN|latest]
       $0 accept [RUN|latest] [MODE]
       $0 all
       $0 status
EOF
	exit 2
}

die()
{
	last_error_reason=$1
	printf 'P7_CYCLE result=FAIL reason=%s p8_write=NONE\n' "$1" >&2
	exit 1
}

for tool in awk bash cat chmod cmp cp date find flock grep id mkdir mktemp mv \
	realpath readelf rm sed sha256sum sleep sort ssh sshpass stat tee timeout \
	xargs; do
	command -v "$tool" >/dev/null 2>&1 || die "missing-tool:$tool"
done
[[ $(id -u) == 0 ]] || die host-root-required
mkdir -p "$report_root"

acquire_cycle_lock()
{
	if [[ $cycle_lock_acquired == 0 ]]; then
		exec 8>"$workspace/reports/.rg40xxv-p7-cycle.lock"
		flock -w 3600 8 || die cycle-lock-timeout
		cycle_lock_acquired=1
		export RG40XXV_P7_CYCLE_LOCK_FD=8
	fi
}

acquire_host_lock()
{
	if [[ $host_lock_acquired == 0 ]]; then
		exec 9>"$workspace/reports/.rg40xxv-p7-host-build.lock"
		flock -w 3600 9 || die host-build-lock-timeout
		host_lock_acquired=1
		export P7_HOST_LOCK_HELD=1
	fi
}

write_state()
{
	local run=$1 state=$2 temporary
	temporary=$run/.STATE.env.$$
	{
		printf 'schema=rg40xxv-p7-cycle-state-v1\n'
		printf 'run_id=%s\n' "${run##*/}"
		printf 'state=%s\n' "$state"
		printf 'updated_taipei=%s\n' "$(date '+%Y-%m-%dT%H:%M:%S%:z')"
		printf 'p8_write=NONE\n'
	} >"$temporary"
	chmod 0644 "$temporary"
	mv -f -- "$temporary" "$run/STATE.env"
}

read_state()
{
	local run=$1 state
	[[ -f $run/STATE.env && ! -L $run/STATE.env ]] || die state-missing
	state=$(env_value "$run/STATE.env" state)
	[[ $state =~ ^[A-Z_]+$ ]] || die invalid-state
	printf '%s' "$state"
}

require_state()
{
	local run=$1 current allowed
	shift
	current=$(read_state "$run")
	for allowed in "$@"; do
		[[ $current == "$allowed" ]] && return 0
	done
	die "invalid-state-transition:$current"
}

write_last_error()
{
	local run=$1 rc=$2 state=$3 temporary
	temporary=$run/.LAST-ERROR.env.$$
	{
		printf 'schema=rg40xxv-p7-cycle-error-v1\n'
		printf 'run_id=%s\n' "${run##*/}"
		printf 'phase=%s\n' "$active_phase"
		printf 'state=%s\n' "$state"
		printf 'exit_status=%s\n' "$rc"
		printf 'reason=%s\n' "$last_error_reason"
		printf 'recorded_taipei=%s\n' "$(date '+%Y-%m-%dT%H:%M:%S%:z')"
		printf 'p8_write=NONE\n'
	} >"$temporary"
	chmod 0644 "$temporary"
	mv -f -- "$temporary" "$run/LAST-ERROR.env"
}

resolve_run()
{
	local requested=${1:-latest} run_id resolved original
	if [[ $requested == latest ]]; then
		[[ -f $report_root/LATEST && ! -L $report_root/LATEST ]] || \
			die no-latest-cycle
		mapfile -t latest_rows <"$report_root/LATEST"
		[[ ${#latest_rows[@]} == 1 ]] || die invalid-latest-cycle
		run_id=${latest_rows[0]}
		[[ $run_id =~ ^[0-9]{8}T[0-9]{6}[+-][0-9]{4}-[0-9]+$ ]] || \
			die invalid-latest-cycle
		requested=$report_root/$run_id
	else
		original=$requested
		[[ ! -L $original ]] || die cycle-symlink
		if [[ $requested != /* ]]; then
			[[ $requested =~ ^[0-9]{8}T[0-9]{6}[+-][0-9]{4}-[0-9]+$ ]] || \
				die invalid-cycle-id
			requested=$report_root/$requested
		else
			[[ ${requested##*/} =~ ^[0-9]{8}T[0-9]{6}[+-][0-9]{4}-[0-9]+$ ]] || \
				die invalid-cycle-id
		fi
	fi
	[[ ! -L $requested ]] || die cycle-symlink
	resolved=$(realpath -e -- "$requested") || die cycle-not-found
	case $resolved in "$report_root"/*) ;; *) die cycle-outside-report-root ;; esac
	[[ ${resolved%/*} == "$report_root" ]] || die cycle-nested-path
	[[ ${resolved##*/} =~ ^[0-9]{8}T[0-9]{6}[+-][0-9]{4}-[0-9]+$ ]] || \
		die invalid-cycle-id
	[[ -d $resolved && ! -L $resolved ]] || die cycle-not-directory
	printf '%s' "$resolved"
}

run_logged()
{
	local label=$1 limit=$2 log=$3 rc
	shift 3
	printf 'P7_CYCLE step=%s status=START\n' "$label"
	set +e
	timeout --signal=TERM --kill-after=15 "$limit" "$@" 2>&1 | tee "$log"
	rc=${PIPESTATUS[0]}
	set -e
	if [[ $rc -ne 0 ]]; then
		printf 'P7_CYCLE step=%s status=FAIL exit=%s log=%s\n' \
			"$label" "$rc" "$log" >&2
		return "$rc"
	fi
	printf 'P7_CYCLE step=%s status=PASS log=%s\n' "$label" "$log"
}

require_sha()
{
	[[ $2 =~ ^[0-9a-f]{64}$ ]] || die "invalid-sha:$1"
}

env_value()
{
	local file=$1 key=$2 value
	value=$(awk -F= -v key="$key" '
		$1 == key { sub(/^[^=]*=/, ""); print; count++ }
		END { if (count != 1) exit 1 }
	' "$file") || die "env-key:$key"
	printf '%s' "$value"
}

steps_all_pass()
{
	awk -F '\t' 'NR == 1 { next }
		$1 != "PASS" { exit 1 }
		{ count++ }
		END { if (count == 0) exit 1 }' "$1"
}

copy_gate_evidence()
{
	local kind=$1 log=$2 run=$3 report source_dir destination
	case $kind in
	ui)
		mapfile -t gate_reports < <(sed -n \
			's/^P7_UI_PIPELINE result=PASS mode=candidate report=//p' "$log")
		;;
	component)
		mapfile -t gate_reports < <(sed -n \
			's/^P7_COMPONENT_HOST result=PASS report=\([^ ]*\) evidence_sha_check=0$/\1/p' "$log")
		;;
	*) die invalid-gate-kind ;;
	esac
	[[ ${#gate_reports[@]} == 1 ]] || die "$kind-gate-receipt"
	report=$(realpath -e -- "${gate_reports[0]}") || die "$kind-report-missing"
	case $kind:$report in
		ui:"$workspace"/reports/p7-ui-runs/*/REPORT.md) ;;
		component:"$workspace"/reports/p7-component-host-runs/*/REPORT.md) ;;
		*) die "$kind-report-path" ;;
	esac
	source_dir=${report%/REPORT.md}
	[[ -f $source_dir/EVIDENCE-SHA256SUMS && ! -L $source_dir/EVIDENCE-SHA256SUMS ]] || \
		die "$kind-evidence-missing"
	(cd "$source_dir" && sha256sum -c EVIDENCE-SHA256SUMS >/dev/null) || \
		die "$kind-evidence-check"
	steps_all_pass "$source_dir/steps.tsv" || die "$kind-step-failure"
	grep -Fqx -- '- Result: `PASS`' "$report" || die "$kind-report-not-pass"
	grep -Fqx -- '- Device write: `NONE`' "$report" || die "$kind-device-write"
	grep -Fqx -- '- p8 write: `NONE`' "$report" || die "$kind-p8-write"
	if [[ $kind == ui ]]; then
		grep -Fqx -- '- Mode: `candidate`' "$report" || die ui-mode-not-candidate
	else
		grep -Fqx -- '- Source snapshot unchanged: `YES`' "$report" || \
			die component-source-drift
	fi
	destination=$run/evidence/$kind
	[[ ! -e $destination && ! -L $destination ]] || die "$kind-evidence-destination"
	cp -a -- "$source_dir" "$destination"
	(cd "$destination" && sha256sum -c EVIDENCE-SHA256SUMS >/dev/null) || \
		die "$kind-copied-evidence-check"
}

verify_component_manifest()
{
	local root=$1 relative=$2
	[[ -d $root && ! -L $root && -f $root/$relative && ! -L $root/$relative ]] || \
		die "component-manifest-missing:$root/$relative"
	(cd "$root" && sha256sum -c "$relative" >/dev/null) || \
		die "component-manifest-failed:$root"
}

tree_sha()
{
	local root=$1
	(
		cd "$root"
		{
			find . -mindepth 1 -printf 'META\t%y\t%m\t%U\t%G\t%s\t%p\t%l\n' | \
				LC_ALL=C sort
			find . -type f -print0 | LC_ALL=C sort -z | xargs -0 sha256sum | \
				sed 's#  \./#FILE\t#'
		} | sha256sum | awk '{print $1}'
	)
}

host_cycle()
{
	local run_id run before after component_log ui_log ui_sha emulator_sha
	local rmut_sha wpe_sha rpg_input_sha bluetooth_sha youtube_sha device_sha
	local rpg_content_sha source_sha source_tree_sha ui_report component_report
	local lock_tmp

	acquire_cycle_lock
	acquire_host_lock
	active_phase=host
	run_id=$(date '+%Y%m%dT%H%M%S%z')-$$
	run=$report_root/$run_id
	[[ ! -e $run && ! -L $run ]] || die cycle-collision
	mkdir -m 0755 "$run" "$run/logs" "$run/evidence"
	active_run=$run
	printf '%s\n' "$run_id" >"$report_root/.LATEST.$$"
	mv -f -- "$report_root/.LATEST.$$" "$report_root/LATEST"
	write_state "$run" HOST_RUNNING

	before=$run/.source-receipt.before
	"$source_receipt_tool" "$before" >"$run/logs/source-before.log"

	component_log=$run/logs/component-host.log
	run_logged component-host 5400 "$component_log" "$component_runner"
	copy_gate_evidence component "$component_log" "$run"

	ui_log=$run/logs/ui-candidate.log
	run_logged ui-candidate 3600 "$ui_log" "$ui_runner" candidate
	copy_gate_evidence ui "$ui_log" "$run"

	after=$run/source-receipt
	"$source_receipt_tool" "$after" >"$run/logs/source-after.log"
	cmp -s "$before/SOURCE-SHA256SUMS" "$after/SOURCE-SHA256SUMS" || \
		die source-bytes-changed-during-gates
	cmp -s "$before/SOURCE-TREE.tsv" "$after/SOURCE-TREE.tsv" || \
		die source-tree-changed-during-gates

	ui_binary=$candidate/ui/build/release-root/bin/rg40xxv-shell
	emulator_archive=$candidate/emulator-runtime/build/rg40xxv-emulator-runtime-candidate-v1.tar.xz
	rmut_root=$candidate/rmut-h700/build/release-root
	wpe_root=$candidate/wpe-rpgmaker-h700/build/release-root
	rpg_input_root=$candidate/rpgmaker-h700-input/build/release-root
	bluetooth_root=$candidate/bluetooth-runtime/build/release-root
	youtube_root=$candidate/youtube-web-h700/build/release-root
	device_root=$candidate/device-control/build/stage
	rpg_content_archive=$candidate/rpg-content/build/rg40xxv-easyrpg-content-v1.tar.xz
	for file in "$ui_binary" "$emulator_archive" "$rpg_content_archive"; do
		[[ -f $file && ! -L $file ]] || die "candidate-output-missing:$file"
	done
	verify_component_manifest "$rmut_root" manifest/SHA256SUMS
	verify_component_manifest "$wpe_root" manifest/SHA256SUMS
	verify_component_manifest "$rpg_input_root" manifest/SHA256SUMS
	verify_component_manifest "$bluetooth_root" manifest/SHA256SUMS
	verify_component_manifest "$youtube_root" youtube/manifest/SHA256SUMS
	[[ -d $device_root && ! -L $device_root ]] || die device-control-stage-missing

	ui_sha=$(sha256sum "$ui_binary" | awk '{print $1}')
	emulator_sha=$(sha256sum "$emulator_archive" | awk '{print $1}')
	rmut_sha=$(sha256sum "$rmut_root/manifest/SHA256SUMS" | awk '{print $1}')
	wpe_sha=$(sha256sum "$wpe_root/manifest/SHA256SUMS" | awk '{print $1}')
	rpg_input_sha=$(sha256sum "$rpg_input_root/manifest/SHA256SUMS" | awk '{print $1}')
	bluetooth_sha=$(sha256sum "$bluetooth_root/manifest/SHA256SUMS" | awk '{print $1}')
	youtube_sha=$(sha256sum "$youtube_root/youtube/manifest/SHA256SUMS" | awk '{print $1}')
	device_sha=$(tree_sha "$device_root")
	rpg_content_sha=$(sha256sum "$rpg_content_archive" | awk '{print $1}')
	source_sha=$(sha256sum "$after/SOURCE-SHA256SUMS" | awk '{print $1}')
	source_tree_sha=$(sha256sum "$after/SOURCE-TREE.tsv" | awk '{print $1}')
	ui_report=$run/evidence/ui/REPORT.md
	component_report=$run/evidence/component/REPORT.md

	for pair in ui:$ui_sha emulator:$emulator_sha rmut:$rmut_sha wpe:$wpe_sha \
		rpg-input:$rpg_input_sha bluetooth:$bluetooth_sha youtube:$youtube_sha \
		device:$device_sha rpg-content:$rpg_content_sha source:$source_sha \
		source-tree:$source_tree_sha; do
		require_sha "${pair%%:*}" "${pair#*:}"
	done
	observed_ui=$(awk -F'`' '/^\| UI binary SHA-256 / { print $4; count++ }
		END { if (count != 1) exit 1 }' "$ui_report") || die ui-report-identity
	[[ $observed_ui == "$ui_sha" ]] || die ui-report-output-mismatch

	lock_tmp=$run/.BUILD-LOCK.env.$$
	{
		printf 'schema=rg40xxv-p7-build-lock-v1\n'
		printf 'run_id=%s\n' "$run_id"
		printf 'created_taipei=%s\n' "$(date '+%Y-%m-%dT%H:%M:%S%:z')"
		printf 'ui_sha256=%s\n' "$ui_sha"
		printf 'emulator_archive_sha256=%s\n' "$emulator_sha"
		printf 'rmut_manifest_sha256=%s\n' "$rmut_sha"
		printf 'wpe_manifest_sha256=%s\n' "$wpe_sha"
		printf 'rpg_input_manifest_sha256=%s\n' "$rpg_input_sha"
		printf 'bluetooth_manifest_sha256=%s\n' "$bluetooth_sha"
		printf 'youtube_manifest_sha256=%s\n' "$youtube_sha"
		printf 'device_control_tree_sha256=%s\n' "$device_sha"
		printf 'rpg_content_archive_sha256=%s\n' "$rpg_content_sha"
		printf 'source_manifest_sha256=%s\n' "$source_sha"
		printf 'source_manifest_path=%s\n' "$after/SOURCE-SHA256SUMS"
		printf 'source_tree_sha256=%s\n' "$source_tree_sha"
		printf 'source_tree_path=%s\n' "$after/SOURCE-TREE.tsv"
		printf 'source_receipt_tool_sha256=%s\n' "$(sha256sum "$source_receipt_tool" | awk '{print $1}')"
		printf 'ui_report_sha256=%s\n' "$(sha256sum "$ui_report" | awk '{print $1}')"
		printf 'ui_report_path=%s\n' "$ui_report"
		printf 'ui_evidence_sha256=%s\n' "$(sha256sum "$run/evidence/ui/EVIDENCE-SHA256SUMS" | awk '{print $1}')"
		printf 'ui_evidence_path=%s\n' "$run/evidence/ui/EVIDENCE-SHA256SUMS"
		printf 'component_report_sha256=%s\n' "$(sha256sum "$component_report" | awk '{print $1}')"
		printf 'component_report_path=%s\n' "$component_report"
		printf 'component_evidence_sha256=%s\n' "$(sha256sum "$run/evidence/component/EVIDENCE-SHA256SUMS" | awk '{print $1}')"
		printf 'component_evidence_path=%s\n' "$run/evidence/component/EVIDENCE-SHA256SUMS"
		printf 'device_write=NONE\n'
		printf 'p8_write=NONE\n'
	} >"$lock_tmp"
	chmod 0444 "$lock_tmp" "$after/SOURCE-SHA256SUMS" "$after/SOURCE-TREE.tsv"
	mv -- "$lock_tmp" "$run/BUILD-LOCK.env"
	sha256sum "$run/BUILD-LOCK.env" >"$run/BUILD-LOCK.env.sha256"
	chmod 0444 "$run/BUILD-LOCK.env.sha256"
	write_state "$run" HOST_PASS
	printf 'P7_CYCLE result=PASS phase=host run=%s build_lock=%s p8_write=NONE\n' \
		"$run_id" "$run/BUILD-LOCK.env"
}

package_cycle()
{
	local run build_lock current_receipt log release_id release_dir archive kit
	local archive_sha artifact_tmp bundle_tmp bundle bundle_manifest kit_sha
	run=$(resolve_run "${1:-latest}")
	active_phase=package
	acquire_cycle_lock
	acquire_host_lock
	active_run=$run
	require_state "$run" HOST_PASS
	build_lock=$run/BUILD-LOCK.env
	[[ -f $build_lock && ! -L $build_lock ]] || die build-lock-missing
	[[ $(sha256sum "$build_lock") == "$(<"$run/BUILD-LOCK.env.sha256")" ]] || \
		die build-lock-sidecar

	write_state "$run" PACKAGE_RUNNING
	current_receipt=$run/.source-receipt.package.$$
	"$source_receipt_tool" "$current_receipt" >"$run/logs/source-package.log"
	cmp -s "$run/source-receipt/SOURCE-SHA256SUMS" \
		"$current_receipt/SOURCE-SHA256SUMS" || die source-bytes-changed-before-package
	cmp -s "$run/source-receipt/SOURCE-TREE.tsv" \
		"$current_receipt/SOURCE-TREE.tsv" || die source-tree-changed-before-package
	rm -rf -- "$current_receipt"

	log=$run/logs/package.log
	run_logged package 3600 "$log" env RG40XXV_BUILD_LOCK="$build_lock" "$builder"
	grep -Fqx 'NEXT_RELEASE_BUILD result=PASS' "$log" || die package-no-pass

	log_value()
	{
		local key=$1 value
		value=$(awk -F= -v key="$key" '$1 == key { sub(/^[^=]*=/, ""); print; n++ }
			END { if (n != 1) exit 1 }' "$log") || die "package-output:$key"
		printf '%s' "$value"
	}
	release_id=$(log_value RELEASE_ID)
	release_dir=$(realpath -e -- "$(log_value RELEASE_DIR)") || die release-dir-missing
	archive=$(realpath -e -- "$(log_value ARCHIVE)") || die archive-missing
	kit=$(realpath -e -- "$(log_value DEPLOY_KIT)") || die kit-missing
	archive_sha=$(log_value ARCHIVE_SHA256)
	require_sha release "$release_id"
	require_sha archive "$archive_sha"
	case $release_dir in "$project"/build/releases/$release_id) ;; *) die release-dir-path ;; esac
	case $archive in "$project"/build/rg40xxv-release-$release_id.tar.xz) ;; *) die archive-path ;; esac
	case $kit in "$project"/build/deploy-kit-$release_id) ;; *) die kit-path ;; esac
	[[ $(sha256sum "$archive" | awk '{print $1}') == "$archive_sha" ]] || die archive-sha
	[[ $(<"$archive.sha256") == "$archive_sha  ${archive##*/}" ]] || die archive-sidecar
	[[ $(<"$kit/EXPECTED_RELEASE_ID") == "$release_id" ]] || die kit-release
	[[ $(<"$kit/EXPECTED_ARCHIVE_SHA256") == "$archive_sha" ]] || die kit-archive

	# Freeze the exact deployable bytes inside the cycle.  Build output paths are
	# mutable work areas and are never referenced by ARTIFACT.env directly.
	bundle=$run/artifacts
	[[ ! -e $bundle && ! -L $bundle ]] || die artifact-bundle-exists
	bundle_tmp=$run/.artifacts.$$
	mkdir -m 0700 "$bundle_tmp"
	cp --reflink=auto -- "$archive" "$archive.sha256" "$bundle_tmp/"
	cp -a -- "$kit" "$bundle_tmp/${kit##*/}"
	chmod -R go-w "$bundle_tmp"
	[[ $(sha256sum "$bundle_tmp/${archive##*/}" | awk '{print $1}') == \
		"$archive_sha" ]] || die copied-archive-sha
	[[ $(<"$bundle_tmp/${archive##*/}.sha256") == \
		"$archive_sha  ${archive##*/}" ]] || die copied-archive-sidecar
	kit_sha=$(tree_sha "$bundle_tmp/${kit##*/}")
	require_sha deploy-kit "$kit_sha"
	bundle_manifest=$bundle_tmp/BUNDLE-SHA256SUMS
	(
		cd "$bundle_tmp"
		find . -type f ! -name BUNDLE-SHA256SUMS -print0 | \
			LC_ALL=C sort -z | xargs -0 sha256sum | \
			sed 's#  \./#  #' >BUNDLE-SHA256SUMS
		sha256sum -c BUNDLE-SHA256SUMS >/dev/null
	)
	chmod 0444 "$bundle_manifest"
	mv -- "$bundle_tmp" "$bundle"
	bundle_tmp=
	archive=$bundle/${archive##*/}
	kit=$bundle/${kit##*/}

	artifact_tmp=$run/.ARTIFACT.env.$$
	{
		printf 'schema=rg40xxv-p7-artifact-v1\n'
		printf 'run_id=%s\n' "${run##*/}"
		printf 'created_taipei=%s\n' "$(date '+%Y-%m-%dT%H:%M:%S%:z')"
		printf 'release_id=%s\n' "$release_id"
		printf 'release_dir=%s\n' "$release_dir"
		printf 'archive_path=%s\n' "$archive"
		printf 'archive_sidecar_path=%s\n' "$archive.sha256"
		printf 'archive_sha256=%s\n' "$archive_sha"
		printf 'archive_bytes=%s\n' "$(stat -c %s "$archive")"
		printf 'deploy_kit_path=%s\n' "$kit"
		printf 'deploy_kit_tree_sha256=%s\n' "$kit_sha"
		printf 'bundle_manifest_path=%s\n' "$bundle/BUNDLE-SHA256SUMS"
		printf 'bundle_manifest_sha256=%s\n' \
			"$(sha256sum "$bundle/BUNDLE-SHA256SUMS" | awk '{print $1}')"
		printf 'build_lock_path=%s\n' "$build_lock"
		printf 'build_lock_sha256=%s\n' "$(sha256sum "$build_lock" | awk '{print $1}')"
		printf 'frozen_p8_sha256=%s\n' "$frozen_p8_sha"
		printf 'p8_payload=NONE\n'
		printf 'p8_write=NONE\n'
	} >"$artifact_tmp"
	chmod 0444 "$artifact_tmp"
	mv -- "$artifact_tmp" "$run/ARTIFACT.env"
	sha256sum "$run/ARTIFACT.env" >"$run/ARTIFACT.env.sha256"
	chmod 0444 "$run/ARTIFACT.env.sha256"
	write_state "$run" PACKAGED
	printf 'P7_CYCLE result=PASS phase=package run=%s release=%s artifact=%s p8_write=NONE\n' \
		"${run##*/}" "$release_id" "$run/ARTIFACT.env"
}

artifact_for_run()
{
	local run=$1 artifact=$run/ARTIFACT.env metadata sidecar_metadata
	[[ -f $artifact && ! -L $artifact && -f $artifact.sha256 &&
	   ! -L $artifact.sha256 ]] || die artifact-missing
	[[ $(sha256sum "$artifact") == "$(<"$artifact.sha256")" ]] || die artifact-sidecar
	metadata=$(stat -c '%u:%g:%a' "$artifact") || die artifact-stat
	sidecar_metadata=$(stat -c '%u:%g:%a' "$artifact.sha256") || \
		die artifact-sidecar-stat
	[[ $metadata == 0:0:444 && $sidecar_metadata == 0:0:444 ]] || \
		die artifact-permissions
	printf '%s' "$artifact"
}

deploy_cycle()
{
	local run artifact
	run=$(resolve_run "${1:-latest}")
	active_phase=deploy
	acquire_cycle_lock
	active_run=$run
	require_state "$run" PACKAGED
	artifact=$(artifact_for_run "$run")
	write_state "$run" DEPLOY_RUNNING
	RG40XXV_P7_CYCLE_RUN="$run" "$deployer" "$artifact" | \
		tee "$run/logs/deploy.log"
	grep -Fq 'P7_ARTIFACT_DEPLOY result=PASS' "$run/logs/deploy.log" || \
		die deploy-no-pass
	write_state "$run" DEPLOYED
	printf 'P7_CYCLE result=PASS phase=deploy run=%s p8_write=NONE\n' "${run##*/}"
}

ssh_options=(
	-o StrictHostKeyChecking=yes
	-o UserKnownHostsFile="$known_hosts"
	-o ConnectTimeout=5
)
remote()
{
	sshpass -p "$password" ssh "${ssh_options[@]}" "$device" "$@"
}

reboot_cycle()
{
	local run artifact release old_boot new_boot deadline rc
	run=$(resolve_run "${1:-latest}")
	active_phase=reboot
	acquire_cycle_lock
	active_run=$run
	require_state "$run" DEPLOYED
	artifact=$(artifact_for_run "$run")
	release=$(env_value "$artifact" release_id)
	old_boot=$(remote 'cat /proc/sys/kernel/random/boot_id') || die reboot-preflight-ssh
	[[ $(remote 'cat /mnt/data/rg40xxv/current.release') == "$release" ]] || \
		die reboot-current-release
	[[ $(remote "dd if=/dev/mmcblk0p8 bs=4M count=16 iflag=fullblock status=none 2>/dev/null | sha256sum | cut -d' ' -f1") == "$frozen_p8_sha" ]] || \
		die reboot-p8-not-frozen
	write_state "$run" REBOOT_RUNNING
	set +e
	remote '/usr/sbin/rg40xxv-reboot-target custom' >"$run/logs/reboot-request.log" 2>&1
	rc=$?
	set -e
	if ! grep -Fq 'REBOOT_TARGET result=PASS target=custom' \
		"$run/logs/reboot-request.log"; then
		die reboot-request
	fi
	deadline=$((SECONDS + 180))
	new_boot=
	while (( SECONDS < deadline )); do
		set +e
		new_boot=$(remote 'cat /proc/sys/kernel/random/boot_id' 2>/dev/null)
		rc=$?
		set -e
		if [[ $rc -eq 0 && -n $new_boot && $new_boot != "$old_boot" ]]; then
			break
		fi
		sleep 2
	done
	[[ -n $new_boot && $new_boot != "$old_boot" ]] || die reboot-boot-id-timeout
	RG40XXV_EXPECTED_RELEASE_ID="$release" \
		"$acceptance" snapshot | tee "$run/logs/post-reboot-acceptance.log"
	write_state "$run" ACCEPTED
	printf 'P7_CYCLE result=PASS phase=reboot run=%s old_boot=%s new_boot=%s p8_write=NONE\n' \
		"${run##*/}" "$old_boot" "$new_boot"
}

accept_cycle()
{
	local run mode artifact release prior_state
	run=$(resolve_run "${1:-latest}")
	mode=${2:-snapshot}
	active_phase=accept
	acquire_cycle_lock
	active_run=$run
	require_state "$run" DEPLOYED ACCEPTED
	prior_state=$(read_state "$run")
	artifact=$(artifact_for_run "$run")
	release=$(env_value "$artifact" release_id)
	RG40XXV_EXPECTED_RELEASE_ID="$release" "$acceptance" "$mode" | \
		tee "$run/logs/accept-$mode.log"
	if [[ $prior_state == DEPLOYED ]]; then
		write_state "$run" ACCEPTED
	fi
	printf 'P7_CYCLE result=PASS phase=accept mode=%s run=%s p8_write=NONE\n' \
		"$mode" "${run##*/}"
}

status_cycle()
{
	local actual_p8=UNAVAILABLE run state
	if [[ -f $frozen_p8 && ! -L $frozen_p8 ]]; then
		actual_p8=$(sha256sum "$frozen_p8" | awk '{print $1}')
	fi
	printf 'P8_FROZEN_HOST_ARTIFACT expected=%s observed=%s match=%s device=NOT_READ\n' \
		"$frozen_p8_sha" "$actual_p8" \
		"$([[ $actual_p8 == "$frozen_p8_sha" ]] && printf YES || printf NO)"
	if [[ -f $report_root/LATEST ]]; then
		run=$(resolve_run latest)
		state=$(env_value "$run/STATE.env" state)
		printf 'P7_LATEST run=%s state=%s build_lock=%s artifact=%s\n' \
			"${run##*/}" "$state" \
			"$([[ -f $run/BUILD-LOCK.env ]] && printf YES || printf NO)" \
			"$([[ -f $run/ARTIFACT.env ]] && printf YES || printf NO)"
	else
		printf 'P7_LATEST state=NONE\n'
	fi
}

command=${1:-}

finish_cycle()
{
	local rc=$? state=UNAVAILABLE
	trap - EXIT
	if [[ $rc -ne 0 && -n ${active_run:-} && -d $active_run ]]; then
		set +e
		if [[ -f $active_run/STATE.env && ! -L $active_run/STATE.env ]]; then
			state=$(env_value "$active_run/STATE.env" state 2>/dev/null)
			case $state in
				HOST_RUNNING|PACKAGE_RUNNING|DEPLOY_RUNNING|REBOOT_RUNNING)
					write_state "$active_run" FAILED
					state=FAILED
					;;
			esac
		fi
		write_last_error "$active_run" "$rc" "$state"
		set -e
	fi
	exit "$rc"
}
trap finish_cycle EXIT

case $command in
host)
	[[ $# == 1 ]] || usage
	host_cycle
	;;
package)
	[[ $# -le 2 ]] || usage
	package_cycle "${2:-latest}"
	;;
deploy)
	[[ $# -le 2 ]] || usage
	deploy_cycle "${2:-latest}"
	;;
reboot)
	[[ $# -le 2 ]] || usage
	reboot_cycle "${2:-latest}"
	;;
accept)
	[[ $# -le 3 ]] || usage
	accept_cycle "${2:-latest}" "${3:-snapshot}"
	;;
all)
	[[ $# == 1 ]] || usage
	host_cycle
	package_cycle "$active_run"
	deploy_cycle "$active_run"
	reboot_cycle "$active_run"
	;;
status)
	[[ $# == 1 ]] || usage
	status_cycle
	;;
*) usage ;;
esac
