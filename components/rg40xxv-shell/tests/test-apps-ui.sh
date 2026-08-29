#!/bin/sh
set -eu

project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
workspace=$(CDPATH= cd -- "$project/../.." && pwd -P)
temporary=$(mktemp -d)
trap 'status=$?; trap - EXIT; rm -rf -- "$temporary"; exit "$status"' \
	EXIT HUP INT TERM

rom_root="$temporary/roms"
app_dir="$rom_root/APPS/File Manager"
launcher="$temporary/fake-launcher.sh"
capture="$temporary/arguments"
output="$temporary/stdout"
screenshot="$temporary/apps.bmp"

mkdir -p "$rom_root/GBA" "$app_dir"
touch "$rom_root/GBA/Current Game.gba"
printf '%s\n' '#!/bin/sh' 'exit 0' >"$app_dir/launch.sh"
chmod 0755 "$app_dir/launch.sh"
printf '%s\n' 'system=GBA' >"$temporary/filters.conf"
cp "$project/tests/fake-launcher.fixture" "$launcher"
chmod 0755 "$launcher"

SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
	LAUNCH_CAPTURE="$capture" \
	qemu-aarch64-static -L "$workspace/firmware/mnt/rootfs" \
	"$project/build/rg40xxv-shell" \
	--windowed --font "$project/assets/RG40XXV-UI-Sans.otf" \
	--rom-root "$rom_root" --filter-state "$temporary/filters.conf" \
	--launcher "$launcher" --launch-log "$temporary/launch.log" \
	--history-file "$temporary/history.tsv" \
	--favorites-file "$temporary/favorites.tsv" \
	--settings-file "$temporary/settings.conf" \
	--apps-preview --launch-once --screenshot "$screenshot" --demo-ms 1400 \
	>"$output"

test "$(sed -n '1p' "$capture")" = '<-->'
test "$(sed -n '2p' "$capture")" = '<APPS>'
test "$(sed -n '3p' "$capture")" = "<$app_dir/launch.sh>"
if grep -Fq 'Current Game.gba' "$capture"; then
	printf '%s\n' 'APPS page launched the previously filtered game' >&2
	exit 1
fi
grep -Fq 'roms=2 visible=1' "$output"
grep -Fq 'apps_view=yes' "$output"
grep -Fq "$app_dir/launch.sh" "$temporary/history.tsv"
test -s "$screenshot"

printf '%s\n' 'UI APPS-only launch integration: PASS'
