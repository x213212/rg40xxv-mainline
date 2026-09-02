#!/bin/sh
set -eu

project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
workspace=${RG40XXV_WORKSPACE:-$(CDPATH= cd -- "$project/../../../.." && pwd -P)}
rootfs="$workspace/firmware/mnt/rootfs"
temporary=$(mktemp -d)
helper_pid=
cleanup()
{
	status=$?
	if test "$status" -ne 0; then
		printf '%s\n' '--- uinput log ---' >&2
		cat "$temporary/uinput.log" >&2 2>/dev/null || :
		printf '%s\n' '--- UI stderr ---' >&2
		cat "$temporary/ui.stderr" >&2 2>/dev/null || :
		printf '%s\n' '--- hardware argv ---' >&2
		cat "$temporary/hardware.argv" >&2 2>/dev/null || :
	fi
	test -z "$helper_pid" || kill "$helper_pid" 2>/dev/null || :
	wait "$helper_pid" 2>/dev/null || :
	rm -rf -- "$temporary"
	exit "$status"
}
trap cleanup EXIT HUP INT TERM

if test ! -c /dev/uinput || test ! -w /dev/uinput; then
	printf '%s\n' 'POWER_INPUT_UINPUT_TEST SKIP reason=uinput-unavailable'
	exit 0
fi

# qemu-user cannot translate variable-sized evdev ioctls such as EVIOCGNAME
# (it returns ENOSYS), so this end-to-end test is meaningful only when the UI
# binary can run natively.  The shared event state machine is always exercised
# by test-power-input-filter.sh.
if test "$(uname -m)" != aarch64; then
	printf '%s\n' \
		'POWER_INPUT_UINPUT_TEST SKIP reason=qemu-user-evdev-ioctl-enosys coverage=power-input-filter'
	exit 0
fi

cc -std=c11 -O2 -Wall -Wextra -Werror -Wpedantic \
	"$project/tests/power_input_uinput.c" -o "$temporary/uinput-driver"

mkdir -p "$temporary/roms/GBA" "$temporary/state" "$temporary/hardware"
: >"$temporary/roms/GBA/demo.gba"
cat >"$temporary/hardwarectl" <<'EOF'
#!/bin/sh
set -eu
case ${1:-} in screen-off|screen-on) test "$#" -eq 1 ;; *) exit 2 ;; esac
printf '%s\n' "$1" >>"$POWER_INPUT_CAPTURE"
EOF
chmod 0755 "$temporary/hardwarectl"

"$temporary/uinput-driver" >"$temporary/uinput.log" 2>&1 &
helper_pid=$!
ready=0
for _ in 1 2 3 4 5 6 7 8 9 10; do
	if grep -Fqx READY "$temporary/uinput.log" 2>/dev/null; then
		ready=1
		break
	fi
	sleep 0.1
done
test "$ready" -eq 1

POWER_INPUT_CAPTURE="$temporary/hardware.argv" \
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
qemu-aarch64-static -L "$rootfs" \
	"$project/build/rg40xxv-shell" \
	--windowed --font "$project/assets/RG40XXV-UI-Sans.otf" \
	--icon-atlas "$project/assets/RG40XXV-Material-Icons.png" \
	--rom-root "$temporary/roms" \
	--platform-routes "$project/tests/platform-routes.fixture.json" \
	--launcher "$project/tests/fake-launcher.fixture" \
	--launch-log "$temporary/launch.log" \
	--history-file "$temporary/history.tsv" \
	--favorites-file "$temporary/favorites.tsv" \
	--settings-file "$temporary/settings.conf" \
	--filter-state "$temporary/filters.conf" \
	--state-dir "$temporary/state" --hardware-root "$temporary/hardware" \
	--hardwarectl "$temporary/hardwarectl" --demo-ms 7600 \
	>"$temporary/ui.stdout" 2>"$temporary/ui.stderr"

wait "$helper_pid"
helper_pid=
grep -Fq 'UI_RESULT PASS' "$temporary/ui.stdout"
grep -Fq 'INPUT_POWER_EVDEV status=ready name=axp20x-pek' \
	"$temporary/ui.stderr"
grep -Fq 'INPUT_POWER_EVDEV status=lost' "$temporary/ui.stderr"
test "$(grep -c '^screen-off$' "$temporary/hardware.argv")" -eq 2
test "$(grep -c '^screen-on$' "$temporary/hardware.argv")" -eq 2
test "$(wc -l <"$temporary/hardware.argv")" -eq 4

printf '%s\n' 'POWER_INPUT_UINPUT_TEST PASS raw_gamepad=present dedicated_power=PASS sdl_duplicate=NONE hotplug_reopen=PASS actions=4'
