#!/bin/sh
set -eu

project=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
workspace=$(CDPATH= cd -- "$project/../.." && pwd -P)
rootfs="$workspace/firmware/mnt/rootfs"
font=${RG40XXV_UI_PREVIEW_FONT:-"$project/assets/RG40XXV-UI-Sans.otf"}
output=${1:-"$project/build/rg40xxv-shell-preview.bmp"}

case "$output" in
	/*) ;;
	*) output="$PWD/$output" ;;
esac

test -x "$project/build/rg40xxv-shell"
test -f "$font"
qemu=${QEMU_AARCH64:-qemu-aarch64-static}
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
	"$qemu" -L "$rootfs" "$project/build/rg40xxv-shell" \
	--windowed --font "$font" --screenshot "$output"
file "$output"
sha256sum "$output"
