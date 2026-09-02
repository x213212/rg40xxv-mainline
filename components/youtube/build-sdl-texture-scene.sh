#!/bin/sh
set -eu
project=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
workspace=${RG40XXV_WORKSPACE:-$(CDPATH= cd -- "$project/../../../.." && pwd -P)}
rootfs=$workspace/firmware/mnt/rootfs
out=$project/build
cxx=${CXX:-aarch64-linux-gnu-g++-12}
temporary=$out/.rg40xxv-youtube-sdl-texture-scene.$$
mkdir -p "$out"
trap 'rm -f -- "$temporary"' EXIT HUP INT TERM
"$cxx" -std=c++17 -O2 -pipe -mcpu=cortex-a53 -Wall -Wextra -Werror -fno-exceptions -fno-rtti -ffile-prefix-map="$workspace"=/usr/src/rg40xxv-youtube -fdebug-prefix-map="$workspace"=/usr/src/rg40xxv-youtube -fno-record-gcc-switches -idirafter "$rootfs/usr/include" -I"$rootfs/usr/include/SDL2" -I"$project/include" -I"$project/src" "$project/src/sdl_texture_scene.cpp" "$project/src/home_catalog.cpp" "$project/src/home_view.cpp" "$project/src/player_controls.cpp" "$rootfs/usr/lib/aarch64-linux-gnu/libmpv.so.1.109.0" "$rootfs/usr/lib/aarch64-linux-gnu/libSDL2.so" "$rootfs/usr/lib/aarch64-linux-gnu/libSDL2_ttf.so" "$rootfs/usr/lib/aarch64-linux-gnu/libSDL2_image.so" -Wl,-rpath-link,"$rootfs/lib/aarch64-linux-gnu" -Wl,-rpath-link,"$rootfs/usr/lib" -Wl,-rpath-link,"$rootfs/usr/lib/aarch64-linux-gnu" -Wl,-rpath-link,"$rootfs/usr/lib/aarch64-linux-gnu/pulseaudio" -Wl,-rpath-link,"$rootfs/usr/lib/aarch64-linux-gnu/blas" -Wl,-rpath-link,"$rootfs/usr/lib/aarch64-linux-gnu/lapack" -Wl,--allow-shlib-undefined -Wl,--build-id=sha1 -o "$temporary"
aarch64-linux-gnu-strip --strip-unneeded "$temporary"
chmod 0755 "$temporary"
mv -f -- "$temporary" "$out/rg40xxv-youtube-sdl-texture-scene"
trap - EXIT HUP INT TERM
file "$out/rg40xxv-youtube-sdl-texture-scene"
aarch64-linux-gnu-readelf -d "$out/rg40xxv-youtube-sdl-texture-scene" | sed -n '/NEEDED/p'
printf '%s\n' 'YOUTUBE_TEXTURE_BUILD PASS target=rg40xxv-youtube-sdl-texture-scene existing_targets=untouched'
