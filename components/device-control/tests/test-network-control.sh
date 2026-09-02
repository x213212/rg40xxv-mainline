#!/usr/bin/env bash
set -euo pipefail

TEST_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(cd -- "$TEST_DIR/.." && pwd)
FIXTURE=$(mktemp -d /tmp/rg40xxv-device-control-test.network.XXXXXX)
ROOTFS=$FIXTURE/rootfs
MOCK_BIN=$FIXTURE/mock-bin
STATE=$ROOTFS/run/mock-network-state
KNOWN_UUID=550e8400-e29b-41d4-a716-446655440000
NEW_UUID=123e4567-e89b-42d3-a456-426614174000
trap 'rm -rf -- "$FIXTURE"' EXIT

mkdir -p "$ROOTFS/run" "$MOCK_BIN" \
	"$ROOTFS/sys/class/net/wlan0" \
	"$ROOTFS/sys/bus/platform/drivers/sunxi-mmc" \
	"$ROOTFS/sys/bus/platform/devices/4021000.mmc"
touch "$ROOTFS/sys/bus/platform/drivers/sunxi-mmc/bind" \
	"$ROOTFS/sys/bus/platform/drivers/sunxi-mmc/unbind" \
	"$ROOTFS/sys/bus/platform/drivers/sunxi-mmc/4021000.mmc"
printf '%s\n' connected >"$STATE"

MOCK_SYSTEMCTL=$MOCK_BIN/systemctl
{
	printf '#!/usr/bin/env bash\n'
	printf 'set -euo pipefail\n'
	printf 'printf "%%s\\n" "$*" >>"$DEVICE_CONTROL_TEST_ROOT/rootfs/run/systemctl.argv"\n'
} >"$MOCK_SYSTEMCTL"
chmod 0755 "$MOCK_SYSTEMCTL"

MOCK_RFKILL=$MOCK_BIN/rfkill
{
	printf '#!/usr/bin/env bash\n'
	printf 'set -euo pipefail\n'
	printf '[[ $# == 2 && $1 == unblock && $2 == wifi ]]\n'
	printf 'printf "%%s\\n" "$*" >>"$DEVICE_CONTROL_TEST_ROOT/rootfs/run/rfkill.argv"\n'
} >"$MOCK_RFKILL"
chmod 0755 "$MOCK_RFKILL"

MOCK_WIFI_SDIO=$MOCK_BIN/wifi-sdio-control
{
	printf '#!/usr/bin/env bash\n'
	printf 'set -euo pipefail\n'
	printf 'root=$DEVICE_CONTROL_TEST_ROOT/rootfs\n'
	printf 'driver=$root/sys/bus/platform/drivers/sunxi-mmc\n'
	printf '[[ $# == 2 && $2 == 4021000.mmc ]]\n'
	printf 'printf "%%s %%s\\n" "$1" "$2" >>"$root/run/wifi-sdio.argv"\n'
	printf 'case $1 in\n'
	printf '  unbind) rm -f -- "$driver/$2" ;;\n'
	printf '  bind)\n'
	printf '    : >"$driver/$2"\n'
	printf '    count=0\n'
	printf '    [[ ! -f "$root/run/wifi-sdio.bind-count" ]] || count=$(<"$root/run/wifi-sdio.bind-count")\n'
	printf '    count=$((count + 1))\n'
	printf '    printf "%%s\\n" "$count" >"$root/run/wifi-sdio.bind-count"\n'
	printf '    success_after=$(<"$root/run/wifi-sdio.success-after")\n'
	printf '    ((count < success_after)) || mkdir -p "$root/sys/class/net/wlan0"\n'
	printf '    ;;\n'
	printf '  *) exit 2 ;;\n'
	printf 'esac\n'
} >"$MOCK_WIFI_SDIO"
chmod 0755 "$MOCK_WIFI_SDIO"

