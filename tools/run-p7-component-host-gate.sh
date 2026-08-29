#!/usr/bin/env bash
set -euo pipefail

# Host/QEMU-only component gate for the p7 userspace candidate.  This runner
# deliberately has no device, fastboot, SSH, p8, GPU, or DE33 operation.

script=$(realpath -- "$0")
workspace=$(CDPATH= cd -- "$(dirname -- "$script")/.." && pwd -P)
candidate=$workspace/lab/candidates/rg40xxv-next-v1-src
report_root=$workspace/reports/p7-component-host-runs
execution_profile=${P7_COMPONENT_EXECUTION_PROFILE:-managed-default}

# Component builds and the UI integration tests share generated RPG/runtime
# trees.  A single cross-run lock turns races into bounded waits instead of
# misleading checksum/tar failures.
host_lock=$workspace/reports/.rg40xxv-p7-host-build.lock
mkdir -p "$workspace/reports"
if [[ ${P7_HOST_LOCK_HELD:-0} == 1 ]]; then
	: >&9 2>/dev/null || {
		printf 'P7_COMPONENT_HOST result=FAIL reason=inherited-lock-fd-missing\n' >&2
			exit 1
	}
	[[ $(stat -Lc '%d:%i' /proc/$$/fd/9) == \
	   "$(stat -Lc '%d:%i' "$host_lock")" ]] || {
		printf 'P7_COMPONENT_HOST result=FAIL reason=inherited-lock-inode\n' >&2
		exit 1
	}
	flock -n 9 || {
		printf 'P7_COMPONENT_HOST result=FAIL reason=inherited-lock-invalid\n' >&2
		exit 1
	}
else
	exec 9>"$host_lock"
	flock -w 3600 9 || {
		printf 'P7_COMPONENT_HOST result=FAIL reason=host-build-lock-timeout\n' >&2
		exit 1
	}
fi

export LC_ALL=C
export TZ=Asia/Taipei

for tool in awk bash cc cmp date diff dirname find flock grep mkdir mv \
	qemu-aarch64-static realpath sed sha256sum sort stat tail timeout uname wc \
	xargs; do
	command -v "$tool" >/dev/null 2>&1 || {
		printf 'P7_COMPONENT_HOST result=FAIL reason=missing-tool tool=%s\n' \
			"$tool" >&2
		exit 1
	}
done

