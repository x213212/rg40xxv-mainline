#!/usr/bin/env bash
set -euo pipefail

# One host-only entry point for the RG40XX V p7 UI.  This script never opens a
# block device, SSH connection, fastboot transport, or p8 image for writing.

script=$(realpath -- "$0")
workspace=$(CDPATH= cd -- "$(dirname -- "$script")/.." && pwd -P)
ui=$workspace/lab/candidates/rg40xxv-next-v1-src/ui
identity=$ui/PRODUCTION-IDENTITY.env
next_deploy=$workspace/lab/deploy/rg40xxv-next-v1
emulator_archive=$workspace/lab/candidates/rg40xxv-next-v1-src/emulator-runtime/build/rg40xxv-emulator-runtime-candidate-v1.tar.xz
report_root=$workspace/reports/p7-ui-runs
mode=${1:-quick}

# Every p7 host pipeline mutates shared build directories.  Serialize direct
# invocations as well as the top-level cycle runner so two valid test jobs can
# never delete each other's fixtures or package staging trees.
host_lock=$workspace/reports/.rg40xxv-p7-host-build.lock
mkdir -p "$workspace/reports"
if [[ ${P7_HOST_LOCK_HELD:-0} == 1 ]]; then
	: >&9 2>/dev/null || {
		printf 'P7_UI_PIPELINE result=FAIL reason=inherited-lock-fd-missing\n' >&2
			exit 1
	}
	[[ $(stat -Lc '%d:%i' /proc/$$/fd/9) == \
	   "$(stat -Lc '%d:%i' "$host_lock")" ]] || {
		printf 'P7_UI_PIPELINE result=FAIL reason=inherited-lock-inode\n' >&2
		exit 1
	}
	flock -n 9 || {
		printf 'P7_UI_PIPELINE result=FAIL reason=inherited-lock-invalid\n' >&2
		exit 1
	}
else
	exec 9>"$host_lock"
	flock -w 3600 9 || {
		printf 'P7_UI_PIPELINE result=FAIL reason=host-build-lock-timeout\n' >&2
		exit 1
	}
fi

case $mode in
	version|build|quick|candidate|full) ;;
	*)
		if [[ $mode == release ]]; then
			printf '%s\n' \
				'release refused: use tools/rg40xxv.sh p7 host/package' >&2
			exit 1
		fi
		printf 'usage: %s {version|build|quick|candidate|full}\n' "$0" >&2
		exit 2
		;;