MOCK_NMCLI=$MOCK_BIN/nmcli
{
	printf '#!/usr/bin/env bash\n'
	printf 'set -euo pipefail\n'
	printf 'root=$DEVICE_CONTROL_TEST_ROOT/rootfs\n'
	printf 'state=$(<"$root/run/mock-network-state")\n'
	printf 'known=550e8400-e29b-41d4-a716-446655440000\n'
	printf 'fresh=123e4567-e89b-42d3-a456-426614174000\n'
	printf 'printf "%%s\\n" "$*" >>"$root/run/nmcli.argv"\n'
	printf 'case "$*" in\n'
	printf '  "--terse --escape yes --fields UUID,TYPE connection show")\n'
	printf '    [[ ! -e "$root/run/known-deleted" ]] && printf "%%s:802-11-wireless\\n" "$known"\n'
	printf '    [[ $state == fresh || -e "$root/run/fresh-profile" ]] && printf "%%s:802-11-wireless\\n" "$fresh"\n'
	printf '    exit 0 ;;\n'
	printf '  "--get-values connection.id connection show uuid $fresh") printf "rg40xxv-wifi-112233445566\\n"; exit 0 ;;\n'
	printf '  "--get-values connection.id connection show uuid $known") printf "home-profile\\n"; exit 0 ;;\n'
	printf '  "--get-values 802-11-wireless.mode connection show uuid "*)\n'
	printf '    [[ ${*: -1} == "$fresh" || ${*: -1} == "$known" ]] && printf "infrastructure\\n" || printf "ap\\n"\n'
	printf '    exit 0 ;;\n'
	printf '  "--get-values 802-11-wireless.ssid connection show uuid "*)\n'
	printf '    case ${*: -1} in "$known") printf "Home:WiFi\\n" ;; "$fresh") printf "Cafe Net\\n" ;; *) printf "RG40XXV-Hotspot\\n" ;; esac\n'
	printf '    exit 0 ;;\n'
	printf '  "--get-values connection.type connection show uuid "*) printf "802-11-wireless\\n"; exit 0 ;;\n'
	printf '  "--get-values GENERAL.CON-UUID device show wlan0")\n'
	printf '    case $state in connected) printf "%%s\\n" "$known" ;; fresh) printf "%%s\\n" "$fresh" ;; hotspot) printf "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee\\n" ;; esac\n'
	printf '    exit 0 ;;\n'
	printf '  "--get-values GENERAL.STATE device show wlan0")\n'
	printf '    case $state in connected|fresh|hotspot) printf "100 (connected)\\n" ;; *) printf "30 (disconnected)\\n" ;; esac\n'
	printf '    exit 0 ;;\n'
	printf '  "--get-values IP4.ADDRESS[1] device show wlan0")\n'
	printf '    [[ $state == disconnected ]] || printf "192.168.0.125/24\\n"\n'
	printf '    exit 0 ;;\n'
	printf '  "radio wifi") printf "enabled\\n"; exit 0 ;;\n'
	printf '  *"--fields IN-USE,BSSID,SIGNAL,SECURITY,SSID device wifi list ifname wlan0 --rescan "*)\n'
	printf '    [[ ${*: -1} != yes || ! -e "$root/run/scan-fail" ]] || exit 9\n'
	printf '    if [[ $state == connected ]]; then active="*"; else active=""; fi\n'
	printf '    printf "%%s:AA\\:BB\\:CC\\:DD\\:EE\\:FF:78:WPA2:Home\\:WiFi\\n" "$active"\n'
	printf '    if [[ $state == fresh ]]; then active="*"; else active=""; fi\n'
	printf '    printf "%%s:11\\:22\\:33\\:44\\:55\\:66:64:WPA2:Cafe Net\\n" "$active"\n'
	printf '    printf ":22\\:33\\:44\\:55\\:66\\:77:42:--:Open Net\\n"\n'
	printf '    exit 0 ;;\n'
	printf '  "networking on"|"radio wifi on"|"connection reload") exit 0 ;;\n'
	printf '  "connection modify uuid "*) exit 0 ;;\n'
	printf '  "--wait 12 connection up uuid $known ifname wlan0"|"--wait 20 connection up uuid $known ifname wlan0")\n'
	printf '    printf "connected\\n" >"$root/run/mock-network-state"; exit 0 ;;\n'
	printf '  "--ask --wait 20 device wifi connect 11:22:33:44:55:66 ifname wlan0 name rg40xxv-wifi-112233445566")\n'
	printf '    IFS= read -r secret\n'
	printf '    : >"$root/run/fresh-profile"\n'
	printf '    [[ $secret == fixture-network-secret ]] || exit 10\n'
	printf '    printf "fresh\\n" >"$root/run/mock-network-state"\n'
	printf '    printf "secret-bytes=%%s\\n" "${#secret}" >"$root/run/nmcli.secret"\n'
	printf '    exit 0 ;;\n'
	printf '  "device disconnect wlan0") printf "disconnected\\n" >"$root/run/mock-network-state"; exit 0 ;;\n'
	printf '  "connection delete uuid $known") : >"$root/run/known-deleted"; exit 0 ;;\n'
	printf '  "connection delete uuid $fresh"|"connection delete id rg40xxv-wifi-112233445566") rm -f -- "$root/run/fresh-profile"; exit 0 ;;\n'
	printf '  "connection show id rg40xxv-hotspot") [[ -e "$root/run/hotspot-exists" ]]; exit ;;\n'
	printf '  "--wait 20 device wifi hotspot ifname wlan0 con-name rg40xxv-hotspot ssid RG40XXV-Hotspot")\n'
	printf '    : >"$root/run/hotspot-exists"; printf "hotspot\\n" >"$root/run/mock-network-state"; exit 0 ;;\n'
	printf '  "--wait 20 connection up id rg40xxv-hotspot ifname wlan0") printf "hotspot\\n" >"$root/run/mock-network-state"; exit 0 ;;\n'
	printf '  "connection down id rg40xxv-hotspot") printf "disconnected\\n" >"$root/run/mock-network-state"; exit 0 ;;\n'
	printf '  *) printf "unexpected nmcli argv: %%s\\n" "$*" >&2; exit 64 ;;\n'
	printf 'esac\n'
} >"$MOCK_NMCLI"
chmod 0755 "$MOCK_NMCLI"

