#!/bin/sh
set -eu

project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
ensure=$project/bin/rg40xxv-youtube-resolver-ensure
fixture=$(mktemp -d /tmp/rg40xxv-youtube-resolver-ensure.XXXXXXXX)
child_pid=
cleanup()
{
	if [ -n "$child_pid" ]; then
		kill "$child_pid" 2>/dev/null || true
		wait "$child_pid" 2>/dev/null || true
	fi
	find "$fixture" -mindepth 1 -delete 2>/dev/null || true
	rmdir "$fixture" 2>/dev/null || true
}
trap cleanup EXIT HUP INT TERM

root=$fixture/component
state=$fixture/state
runtime=$fixture/runtime
cache=$state/.cache/rg40xxv-youtube/yt-dlp
socket_path=$runtime/yt-dlp.sock
fake_bin=$fixture/bin
run_log=$fixture/systemd-run.log
install -d -m 0755 "$root/tools" "$root/vendor" "$fake_bin"

cat >"$root/tools/yt_dlp_server.py" <<'PY'
#!/usr/bin/python3
import argparse
import json
import os
import signal
import socket

parser = argparse.ArgumentParser()
parser.add_argument("--runtime-root")
parser.add_argument("--cache-dir")
parser.add_argument("--socket", required=True)
parser.add_argument("--yt-dlp")
parser.add_argument("--workers")
args = parser.parse_args()
listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
listener.bind(args.socket)
os.chmod(args.socket, 0o600)
listener.listen(4)
running = True
def stop(_signum, _frame):
    global running
    running = False
    listener.close()
for signum in (signal.SIGTERM, signal.SIGINT):
    signal.signal(signum, stop)
while running:
    try:
        client, _ = listener.accept()
    except OSError:
        break
    try:
        client.recv(4096)
        client.sendall(b'{"version":1,"ok":true,"workers":2}\n')
    finally:
        client.close()
try:
    os.unlink(args.socket)
except FileNotFoundError:
    pass
PY
cat >"$root/vendor/yt-dlp" <<'SH'
#!/bin/sh
exit 0
SH
cat >"$fake_bin/systemctl" <<'SH'
#!/bin/sh
set -eu
case "$1" in
	is-active)
		[ "${RG_TEST_ACTIVE:-0}" = 1 ]
		;;
	show)
		printf '%s\n' "${RG_TEST_PID:-0}"
		;;
	*) exit 64 ;;
esac
SH
cat >"$fake_bin/systemd-run" <<'SH'
#!/bin/sh
set -eu
printf '%s\n' "$@" >"$RG_TEST_SYSTEMD_RUN_LOG"
exit "${RG_TEST_SYSTEMD_RUN_STATUS:-0}"
SH
chmod 0755 "$root/tools/yt_dlp_server.py" "$root/vendor/yt-dlp" \
	"$fake_bin/systemctl" "$fake_bin/systemd-run"

run_ensure()
{
	PATH=$fake_bin:/usr/bin:/bin \
	RG_YOUTUBE_TEXTURE_ROOT=$root \
	RG_YOUTUBE_STATE_HOME=$state \
	RG_YOUTUBE_RUNTIME_ROOT=$runtime \
	RG_YOUTUBE_PYTHON=/usr/bin/python3 \
	RG_TEST_SYSTEMD_RUN_LOG=$run_log \
	RG_TEST_ACTIVE=${RG_TEST_ACTIVE:-0} \
	RG_TEST_PID=${RG_TEST_PID:-0} \
		"$ensure"
}

start_output=$(run_ensure)
printf '%s\n' "$start_output" | \
	grep -Fqx 'YOUTUBE_RESOLVER_ENSURE result=PASS action=START unit=rg40xxv-youtube-resolver.service'
grep -Fqx -- '--property=KillMode=control-group' "$run_log"
grep -Fqx -- '--property=Restart=on-failure' "$run_log"
grep -Fqx "$root/tools/yt_dlp_server.py" "$run_log"
grep -Fqx "$socket_path" "$run_log"

/usr/bin/python3 "$root/tools/yt_dlp_server.py" \
	--runtime-root "$runtime" --cache-dir "$cache" \
	--socket "$socket_path" --yt-dlp "$root/vendor/yt-dlp" --workers 2 &
child_pid=$!
for _attempt in $(seq 1 100); do
	[ "$(readlink -f -- "/proc/$child_pid/exe" 2>/dev/null || true)" = \
		"$(readlink -f -- /usr/bin/python3)" ] && [ -S "$socket_path" ] && break
	sleep 0.01
done
RG_TEST_ACTIVE=1 RG_TEST_PID=$child_pid reuse_output=$(run_ensure)
printf '%s\n' "$reuse_output" | \
	grep -Fqx 'YOUTUBE_RESOLVER_ENSURE result=PASS action=REUSE unit=rg40xxv-youtube-resolver.service'
kill "$child_pid"
wait "$child_pid" 2>/dev/null || true
child_pid=
rm -f -- "$socket_path"
RG_TEST_ACTIVE=0
RG_TEST_PID=0

install -d -m 0700 "$runtime"
/usr/bin/python3 - "$socket_path" <<'PY'
import os
import socket
import sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.bind(sys.argv[1])
s.close()
os.chmod(sys.argv[1], 0o600)
PY
stale_output=$(run_ensure)
printf '%s\n' "$stale_output" | \
	grep -Fqx 'YOUTUBE_RESOLVER_ENSURE result=PASS action=START unit=rg40xxv-youtube-resolver.service'
rm -f -- "$socket_path"

/usr/bin/python3 - "$socket_path" "$fixture/live-ready" <<'PY' &
import os
import socket
import sys
import time
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.bind(sys.argv[1])
os.chmod(sys.argv[1], 0o600)
s.listen(1)
open(sys.argv[2], "w").close()
time.sleep(30)
PY
child_pid=$!
for _attempt in $(seq 1 100); do
	[ -f "$fixture/live-ready" ] && break
	sleep 0.01
done
[ -f "$fixture/live-ready" ]
set +e
run_ensure >"$fixture/live.out" 2>"$fixture/live.err"
live_status=$?
set -e
[ "$live_status" -eq 66 ]
grep -Fq 'reason=live-socket-without-unit' "$fixture/live.err"
kill "$child_pid"
wait "$child_pid" 2>/dev/null || true
child_pid=
rm -f -- "$socket_path"

/usr/bin/python3 "$root/tools/yt_dlp_server.py" \
	--runtime-root "$runtime" --cache-dir "$cache/wrong" \
	--socket "$socket_path" --yt-dlp "$root/vendor/yt-dlp" --workers 2 &
child_pid=$!
for _attempt in $(seq 1 100); do
	[ "$(readlink -f -- "/proc/$child_pid/exe" 2>/dev/null || true)" = \
		"$(readlink -f -- /usr/bin/python3)" ] && [ -S "$socket_path" ] && break
	sleep 0.01
done
set +e
RG_TEST_ACTIVE=1 RG_TEST_PID=$child_pid run_ensure \
	>"$fixture/identity.out" 2>"$fixture/identity.err"
identity_status=$?
set -e
[ "$identity_status" -eq 66 ]
grep -Fq 'reason=unit-identity' "$fixture/identity.err"

printf '%s\n' 'YOUTUBE_RESOLVER_ENSURE_TEST PASS owner=SYSTEMD scene_exit=INDEPENDENT stale_socket=RECOVERABLE live_orphan=REJECTED unit_identity=EXACT'
