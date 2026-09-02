#!/bin/sh
set -eu

project=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
workspace=${RG40XXV_WORKSPACE:-$(CDPATH= cd -- "$project/../../../.." && pwd -P)}
rootfs="$workspace/firmware/mnt/rootfs"
player="$project/build/rg40xxv-youtube-player-probe"
sample="$project/testdata/h700-640x480-h264.mp4"

test -x "$player"
test -s "$sample"

target_library_path=/usr/lib:/lib/aarch64-linux-gnu:/usr/lib/aarch64-linux-gnu:/usr/lib/aarch64-linux-gnu/pulseaudio:/usr/lib/aarch64-linux-gnu/blas:/usr/lib/aarch64-linux-gnu/lapack
exec qemu-aarch64-static -L "$rootfs" -E LD_LIBRARY_PATH="$target_library_path" \
	"$player" --headless-play "$sample"
