#!/bin/sh
set -eu

project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
workspace=$(CDPATH= cd -- "$project/../.." && pwd -P)
temporary=$(mktemp -d)
trap 'rm -rf -- "$temporary"' EXIT HUP INT TERM

mkdir -p "$temporary/roms/GBA"
touch "$temporary/roms/GBA/Game With Spaces.gba"
cp "$project/tests/fake-launcher.fixture" "$temporary/fake-launcher.sh"
chmod 0755 "$temporary/fake-launcher.sh"

event_log="$temporary/events.log"
result=$(SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
	LAUNCH_CAPTURE="$temporary/arguments" \
	FAKE_EVENT_MARKER=1 \
	qemu-aarch64-static -L "$workspace/firmware/mnt/rootfs" \
	"$project/build/rg40xxv-shell" \
	--windowed --font "$project/assets/RG40XXV-UI-Sans.otf" \
	--rom-root "$temporary/roms" \
	--platform-routes "$project/tests/platform-routes.fixture.json" \
	--launcher "$temporary/fake-launcher.sh" \
	--launch-log "$event_log" \
	--history-file "$temporary/history.tsv" \
	--favorites-file "$temporary/favorites.tsv" \
	--settings-file "$temporary/settings.conf" \
	--filter-state "$temporary/filters.conf" \
	--launch-once --demo-ms 1800 2>>"$event_log")

test "$(sed -n '1p' "$temporary/arguments")" = '<-->'
test "$(sed -n '2p' "$temporary/arguments")" = '<GBA>'
test "$(sed -n '3p' "$temporary/arguments")" = \
	"<$temporary/roms/GBA/Game With Spaces.gba>"
grep -Fq "${temporary}/roms/GBA/Game With Spaces.gba" "$temporary/history.tsv"
printf '%s\n' "$result" | grep -q 'UI_RESULT PASS'
printf '%s\n' "$result" | grep -q 'routes=1'

starting_line=$(sed -n '/UI_LAUNCH_TRANSITION PRESENTED phase=starting/=' \
	"$event_log" | sed -n '1p')
launcher_line=$(sed -n '/FAKE_LAUNCHER STARTED/=' "$event_log" | sed -n '1p')
returned_line=$(sed -n '/UI_LAUNCH_TRANSITION PRESENTED phase=returned/=' \
	"$event_log" | sed -n '1p')
test -n "$starting_line"
test -n "$launcher_line"
test -n "$returned_line"
test "$starting_line" -lt "$launcher_line"
test "$launcher_line" -lt "$returned_line"

if grep -Eq 'SDL_Delay|(^|[^[:alpha:]_])sleep[[:space:]]*\(|(^|[^[:alpha:]_])sync[[:space:]]*\(' \
	"$project/src/launch_ui.c" "$project/src/render_scene.c"; then
	printf '%s\n' 'launch transition contains a blocking delay or sync' >&2
	exit 1
fi

error_log="$temporary/error-events.log"
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
	LAUNCH_CAPTURE="$temporary/error-arguments" \
	FAKE_EVENT_MARKER=1 FAKE_EXIT=7 \
	qemu-aarch64-static -L "$workspace/firmware/mnt/rootfs" \
	"$project/build/rg40xxv-shell" \
	--windowed --font "$project/assets/RG40XXV-UI-Sans.otf" \
	--rom-root "$temporary/roms" \
	--platform-routes "$project/tests/platform-routes.fixture.json" \
	--launcher "$temporary/fake-launcher.sh" \
	--launch-log "$error_log" \
	--history-file "$temporary/error-history.tsv" \
	--favorites-file "$temporary/error-favorites.tsv" \
	--settings-file "$temporary/error-settings.conf" \
	--filter-state "$temporary/error-filters.conf" \
	--launch-once --demo-ms 900 >>"$temporary/error-ui.log" 2>>"$error_log"
grep -q 'UI_LAUNCH_TRANSITION PRESENTED phase=error' "$error_log"
grep -q 'UI_RESULT PASS' "$temporary/error-ui.log"
printf '%s\n' 'UI launcher suspend/resume integration: PASS'