components=(
	emulator-runtime
	rpg-content
	bluetooth-runtime
	device-control
	youtube-web-h700
	wpe-rpgmaker-h700
	rmut-h700
	rpgmaker-h700-input
)
[[ $# == 0 ]] || {
	printf 'usage: %s\n' "$0" >&2
	exit 2
}
for component in "${components[@]}"; do
	[[ -d $candidate/$component && ! -L $candidate/$component ]] || {
		printf 'P7_COMPONENT_HOST result=FAIL reason=missing-component component=%s\n' \
			"$component" >&2
		exit 1
	}
done

run_id=$(date '+%Y%m%dT%H%M%S%z')-$$
run_dir=$report_root/$run_id
logs=$run_dir/logs
steps=$run_dir/steps.tsv
report=$run_dir/REPORT.md
environment=$run_dir/environment.txt
source_before=$run_dir/SOURCE-SHA256SUMS.before
source_after=$run_dir/SOURCE-SHA256SUMS.after
source_diff=$run_dir/source-drift.diff
pending_evidence=$run_dir/embedded-pending.txt
mkdir -p "$logs"

printf 'status\telapsed_ms\texit_code\ttimeout_s\tstarted_taipei\tstep\tlog\tcommand\n' \
	>"$steps"

started_taipei=$(date '+%Y-%m-%dT%H:%M:%S%:z')
started_utc=$(date -u '+%Y-%m-%dT%H:%M:%SZ')
pipeline_started_ns=$(date +%s%N)
overall=PASS
finalized=0

planned_labels=(
	emulator-runtime-build
	emulator-runtime-main
	rpg-content-main
	bluetooth-runtime-main
	device-control-main
	youtube-web-host
	youtube-web-package
	wpe-build
	wpe-capability-ui
	wpe-cover-library
	wpe-launcher-syntax
	wpe-offline-covers
	wpe-runner-host
	wpe-rpgmaker-host
	rmut-h700-main
	rpgmaker-input-main
)
planned_limits=(1800 1200 600 600 600 300 600 600 180 180 180 180 180 180 1200 300)
declare -A completed=()

source_manifest()
{
	(
		cd "$workspace"
		find \
			lab/candidates/rg40xxv-next-v1-src/emulator-runtime \
			lab/candidates/rg40xxv-next-v1-src/rpg-content \
			lab/candidates/rg40xxv-next-v1-src/bluetooth-runtime \
			lab/candidates/rg40xxv-next-v1-src/device-control \
			lab/candidates/rg40xxv-next-v1-src/youtube-web-h700 \
			lab/candidates/rg40xxv-next-v1-src/wpe-rpgmaker-h700 \
			lab/candidates/rg40xxv-next-v1-src/rmut-h700 \
			lab/candidates/rg40xxv-next-v1-src/rpgmaker-h700-input \
			lab/candidates/rg40xxv-next-v1-src/payload \
			\( -path '*/build' -o -path '*/__pycache__' \) -prune -o \
			-type f ! -name '*.pyc' -print0 | \
			LC_ALL=C sort -z | xargs -0 sha256sum
	)
}

source_manifest >"$source_before"
source_before_sha=$(sha256sum "$source_before" | awk '{print $1}')

{
	printf 'run_id=%s\n' "$run_id"
	printf 'started_taipei=%s\n' "$started_taipei"
	printf 'started_utc=%s\n' "$started_utc"
	printf 'workspace=%s\n' "$workspace"
	printf 'execution_profile=%s\n' "$execution_profile"
	printf 'kernel=%s\n' "$(uname -srmo)"
	printf 'host_cc=%s\n' "$(cc --version 2>/dev/null | sed -n '1p' || printf UNAVAILABLE)"
	printf 'aarch64_cc=%s\n' "$(aarch64-linux-gnu-gcc-12 --version 2>/dev/null | sed -n '1p' || printf UNAVAILABLE)"
	printf 'qemu_aarch64=%s\n' "$(qemu-aarch64-static --version 2>/dev/null | sed -n '1p' || printf UNAVAILABLE)"
	printf 'timeout=%s\n' "$(timeout --version 2>/dev/null | sed -n '1p')"
	printf 'runner_sha256=%s\n' "$(sha256sum "$script" | awk '{print $1}')"
	printf 'source_manifest_before_sha256=%s\n' "$source_before_sha"
	printf 'device_write=NONE\n'
	printf 'p8_write=NONE\n'
} >"$environment"

safe_label()
{
	printf '%s' "$1" | sed 's/[^A-Za-z0-9._-]/_/g'
}

raise_overall()
{
	case $1 in
	FAIL)
		overall=FAIL
		;;
	TIMEOUT)
		[[ $overall == FAIL ]] || overall=TIMEOUT
		;;
	PENDING)
		[[ $overall == FAIL || $overall == TIMEOUT ]] || overall=PENDING
		;;
	esac
}

run_step()
{
	local label=$1
	local limit=$2
	shift 2
	local safe log started start_ns end_ns elapsed rc status command

	safe=$(safe_label "$label")
	log=$logs/$safe.log
	started=$(date '+%Y-%m-%dT%H:%M:%S%:z')
	start_ns=$(date +%s%N)
	printf -v command '%q ' "$@"
	{
		printf 'step=%s\n' "$label"
		printf 'started_taipei=%s\n' "$started"
		printf 'timeout_s=%s\n' "$limit"
		printf 'command=%s\n' "$command"
		printf '%s\n' '--- output ---'
	} >"$log"

	set +e
	timeout --signal=TERM --kill-after=10 "$limit" "$@" >>"$log" 2>&1
	rc=$?
	set -e
	end_ns=$(date +%s%N)
	elapsed=$(( (end_ns - start_ns) / 1000000 ))
	case $rc in
	0)
		status=PASS
		;;
	124|137)
		status=TIMEOUT
		;;
	*)
		status=FAIL
		;;
	esac
	completed["$label"]=1
	raise_overall "$status"
	{
		printf '%s\n' '--- result ---'
		printf 'status=%s\n' "$status"
		printf 'exit_code=%s\n' "$rc"
		printf 'elapsed_ms=%s\n' "$elapsed"
	} >>"$log"
	printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
		"$status" "$elapsed" "$rc" "$limit" "$started" "$label" \
		"${log#$workspace/}" "$command" >>"$steps"
	printf 'P7_COMPONENT_STEP status=%s elapsed_ms=%s name=%s\n' \
		"$status" "$elapsed" "$label"
	if [[ $status != PASS ]]; then
		tail -n 60 "$log" >&2 || true
	fi
}

