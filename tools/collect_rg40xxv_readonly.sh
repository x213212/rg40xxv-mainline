#!/bin/sh

# Read-only inventory for an owner-controlled Anbernic RG40XX V.
# It deliberately avoids Wi-Fi profiles, credentials, ROMs, saves, and SID/eFuse dumps.

section() {
    printf '\n[%s]\n' "$1"
}

section SYSTEM
uname -a
cat /proc/version 2>/dev/null
cat /etc/os-release 2>/dev/null
printf 'cmdline=' && cat /proc/cmdline 2>/dev/null
printf 'model=' && tr '\000' '\n' </proc/device-tree/model 2>/dev/null
printf 'compatible=' && tr '\000' '\n' </proc/device-tree/compatible 2>/dev/null

section CPU
lscpu 2>/dev/null || cat /proc/cpuinfo
for policy in /sys/devices/system/cpu/cpufreq/policy* /sys/devices/system/cpu/cpu0/cpufreq; do
    [ -d "$policy" ] || continue
    printf 'policy=%s\n' "$policy"
    for field in affected_cpus related_cpus scaling_driver scaling_available_frequencies scaling_available_governors cpuinfo_min_freq cpuinfo_max_freq scaling_min_freq scaling_max_freq scaling_cur_freq scaling_governor; do
        [ -r "$policy/$field" ] || continue
        printf '%s=' "$field"
        cat "$policy/$field"
    done
done
[ -r /sys/devices/system/cpu/cpufreq/boost ] && {
    printf 'boost='
    cat /sys/devices/system/cpu/cpufreq/boost
}

section CPU_IDLE
for field in /sys/devices/system/cpu/cpuidle/current_driver /sys/devices/system/cpu/cpuidle/current_governor; do
    [ -r "$field" ] && printf '%s=' "$field" && cat "$field"
done
for state in /sys/devices/system/cpu/cpu0/cpuidle/state*; do
    [ -d "$state" ] || continue
    printf 'state=%s\n' "$state"
    for field in name desc latency residency usage time; do
        [ -r "$state/$field" ] && printf '%s=' "$field" && cat "$state/$field"
    done
done

section THERMAL
for zone in /sys/class/thermal/thermal_zone*; do
    [ -d "$zone" ] || continue
    printf 'zone=%s\n' "$zone"
    for field in type temp policy mode; do
        [ -r "$zone/$field" ] && printf '%s=' "$field" && cat "$zone/$field"
    done
done
for cooler in /sys/class/thermal/cooling_device*; do
    [ -d "$cooler" ] || continue
    printf 'cooler=%s\n' "$cooler"
    for field in type cur_state max_state; do
        [ -r "$cooler/$field" ] && printf '%s=' "$field" && cat "$cooler/$field"
    done
done

section DEVFREQ
for dev in /sys/class/devfreq/*; do
    [ -d "$dev" ] || continue
    printf 'device=%s\n' "$dev"
    for field in name governor available_frequencies cur_freq min_freq max_freq; do
        [ -r "$dev/$field" ] && printf '%s=' "$field" && cat "$dev/$field"
    done
done

section MEMORY_STORAGE
head -n 20 /proc/meminfo 2>/dev/null
lsblk -o NAME,SIZE,FSTYPE,LABEL,MOUNTPOINTS 2>/dev/null
df -hT 2>/dev/null

section MEDIA_GPU
ls -l /dev/dri /dev/video* /dev/cedar_dev /dev/mali0 2>/dev/null
lsmod 2>/dev/null

section RUNNING_SERVICES
systemctl --no-pager --plain --type=service --state=running 2>/dev/null

section PROCESSES
ps -eo pid,ppid,pcpu,pmem,rss,stat,comm,args --sort=-pcpu 2>/dev/null | head -n 100

section KERNEL_CONFIG
if [ -r /proc/config.gz ]; then
    zgrep -E 'CONFIG_(CC_VERSION_TEXT|CC_OPTIMIZE|PREEMPT|HZ_|HZ=|CPU_FREQ|CPU_IDLE|PM_OPP|THERMAL|DEVFREQ|MALI|PANFROST|SUNXI|CEDAR|V4L2|IKCONFIG|DEBUG_KERNEL|FRAME_POINTER|ZRAM|SWAP)=' /proc/config.gz 2>/dev/null
else
    printf 'no /proc/config.gz\n'
fi

section RELEVANT_DMESG
dmesg 2>/dev/null | grep -Ei 'Linux version|Machine model|sunxi|allwinner|cpufreq|opp|thermal|mali|panfrost|cedar|vpu|video|drm|display|panel|8821|wifi|wlan|mmc|axp' | tail -n 240
