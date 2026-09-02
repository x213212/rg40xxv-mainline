#!/usr/bin/env bash
set -euo pipefail

TEST_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(cd -- "$TEST_DIR/.." && pwd)
FIXTURE=$(mktemp -d /tmp/rg40xxv-device-control-test.cpu.XXXXXX)
ROOTFS="$FIXTURE/rootfs"
trap 'rm -rf -- "$FIXTURE"' EXIT

for policy in policy0 policy4; do
    mkdir -p "$ROOTFS/sys/devices/system/cpu/cpufreq/$policy"
    printf 'performance powersave schedutil' >"$ROOTFS/sys/devices/system/cpu/cpufreq/$policy/scaling_available_governors"
    printf 'performance' >"$ROOTFS/sys/devices/system/cpu/cpufreq/$policy/scaling_governor"
    printf '480000 1008000 1512000' >"$ROOTFS/sys/devices/system/cpu/cpufreq/$policy/scaling_available_frequencies"
    if [[ $policy == policy0 ]]; then printf '0 1 2 3'; else printf '4 5 6 7'; fi \
        >"$ROOTFS/sys/devices/system/cpu/cpufreq/$policy/related_cpus"
done
mkdir -p "$ROOTFS/sys/bus/nvmem/devices/sunxi-sid0"
printf 'fake-not-read' >"$ROOTFS/sys/bus/nvmem/devices/sunxi-sid0/nvmem"

export DEVICE_CONTROL_TESTING=1
export DEVICE_CONTROL_TEST_ROOT="$FIXTURE"
CTL="$ROOT/cpu-policy/cpu-policyctl"

"$CTL" validate | grep -q '^efuse=exposed-not-read$'
"$CTL" ui-default >/dev/null
[[ $(<"$ROOTFS/sys/devices/system/cpu/cpufreq/policy0/scaling_governor") == schedutil ]]
[[ $(<"$ROOTFS/sys/devices/system/cpu/cpufreq/policy4/scaling_governor") == schedutil ]]

# A stale/parallel policy owner must not leave the UI or next launch waiting
# forever behind the game-lifetime lock. Timeout must not touch either policy.
LOCK_FILE="$ROOTFS/run/rg40xxv/cpu-policy/.lock"
LOCK_READY="$FIXTURE/lock-ready"
(
	exec 8>"$LOCK_FILE"
	flock -x 8
	: >"$LOCK_READY"
	sleep 2
) &
LOCK_HOLDER=$!
for _ in {1..100}; do
	[[ -e $LOCK_READY ]] && break
	sleep 0.01
done
[[ -e $LOCK_READY ]]
LOCK_STARTED=$(date +%s%3N)
set +e
"$CTL" ui-default >/dev/null 2>"$FIXTURE/lock-timeout.err"
LOCK_RC=$?
set -e
LOCK_ELAPSED=$(( $(date +%s%3N) - LOCK_STARTED ))
[[ $LOCK_RC == 75 ]]
((LOCK_ELAPSED >= 850 && LOCK_ELAPSED < 1600))
grep -Fq 'lock timed out after 1 second' "$FIXTURE/lock-timeout.err"
[[ $(<"$ROOTFS/sys/devices/system/cpu/cpufreq/policy0/scaling_governor") == schedutil ]]
[[ $(<"$ROOTFS/sys/devices/system/cpu/cpufreq/policy4/scaling_governor") == schedutil ]]
wait "$LOCK_HOLDER"

CHECK="$FIXTURE/check-performance.sh"
{
    printf '#!/usr/bin/env bash\n'
    printf 'set -euo pipefail\n'
    # shellcheck disable=SC2016
    printf '[[ $(<"%s") == performance ]]\n' "$ROOTFS/sys/devices/system/cpu/cpufreq/policy0/scaling_governor"
    # shellcheck disable=SC2016
    printf '[[ $(<"%s") == performance ]]\n' "$ROOTFS/sys/devices/system/cpu/cpufreq/policy4/scaling_governor"
} >"$CHECK"
chmod +x "$CHECK"
"$CTL" run-performance -- "$CHECK"
[[ $(<"$ROOTFS/sys/devices/system/cpu/cpufreq/policy0/scaling_governor") == schedutil ]]
[[ $(<"$ROOTFS/sys/devices/system/cpu/cpufreq/policy4/scaling_governor") == schedutil ]]

