#!/usr/bin/env bash
set -euo pipefail

# Read-only device evidence collector for the RG40XX V p7 userspace line.
# It never installs a release, changes a setting, starts a game, reboots, or
# writes p8.  Interactive feature tests can append their receipts to the run
# directory after this identity gate has passed.

workspace=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
mode=${1:-snapshot}
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
expected_p8=${RG40XXV_EXPECTED_P8_SHA256:-6455c4d82d1594c03ab4f799c276da7249c6ffefd6fa22dd55b9077b1dfd4be3}
expected_release=${RG40XXV_EXPECTED_RELEASE_ID:-}
report_root=$workspace/reports/p7-device-runs

case $mode in
	preflight|ui|connectivity|runtime|performance|snapshot) ;;
	*)
		printf 'usage: %s {preflight|ui|connectivity|runtime|performance|snapshot}\n' "$0" >&2
		exit 2
		;;
esac

for tool in awk date find mkdir realpath sed sha256sum sort ssh sshpass xargs; do
	command -v "$tool" >/dev/null 2>&1 || {
		printf 'P7_DEVICE_ACCEPTANCE result=FAIL reason=missing-tool tool=%s\n' "$tool" >&2
		exit 1
	}
done
[[ $expected_p8 =~ ^[0-9a-f]{64}$ ]] || {
	printf 'P7_DEVICE_ACCEPTANCE result=FAIL reason=invalid-expected-p8\n' >&2
	exit 1
}
if [[ -n $expected_release && ! $expected_release =~ ^[0-9a-f]{64}$ ]]; then
	printf 'P7_DEVICE_ACCEPTANCE result=FAIL reason=invalid-expected-release\n' >&2
	exit 1
fi

run_id=$(date '+%Y%m%dT%H%M%S%z')-$$
run_dir=$report_root/$run_id
raw=$run_dir/device.txt
summary=$run_dir/SUMMARY.env
report=$run_dir/REPORT.md
mkdir -p "$run_dir"

ssh_options=(
	-o StrictHostKeyChecking=yes
	-o UserKnownHostsFile="$known_hosts"
	-o ConnectTimeout=8
	-o ServerAliveInterval=10
	-o ServerAliveCountMax=3
)

started=$(date '+%Y-%m-%dT%H:%M:%S%:z')
set +e
sshpass -p "$password" ssh "${ssh_options[@]}" "$device" \
	"RG_ACCEPTANCE_MODE='$mode' bash -s" >"$raw" 2>"$run_dir/ssh.stderr" <<'REMOTE'
set +e

section()
{
	printf '\n[%s]\n' "$1"
}

section IDENTITY
printf 'kernel=%s\n' "$(uname -r 2>/dev/null)"
printf 'boot_id=%s\n' "$(cat /proc/sys/kernel/random/boot_id 2>/dev/null)"
printf 'uptime_seconds=%s\n' "$(awk '{print $1}' /proc/uptime 2>/dev/null)"
printf 'cmdline=%s\n' "$(cat /proc/cmdline 2>/dev/null)"
p8_sha256=$(dd if=/dev/mmcblk0p8 bs=4M count=16 iflag=fullblock \
	status=none 2>/dev/null | sha256sum | awk '{print $1}')
printf 'p8_sha256=%s\n' "$p8_sha256"
printf 'current_release=%s\n' "$(cat /mnt/data/rg40xxv/current.release 2>/dev/null)"
printf 'previous_release=%s\n' "$(cat /mnt/data/rg40xxv/previous.release 2>/dev/null)"

section STORAGE
findmnt -rn -o SOURCE,TARGET,FSTYPE,OPTIONS / /opt/rg40xxv /mnt/data /mnt/mmc /mnt/sdcard 2>/dev/null
df -B1 / /opt/rg40xxv /mnt/data /mnt/mmc /mnt/sdcard 2>/dev/null

