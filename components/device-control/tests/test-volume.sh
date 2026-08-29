#!/usr/bin/env bash
set -euo pipefail

TEST_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(cd -- "$TEST_DIR/.." && pwd)
FIXTURE=$(mktemp -d /tmp/rg40xxv-device-control-test.volume.XXXXXX)
BAD_FIXTURE=$(mktemp -d /tmp/rg40xxv-device-control-test.volume.XXXXXX)
DAEMON_PID=
PERSISTENT_DIR=$FIXTURE/mnt/data/rg40xxv/state/audio
PERSISTENT_STATE=$PERSISTENT_DIR/volume.v1

cleanup() {
	local status=$?

	if [[ -n ${DAEMON_PID:-} ]]; then
		kill "$DAEMON_PID" 2>/dev/null || true
		wait "$DAEMON_PID" 2>/dev/null || true
	fi
	rm -rf -- "$FIXTURE" "$BAD_FIXTURE"
	exit "$status"
}
trap cleanup EXIT

make_input_fixture() {
	local root=$1

	mkdir -p "$root/dev/input" "$root/mixer" "$root/run" "$root/bin" \
		"$root/mnt/data/rg40xxv/state"
	mkfifo -m 0600 "$root/dev/input/event2" "$root/dev/input/event9"
	printf '%s\n' 'other-gpio-keys' >"$root/dev/input/event2.name"
	printf '%s\n' 'KEY_VOLUMEUP=1' 'KEY_VOLUMEDOWN=1' \
		>"$root/dev/input/event2.keys"
	printf '%s\n' 'gpio-keys-volume' >"$root/dev/input/event9.name"
	printf '%s\n' 'KEY_VOLUMEUP=1' 'KEY_VOLUMEDOWN=1' \
		>"$root/dev/input/event9.keys"
}

make_mixer_fixture() {
	local root=$1

	printf '%s\n' 42 >"$root/mixer/dac-volume"
	printf '%s\n' 17 >"$root/mixer/lineout-volume"
	printf '%s\n' '1 1' >"$root/mixer/lineout-switch"
	printf '%s\n' '1 1' >"$root/mixer/dac-switch"
	printf '%s\n' '0 0' >"$root/mixer/dac-reversed-switch"
	printf '%s\n' 0 >"$root/mixer/lineout-source-route"
	printf '%s\n' 1 >"$root/mixer/speaker-switch"
	chmod 0644 "$root/mixer/"*
}

wait_for_state() {
	local expected_volume=$1 expected_muted=$2
	local marker="$FIXTURE/run/rg40xxv-ui/alsa-volume"

	for _ in {1..100}; do
		if [[ -f $marker && ! -L $marker ]] &&
		   grep -Fqx "volume_percent=$expected_volume" "$marker" &&
		   grep -Fqx "muted=$expected_muted" "$marker"; then
			return 0
		fi
		sleep 0.01
	done
	printf 'FAIL: volume state did not reach %s/%s\n' \
		"$expected_volume" "$expected_muted" >&2
	sed -n '1,120p' "$FIXTURE/daemon.log" >&2 || true
	return 1
}

start_daemon() {
	local expected_volume=$1 expected_muted=$2

	"$FIXTURE/bin/volume-daemon" --test-root "$FIXTURE" \
		>>"$FIXTURE/daemon.log" 2>&1 &
	DAEMON_PID=$!
	for _ in {1..100}; do
		[[ -S $FIXTURE/run/rg40xxv-volume/control.sock ]] && break
		kill -0 "$DAEMON_PID" 2>/dev/null || {
			sed -n '1,200p' "$FIXTURE/daemon.log" >&2 || true
			return 1
		}
		sleep 0.01
	done
	[[ -S $FIXTURE/run/rg40xxv-volume/control.sock ]]
	wait_for_state "$expected_volume" "$expected_muted"
}

stop_daemon() {
	if [[ -n ${DAEMON_PID:-} ]]; then
		kill "$DAEMON_PID" 2>/dev/null || true
		wait "$DAEMON_PID" 2>/dev/null || true
		DAEMON_PID=
	fi
	rm -f -- "$FIXTURE/run/rg40xxv-volume/control.sock"
}

assert_persistent_state() {
	local expected_volume=$1 expected_muted=$2 expected_last=$3
	local expected=$FIXTURE/expected-volume.v1

	printf 'RG40XXV_VOLUME_V1\nvolume_percent=%s\nmuted=%s\nlast_nonzero=%s\n' \
		"$expected_volume" "$expected_muted" "$expected_last" >"$expected"
	[[ -f $PERSISTENT_STATE && ! -L $PERSISTENT_STATE ]]
	[[ $(stat -c %a "$PERSISTENT_STATE") == 600 ]]
	cmp -s "$expected" "$PERSISTENT_STATE"
}