set +e
"$CTL" run-performance -- bash -c 'exit 7'
CHILD_RC=$?
set -e
[[ $CHILD_RC == 7 ]]
[[ $(<"$ROOTFS/sys/devices/system/cpu/cpufreq/policy0/scaling_governor") == schedutil ]]
[[ $(<"$ROOTFS/sys/devices/system/cpu/cpufreq/policy4/scaling_governor") == schedutil ]]

# wrapper 收到 TERM 時也必須還原；以獨立 process group 避免留下 child。
# 只等 policy0 第一次變 performance 就立即送 TERM，特意覆蓋
# policy0/policy4 切換中間的競態窗。wait 返回後直接斷言還原，不以 sleep 猜測。
SIGNAL_CHILD="$FIXTURE/wait-for-term.sh"
{
    printf '#!/usr/bin/env bash\n'
    printf '%s\n' "trap 'exit 0' TERM INT HUP"
    printf 'while :; do sleep 0.05; done\n'
} >"$SIGNAL_CHILD"
chmod +x "$SIGNAL_CHILD"
setsid "$CTL" run-performance -- "$SIGNAL_CHILD" >/dev/null 2>&1 &
WRAPPER_PID=$!
for _ in {1..100}; do
    if [[ $(<"$ROOTFS/sys/devices/system/cpu/cpufreq/policy0/scaling_governor") == performance ]]; then break; fi
    sleep 0.01
done
[[ $(<"$ROOTFS/sys/devices/system/cpu/cpufreq/policy0/scaling_governor") == performance ]]
kill -TERM -- "-$WRAPPER_PID"
set +e
wait "$WRAPPER_PID"
SIGNAL_RC=$?
set -e
[[ $SIGNAL_RC == 143 ]]
[[ $(<"$ROOTFS/sys/devices/system/cpu/cpufreq/policy0/scaling_governor") == schedutil ]]
[[ $(<"$ROOTFS/sys/devices/system/cpu/cpufreq/policy4/scaling_governor") == schedutil ]]
if find "$ROOTFS/run/rg40xxv/cpu-policy" -name '.governors.*' -print -quit | grep -q .; then
    printf 'FAIL: TERM 還原後留下 governor 快照\n' >&2
    exit 1
fi

if CPU_POLICY_TEST_FAIL_POLICY=policy4 "$CTL" run-performance -- true >/dev/null 2>&1; then
    printf 'FAIL: governor 寫入失敗時仍回報成功\n' >&2
    exit 1
fi
[[ $(<"$ROOTFS/sys/devices/system/cpu/cpufreq/policy0/scaling_governor") == schedutil ]]
[[ $(<"$ROOTFS/sys/devices/system/cpu/cpufreq/policy4/scaling_governor") == schedutil ]]

# 關屏省電會持久保存原 governor，重複 enter 不覆蓋原快照；喚醒精確還原。
"$CTL" lock-idle | grep -q '^governor=powersave$'
[[ $(<"$ROOTFS/sys/devices/system/cpu/cpufreq/policy0/scaling_governor") == powersave ]]
[[ $(<"$ROOTFS/sys/devices/system/cpu/cpufreq/policy4/scaling_governor") == powersave ]]
[[ -f "$ROOTFS/run/rg40xxv/cpu-policy/sleep-governors.v1" ]]
"$CTL" lock-idle | grep -q '^governor=powersave$'
"$CTL" unlock-idle | grep -q '^governor=restored$'
[[ $(<"$ROOTFS/sys/devices/system/cpu/cpufreq/policy0/scaling_governor") == schedutil ]]
[[ $(<"$ROOTFS/sys/devices/system/cpu/cpufreq/policy4/scaling_governor") == schedutil ]]
[[ ! -e "$ROOTFS/run/rg40xxv/cpu-policy/sleep-governors.v1" ]]
"$CTL" unlock-idle | grep -q '^governor=unchanged$'