export DEVICE_CONTROL_TESTING=1
export DEVICE_CONTROL_TEST_ROOT=$FIXTURE
CTL=$ROOT/network/rg40xxv-network-control
UI_DROP_IN=$ROOT/network/20-network-ready-ui.conf

expect_rejected() {
	if "$CTL" "$@" >/dev/null 2>&1; then
		printf 'FAIL: 應拒絕 argv：%s\n' "$*" >&2
		exit 1
	fi
}

expect_rejected connect '11:22:33:44:55:66;id'
expect_rejected connect 11:22:33:44:55
expect_rejected forget ../../etc/shadow
expect_rejected hotspot 'on;id'
expect_rejected scan now

grep -Fxq '[Unit]' "$UI_DROP_IN"
grep -Fxq 'Wants=rg40xxv-network-prepare.service' "$UI_DROP_IN"
if grep -Eq '^[[:space:]]*(After|Requires)=.*(rg40xxv-network-prepare|network-online|systemd-udev-settle)' "$UI_DROP_IN"; then
	printf 'FAIL: UI 不得被網路準備、network-online 或 udev-settle 排序阻塞\n' >&2
	exit 1
fi

"$CTL" prepare >"$FIXTURE/prepare.snapshot"
grep -Fxq $'RG40XXV_NETWORK_SNAPSHOT\t1' "$FIXTURE/prepare.snapshot"
grep -Eq $'^S\t1\t1\t1\t0\t550e8400-e29b-41d4-a716-446655440000\tAA:BB:CC:DD:EE:FF\tHome%3AWiFi\t192.168.0.125%2F24\t-$' "$FIXTURE/prepare.snapshot"
grep -Eq $'^A\tAA:BB:CC:DD:EE:FF\t78\tWPA2\tHome%3AWiFi\t1\t1\t550e8400-e29b-41d4-a716-446655440000$' "$FIXTURE/prepare.snapshot"
grep -Fxq 'unmask NetworkManager.service' "$ROOTFS/run/systemctl.argv"
grep -Fxq 'enable NetworkManager.service' "$ROOTFS/run/systemctl.argv"
grep -Fxq 'start NetworkManager.service' "$ROOTFS/run/systemctl.argv"
grep -Fxq 'unblock wifi' "$ROOTFS/run/rfkill.argv"
grep -Fq '802-11-wireless.powersave 2 connection.autoconnect yes connection.autoconnect-retries 0 ipv6.method disabled' "$ROOTFS/run/nmcli.argv"
grep -Fq -- '--rescan no' "$ROOTFS/run/nmcli.argv"
[[ ! -e $ROOTFS/run/wifi-sdio.argv ]]

"$CTL" status >"$FIXTURE/status.snapshot"
cmp -s "$FIXTURE/prepare.snapshot" "$FIXTURE/status.snapshot"

: >"$ROOTFS/run/systemctl.argv"
: >"$ROOTFS/run/rfkill.argv"
: >"$ROOTFS/run/nmcli.argv"
: >"$ROOTFS/run/scan-fail"
if "$CTL" scan >"$FIXTURE/failed-scan.snapshot" 2>/dev/null; then
	printf 'FAIL: nmcli rescan 失敗不得回報成功\n' >&2
	exit 1
fi
cmp -s "$FIXTURE/status.snapshot" \
	"$ROOTFS/run/rg40xxv/network/snapshot.v1"
