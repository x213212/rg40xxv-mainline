#!/bin/sh
set -eu

NETSTREAM_TEST_BIN=${1:?usage: test_cli.sh /absolute/path/to/netstreamctl}
NETSTREAM_TEST_ROOT=$(mktemp -d /tmp/netstream-cli-XXXXXX)
NETSTREAM_TEST_STATE="$NETSTREAM_TEST_ROOT/state"
NETSTREAM_TEST_SECRET="$NETSTREAM_TEST_ROOT/secret"
NETSTREAM_TEST_VENDOR="$NETSTREAM_TEST_ROOT/vendor.wifi"
NETSTREAM_TEST_OUT="$NETSTREAM_TEST_ROOT/stdout"
NETSTREAM_TEST_ERR="$NETSTREAM_TEST_ROOT/stderr"
NETSTREAM_TEST_SECRET_LINK="$NETSTREAM_TEST_ROOT/secret-link"

cleanup_netstream_test() {
    rm -f "$NETSTREAM_TEST_STATE/wifi.v1" \
        "$NETSTREAM_TEST_STATE/hosts.v1" \
        "$NETSTREAM_TEST_STATE/.lock" \
        "$NETSTREAM_TEST_SECRET" \
        "$NETSTREAM_TEST_VENDOR" \
        "$NETSTREAM_TEST_OUT" \
        "$NETSTREAM_TEST_ERR" \
        "$NETSTREAM_TEST_SECRET_LINK"
    rmdir "$NETSTREAM_TEST_STATE" 2>/dev/null || true
    rmdir "$NETSTREAM_TEST_ROOT" 2>/dev/null || true
}
trap cleanup_netstream_test EXIT HUP INT TERM

fail_netstream_test() {
    printf 'FAIL: %s\n' "$1" >&2
    exit 1
}

mkdir -m 0700 "$NETSTREAM_TEST_STATE"
(umask 077 && printf '%s\n' 'cli-super-secret' >"$NETSTREAM_TEST_SECRET")

"$NETSTREAM_TEST_BIN" --state-dir "$NETSTREAM_TEST_STATE" wifi set \
    --ssid '家用網路' --security wpa2-psk \
    --password-file "$NETSTREAM_TEST_SECRET" --make-default \
    >"$NETSTREAM_TEST_OUT" 2>"$NETSTREAM_TEST_ERR"

printf '%s\n' 'guest-password' | \
    "$NETSTREAM_TEST_BIN" --state-dir "$NETSTREAM_TEST_STATE" wifi set \
        --ssid Guest --security wpa3-sae --password-stdin --priority -10 \
        >>"$NETSTREAM_TEST_OUT" 2>>"$NETSTREAM_TEST_ERR"

"$NETSTREAM_TEST_BIN" --state-dir "$NETSTREAM_TEST_STATE" wifi list \
    >"$NETSTREAM_TEST_OUT" 2>"$NETSTREAM_TEST_ERR"
grep -F '家用網路' "$NETSTREAM_TEST_OUT" >/dev/null || \
    fail_netstream_test 'Traditional Chinese SSID was not listed'
grep -F 'Guest' "$NETSTREAM_TEST_OUT" >/dev/null || \
    fail_netstream_test 'second Wi-Fi profile was not listed'
if grep -F 'cli-super-secret' "$NETSTREAM_TEST_OUT" "$NETSTREAM_TEST_ERR" >/dev/null || \
   grep -F 'guest-password' "$NETSTREAM_TEST_OUT" "$NETSTREAM_TEST_ERR" >/dev/null; then
    fail_netstream_test 'Wi-Fi secret appeared in command output'
fi
[ "$(stat -c '%a' "$NETSTREAM_TEST_STATE/wifi.v1")" = 600 ] || \
    fail_netstream_test 'wifi.v1 is not mode 0600'
[ "$(stat -c '%a' "$NETSTREAM_TEST_STATE/.lock")" = 600 ] || \
    fail_netstream_test '.lock is not mode 0600'
[ "$(stat -c '%a' "$NETSTREAM_TEST_STATE")" = 700 ] || \
    fail_netstream_test 'state directory is not mode 0700'

"$NETSTREAM_TEST_BIN" --state-dir "$NETSTREAM_TEST_STATE" wifi forget \
    --ssid Guest >/dev/null
"$NETSTREAM_TEST_BIN" --state-dir "$NETSTREAM_TEST_STATE" wifi list \
    >"$NETSTREAM_TEST_OUT"
grep -F '家用網路' "$NETSTREAM_TEST_OUT" >/dev/null || \
    fail_netstream_test 'forget removed the wrong Wi-Fi profile'
if grep -F 'Guest' "$NETSTREAM_TEST_OUT" >/dev/null; then
    fail_netstream_test 'forget did not remove the selected Wi-Fi profile'
fi
if "$NETSTREAM_TEST_BIN" --state-dir "$NETSTREAM_TEST_STATE" wifi forget \
    --ssid Missing >"$NETSTREAM_TEST_OUT" 2>"$NETSTREAM_TEST_ERR"; then
    fail_netstream_test 'forget accepted a missing Wi-Fi profile'
fi
"$NETSTREAM_TEST_BIN" --state-dir "$NETSTREAM_TEST_STATE" wifi list \
    >"$NETSTREAM_TEST_OUT"
grep -F '家用網路' "$NETSTREAM_TEST_OUT" >/dev/null || \
    fail_netstream_test 'failed forget modified remaining profiles'