# 還原第二個 policy 失敗時，第一個不得留在 schedutil 形成混合狀態；
# 兩者回到 powersave、保留原快照，下一次重試才刪除。
"$CTL" lock-idle >/dev/null
if CPU_POLICY_TEST_FAIL_RESTORE_POLICY=policy4 "$CTL" unlock-idle >/dev/null 2>&1; then
    printf 'FAIL: governor 部分還原失敗仍回報成功\n' >&2
    exit 1
fi
[[ $(<"$ROOTFS/sys/devices/system/cpu/cpufreq/policy0/scaling_governor") == powersave ]]
[[ $(<"$ROOTFS/sys/devices/system/cpu/cpufreq/policy4/scaling_governor") == powersave ]]
[[ -f "$ROOTFS/run/rg40xxv/cpu-policy/sleep-governors.v1" ]]
"$CTL" unlock-idle >/dev/null
[[ $(<"$ROOTFS/sys/devices/system/cpu/cpufreq/policy0/scaling_governor") == schedutil ]]
[[ $(<"$ROOTFS/sys/devices/system/cpu/cpufreq/policy4/scaling_governor") == schedutil ]]

# 睡眠中新增的 policy 不在原快照內；還原既有 policy 時不得改動它。
"$CTL" lock-idle >/dev/null
mkdir -p "$ROOTFS/sys/devices/system/cpu/cpufreq/policy8"
printf 'performance powersave schedutil' >"$ROOTFS/sys/devices/system/cpu/cpufreq/policy8/scaling_available_governors"
printf 'performance' >"$ROOTFS/sys/devices/system/cpu/cpufreq/policy8/scaling_governor"
printf '480000 1008000 1512000' >"$ROOTFS/sys/devices/system/cpu/cpufreq/policy8/scaling_available_frequencies"
printf '8' >"$ROOTFS/sys/devices/system/cpu/cpufreq/policy8/related_cpus"
"$CTL" unlock-idle >/dev/null
[[ $(<"$ROOTFS/sys/devices/system/cpu/cpufreq/policy0/scaling_governor") == schedutil ]]
[[ $(<"$ROOTFS/sys/devices/system/cpu/cpufreq/policy4/scaling_governor") == schedutil ]]
[[ $(<"$ROOTFS/sys/devices/system/cpu/cpufreq/policy8/scaling_governor") == performance ]]
rm -rf -- "$ROOTFS/sys/devices/system/cpu/cpufreq/policy8"

# 第二個 policy 寫入失敗時，第一個已切換的 policy 也必須回復。
if CPU_POLICY_TEST_FAIL_POLICY=policy4 "$CTL" lock-idle >/dev/null 2>&1; then
    printf 'FAIL: powersave 部分寫入失敗仍回報成功\n' >&2
    exit 1
fi
[[ $(<"$ROOTFS/sys/devices/system/cpu/cpufreq/policy0/scaling_governor") == schedutil ]]
[[ $(<"$ROOTFS/sys/devices/system/cpu/cpufreq/policy4/scaling_governor") == schedutil ]]
[[ ! -e "$ROOTFS/run/rg40xxv/cpu-policy/sleep-governors.v1" ]]
if find "$ROOTFS/sys/devices/system/cpu" -name online -o -name scaling_max_freq -o -name scaling_setspeed | grep -q .; then
    printf 'FAIL: fixture 意外出現禁止寫入節點\n' >&2
    exit 1
fi

printf 'PASS cpu-policy：schedutil UI、關屏 powersave、喚醒還原、遊戲 performance、1s lock timeout=%dms、失敗還原、公開 OPP、無超頻／hotplug\n' \
	"$LOCK_ELAPSED"