rm -f -- "$ROOTFS/run/scan-fail"
: >"$ROOTFS/run/systemctl.argv"
: >"$ROOTFS/run/rfkill.argv"
: >"$ROOTFS/run/nmcli.argv"
"$CTL" scan >"$FIXTURE/scan.snapshot"
grep -Fq -- '--rescan yes' "$ROOTFS/run/nmcli.argv"
grep -Eq $'^A\t11:22:33:44:55:66\t64\tWPA2\tCafe%20Net\t0\t0\t-$' "$FIXTURE/scan.snapshot"
[[ ! -s $ROOTFS/run/systemctl.argv ]]
[[ ! -s $ROOTFS/run/rfkill.argv ]]
! grep -Fxq 'networking on' "$ROOTFS/run/nmcli.argv"
! grep -Fxq 'radio wifi on' "$ROOTFS/run/nmcli.argv"
! grep -Fxq 'connection reload' "$ROOTFS/run/nmcli.argv"
[[ $(<"$STATE") == connected ]]

# A rejected WPA secret must not poison the next attempt. NetworkManager may
# create the fixed-name profile before activation fails; the helper removes
# exactly that candidate and leaves unrelated saved profiles untouched.
if printf '%s\n' definitely-wrong-password | \
	"$CTL" connect 11:22:33:44:55:66 >"$FIXTURE/wrong.snapshot" \
	2>"$FIXTURE/wrong.stderr"; then
	printf 'FAIL: 錯誤 WPA 密碼仍回報成功\n' >&2
	exit 1
fi
[[ ! -e $ROOTFS/run/fresh-profile ]]
grep -Fq '已清除失敗的候選 profile' "$FIXTURE/wrong.stderr"
grep -Fxq 'connection delete id rg40xxv-wifi-112233445566' \
	"$ROOTFS/run/nmcli.argv"

printf '%s\n' fixture-network-secret | \
	"$CTL" connect 11:22:33:44:55:66 >"$FIXTURE/connect.snapshot"
[[ $(<"$ROOTFS/run/nmcli.secret") == secret-bytes=22 ]]
! grep -Fq fixture-network-secret "$ROOTFS/run/nmcli.argv"
grep -Fq 'connection modify uuid 123e4567-e89b-42d3-a456-426614174000 802-11-wireless.powersave 2 connection.autoconnect yes connection.autoconnect-retries 0 ipv6.method disabled' \
	"$ROOTFS/run/nmcli.argv"
grep -Eq $'^S\t1\t1\t1\t0\t123e4567-e89b-42d3-a456-426614174000\t11:22:33:44:55:66\tCafe%20Net\t192.168.0.125%2F24\t-$' "$FIXTURE/connect.snapshot"

"$CTL" disconnect >/dev/null
[[ $(<"$STATE") == disconnected ]]
rm -f -- "$ROOTFS/run/fresh-profile"
"$CTL" recover >/dev/null
[[ $(<"$STATE") == connected ]]
"$CTL" forget "$KNOWN_UUID" >/dev/null
[[ -e $ROOTFS/run/known-deleted ]]

"$CTL" hotspot on >"$FIXTURE/hotspot.snapshot"
grep -Eq $'^S\t1\t1\t1\t1\taaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee\t-\t-\t192.168.0.125%2F24\tRG40XXV%2DHotspot$' "$FIXTURE/hotspot.snapshot"
"$CTL" hotspot off >/dev/null
[[ $(<"$STATE") == disconnected ]]

[[ -f $ROOTFS/run/rg40xxv/network/snapshot.v1 ]]
[[ ! -L $ROOTFS/run/rg40xxv/network/snapshot.v1 ]]
[[ $(stat -c %a "$ROOTFS/run/rg40xxv/network/snapshot.v1") == 600 ]]
! find "$ROOTFS/run/rg40xxv/network" -maxdepth 1 -name '.snapshot.*' -print -quit | grep -q .

grep -Fxq 'WIFI_SDIO_DEVICE=4021000.mmc' "$CTL"
grep -Fxq 'WIFI_SDIO_RECOVERY_ATTEMPTS=2' "$CTL"
grep -Fq 'sleep 0.25' "$CTL"

# A missing wlan0 must reset only the dedicated Wi-Fi SDIO host.  The first
# fixture succeeds on the first bind and proves the normal recovery path.
printf '%s\n' connected >"$STATE"
rm -f -- "$ROOTFS/run/known-deleted" "$ROOTFS/run/hotspot-exists"
rm -rf -- "$ROOTFS/sys/class/net/wlan0"
: >"$ROOTFS/sys/bus/platform/drivers/sunxi-mmc/4021000.mmc"
rm -f -- "$ROOTFS/run/wifi-sdio.argv" "$ROOTFS/run/wifi-sdio.bind-count"
printf '%s\n' 1 >"$ROOTFS/run/wifi-sdio.success-after"
"$CTL" recover >"$FIXTURE/recover-sdio-first.snapshot" \
	2>"$FIXTURE/recover-sdio-first.stderr"