send_events() {
	local specification=$1
	RG40XXV_VOLUME_EVENT_FIFO="$FIXTURE/dev/input/event9" \
	RG40XXV_VOLUME_EVENT_SPEC="$specification" python3 - <<'PY'
import os
import struct

codes = {"up": 115, "down": 114}
events = []
for item in os.environ["RG40XXV_VOLUME_EVENT_SPEC"].split(","):
    key, value = item.split(":", 1)
    events.append(struct.pack("@llHHi", 0, 0, 1, codes[key], int(value)))
with open(os.environ["RG40XXV_VOLUME_EVENT_FIFO"], "wb", buffering=0) as fifo:
    fifo.write(b"".join(events))
PY
}

make_input_fixture "$FIXTURE"
make_mixer_fixture "$FIXTURE"

gcc -std=c11 -O2 -Wall -Wextra -Werror -Wpedantic \
	-DRG40XXV_VOLUME_TESTING=1 \
	"$ROOT/volume/rg40xxv-volume-daemon.c" -o "$FIXTURE/bin/volume-daemon"
gcc -std=c11 -O2 -Wall -Wextra -Werror -Wpedantic \
	-DRG40XXV_VOLUME_TESTING=1 \
	"$ROOT/volume/rg40xxv-volume-ctl.c" -o "$FIXTURE/bin/volume-ctl"

start_daemon 60 0
assert_persistent_state 60 0 60
[[ $(<"$FIXTURE/mixer/dac-volume") == 63 ]]
[[ $(<"$FIXTURE/mixer/lineout-volume") == 19 ]]
[[ $(<"$FIXTURE/mixer/lineout-switch") == '1 1' ]]
[[ $(<"$FIXTURE/mixer/speaker-switch") == 1 ]]
[[ $(stat -c %a "$FIXTURE/run/rg40xxv-ui/alsa-volume") == 644 ]]
[[ $(stat -c %a "$PERSISTENT_DIR") == 700 ]]

export RG40XXV_VOLUME_TEST_ROOT=$FIXTURE
CTL="$FIXTURE/bin/volume-ctl"
[[ $("$CTL" get) == $'volume_percent=60\nmuted=0' ]]
[[ $("$CTL" set 70) == $'volume_percent=70\nmuted=0' ]]
assert_persistent_state 70 0 70
[[ $(<"$FIXTURE/mixer/lineout-volume") == 22 ]]
[[ $("$CTL" up) == $'volume_percent=75\nmuted=0' ]]
[[ $("$CTL" down) == $'volume_percent=70\nmuted=0' ]]
[[ $("$CTL" mute-toggle) == $'volume_percent=70\nmuted=1' ]]
assert_persistent_state 70 1 70
[[ $(<"$FIXTURE/mixer/lineout-volume") == 22 ]]
[[ $(<"$FIXTURE/mixer/lineout-switch") == '0 0' ]]
[[ $(<"$FIXTURE/mixer/speaker-switch") == 0 ]]

# A cold-start mixer reset must not erase the user's persisted volume/mute.
stop_daemon
printf '%s\n' 0 >"$FIXTURE/mixer/dac-volume"
printf '%s\n' 0 >"$FIXTURE/mixer/lineout-volume"
printf '%s\n' '0 0' >"$FIXTURE/mixer/lineout-switch"
printf '%s\n' '0 0' >"$FIXTURE/mixer/dac-switch"
printf '%s\n' '0 0' >"$FIXTURE/mixer/dac-reversed-switch"
printf '%s\n' 0 >"$FIXTURE/mixer/lineout-source-route"
printf '%s\n' 0 >"$FIXTURE/mixer/speaker-switch"
start_daemon 70 1
assert_persistent_state 70 1 70
[[ $(<"$FIXTURE/mixer/dac-volume") == 63 ]]
[[ $(<"$FIXTURE/mixer/lineout-volume") == 22 ]]
[[ $(<"$FIXTURE/mixer/lineout-switch") == '0 0' ]]
[[ $(<"$FIXTURE/mixer/speaker-switch") == 0 ]]
[[ $("$CTL" mute-toggle) == $'volume_percent=70\nmuted=0' ]]
[[ $(<"$FIXTURE/mixer/lineout-switch") == '1 1' ]]
[[ $(<"$FIXTURE/mixer/speaker-switch") == 1 ]]
[[ $("$CTL" set 0) == $'volume_percent=0\nmuted=1' ]]
assert_persistent_state 0 1 70
[[ $(<"$FIXTURE/mixer/lineout-volume") == 0 ]]
[[ $("$CTL" mute-toggle) == $'volume_percent=70\nmuted=0' ]]
assert_persistent_state 70 0 70

