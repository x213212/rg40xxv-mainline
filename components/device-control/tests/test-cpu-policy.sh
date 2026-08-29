#!/usr/bin/env bash
set -euo pipefail

TEST_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(cd -- "$TEST_DIR/.." && pwd)
FIXTURE=$(mktemp -d /tmp/rg40xxv-device-control-test.cpu.XXXXXX)
ROOTFS="$FIXTURE/rootfs"
trap 'rm -rf -- "$FIXTURE"' EXIT

for policy in policy0 policy4; do
    mkdir -p "$ROOTFS/sys/devices/system/cpu/cpufreq/$policy"
    printf 'performance schedutil' >"$ROOTFS/sys/devices/system/cpu/cpufreq/$policy/scaling_available_governors"
    printf 'performance' >"$ROOTFS/sys/devices/system/cpu/cpufreq/$policy/scaling_governor"
    printf '480000 1008000 1512000' >"$ROOTFS/sys/devices/system/cpu/cpufreq/$policy/scaling_available_frequencies"
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

BEFORE=$(sha256sum "$ROOTFS/sys/devices/system/cpu/cpufreq/"policy*/scaling_governor)
"$CTL" lock-idle | grep -q '^governor=unchanged$'
AFTER=$(sha256sum "$ROOTFS/sys/devices/system/cpu/cpufreq/"policy*/scaling_governor)
[[ $BEFORE == "$AFTER" ]]
if find "$ROOTFS/sys/devices/system/cpu" -name online -o -name scaling_max_freq -o -name scaling_setspeed | grep -q .; then
    printf 'FAIL: fixture 意外出現禁止寫入節點\n' >&2
    exit 1
fi

printf 'PASS cpu-policy：schedutil UI、遊戲 performance、失敗還原、公開 OPP、無超頻／hotplug\n'