esac
[[ $# -le 1 ]] || {
	printf 'usage: %s {version|build|quick|candidate|full}\n' "$0" >&2
	exit 2
}

export LC_ALL=C
export TZ=Asia/Taipei

for tool in aarch64-linux-gnu-gcc-12 awk bash basename cmp cp date dirname env find flock mkdir \
	mktemp mv qemu-aarch64-static realpath readelf sed sh sha256sum sort stat \
	tail timeout xargs; do
	command -v "$tool" >/dev/null 2>&1 || {
		printf 'P7_UI_PIPELINE result=FAIL reason=missing-tool tool=%s\n' \
			"$tool" >&2
		exit 1
	}
done

[[ -d $ui && ! -L $ui ]] || {
	printf 'P7_UI_PIPELINE result=FAIL reason=canonical-ui-missing\n' >&2
	exit 1
}
[[ -f $identity && ! -L $identity ]] || {
	printf 'P7_UI_PIPELINE result=FAIL reason=identity-missing\n' >&2
	exit 1
}

identity_value()
{
	local key=$1
	local value

	value=$(awk -F= -v key="$key" '$1 == key { sub(/^[^=]*=/, ""); print; n++ }
		END { if (n != 1) exit 1 }' "$identity") || {
		printf 'invalid identity key: %s\n' "$key" >&2
		exit 1
	}
	printf '%s\n' "$value"
}

version=$(identity_value version)
pinned_input_sha=$(identity_value input_manifest_sha256)
pinned_binary_sha=$(identity_value binary_sha256)
pinned_build_id=$(identity_value elf_build_id)
pinned_release_id=$(identity_value verified_release_id)
pinned_emulator_sha=$(identity_value emulator_archive_sha256)

for digest in "$pinned_input_sha" "$pinned_binary_sha" "$pinned_emulator_sha"; do
	[[ $digest =~ ^[0-9a-f]{64}$ ]] || {
		printf 'P7_UI_PIPELINE result=FAIL reason=invalid-identity-sha\n' >&2
		exit 1
	}
done
[[ $pinned_build_id =~ ^[0-9a-f]{40}$ ]] || {
	printf 'P7_UI_PIPELINE result=FAIL reason=invalid-build-id\n' >&2
	exit 1
}
[[ $pinned_release_id =~ ^[0-9a-f]{64}$ ]] || {
	printf 'P7_UI_PIPELINE result=FAIL reason=invalid-release-id\n' >&2
	exit 1
}

run_id=$(date '+%Y%m%dT%H%M%S%z')-$$
run_dir=$report_root/$run_id
logs=$run_dir/logs
steps=$run_dir/steps.tsv
inputs=$run_dir/BUILD-INPUT-SHA256SUMS
report=$run_dir/REPORT.md
mkdir -p "$logs"
printf 'status\telapsed_ms\texit_code\ttimeout_s\tstarted_taipei\tstep\tlog\tcommand\n' >"$steps"

started_taipei=$(date '+%Y-%m-%dT%H:%M:%S%:z')
started_utc=$(date -u '+%Y-%m-%dT%H:%M:%SZ')
pipeline_started_ns=$(date +%s%N)
overall=PASS
finalized=0

generate_input_manifest()
{
	(
		cd "$workspace"
		{
			printf '%s\0' \
				lab/candidates/rg40xxv-next-v1-src/ui/build.sh
			find lab/candidates/rg40xxv-next-v1-src/ui/src \
				lab/candidates/rg40xxv-next-v1-src/ui/include \
				lab/candidates/rg40xxv-next-v1-src/ui/assets \
				-type f -print0
			printf '%s\0' \
				services/netstream/src/netstream.c \
				services/netstream/include/netstream.h \
				firmware/mnt/rootfs/usr/lib/aarch64-linux-gnu/libasound.so \
				firmware/mnt/rootfs/usr/lib/aarch64-linux-gnu/libSDL2_ttf.so \
				firmware/mnt/rootfs/usr/lib/aarch64-linux-gnu/libSDL2_image.so \
				firmware/mnt/rootfs/usr/lib/aarch64-linux-gnu/libSDL2.so
		} | LC_ALL=C sort -z | xargs -0 sha256sum
	) >"$inputs"
}

generate_input_manifest
live_input_sha=$(sha256sum "$inputs" | awk '{print $1}')

safe_label()
{
	printf '%s' "$1" | sed 's/[^A-Za-z0-9._-]/_/g'
}

run_step()
{
	local label=$1
	local limit=$2
	shift 2
	local safe log start_ns end_ns elapsed rc status started command

	safe=$(safe_label "$label")
	log=$logs/$safe.log
	started=$(date '+%Y-%m-%dT%H:%M:%S%:z')
	start_ns=$(date +%s%N)
	printf -v command '%q ' "$@"
	set +e
	timeout --signal=TERM --kill-after=5 "$limit" "$@" >"$log" 2>&1
	rc=$?
	set -e
	end_ns=$(date +%s%N)
	elapsed=$(( (end_ns - start_ns) / 1000000 ))
	if [[ $rc -eq 0 ]]; then
		status=PASS
	else
		status=FAIL
		overall=FAIL
	fi
	printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
		"$status" "$elapsed" "$rc" "$limit" "$started" "$label" \
		"${log#$workspace/}" "$command" >>"$steps"
	printf 'P7_UI_STEP status=%s elapsed_ms=%s name=%s\n' \
		"$status" "$elapsed" "$label"
	if [[ $rc -ne 0 ]]; then
		tail -n 80 "$log" >&2 || true
		return "$rc"
	fi
}

current_binary_metadata()
{
	local binary=$ui/build/rg40xxv-shell

	live_binary_sha=UNAVAILABLE
	live_build_id=UNAVAILABLE
	live_binary_bytes=0
	if [[ -f $binary && ! -L $binary ]]; then
		live_binary_sha=$(sha256sum "$binary" | awk '{print $1}')
		live_build_id=$(readelf -n "$binary" 2>/dev/null | \
			awk '/Build ID:/ {print $3; exit}')
		live_binary_bytes=$(stat -c %s "$binary")
	fi
}

finalize()
{
	local incoming_rc=$1
	local ended_taipei ended_utc pipeline_end_ns elapsed_ms
	local input_match binary_match build_id_match emulator_sha emulator_match
	local release_contract_ui release_match compiler qemu runner_sha finalize_rc

	[[ $finalized -eq 0 ]] || return
	finalized=1
	set +e
	finalize_rc=$incoming_rc
	if [[ $incoming_rc -ne 0 ]]; then
		overall=FAIL
	fi
	current_binary_metadata
	ended_taipei=$(date '+%Y-%m-%dT%H:%M:%S%:z')
	ended_utc=$(date -u '+%Y-%m-%dT%H:%M:%SZ')
	pipeline_end_ns=$(date +%s%N)
	elapsed_ms=$(( (pipeline_end_ns - pipeline_started_ns) / 1000000 ))
	input_match=NO
	binary_match=NO
	build_id_match=NO
	[[ $live_input_sha == "$pinned_input_sha" ]] && input_match=YES
	[[ $live_binary_sha == "$pinned_binary_sha" ]] && binary_match=YES
	[[ $live_build_id == "$pinned_build_id" ]] && build_id_match=YES
	emulator_sha=UNAVAILABLE
	emulator_match=NO
	if [[ -f $emulator_archive && ! -L $emulator_archive ]]; then
		emulator_sha=$(sha256sum "$emulator_archive" | awk '{print $1}')
		[[ $emulator_sha == "$pinned_emulator_sha" ]] && emulator_match=YES
	fi
	release_contract_ui=UNAVAILABLE
	release_match=NO
	if [[ -f $next_deploy/build/releases/$pinned_release_id/metadata/next-release-contract.env ]]; then
		release_contract_ui=$(sed -n 's/^ui_sha256=//p' \
			"$next_deploy/build/releases/$pinned_release_id/metadata/next-release-contract.env")
		[[ $release_contract_ui == "$pinned_binary_sha" ]] && release_match=YES
	fi
	compiler=$(aarch64-linux-gnu-gcc-12 --version 2>/dev/null | sed -n '1p')
	qemu=$(qemu-aarch64-static --version 2>/dev/null | sed -n '1p')
	runner_sha=$(sha256sum "$script" | awk '{print $1}')
	{
		printf '# RG40XX V p7 UI build/test report\n\n'
		printf -- '- Result: `%s`\n' "$overall"
		printf -- '- Mode: `%s`\n' "$mode"
		printf -- '- UI version: `%s`\n' "$version"
		printf -- '- Started: `%s` (`%s`)\n' "$started_taipei" "$started_utc"
		printf -- '- Ended: `%s` (`%s`)\n' "$ended_taipei" "$ended_utc"
		printf -- '- Wall time: `%s ms`\n' "$elapsed_ms"
		printf -- '- Device write: `NONE`\n'
		printf -- '- p8 write: `NONE`\n\n'
		printf '## Identity\n\n'
		printf '| Field | Pinned | Observed | Match |\n'
		printf '|---|---|---|---|\n'
		printf '| Build-input manifest SHA-256 | `%s` | `%s` | `%s` |\n' \
			"$pinned_input_sha" "$live_input_sha" "$input_match"
		printf '| UI binary SHA-256 | `%s` | `%s` | `%s` |\n' \
			"$pinned_binary_sha" "$live_binary_sha" "$binary_match"
		printf '| ELF Build ID | `%s` | `%s` | `%s` |\n' \
			"$pinned_build_id" "$live_build_id" "$build_id_match"
		printf '| Emulator archive SHA-256 | `%s` | `%s` | `%s` |\n' \
			"$pinned_emulator_sha" "$emulator_sha" "$emulator_match"
		printf '| Verified release contract UI | `%s` | `%s` | `%s` |\n' \
			"$pinned_binary_sha" "$release_contract_ui" "$release_match"
		printf '\n- UI binary bytes: `%s`\n' "$live_binary_bytes"
		printf -- '- Verified release ID: `%s`\n' "$pinned_release_id"
		printf -- '- Compiler: `%s`\n' "$compiler"
		printf -- '- QEMU: `%s`\n' "$qemu"
		printf -- '- Pipeline SHA-256: `%s`\n\n' "$runner_sha"
		printf '## Steps\n\n'
		printf '| Status | ms | Exit | Timeout | Step | Log |\n'
		printf '|---|---:|---:|---:|---|---|\n'
		tail -n +2 "$steps" | while IFS=$'\t' read -r status ms rc limit _ step log _; do
			printf '| %s | %s | %s | %s | `%s` | `%s` |\n' \
				"$status" "$ms" "$rc" "$limit" "$step" "$log"
		done
		printf '\n## Evidence boundary\n\n'
		printf 'Host/QEMU PASS proves build, ABI, state-machine and fixture behavior only. '
		printf 'It does not convert `PENDING_DEVICE`, `UNVERIFIED`, or recorded device FAIL into PASS.\n'
		} >"$report"
	if [[ $? -ne 0 ]]; then
		overall=FAIL
		finalize_rc=1
	fi
	(
		cd "$run_dir"
		find . -type f ! -name EVIDENCE-SHA256SUMS -print0 | \
			LC_ALL=C sort -z | xargs -0 sha256sum | sed 's#  \./#  #' \
			> EVIDENCE-SHA256SUMS
	)
	if [[ $? -ne 0 ]] || ! (
		cd "$run_dir" && sha256sum -c EVIDENCE-SHA256SUMS >/dev/null
	); then
		overall=FAIL
		finalize_rc=1
	fi
	latest_tmp=$report_root/.LATEST.$$
	printf '%s\n' "$run_id" >"$latest_tmp"
	if ! mv -- "$latest_tmp" "$report_root/LATEST"; then
		overall=FAIL
		finalize_rc=1
	fi
	printf 'P7_UI_PIPELINE result=%s mode=%s report=%s\n' \
		"$overall" "$mode" "$report"
	set -e
	[[ $finalize_rc -eq 0 && $overall == PASS ]]
}

trap 'rc=$?; trap - EXIT; if finalize "$rc"; then exit 0; else exit 1; fi' EXIT

build_once()
{
	run_step ui-build 60 "$ui/build.sh"
}

run_quick_tests()
{
	local test_name
	local tests=(
		test-ui-layout.sh
		test-frame-scheduler.sh
		test-input-navigation.sh
		test-apps-selection-handoff.sh
		test-network-flow.sh
		test-bluetooth-backend.sh
		test-settings-ui.sh
		test-stream-control.sh
		test-rpg-ui.sh
		test-youtube-web-ui.sh
		test-system-hardware.sh
		test-resident-launch-handoff.sh
	)

	for test_name in "${tests[@]}"; do
		run_step "ui-${test_name%.sh}" 90 sh "$ui/tests/$test_name"
	done
}

run_full_tests()
{
	local test_path

	while IFS= read -r test_path; do
		run_step "ui-$(basename "${test_path%.sh}")" 180 sh "$test_path" || true
	done < <(find "$ui/tests" -maxdepth 1 -type f -name 'test-*.sh' | \
		LC_ALL=C sort)
	while IFS= read -r test_path; do
		run_step "payload-$(basename "${test_path%.sh}")" 180 bash "$test_path" || true
	done < <(find "$workspace/lab/candidates/rg40xxv-next-v1-src/payload/tests" \
		-maxdepth 1 -type f -name 'test-*.sh' | LC_ALL=C sort)
}

verify_pinned_release()
{
	local release kit scratch archive sidecar

	release=$next_deploy/build/releases/$pinned_release_id
	kit=$next_deploy/build/deploy-kit-$pinned_release_id
	archive=$next_deploy/build/rg40xxv-release-$pinned_release_id.tar.xz
	sidecar=$archive.sha256
	[[ -d $release && -d $kit && -f $archive && -f $sidecar ]] || {
		printf 'pinned release evidence is incomplete\n' >&2
		return 1
	}
	scratch=$(mktemp -d "$run_dir/release-verify.XXXXXX")
	run_step release-tree-verify 180 env \
		RG40XXV_GENERIC_VERIFIER="$kit/rg40xxv-verify-release" \
		"$kit/rg40xxv-verify-next-release" \
		"$release" "$pinned_release_id" "$scratch" || true
	run_step release-archive-sha 60 sh -c \
		'cd "$1" && sha256sum -c "$2"' sh \
		"$(dirname -- "$archive")" "$(basename -- "$sidecar")" || true
}

case $mode in
version)
	current_binary_metadata
	printf 'UI_VERSION=%s\n' "$version"
	printf 'UI_INPUT_SHA256=%s PINNED=%s\n' "$live_input_sha" "$pinned_input_sha"
	printf 'UI_BINARY_SHA256=%s PINNED=%s\n' "$live_binary_sha" "$pinned_binary_sha"
	printf 'UI_BUILD_ID=%s PINNED=%s\n' "$live_build_id" "$pinned_build_id"
	printf 'VERIFIED_RELEASE_ID=%s\n' "$pinned_release_id"
	;;
build)
	build_once
	;;
quick)
	build_once
	run_quick_tests
	;;
candidate|full)
	build_once
	first_binary=$run_dir/rg40xxv-shell.first
	cp -- "$ui/build/rg40xxv-shell" "$first_binary"
	run_step ui-rebuild 60 "$ui/build.sh"
	run_step ui-reproducible 10 cmp -s "$first_binary" "$ui/build/rg40xxv-shell"
	run_full_tests
	# A candidate gate validates the current source/output pair and deliberately
	# does not require the previously sealed production identity.  The cycle
	# runner records the observed identities in BUILD-LOCK.env after this gate.
	# The legacy full mode retains its old frozen-release check.
	if [[ $mode != candidate ]]; then
		verify_pinned_release
	fi
	if [[ $overall != PASS ]]; then
		printf 'full gate failed; see generated report\n' >&2
		exit 1
	fi
	;;
esac

exit 0
