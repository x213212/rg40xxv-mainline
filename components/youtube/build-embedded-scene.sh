#!/bin/sh
set -eu

project=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
workspace=${RG40XXV_WORKSPACE:-$(CDPATH= cd -- "$project/../../../.." && pwd -P)}
rootfs=$workspace/firmware/mnt/rootfs
out=$project/build
cxx=${CXX:-aarch64-linux-gnu-g++-12}
output=$out/rg40xxv-youtube-embedded-scene
temporary=$out/.rg40xxv-youtube-embedded-scene.$$

case $workspace in
	/*) ;;
	*) printf '%s\n' 'RG40XXV_WORKSPACE must be absolute' >&2; exit 2 ;;
esac

cleanup()
{
	if [ -f "$temporary" ]; then
		rm -f -- "$temporary"
	fi
}
trap cleanup EXIT HUP INT TERM

mkdir -p "$out"
export LC_ALL=C
export SOURCE_DATE_EPOCH=0

"$cxx" \
	-std=c++17 -O2 -pipe -mcpu=cortex-a53 \
	-Wall -Wextra -Werror -fno-exceptions -fno-rtti \
	-ffile-prefix-map="$workspace"=/usr/src/rg40xxv-youtube \
	-fdebug-prefix-map="$workspace"=/usr/src/rg40xxv-youtube \
	-fno-record-gcc-switches \
	-idirafter "$rootfs/usr/include" \
	-I"$project/include" \
	-I"$rootfs/usr/include/SDL2" \
	"$project/src/embedded_scene.cpp" \
	"$rootfs/usr/lib/aarch64-linux-gnu/libmpv.so.1.109.0" \
	"$rootfs/usr/lib/aarch64-linux-gnu/libSDL2_ttf.so" \
	"$rootfs/usr/lib/aarch64-linux-gnu/libSDL2.so" \
	"$rootfs/usr/lib/aarch64-linux-gnu/libGLESv2.so.2.1.0" \
	-Wl,-rpath-link,"$rootfs/lib/aarch64-linux-gnu" \
	-Wl,-rpath-link,"$rootfs/usr/lib" \
	-Wl,-rpath-link,"$rootfs/usr/lib/aarch64-linux-gnu" \
	-Wl,-rpath-link,"$rootfs/usr/lib/aarch64-linux-gnu/pulseaudio" \
	-Wl,-rpath-link,"$rootfs/usr/lib/aarch64-linux-gnu/blas" \
	-Wl,-rpath-link,"$rootfs/usr/lib/aarch64-linux-gnu/lapack" \
	-Wl,--allow-shlib-undefined \
	-Wl,--build-id=sha1 \
	-o "$temporary"

aarch64-linux-gnu-strip --strip-unneeded "$temporary"
chmod 0755 "$temporary"
mv -f -- "$temporary" "$output"
trap - EXIT HUP INT TERM

file "$output"
aarch64-linux-gnu-readelf -d "$output" | sed -n '/NEEDED/p'
sha256sum "$output"
printf '%s\n' 'YOUTUBE_EMBEDDED_BUILD PASS target=rg40xxv-youtube-embedded-scene existing_targets=untouched'