# A short press and Linux EV_KEY value=2 autorepeat are both one 5-point step.
event_started=$(date +%s%N)
send_events 'up:1,up:2,down:1'
wait_for_state 75 0
event_finished=$(date +%s%N)
event_latency_ms=$(((event_finished - event_started) / 1000000))
((event_latency_ms < 500))

# Idle work must remain blocked in poll rather than consume a timer loop.
ticks_before=$(awk '{print $14 + $15}' "/proc/$DAEMON_PID/stat")
sleep 0.25
ticks_after=$(awk '{print $14 + $15}' "/proc/$DAEMON_PID/stat")
idle_ticks=$((ticks_after - ticks_before))
((idle_ticks <= 2))

# A late ALSA write failure rolls every earlier control back and leaves the
# published state untouched.
printf '%s\n' lineout-switch >"$FIXTURE/mixer/fail-control"
if "$CTL" set 80 >"$FIXTURE/failure.out" 2>"$FIXTURE/failure.err"; then
	printf 'FAIL: injected mixer failure returned success\n' >&2
	exit 1
fi
grep -Fq 'hardware-transaction' "$FIXTURE/failure.err"
wait_for_state 75 0
[[ $(<"$FIXTURE/mixer/lineout-volume") == 23 ]]
[[ $(<"$FIXTURE/mixer/dac-volume") == 63 ]]
assert_persistent_state 75 0 75

# A persistent-media failure is part of the same transaction: ALSA, the
# runtime marker, and the last durable state all roll back together.
: >"$PERSISTENT_DIR/.inject-write-failure"
if "$CTL" set 80 >"$FIXTURE/persist-failure.out" \
	2>"$FIXTURE/persist-failure.err"; then
	printf 'FAIL: injected persistent write failure returned success\n' >&2
	exit 1
fi
grep -Fq 'hardware-transaction' "$FIXTURE/persist-failure.err"
wait_for_state 75 0
[[ $(<"$FIXTURE/mixer/lineout-volume") == 23 ]]
[[ $(<"$FIXTURE/mixer/dac-volume") == 63 ]]
assert_persistent_state 75 0 75
rm -f -- "$PERSISTENT_DIR/.inject-write-failure"

# Neither CLI argv nor a raw socket packet is interpreted by a shell.
INJECTION_MARKER="$FIXTURE/injected"
if "$CTL" set "50;touch $INJECTION_MARKER" >/dev/null 2>&1; then
	printf 'FAIL: CLI accepted an injected percent\n' >&2
	exit 1
fi
RG40XXV_VOLUME_SOCKET="$FIXTURE/run/rg40xxv-volume/control.sock" \
	RG40XXV_VOLUME_REPLY="$FIXTURE/raw.reply" python3 - <<'PY'
import os
import socket

client = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
client.connect(os.environ["RG40XXV_VOLUME_SOCKET"])
client.sendall(b"set 50;touch /tmp/never")
reply = client.recv(128)
with open(os.environ["RG40XXV_VOLUME_REPLY"], "wb") as output:
    output.write(reply)
PY
grep -Fqx 'ERR invalid-command' "$FIXTURE/raw.reply"
[[ ! -e $INJECTION_MARKER ]]
wait_for_state 75 0

# Atomic rename replaces a hostile final-name symlink without following it.
printf '%s\n' outside-guard >"$FIXTURE/outside-marker"
rm -f -- "$FIXTURE/run/rg40xxv-ui/alsa-volume"
ln -s "$FIXTURE/outside-marker" "$FIXTURE/run/rg40xxv-ui/alsa-volume"
"$CTL" set 60 >/dev/null
wait_for_state 60 0
assert_persistent_state 60 0 60
[[ $(<"$FIXTURE/outside-marker") == outside-guard ]]
[[ -f "$FIXTURE/run/rg40xxv-ui/alsa-volume" &&
   ! -L "$FIXTURE/run/rg40xxv-ui/alsa-volume" ]]

# The durable final name has the same anti-symlink guarantee.
printf '%s\n' persistent-guard >"$FIXTURE/outside-persistent"
rm -f -- "$PERSISTENT_STATE"
ln -s "$FIXTURE/outside-persistent" "$PERSISTENT_STATE"
"$CTL" set 65 >/dev/null
wait_for_state 65 0
[[ $(<"$FIXTURE/outside-persistent") == persistent-guard ]]
assert_persistent_state 65 0 65

stop_daemon

# A pre-existing socket-name symlink is rejected rather than unlinked/followed.
printf '%s\n' socket-guard >"$FIXTURE/outside-socket"
ln -s "$FIXTURE/outside-socket" "$FIXTURE/run/rg40xxv-volume/control.sock"
if "$FIXTURE/bin/volume-daemon" --test-root "$FIXTURE" \
	>"$FIXTURE/symlink-socket.log" 2>&1; then
	printf 'FAIL: daemon accepted a symlink socket path\n' >&2
	exit 1
