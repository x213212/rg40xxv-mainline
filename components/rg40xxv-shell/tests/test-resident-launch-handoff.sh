#!/bin/sh
set -eu

project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
workspace=${RG40XXV_WORKSPACE:-$(CDPATH= cd -- "$project/../../../.." && pwd -P)}
temporary=$(mktemp -d)
trap 'rm -rf -- "$temporary"' EXIT HUP INT TERM

mkdir -p "$temporary/roms/GBA" "$temporary/state" "$temporary/hardware"
game="$temporary/roms/GBA/Resident Game With Spaces.gba"
handoff="$temporary/launch-request.v1"
events="$temporary/events.log"
touch "$game"
cp "$project/tests/fake-launcher.fixture" "$temporary/fake-launcher.sh"
chmod 0755 "$temporary/fake-launcher.sh"

set +e
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
	LAUNCH_CAPTURE="$temporary/launcher.arguments" FAKE_EVENT_MARKER=1 \
	qemu-aarch64-static -L "$workspace/firmware/mnt/rootfs" \
	"$project/build/rg40xxv-shell" \
	--windowed --resident --font "$project/assets/RG40XXV-UI-Sans.otf" \
	--rom-root "$temporary/roms" \
	--platform-routes "$project/tests/platform-routes.fixture.json" \
	--launcher "$temporary/fake-launcher.sh" \
	--launch-log "$temporary/launch.log" \
	--handoff-file "$handoff" \
	--history-file "$temporary/history.tsv" \
	--favorites-file "$temporary/favorites.tsv" \
	--settings-file "$temporary/settings.conf" \
	--filter-state "$temporary/filters.conf" \
	--state-dir "$temporary/state" \
	--hardware-root "$temporary/hardware" \
	--hardwarectl /not-present/ui-hardwarectl \
	--launch-once --demo-ms 3000 \
	>"$temporary/ui.stdout" 2>"$events"
status=$?
set -e

test "$status" -eq 75
test ! -e "$temporary/launcher.arguments"
test -f "$handoff" && test ! -L "$handoff"
test "$(stat -c '%a' -- "$handoff")" = 600
test "$(sed -n '1p' "$handoff")" = 'schema=rg40xxv-launch-handoff-v1'
test "$(sed -n '2p' "$handoff")" = 'route='
test "$(sed -n '3p' "$handoff")" = 'platform=GBA'
test "$(sed -n '4p' "$handoff")" = "content=$game"
test "$(wc -l <"$handoff")" -eq 4
grep -Fq "$game" "$temporary/history.tsv"
grep -Fq 'UI_RESULT PASS' "$temporary/ui.stdout"
grep -Fq 'UI_LAUNCH_TRANSITION PRESENTED phase=starting' "$events"
grep -Fq 'UI_LAUNCH_HANDOFF REQUEST_READY platform=GBA' "$events"
grep -Fq 'UI_LAUNCH_HANDOFF TEARDOWN_COMPLETE' "$events"
grep -Fq 'covers=0 fonts=0 renderer=none audio=kernel-release status=75' "$events"
if grep -Fq 'FAKE_LAUNCHER STARTED' "$events"; then
	printf '%s\n' 'resident UI launched the game before exiting' >&2
	exit 1
fi

request_line=$(sed -n '/UI_LAUNCH_HANDOFF REQUEST_READY/=' "$events")
teardown_line=$(sed -n '/UI_LAUNCH_HANDOFF TEARDOWN_COMPLETE/=' "$events")
test -n "$request_line" && test -n "$teardown_line"
test "$request_line" -lt "$teardown_line"

printf '%s\n' \
	'RESIDENT_LAUNCH_HANDOFF PASS exit=75 launcher_before_exit=no audio=kernel-release'