"$NETSTREAM_TEST_BIN" --state-dir "$NETSTREAM_TEST_STATE" host set \
    --name '客廳電腦' --address sunshine.local --make-default >/dev/null
"$NETSTREAM_TEST_BIN" --state-dir "$NETSTREAM_TEST_STATE" host list \
    >"$NETSTREAM_TEST_OUT"
grep -F '客廳電腦' "$NETSTREAM_TEST_OUT" >/dev/null || \
    fail_netstream_test 'Traditional Chinese host name was not listed'
grep -F '640' "$NETSTREAM_TEST_OUT" >/dev/null || \
    fail_netstream_test 'default width was not listed'
grep -F '480' "$NETSTREAM_TEST_OUT" >/dev/null || \
    fail_netstream_test 'default height was not listed'
grep -F 'H264' "$NETSTREAM_TEST_OUT" >/dev/null || \
    fail_netstream_test 'default codec was not listed'
grep -F 'fit' "$NETSTREAM_TEST_OUT" >/dev/null || \
    fail_netstream_test 'default aspect was not listed'

"$NETSTREAM_TEST_BIN" --state-dir "$NETSTREAM_TEST_STATE" host set \
    --name '客廳電腦' --paired true --last-used 1777777777 \
    --width 1024 --height 600 --custom true --fps 120 \
    --bitrate 42000 --packet-size 1200 --codec H265 --aspect fill >/dev/null
"$NETSTREAM_TEST_BIN" --state-dir "$NETSTREAM_TEST_STATE" host list \
    >"$NETSTREAM_TEST_OUT"
grep -F '1024' "$NETSTREAM_TEST_OUT" >/dev/null || \
    fail_netstream_test 'custom host update was not persisted'
grep -F 'H265' "$NETSTREAM_TEST_OUT" >/dev/null || \
    fail_netstream_test 'codec update was not persisted'
[ "$(stat -c '%a' "$NETSTREAM_TEST_STATE/hosts.v1")" = 600 ] || \
    fail_netstream_test 'hosts.v1 is not mode 0600'

if "$NETSTREAM_TEST_BIN" --state-dir "$NETSTREAM_TEST_STATE" host set \
    --name Bad --address 'host;reboot' >"$NETSTREAM_TEST_OUT" 2>"$NETSTREAM_TEST_ERR"; then
    fail_netstream_test 'shell-like host address passed validation'
fi
if "$NETSTREAM_TEST_BIN" --state-dir "$NETSTREAM_TEST_STATE" host set \
    --name Bad --address host.local --width 1024 \
    >"$NETSTREAM_TEST_OUT" 2>"$NETSTREAM_TEST_ERR"; then
    fail_netstream_test 'half of a resolution pair passed validation'
fi

printf '%s\n' 'S:原廠網路	P:factory-plain-secret' >"$NETSTREAM_TEST_VENDOR"
chmod 0644 "$NETSTREAM_TEST_VENDOR"
"$NETSTREAM_TEST_BIN" --state-dir "$NETSTREAM_TEST_STATE" wifi import-vendor \
    --file "$NETSTREAM_TEST_VENDOR" --make-default \
    >"$NETSTREAM_TEST_OUT" 2>"$NETSTREAM_TEST_ERR"
grep -F 'imported=1' "$NETSTREAM_TEST_OUT" >/dev/null || \
    fail_netstream_test 'vendor import count is wrong'
grep -F '0644' "$NETSTREAM_TEST_ERR" >/dev/null || \
    fail_netstream_test 'vendor plaintext permission warning is missing'
if grep -F 'factory-plain-secret' "$NETSTREAM_TEST_OUT" "$NETSTREAM_TEST_ERR" >/dev/null; then
    fail_netstream_test 'vendor password appeared in import output'
fi

if "$NETSTREAM_TEST_BIN" --state-dir "$NETSTREAM_TEST_STATE" wifi set \
    --ssid Mistake --security wpa2-psk --password accidental-secret \
    >"$NETSTREAM_TEST_OUT" 2>"$NETSTREAM_TEST_ERR"; then
    fail_netstream_test 'unsupported command-line password was accepted'
fi
if grep -F 'accidental-secret' "$NETSTREAM_TEST_OUT" "$NETSTREAM_TEST_ERR" >/dev/null; then
    fail_netstream_test 'accidental command-line password appeared in an error'
fi

chmod 0644 "$NETSTREAM_TEST_SECRET"
if "$NETSTREAM_TEST_BIN" --state-dir "$NETSTREAM_TEST_STATE" wifi set \
    --ssid Rejected --security wpa2-psk \
    --password-file "$NETSTREAM_TEST_SECRET" \
    >"$NETSTREAM_TEST_OUT" 2>"$NETSTREAM_TEST_ERR"; then
    fail_netstream_test 'world-readable password file was accepted'
fi
ln -s "$NETSTREAM_TEST_SECRET" "$NETSTREAM_TEST_SECRET_LINK"
if "$NETSTREAM_TEST_BIN" --state-dir "$NETSTREAM_TEST_STATE" wifi set \
    --ssid Rejected --security wpa2-psk \
    --password-file "$NETSTREAM_TEST_SECRET_LINK" \
    >"$NETSTREAM_TEST_OUT" 2>"$NETSTREAM_TEST_ERR"; then
    fail_netstream_test 'symlink password file was accepted'
fi

printf 'PASS: CLI persistence and security checks\n'
