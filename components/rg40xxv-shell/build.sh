#!/bin/sh
set -eu

project=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
workspace=$(CDPATH= cd -- "$project/../.." && pwd -P)
rootfs="$workspace/firmware/mnt/rootfs"
out="$project/build"
cc=${CC:-aarch64-linux-gnu-gcc-12}

export LC_ALL=C
export SOURCE_DATE_EPOCH=0
mkdir -p "$out"

"$cc" \
	-std=c11 -O2 -pipe -mcpu=cortex-a53 \
	-Wall -Wextra -Werror \
	-ffile-prefix-map="$workspace"=/usr/src/rg40xxv-ui \
	-fdebug-prefix-map="$workspace"=/usr/src/rg40xxv-ui \
	-fno-record-gcc-switches \
	-I"$project/include" \
	-I"$workspace/services/netstream/include" \
	-idirafter "$rootfs/usr/include" \
	-I"$rootfs/usr/include/SDL2" \
	"$project"/src/*.c \
	"$workspace/services/netstream/src/netstream.c" \
	"$rootfs/usr/lib/aarch64-linux-gnu/libasound.so" \
	"$rootfs/usr/lib/aarch64-linux-gnu/libSDL2_ttf.so" \
	"$rootfs/usr/lib/aarch64-linux-gnu/libSDL2_image.so" \
	"$rootfs/usr/lib/aarch64-linux-gnu/libSDL2.so" \
	-Wl,-rpath-link,"$rootfs/usr/lib/aarch64-linux-gnu" \
	-Wl,--build-id=sha1 \
	-lm \
	-o "$out/rg40xxv-shell"

aarch64-linux-gnu-strip --strip-unneeded "$out/rg40xxv-shell"
file "$out/rg40xxv-shell"
aarch64-linux-gnu-readelf -d "$out/rg40xxv-shell" |
	sed -n '/NEEDED/p'
sha256sum "$out/rg40xxv-shell"