append_unstarted_steps()
{
	local index label limit now
	now=$(date '+%Y-%m-%dT%H:%M:%S%:z')
	for index in "${!planned_labels[@]}"; do
		label=${planned_labels[$index]}
		limit=${planned_limits[$index]}
		if [[ -z ${completed[$label]:-} ]]; then
			printf 'PENDING\t0\t-\t%s\t%s\t%s\t-\tNOT_STARTED\n' \
				"$limit" "$now" "$label" >>"$steps"
			raise_overall PENDING
		fi
	done
}

rollup()
{
	local selector=$1
	local status step found=0 result=PASS
	while IFS=$'\t' read -r status _ _ _ _ step _ _; do
		[[ $status == status ]] && continue
		case $step in
		$selector)
			found=1
			case $status in
			FAIL) result=FAIL ;;
			TIMEOUT) [[ $result == FAIL ]] || result=TIMEOUT ;;
			PENDING) [[ $result == FAIL || $result == TIMEOUT ]] || result=PENDING ;;
			esac
			;;
		esac
	done <"$steps"
	if [[ $found -eq 0 ]]; then
		printf PENDING
	else
		printf '%s' "$result"
	fi
}

finalize()
{
	local incoming_rc=$1
	local ended_taipei ended_utc pipeline_end_ns elapsed_ms runner_sha
	local before_after_match pending_lines evidence_sha_rc

	[[ $finalized -eq 0 ]] || return
	finalized=1
	set +e
	append_unstarted_steps
	[[ $incoming_rc -eq 0 ]] || raise_overall FAIL

	source_manifest >"$source_after"
	source_after_sha=$(sha256sum "$source_after" | awk '{print $1}')
	before_after_match=YES
	if ! cmp -s "$source_before" "$source_after"; then
		before_after_match=NO
		diff -u "$source_before" "$source_after" >"$source_diff"
		raise_overall FAIL
	else
		: >"$source_diff"
	fi
	grep -HnEi '(^|[^A-Za-z])(PENDING|UNVERIFIED)([^A-Za-z]|$)' \
		"$logs"/*.log >"$pending_evidence"
	pending_lines=$(wc -l <"$pending_evidence" | awk '{print $1}')

	ended_taipei=$(date '+%Y-%m-%dT%H:%M:%S%:z')
	ended_utc=$(date -u '+%Y-%m-%dT%H:%M:%SZ')
	pipeline_end_ns=$(date +%s%N)
	elapsed_ms=$(( (pipeline_end_ns - pipeline_started_ns) / 1000000 ))
	runner_sha=$(sha256sum "$script" | awk '{print $1}')
	{
		printf '# RG40XX V p7 component host/QEMU gate\n\n'
		printf -- '- Result: `%s`\n' "$overall"
		printf -- '- Started: `%s` (`%s`)\n' "$started_taipei" "$started_utc"
		printf -- '- Ended: `%s` (`%s`)\n' "$ended_taipei" "$ended_utc"
		printf -- '- Wall time: `%s ms`\n' "$elapsed_ms"
		printf -- '- Execution profile: `%s`\n' "$execution_profile"
		printf -- '- Device write: `NONE`\n'
		printf -- '- p8 write: `NONE`\n'
		printf -- '- GPU/DE33 operation: `NONE`\n'
		printf -- '- Runner SHA-256: `%s`\n' "$runner_sha"
		printf -- '- Source snapshot before: `%s`\n' "$source_before_sha"
		printf -- '- Source snapshot after: `%s`\n' "$source_after_sha"
		printf -- '- Source snapshot unchanged: `%s`\n\n' "$before_after_match"

		printf '## Component matrix\n\n'
		printf '| Component | Host/QEMU gate | Device gate | Coverage |\n'
		printf '|---|---|---|---|\n'
		printf '| emulator-runtime | `%s` | `PENDING` | main host integration |\n' \
			"$(rollup 'emulator-runtime-*')"
		printf '| rpg-content | `%s` | `PENDING` | build, catalog, save/launch contracts |\n' \
			"$(rollup 'rpg-content-*')"
		printf '| bluetooth-runtime | `%s` | `PENDING` | build, HCI fixture, model, QEMU helper, payload |\n' \
			"$(rollup 'bluetooth-runtime-*')"
		printf '| device-control | `%s` | `PENDING` | check plus nine host fixtures |\n' \
			"$(rollup 'device-control-*')"
		printf '| youtube-web-h700 | `%s` | `PENDING` | host and package gates |\n' \
			"$(rollup 'youtube-web-*')"
		printf '| wpe-rpgmaker-h700 | `%s` | `PENDING` | all six short host tests |\n\n' \
			"$(rollup 'wpe-*')"
		printf '| rmut-h700 | `%s` | `PENDING` | runtime build and QEMU contract |\n' \
			"$(rollup 'rmut-h700-*')"
		printf '| rpgmaker-h700-input | `%s` | `PENDING` | input bridge reproducibility |\n\n' \
			"$(rollup 'rpgmaker-input-*')"

		printf '## Steps\n\n'
		printf '| Status | ms | Exit | Timeout s | Step | Log |\n'
		printf '|---|---:|---:|---:|---|---|\n'
		tail -n +2 "$steps" | while IFS=$'\t' read -r status ms rc limit _ step log _; do
			printf '| %s | %s | %s | %s | `%s` | `%s` |\n' \
				"$status" "$ms" "${rc:--}" "$limit" "$step" "${log:--}"
		done

		printf '\n## Embedded pending evidence\n\n'
		printf '%s matching `PENDING`/`UNVERIFIED` output lines were preserved in ' \
			"$pending_lines"
		printf '%s\n' \
			'`embedded-pending.txt`; these declarations are not promoted to PASS.'
		printf '\n## Evidence boundary\n\n'
		printf 'These results cover host fixtures, package checks, and QEMU user-mode '
		printf 'execution only. Every real-device gate remains `PENDING`; this report '
		printf 'does not claim display, GPU/Panfrost, DE33, Bluetooth radio, controller, '
		printf 'audio, storage durability, thermal, or gameplay validation.\n'
	} >"$report"

	(
		cd "$run_dir"
		find . -type f ! -name EVIDENCE-SHA256SUMS -print0 | \
			LC_ALL=C sort -z | xargs -0 sha256sum | \
			sed 's#  \./#  #' >EVIDENCE-SHA256SUMS
	)
	(
		cd "$run_dir"
		sha256sum -c EVIDENCE-SHA256SUMS >/dev/null
	)
	evidence_sha_rc=$?
	if [[ $evidence_sha_rc -ne 0 ]]; then
		overall=FAIL
	fi
	latest_tmp=$report_root/.LATEST.$$
	printf '%s\n' "$run_id" >"$latest_tmp"
	mv -- "$latest_tmp" "$report_root/LATEST"
	printf 'P7_COMPONENT_HOST result=%s report=%s evidence_sha_check=%s\n' \
		"$overall" "$report" "$evidence_sha_rc"
	set -e
	[[ $overall == PASS ]]
}

trap 'rc=$?; trap - EXIT; if finalize "$rc"; then exit 0; else exit 1; fi' EXIT

emulator=$candidate/emulator-runtime
rpg=$candidate/rpg-content
bluetooth=$candidate/bluetooth-runtime
device_control=$candidate/device-control
youtube=$candidate/youtube-web-h700
wpe=$candidate/wpe-rpgmaker-h700
rmut=$candidate/rmut-h700
rpg_input=$candidate/rpgmaker-h700-input

# The order below is intentional and matches the requested p7 component gate.
run_step emulator-runtime-build 1800 \
	bash "$emulator/build.sh"
run_step emulator-runtime-main 1200 \
	bash "$emulator/tests/test-emulator-runtime.sh"
run_step rpg-content-main 600 \
	bash "$rpg/tests/test-rpg-content.sh"
run_step bluetooth-runtime-main 600 \
	bash "$bluetooth/build.sh"
run_step device-control-main 600 \
	bash "$device_control/build.sh" package
run_step youtube-web-host 300 \
	bash "$youtube/tests/test-youtube-web.sh"
run_step youtube-web-package 600 \
	bash "$youtube/tests/test-youtube-web-package.sh"
run_step wpe-build 600 \
	bash "$wpe/build.sh"
run_step wpe-capability-ui 180 \
	bash "$wpe/tests/test-capability-ui.sh"
run_step wpe-cover-library 180 \
	bash "$wpe/tests/test-cover-library.sh"
run_step wpe-launcher-syntax 180 \
	bash "$wpe/tests/test-launcher-syntax.sh"
run_step wpe-offline-covers 180 \
	bash "$wpe/tests/test-offline-covers.sh"
run_step wpe-runner-host 180 \
	bash "$wpe/tests/test-runner-host.sh"
run_step wpe-rpgmaker-host 180 \
	bash "$wpe/tests/test-wpe-rpgmaker-host.sh"
run_step rmut-h700-main 1200 \
	bash "$rmut/tests/test-rmut-h700.sh"
run_step rpgmaker-input-main 300 \
	bash "$rpg_input/tests/test-input-bridge.sh"

[[ $overall == PASS ]] || exit 1
exit 0