cmp -s "$FIXTURE/status.snapshot" "$FIXTURE/recover-sdio-first.snapshot"
grep -Fxq 'rg40xxv-network-control: Wi-Fi SDIO recovery PASS attempt=1' \
	"$FIXTURE/recover-sdio-first.stderr"
printf '%s\n' 'unbind 4021000.mmc' 'bind 4021000.mmc' \
	>"$FIXTURE/recover-sdio-first.expected"
cmp -s "$FIXTURE/recover-sdio-first.expected" "$ROOTFS/run/wifi-sdio.argv"

# If the first bind still has no netdev, retry exactly once after another
# complete unbind/off-delay/bind cycle.
rm -rf -- "$ROOTFS/sys/class/net/wlan0"
: >"$ROOTFS/sys/bus/platform/drivers/sunxi-mmc/4021000.mmc"
rm -f -- "$ROOTFS/run/wifi-sdio.argv" "$ROOTFS/run/wifi-sdio.bind-count"
printf '%s\n' 2 >"$ROOTFS/run/wifi-sdio.success-after"
"$CTL" recover >"$FIXTURE/recover-sdio-second.snapshot" \
	2>"$FIXTURE/recover-sdio-second.stderr"
cmp -s "$FIXTURE/status.snapshot" "$FIXTURE/recover-sdio-second.snapshot"
grep -Fxq 'rg40xxv-network-control: Wi-Fi SDIO recovery PASS attempt=2' \
	"$FIXTURE/recover-sdio-second.stderr"
printf '%s\n' 'unbind 4021000.mmc' 'bind 4021000.mmc' \
	'unbind 4021000.mmc' 'bind 4021000.mmc' \
	>"$FIXTURE/recover-sdio-second.expected"
cmp -s "$FIXTURE/recover-sdio-second.expected" "$ROOTFS/run/wifi-sdio.argv"

# Two failed binds must be surfaced instead of looping forever or publishing a
# false successful empty snapshot.
rm -rf -- "$ROOTFS/sys/class/net/wlan0"
: >"$ROOTFS/sys/bus/platform/drivers/sunxi-mmc/4021000.mmc"
rm -f -- "$ROOTFS/run/wifi-sdio.argv" "$ROOTFS/run/wifi-sdio.bind-count"
printf '%s\n' 99 >"$ROOTFS/run/wifi-sdio.success-after"
if "$CTL" recover >"$FIXTURE/recover-sdio-fail.snapshot" \
	2>"$FIXTURE/recover-sdio-fail.stderr"; then
	printf 'FAIL: wlan0 缺失且兩次重綁失敗時不得回報成功\n' >&2
	exit 1
fi
grep -Fxq 'rg40xxv-network-control: Wi-Fi SDIO recovery FAIL attempts=2' \
	"$FIXTURE/recover-sdio-fail.stderr"
grep -Fxq 'rg40xxv-network-control: 找不到 wlan0，Wi-Fi SDIO 重置兩次仍失敗' \
	"$FIXTURE/recover-sdio-fail.stderr"
cmp -s "$FIXTURE/recover-sdio-second.expected" "$ROOTFS/run/wifi-sdio.argv"

# A stale NetworkManager device row cannot make an absent sysfs wlan0 look
# present. This status path is read-only and must publish an empty truthful
# snapshot instead of attempting a scan or radio restart.
printf '%s\n' connected >"$STATE"
rm -rf -- "$ROOTFS/sys/class/net/wlan0"
"$CTL" status >"$FIXTURE/status-no-wlan.snapshot"
grep -Fxq $'S\t0\t0\t0\t0\t-\t-\t-\t-\t-' \
	"$FIXTURE/status-no-wlan.snapshot"
! grep -q $'^A\t' "$FIXTURE/status-no-wlan.snapshot"

printf 'PASS network-control：NM prepare、非干擾 rescan、scan 錯誤不假成功、錯誤 PSK profile 清除後可重試、status/scan snapshot、stdin password、connect/disconnect/forget、hotspot toggle、固定 argv、UI 無網路啟動 blocker、wlan0 缺失不假報且專用 SDIO host 最多兩次安全重綁\n'
