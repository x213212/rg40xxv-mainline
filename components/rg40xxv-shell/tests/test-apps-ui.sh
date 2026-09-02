#!/bin/sh
set -eu

project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
workspace=${RG40XXV_WORKSPACE:-$(CDPATH= cd -- "$project/../../../.." && pwd -P)}
ui_binary=${RG40XXV_UI_BINARY:-$project/build/rg40xxv-shell}
temporary=$(mktemp -d)
trap 'status=$?; trap - EXIT; rm -rf -- "$temporary"; exit "$status"' \
	EXIT HUP INT TERM

rom_root="$temporary/roms"
app_dir="$rom_root/APPS/File Manager"
launcher="$temporary/fake-launcher.sh"
youtube_launcher="$temporary/rg40xxv-youtube-native"
youtube_admission="$temporary/youtube-admission.env"
capture="$temporary/arguments"
output="$temporary/stdout"
screenshot="$temporary/apps.bmp"

mkdir -p "$rom_root/GBA" "$app_dir"
touch "$rom_root/GBA/Current Game.gba"
printf '%s\n' '#!/bin/sh' 'exit 0' >"$app_dir/launch.sh"
chmod 0755 "$app_dir/launch.sh"
printf '%s\n' '#!/bin/sh' 'exit 0' >"$youtube_launcher"
chmod 0700 "$youtube_launcher"
printf '%s\n' \
	'schema=rg40xxv-youtube-ui-admission-v3' \
	'native_route=native-texture' \
	"native_launcher=$youtube_launcher" \
	'evidence_scope=COMPONENT_GATE' \
	'native_controller_ui=PASS' \
	'url_resolver=PASS' \
	'range_bridge=PASS' \
	'h264_decode=PASS' \
	'aac_decode=PASS' \
	'drm_kms_display=PASS' \
	'alsa_audio=PASS' \
	'input=PASS' \
	'session_return=PASS' \
	'memory_budget=UNVERIFIED' >"$youtube_admission"
chmod 0600 "$youtube_admission"
printf '%s\n' 'system=GBA' >"$temporary/filters.conf"
cp "$project/tests/fake-launcher.fixture" "$launcher"
chmod 0755 "$launcher"

SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
	RG40XXV_UI_YOUTUBE_CAPABILITY="$youtube_admission" \
	LAUNCH_CAPTURE="$capture" \
	qemu-aarch64-static -L "$workspace/firmware/mnt/rootfs" \
		"$ui_binary" \
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
test "$(sed -n '3p' "$capture")" = "<$youtube_launcher>"
if grep -Fq 'Current Game.gba' "$capture"; then
	printf '%s\n' 'APPS page launched the previously filtered game' >&2
	exit 1
fi
# One real APP plus the release-owned native-texture YouTube tile are visible;
# the synthetic YouTube entry is deliberately first and launches its exact
# admission-owned path.
grep -Fq 'roms=3 visible=2' "$output"
grep -Fq 'apps_view=yes' "$output"
grep -Fq "$youtube_launcher" "$temporary/history.tsv"
test -s "$screenshot"
python3 - "$screenshot" <<'PY'
from pathlib import Path
import sys

payload = Path(sys.argv[1]).read_bytes()
# The release-owned YouTube tile is rendered by the UI's code-native icon
# path.  SDL's little-endian ARGB8888 screenshot stores opaque YouTube red as
# BGRA 00 00 ff ff; require a real filled area, not an incidental pixel.
if payload.count(b"\x00\x00\xff\xff") < 64:
    raise SystemExit("YouTube application icon was not rendered")
PY

printf '%s\n' 'UI APPS-only launch integration: PASS'