section SERVICES
emit_service()
{
	unit=$1
	key=$2
	for property in LoadState ActiveState SubState Result NRestarts \
		ExecMainStatus MainPID; do
		value=$(systemctl show "$unit" --property="$property" --value \
			--no-pager 2>/dev/null | sed -n '1p')
		printf 'service_%s_%s=%s\n' "$key" "$property" "$value"
	done
}

emit_service rg40xxv-ui.service ui
emit_service rg40xxv-boot-health.service boot_health
emit_service rg40xxv-network-prepare.service network_prepare
emit_service NetworkManager.service network_manager
emit_service bluetooth.service bluetooth
emit_service rg40xxv-cpu-ui-policy.service cpu_ui_policy
emit_service rg40xxv-perf-policy.service perf_policy
emit_service rg40xxv-time-sync.service time_sync

case ${RG_ACCEPTANCE_MODE:-snapshot} in
ui|snapshot)
	section UI
	printf 'ready_marker=%s\n' "$(sed -n '1p' /run/rg40xxv-ui.ready 2>/dev/null)"
	pid=$(systemctl show rg40xxv-ui.service -p MainPID --value 2>/dev/null)
	printf 'ui_pid=%s\n' "${pid:-0}"
	if [ -n "${pid:-}" ] && [ "$pid" -gt 0 ] 2>/dev/null; then
		awk '/VmRSS|VmHWM|Threads/ {gsub(/[[:space:]]+/, " "); print "ui_" $0}' \
			"/proc/$pid/status" 2>/dev/null
		awk '$6 ~ /(rg40xxv-shell|libEGL|libGLES|libgbm|panfrost|libSDL2)/ && !seen[$6]++ {print "map=" $6}' \
			"/proc/$pid/maps" 2>/dev/null
	fi
	journalctl -b -u rg40xxv-ui.service --no-pager -n 160 2>/dev/null
	;;
esac

case ${RG_ACCEPTANCE_MODE:-snapshot} in
connectivity|snapshot)
	section WIFI
	nmcli -t -f GENERAL.DEVICE,GENERAL.TYPE,GENERAL.STATE,GENERAL.CONNECTION \
		device show 2>/dev/null
	nmcli -t -f IN-USE,SSID,SECURITY,SIGNAL device wifi list --rescan no 2>/dev/null
	ip -brief address show 2>/dev/null
	rfkill list 2>/dev/null
	section BLUETOOTH
	bluetoothctl show 2>/dev/null
	bluetoothctl devices 2>/dev/null
	for address in $(bluetoothctl devices 2>/dev/null | awk '$1 == "Device" {print $2}'); do
		bluetoothctl info "$address" 2>/dev/null | sed "s/^/device=$address /"
	done
	;;
esac

case ${RG_ACCEPTANCE_MODE:-snapshot} in
runtime|snapshot)
	section RUNTIME
	for path in \
		/opt/rg40xxv/share/platform-routes.json \
		/opt/rg40xxv/rpgmaker/runtime/admission.env \
		/opt/rg40xxv/rpgmaker/wpe/runtime/admission.env \
		/opt/rg40xxv/youtube/runtime/admission.env \
		/opt/rg40xxv/bluetooth/runtime-lock.env; do
		if [ -f "$path" ] && [ ! -L "$path" ]; then
			printf 'file=%s sha256=' "$path"
			sha256sum "$path" | awk '{print $1}'
			sed -n '1,160p' "$path"
		else
			printf 'file=%s missing=yes\n' "$path"
		fi
	done
	printf 'tf1_rpg_roots\n'
	find /mnt/mmc/Roms -mindepth 1 -maxdepth 2 -type d \
		\( -path '*/EASYRPG/*' -o -path '*/RPGMV/*' -o -path '*/RPGMZ/*' \) \
		-printf '%p\n' 2>/dev/null | sort
	printf 'tf2_rpg_roots\n'
	find /mnt/sdcard/Roms -mindepth 1 -maxdepth 2 -type d \
		\( -path '*/EASYRPG/*' -o -path '*/RPGMV/*' -o -path '*/RPGMZ/*' \) \
		-printf '%p\n' 2>/dev/null | sort
	tail -n 240 /mnt/data/rg40xxv/state/log/launcher.log 2>/dev/null
	;;