fi
[[ $(<"$FIXTURE/outside-socket") == socket-guard ]]
rm -f -- "$FIXTURE/run/rg40xxv-volume/control.sock"

# A malformed or incorrectly-permissioned state is never trusted.  Startup
# chooses the audible 60%/unmuted default and atomically repairs the file.
printf 'RG40XXV_VOLUME_V1\nvolume_percent=101\nmuted=0\nlast_nonzero=101\n' \
	>"$PERSISTENT_STATE"
chmod 0644 "$PERSISTENT_STATE"
printf '%s\n' 0 >"$FIXTURE/mixer/dac-volume"
printf '%s\n' 0 >"$FIXTURE/mixer/lineout-volume"
printf '%s\n' '0 0' >"$FIXTURE/mixer/lineout-switch"
printf '%s\n' '0 0' >"$FIXTURE/mixer/dac-switch"
printf '%s\n' '0 0' >"$FIXTURE/mixer/dac-reversed-switch"
printf '%s\n' 0 >"$FIXTURE/mixer/lineout-source-route"
printf '%s\n' 0 >"$FIXTURE/mixer/speaker-switch"
start_daemon 60 0
assert_persistent_state 60 0 60
grep -Fq 'persistent volume invalid; using safe default=60' \
	"$FIXTURE/daemon.log"
stop_daemon

# A fixture control symlink cannot pivot mixer writes outside its safe root.
make_input_fixture "$BAD_FIXTURE"
make_mixer_fixture "$BAD_FIXTURE"
printf '%s\n' 99 >"$BAD_FIXTURE/outside-control"
rm -f -- "$BAD_FIXTURE/mixer/lineout-volume"
ln -s "$BAD_FIXTURE/outside-control" "$BAD_FIXTURE/mixer/lineout-volume"
if "$FIXTURE/bin/volume-daemon" --test-root "$BAD_FIXTURE" \
	>"$BAD_FIXTURE/symlink-mixer.log" 2>&1; then
	printf 'FAIL: daemon accepted a symlink mixer control\n' >&2
	exit 1
fi
[[ $(<"$BAD_FIXTURE/outside-control") == 99 ]]

# The persistent directory itself must be a private real directory, not a
# symlink to an attacker-controlled location.
rm -f -- "$BAD_FIXTURE/mixer/lineout-volume"
printf '%s\n' 17 >"$BAD_FIXTURE/mixer/lineout-volume"
chmod 0644 "$BAD_FIXTURE/mixer/lineout-volume"
mkdir -m 0700 "$BAD_FIXTURE/outside-audio"
ln -s "$BAD_FIXTURE/outside-audio" \
	"$BAD_FIXTURE/mnt/data/rg40xxv/state/audio"
if "$FIXTURE/bin/volume-daemon" --test-root "$BAD_FIXTURE" \
	>"$BAD_FIXTURE/symlink-persistent-dir.log" 2>&1; then
	printf 'FAIL: daemon accepted a symlink persistent directory\n' >&2
	exit 1
fi
[[ -z $(find "$BAD_FIXTURE/outside-audio" -mindepth 1 -maxdepth 1 -print -quit) ]]

# Test-only roots are canonical and cannot use traversal or a symlink pivot.
if "$FIXTURE/bin/volume-daemon" --test-root "$FIXTURE/.." >/dev/null 2>&1; then
	printf 'FAIL: daemon accepted a traversal test root\n' >&2
	exit 1
fi
ln -s "$BAD_FIXTURE" "$FIXTURE/root-link"
if "$FIXTURE/bin/volume-daemon" --test-root "$FIXTURE/root-link" \
	>/dev/null 2>&1; then
	printf 'FAIL: daemon accepted a symlink test root\n' >&2
	exit 1
fi

grep -Fqx 'ReadWritePaths=/run/rg40xxv-volume /run/rg40xxv-ui /mnt/data/rg40xxv/state/audio' \
	"$ROOT/volume/rg40xxv-volume.service"
grep -Fqx 'd /mnt/data/rg40xxv/state/audio 0700 root root -' \
	"$ROOT/volume/rg40xxv-volume.conf"

printf 'PASS volume backend：persistent 0600 volume.v1、cold-boot restore/default、ALSA+marker+storage rollback、name-validated evdev、±5/autorepeat、root-only seqpacket、no symlink/path/injection；fixture_latency_ms=%d idle_ticks=%d DEVICE_WRITE=NONE\n' \
	"$event_latency_ms" "$idle_ticks"
