#!/bin/sh
set -eu

project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
workspace=${RG40XXV_WORKSPACE:-$(CDPATH= cd -- "$project/../../../.." && pwd -P)}
temporary=$(mktemp -d)
ui_pid=
trap 'if test -n "$ui_pid"; then kill -KILL "$ui_pid" 2>/dev/null || true; fi; rm -rf -- "$temporary"' EXIT HUP INT TERM

mkdir -p "$temporary/roms/GBA"
touch "$temporary/roms/GBA/Signal Test.gba"
cp "$project/tests/fake-launcher.fixture" "$temporary/fake-launcher.sh"
chmod 0755 "$temporary/fake-launcher.sh"

SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
	LAUNCH_CAPTURE="$temporary/arguments" \
	LAUNCH_LEADER_PID="$temporary/leader.pid" FAKE_SLEEP=1 \
	qemu-aarch64-static -L "$workspace/firmware/mnt/rootfs" \
	"$project/build/rg40xxv-shell" \
	--windowed --font "$project/assets/RG40XXV-UI-Sans.otf" \
	--rom-root "$temporary/roms" \
	--platform-routes "$project/tests/platform-routes.fixture.json" \
	--launcher "$temporary/fake-launcher.sh" \
	--launch-log "$temporary/launch.log" \
	--history-file "$temporary/history.tsv" \
	--favorites-file "$temporary/favorites.tsv" \
	--settings-file "$temporary/settings.conf" \
	--filter-state "$temporary/filters.conf" \
	--launch-once --demo-ms 15000 >"$temporary/ui.log" 2>&1 &
ui_pid=$!

attempt=0
while test ! -s "$temporary/leader.pid" && test "$attempt" -lt 200; do
	sleep 0.02
	attempt=$((attempt + 1))
done
test -s "$temporary/leader.pid"
leader_pid=$(sed -n '1p' "$temporary/leader.pid")
kill -TERM "$ui_pid"
wait "$ui_pid"
ui_pid=

if kill -0 "$leader_pid" 2>/dev/null; then
	printf '%s\n' 'launcher leader survived UI SIGTERM' >&2
	exit 1
fi
grep -q 'UI_RESULT PASS' "$temporary/ui.log"
printf '%s\n' 'UI SIGTERM launcher-group cleanup: PASS'