esac

case ${RG_ACCEPTANCE_MODE:-snapshot} in
performance|snapshot)
	section PERFORMANCE
	for policy in /sys/devices/system/cpu/cpufreq/policy*; do
		[ -d "$policy" ] || continue
		printf 'cpu_policy=%s governor=' "${policy##*/}"
		cat "$policy/scaling_governor" 2>/dev/null
		printf 'cpu_policy=%s current_khz=' "${policy##*/}"
		cat "$policy/scaling_cur_freq" 2>/dev/null
	done
	for dev in /sys/class/devfreq/*; do
		[ -d "$dev" ] || continue
		printf 'devfreq=%s governor=' "${dev##*/}"
		cat "$dev/governor" 2>/dev/null
		printf 'devfreq=%s current_hz=' "${dev##*/}"
		cat "$dev/cur_freq" 2>/dev/null
	done
	du -sb /mnt/data/rg40xxv/state/shader-cache 2>/dev/null || true
	stat -c 'time_sync_runtime=%Y:%s' /run/rg40xxv/time-sync-success 2>/dev/null
	stat -c 'time_sync_persistent=%Y:%s' /mnt/data/rg40xxv/state/time-sync-success 2>/dev/null
	;;
esac

section FAULTS
faults=$(
	dmesg 2>/dev/null | grep -Ei \
		'panic|oops|BUG:|segfault|illegal instruction|DATA_INVALID|MMU.*FAULT|panfrost.*fault|underflow|watchdog' | \
		tail -n 200
	journalctl -b --no-pager 2>/dev/null | grep -Ei \
		'status=1/FAILURE|status=132|core-dump|segfault|illegal instruction|watchdog' | \
		tail -n 200
)
if [ -n "$faults" ]; then
	fault_count=$(printf '%s\n' "$faults" | awk 'NF { count++ } END { print count + 0 }')
else
	fault_count=0
fi
printf 'fault_count=%s\n' "$fault_count"
[ -z "$faults" ] || printf '%s\n' "$faults"
REMOTE
ssh_rc=$?
set -e

ended=$(date '+%Y-%m-%dT%H:%M:%S%:z')
result=PASS
reason=none
p8=UNAVAILABLE
kernel=UNAVAILABLE
release=UNAVAILABLE
boot_id=UNAVAILABLE
ready_marker=NOT_CHECKED
ui_load_state=NOT_CHECKED
ui_active_state=NOT_CHECKED
ui_sub_state=NOT_CHECKED
ui_result=NOT_CHECKED
ui_restarts=NOT_CHECKED
ui_exec_status=NOT_CHECKED
boot_health_load_state=NOT_CHECKED
boot_health_active_state=NOT_CHECKED
boot_health_sub_state=NOT_CHECKED
boot_health_result=NOT_CHECKED
boot_health_exec_status=NOT_CHECKED
fault_count=UNAVAILABLE
strict_ui_checks=NOT_APPLICABLE

raw_value()
{
	local key=$1
	awk -F= -v key="$key" '
		$1 == key { sub(/^[^=]*=/, ""); print; count++ }
		END { if (count != 1) exit 1 }
	' "$raw"
}

if [[ $ssh_rc -ne 0 ]]; then
	result=FAIL
	reason=ssh
