#!/usr/bin/env bash
set -euo pipefail

TEST_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(cd -- "$TEST_DIR/.." && pwd)
FIXTURE=$(mktemp -d /tmp/rg40xxv-device-control-test.vpn.XXXXXX)
ROOTFS="$FIXTURE/rootfs"
trap 'rm -rf -- "$FIXTURE"' EXIT
mkdir -p "$ROOTFS"

OVPN="$FIXTURE/client.ovpn"
CA="$FIXTURE/ca.crt"
CERT="$FIXTURE/client.crt"
KEY="$FIXTURE/client.key"
AUTH="$FIXTURE/auth.txt"
{
    printf 'client\ndev tun\nproto udp\nremote 198.51.100.20 1194\n'
    printf 'ca old-ca.crt\ncert old.crt\nkey old.key\nauth-user-pass old-auth.txt\n'
    printf 'persist-key\npersist-tun\nremote-cert-tls server\n'
} >"$OVPN"
printf '%s\n' '-----BEGIN CERTIFICATE-----' 'CA-DATA' '-----END CERTIFICATE-----' >"$CA"
printf '%s\n' '-----BEGIN CERTIFICATE-----' 'CERT-DATA' '-----END CERTIFICATE-----' >"$CERT"
printf '%s\n' "-----BEGIN $(printf 'PRIVATE')  KEY-----" 'PRIVATE-KEY-DATA' "-----END $(printf 'PRIVATE')  KEY-----" >"$KEY"
printf 'vpn-user\nsuper-secret-password\n' >"$AUTH"

export DEVICE_CONTROL_TESTING=1
export DEVICE_CONTROL_TEST_ROOT="$FIXTURE"
CTL="$ROOT/vpn/vpn-profilectl"
FW="$ROOT/vpn/vpn-firewall"

IMPORT_OUTPUT=$("$CTL" import home --ovpn "$OVPN" --ca "$CA" --cert "$CERT" --key "$KEY" --auth "$AUTH")
if grep -q 'super-secret-password' <<<"$IMPORT_OUTPUT"; then
    printf 'FAIL: import output 洩漏密碼\n' >&2
    exit 1
fi
PROFILE="$ROOTFS/var/lib/rg40xxv/openvpn/profiles/home"
[[ $(stat -c '%a' "$PROFILE") == 700 ]]
for file in "$PROFILE"/*; do [[ $(stat -c '%a' "$file") == 600 ]]; done
grep -q '^ca ca.crt$' "$PROFILE/client.conf"
grep -q '^auth-user-pass auth.txt$' "$PROFILE/client.conf"
if grep -q 'old-auth' "$PROFILE/client.conf"; then exit 1; fi
VERIFY_OUTPUT=$("$CTL" verify home)
if grep -q 'super-secret-password' <<<"$VERIFY_OUTPUT"; then exit 1; fi
grep -q '^autoconnect=true$' <<<"$VERIFY_OUTPUT"
grep -q '^kill_switch=false$' <<<"$VERIFY_OUTPUT"

BAD="$FIXTURE/bad.ovpn"
printf 'client\nscript-security 2\nup /tmp/evil\n' >"$BAD"
if "$CTL" import evil --ovpn "$BAD" >/dev/null 2>&1; then
    printf 'FAIL: 接受危險 ovpn directive\n' >&2
    exit 1
fi

if "$CTL" profile set home kill-switch true >/dev/null 2>&1; then
    printf 'FAIL: 無 endpoint 仍允許 kill switch\n' >&2
    exit 1
fi
"$CTL" profile set home endpoint 203.0.113.9 1194 udp >/dev/null
if "$CTL" profile set home kill-switch true >/dev/null 2>&1; then
    printf 'FAIL: endpoint 與 ovpn remote 不符仍允許 kill switch\n' >&2
    exit 1
fi
"$CTL" profile set home endpoint 198.51.100.20 1194 udp >/dev/null
"$CTL" profile set home kill-switch true >/dev/null
NFT_LOG="$FIXTURE/nft.log"
export VPN_TEST_NFT_LOG="$NFT_LOG"
FW_OUTPUT=$("$FW" apply home)
grep -q '^kill_switch=enabled$' <<<"$FW_OUTPUT"
grep -q 'oifname "lo" accept' "$NFT_LOG"
grep -q 'ip daddr 192.168.0.0/24 accept' "$NFT_LOG"
grep -q 'oifname "tun0" accept' "$NFT_LOG"
grep -q 'ip daddr 198.51.100.20 udp dport 1194 accept' "$NFT_LOG"
grep -q 'policy drop' "$NFT_LOG"
if grep -q 'established,related accept' "$NFT_LOG"; then exit 1; fi
if grep -q 'super-secret-password' "$NFT_LOG"; then exit 1; fi

[[ $("$CTL" policy get moonlight) == bypass ]]
[[ $("$CTL" policy get games) == vpn ]]
"$CTL" policy set moonlight vpn >/dev/null
"$CTL" policy set games bypass >/dev/null
[[ $("$CTL" policy get moonlight) == vpn ]]
[[ $("$CTL" policy get games) == bypass ]]

"$CTL" profile set home kill-switch false >/dev/null
"$FW" apply home | grep -q '^kill_switch=disabled$'
grep -q 'delete table inet rg40xxv_vpn' "$NFT_LOG"

printf 'PASS vpn：安全匯入、0600、危險 directive 拒絕、重連契約、kill switch/LAN SSH、流量 policy\n'
