#!/bin/sh
set -eu

project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
workspace=${RG40XXV_WORKSPACE:-$(CDPATH= cd -- "$project/../../../.." && pwd -P)}
ui_binary=${RG40XXV_UI_BINARY:-$project/build/rg40xxv-shell}
temporary=$(mktemp -d)
trap 'status=$?; trap - EXIT; rm -rf -- "$temporary"; exit "$status"' \
	EXIT HUP INT TERM

rom_root="$temporary/roms"
image_dir="$rom_root/GBA/Imgs"
output="$temporary/stdout"
screenshot="$temporary/covers.bmp"

mkdir -p "$image_dir"
touch "$rom_root/GBA/Valid.gba" "$rom_root/GBA/Broken.gba" \
	"$rom_root/GBA/Huge.gba"
printf '%s' \
	'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII=' | \
	base64 -d >"$image_dir/Valid.png"
printf '%s\n' 'not an image' >"$image_dir/Broken.png"
printf '%s' 'iVBORw0KGgoAAAANSUhEUgAAJxAAACcQ' | \
	base64 -d >"$image_dir/Huge.png"

SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
	qemu-aarch64-static -L "$workspace/firmware/mnt/rootfs" \
		"$ui_binary" \
	--windowed --font "$project/assets/RG40XXV-UI-Sans.otf" \
	--rom-root "$rom_root" --screenshot "$screenshot" \
	>"$output"

grep -Fq 'UI_RESULT PASS' "$output"
# Three library fixtures plus the single fail-closed native-texture YouTube
# tile.  Missing admission never creates a Web fallback.
grep -Fq 'roms=4 visible=3 covers=1 cover_rejected=2' "$output"
peak=$(sed -n 's/.* cover_decode_peak=\([0-9][0-9]*\) .*/\1/p' "$output")
test -n "$peak"
test "$peak" -gt 0
test "$peak" -le 16777216
test -s "$screenshot"

printf '%s\n' 'UI bounded cover decoder integration: PASS'