else
	p8=$(raw_value p8_sha256) || p8=UNAVAILABLE
	kernel=$(raw_value kernel) || kernel=UNAVAILABLE
	release=$(raw_value current_release) || release=UNAVAILABLE
	boot_id=$(raw_value boot_id) || boot_id=UNAVAILABLE
	fault_count=$(raw_value fault_count) || fault_count=UNAVAILABLE
	if [[ $p8 != "$expected_p8" ]]; then
		result=FAIL
		reason=p8-identity
	elif [[ ! $kernel =~ ^7\.2\. ]]; then
		result=FAIL
		reason=kernel-identity
	elif [[ ! $boot_id =~ ^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$ ]]; then
		result=FAIL
		reason=boot-id
	elif [[ ! $release =~ ^[0-9a-f]{64}$ ]]; then
		result=FAIL
		reason=release-identity
	elif [[ -n $expected_release && $release != "$expected_release" ]]; then
		result=FAIL
		reason=release-mismatch
	fi

	if [[ $mode == ui || $mode == snapshot ]]; then
		strict_ui_checks=FAIL
		ready_marker=$(raw_value ready_marker) || ready_marker=UNAVAILABLE
		ui_load_state=$(raw_value service_ui_LoadState) || ui_load_state=UNAVAILABLE
		ui_active_state=$(raw_value service_ui_ActiveState) || ui_active_state=UNAVAILABLE
		ui_sub_state=$(raw_value service_ui_SubState) || ui_sub_state=UNAVAILABLE
		ui_result=$(raw_value service_ui_Result) || ui_result=UNAVAILABLE
		ui_restarts=$(raw_value service_ui_NRestarts) || ui_restarts=UNAVAILABLE
		ui_exec_status=$(raw_value service_ui_ExecMainStatus) || ui_exec_status=UNAVAILABLE
		boot_health_load_state=$(raw_value service_boot_health_LoadState) || \
			boot_health_load_state=UNAVAILABLE
		boot_health_active_state=$(raw_value service_boot_health_ActiveState) || \
			boot_health_active_state=UNAVAILABLE
		boot_health_sub_state=$(raw_value service_boot_health_SubState) || \
			boot_health_sub_state=UNAVAILABLE
		boot_health_result=$(raw_value service_boot_health_Result) || \
			boot_health_result=UNAVAILABLE
		boot_health_exec_status=$(raw_value service_boot_health_ExecMainStatus) || \
			boot_health_exec_status=UNAVAILABLE

		if [[ $result == PASS && $ready_marker != first-frame-presented ]]; then
			result=FAIL
			reason=ui-ready-marker
		elif [[ $result == PASS && $ui_load_state != loaded ]]; then
			result=FAIL
			reason=ui-load-state
		elif [[ $result == PASS && $ui_active_state != active ]]; then
			result=FAIL
			reason=ui-active-state
		elif [[ $result == PASS && $ui_sub_state != running ]]; then
			result=FAIL
			reason=ui-sub-state
		elif [[ $result == PASS && $ui_result != success ]]; then
			result=FAIL
			reason=ui-result
		elif [[ $result == PASS && $ui_restarts != 0 ]]; then
			result=FAIL
			reason=ui-restarts
		elif [[ $result == PASS && $ui_exec_status != 0 ]]; then
			result=FAIL
			reason=ui-exec-status
		elif [[ $result == PASS && $boot_health_load_state != loaded ]]; then
			result=FAIL
			reason=boot-health-load-state
		elif [[ $result == PASS && $boot_health_active_state != active ]]; then
			result=FAIL
			reason=boot-health-active-state
		elif [[ $result == PASS && $boot_health_sub_state != exited ]]; then
			result=FAIL
			reason=boot-health-sub-state
		elif [[ $result == PASS && $boot_health_result != success ]]; then
			result=FAIL
			reason=boot-health-result
		elif [[ $result == PASS && $boot_health_exec_status != 0 ]]; then
			result=FAIL
			reason=boot-health-exec-status
		elif [[ $result == PASS && ( ! $fault_count =~ ^[0-9]+$ || $fault_count != 0 ) ]]; then
			result=FAIL
			reason=faults-present
		fi
		[[ $result == PASS ]] && strict_ui_checks=PASS
	fi
fi

{
	printf 'result=%s\n' "$result"
	printf 'reason=%s\n' "$reason"
	printf 'mode=%s\n' "$mode"
	printf 'started_taipei=%s\n' "$started"
	printf 'ended_taipei=%s\n' "$ended"
	printf 'device=%s\n' "$device"
	printf 'ssh_exit_code=%s\n' "$ssh_rc"
	printf 'boot_id=%s\n' "$boot_id"
	printf 'kernel=%s\n' "$kernel"
	printf 'p8_sha256=%s\n' "$p8"
	printf 'expected_p8_sha256=%s\n' "$expected_p8"
	printf 'release_id=%s\n' "$release"
	printf 'expected_release_id=%s\n' "${expected_release:-ANY_VALID}"
	printf 'ready_marker=%s\n' "$ready_marker"
	printf 'ui_load_state=%s\n' "$ui_load_state"
	printf 'ui_active_state=%s\n' "$ui_active_state"
	printf 'ui_sub_state=%s\n' "$ui_sub_state"
	printf 'ui_result=%s\n' "$ui_result"
	printf 'ui_nrestarts=%s\n' "$ui_restarts"
	printf 'ui_exec_main_status=%s\n' "$ui_exec_status"
	printf 'boot_health_load_state=%s\n' "$boot_health_load_state"
	printf 'boot_health_active_state=%s\n' "$boot_health_active_state"
	printf 'boot_health_sub_state=%s\n' "$boot_health_sub_state"
	printf 'boot_health_result=%s\n' "$boot_health_result"
	printf 'boot_health_exec_main_status=%s\n' "$boot_health_exec_status"
	printf 'fault_count=%s\n' "$fault_count"
	printf 'strict_ui_checks=%s\n' "$strict_ui_checks"
	printf 'device_write=NONE\n'
	printf 'p8_write=NONE\n'
} >"$summary"

{
	printf '# RG40XX V p7 device evidence\n\n'
	printf -- '- Result: `%s`\n' "$result"
	printf -- '- Reason: `%s`\n' "$reason"
	printf -- '- Mode: `%s`\n' "$mode"
	printf -- '- Started: `%s`\n' "$started"
	printf -- '- Ended: `%s`\n' "$ended"
	printf -- '- Boot ID: `%s`\n' "$boot_id"
	printf -- '- Kernel: `%s`\n' "$kernel"
	printf -- '- P8 SHA-256: `%s`\n' "$p8"
	printf -- '- P7 release: `%s`\n' "$release"
	printf -- '- UI first frame: `%s`\n' "$ready_marker"
	printf -- '- UI service: `%s/%s/%s`, result `%s`, restarts `%s`, exit `%s`\n' \
		"$ui_load_state" "$ui_active_state" "$ui_sub_state" "$ui_result" \
		"$ui_restarts" "$ui_exec_status"
	printf -- '- Boot health: `%s/%s/%s`, result `%s`, exit `%s`\n' \
		"$boot_health_load_state" "$boot_health_active_state" \
		"$boot_health_sub_state" "$boot_health_result" \
		"$boot_health_exec_status"
	printf -- '- Fault matches: `%s`\n' "$fault_count"
	printf -- '- Strict UI checks: `%s`\n' "$strict_ui_checks"
	printf -- '- Device write: `NONE`\n'
	printf -- '- P8 write: `NONE`\n\n'
	printf 'Raw output: `device.txt`; SSH diagnostics: `ssh.stderr`.\n'
} >"$report"

(
	cd "$run_dir"
	find . -type f ! -name EVIDENCE-SHA256SUMS -print0 | LC_ALL=C sort -z |
		xargs -0 sha256sum | sed 's#  \./#  #' >EVIDENCE-SHA256SUMS
)
printf '%s\n' "$run_id" >"$report_root/LATEST"
printf 'P7_DEVICE_ACCEPTANCE result=%s reason=%s mode=%s report=%s device_write=NONE p8_write=NONE\n' \
	"$result" "$reason" "$mode" "$report"
[[ $result == PASS ]]
