#!/bin/sh
set -eu
umask 022

project=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
workspace=${RG40XXV_WORKSPACE:-$(CDPATH= cd -- "$project/../../../.." && pwd -P)}
rootfs="$workspace/firmware/mnt/rootfs"
out="$project/build"
release="$out/release-root"
cxx=${CXX:-aarch64-linux-gnu-g++-12}
stage=
previous=
player_tmp=
native_tmp=

cleanup()
{
	for temporary in "$player_tmp" "$native_tmp"; do
		if [ -n "$temporary" ] && [ -f "$temporary" ] && [ ! -L "$temporary" ]; then
			rm -f -- "$temporary"
		fi
	done
	if [ -n "$stage" ] && [ -d "$stage" ] && [ ! -L "$stage" ]; then
		find "$stage" -mindepth 1 -delete
		rmdir "$stage"
	fi
	if [ -n "$previous" ] && [ -d "$previous" ] && [ ! -L "$previous" ]; then
		if [ ! -e "$release" ] && [ ! -L "$release" ]; then
			mv -- "$previous" "$release"
		else
			find "$previous" -mindepth 1 -delete
			rmdir "$previous"
		fi
	fi
}

trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

case $workspace in
	/*) ;;
	*) printf '%s\n' 'RG40XXV_WORKSPACE must be absolute' >&2; exit 2 ;;
esac
case $out in
	"$project"/build) ;;
	*) printf '%s\n' 'unsafe build directory' >&2; exit 2 ;;
esac
[ ! -L "$out" ] || {
	printf 'refusing symlink build directory: %s\n' "$out" >&2
	exit 2
}

mkdir -p "$out"
command -v flock >/dev/null 2>&1 || {
	printf '%s\n' 'missing build lock tool: flock' >&2
	exit 2
}
# Join the formal p7 source/output lock so a direct component build cannot
# replace release-root while a cycle is hashing or packaging it.  Formal
# runners pass the already-locked fd 9; standalone builds acquire it here.
host_lock=$workspace/reports/.rg40xxv-p7-host-build.lock
mkdir -p "$workspace/reports"
if [ "${P7_HOST_LOCK_HELD:-0}" = 1 ]; then
	: >&9 2>/dev/null || {
		printf '%s\n' 'inherited p7 host build lock fd missing' >&2
		exit 1
	}
	[ "$(stat -Lc '%d:%i' /proc/$$/fd/9)" = \
	  "$(stat -Lc '%d:%i' "$host_lock")" ] || {
		printf '%s\n' 'inherited p7 host build lock inode mismatch' >&2
		exit 1
	}
	flock -n 9 || {
		printf '%s\n' 'inherited p7 host build lock invalid' >&2
		exit 1
	}
else
	exec 9>"$host_lock"
	flock -w 3600 9 || {
		printf '%s\n' 'p7 host build lock timeout' >&2
		exit 1
	}
fi
exec 8>"$out/.youtube-h700-build.lock"
flock -w 3600 8 || {
	printf '%s\n' 'YouTube component build lock timeout' >&2
	exit 1
}
export LC_ALL=C
export SOURCE_DATE_EPOCH=0

player_tmp=$out/.rg40xxv-youtube-player-probe.$$
native_tmp=$out/.rg40xxv-youtube-native.$$

"$cxx" \
	-std=c++17 -O2 -pipe -mcpu=cortex-a53 \
	-Wall -Wextra -Werror -fno-exceptions -fno-rtti \
	-ffile-prefix-map="$workspace"=/usr/src/rg40xxv-youtube \
	-fdebug-prefix-map="$workspace"=/usr/src/rg40xxv-youtube \
	-fno-record-gcc-switches \
	-idirafter "$rootfs/usr/include" \
	"$project/src/player_probe.cpp" \
	"$rootfs/usr/lib/aarch64-linux-gnu/libmpv.so.1.109.0" \
	-Wl,-rpath-link,"$rootfs/lib/aarch64-linux-gnu" \
	-Wl,-rpath-link,"$rootfs/usr/lib" \
	-Wl,-rpath-link,"$rootfs/usr/lib/aarch64-linux-gnu" \
	-Wl,-rpath-link,"$rootfs/usr/lib/aarch64-linux-gnu/pulseaudio" \
	-Wl,-rpath-link,"$rootfs/usr/lib/aarch64-linux-gnu/blas" \
	-Wl,-rpath-link,"$rootfs/usr/lib/aarch64-linux-gnu/lapack" \
	-Wl,--allow-shlib-undefined \
	-Wl,--build-id=sha1 \
	-o "$player_tmp"

"$cxx" \
	-std=c++17 -O2 -pipe -mcpu=cortex-a53 \
	-Wall -Wextra -Werror -fno-exceptions -fno-rtti \
	-ffile-prefix-map="$workspace"=/usr/src/rg40xxv-youtube \
	-fdebug-prefix-map="$workspace"=/usr/src/rg40xxv-youtube \
	-fno-record-gcc-switches \
	-idirafter "$rootfs/usr/include" \
	-I"$project/include" \
	-I"$rootfs/usr/include/SDL2" \
	"$project/src/native_frontend.cpp" \
	"$rootfs/usr/lib/aarch64-linux-gnu/libSDL2_ttf.so" \
	"$rootfs/usr/lib/aarch64-linux-gnu/libSDL2.so" \
	-Wl,-rpath-link,"$rootfs/lib/aarch64-linux-gnu" \
	-Wl,-rpath-link,"$rootfs/usr/lib/aarch64-linux-gnu" \
	-Wl,--build-id=sha1 \
	-o "$native_tmp"

aarch64-linux-gnu-strip --strip-unneeded \
	"$player_tmp"
aarch64-linux-gnu-strip --strip-unneeded \
	"$native_tmp"
chmod 0755 "$player_tmp" "$native_tmp"
mv -f -- "$player_tmp" "$out/rg40xxv-youtube-player-probe"
player_tmp=
mv -f -- "$native_tmp" "$out/rg40xxv-youtube-native"
native_tmp=
"$project/build-sdl-texture-scene.sh"

stage=$(mktemp -d "$out/.release-root.XXXXXXXX")
chmod 0755 "$stage"
install -d -m 0755 "$stage/bin" "$stage/tools" "$stage/vendor" \
	"$stage/manifest" "$stage/runtime"
install -m 0755 "$out/rg40xxv-youtube-native" \
	"$stage/bin/rg40xxv-youtube-native-legacy"
install -m 0755 "$out/rg40xxv-youtube-player-probe" \
	"$stage/bin/rg40xxv-youtube-player"
install -m 0755 "$out/rg40xxv-youtube-sdl-texture-scene" \
	"$stage/bin/rg40xxv-youtube-sdl-texture-scene"
install -m 0755 "$project/bin/rg40xxv-youtube-native-session" \
	"$stage/bin/rg40xxv-youtube-native-session"
install -m 0755 "$project/bin/rg40xxv-youtube-resolver-ensure" \
	"$stage/bin/rg40xxv-youtube-resolver-ensure"
install -m 0755 "$project/bin/rg40xxv-youtube-texture-live-smoke" \
	"$stage/bin/rg40xxv-youtube-native"
install -m 0755 "$project/tools/resolve_youtube.py" \
	"$project/tools/resolver_cache.py" \
	"$project/tools/endpoint_broker.py" \
	"$project/tools/youtube_feed.py" \
	"$project/tools/yt_dlp_server.py" \
	"$project/tools/bounded_range_bridge.py" "$stage/tools/"
install -m 0755 "$project/vendor/yt-dlp" "$stage/vendor/yt-dlp"
install -m 0644 "$project/manifest/runtime-admission.env" \
	"$stage/runtime/admission.env"

(
	cd "$stage"
	find bin runtime tools vendor -type f -print0 | sort -z | xargs -0 sha256sum \
		>manifest/SHA256SUMS
	chmod 0644 manifest/SHA256SUMS
	[ "$(wc -l <manifest/SHA256SUMS)" -eq 14 ]
	sha256sum -c manifest/SHA256SUMS >/dev/null
)

for required in \
	bin/rg40xxv-youtube-native \
	bin/rg40xxv-youtube-native-legacy \
	bin/rg40xxv-youtube-player \
	bin/rg40xxv-youtube-sdl-texture-scene \
	bin/rg40xxv-youtube-native-session \
	bin/rg40xxv-youtube-resolver-ensure \
	tools/resolve_youtube.py \
	tools/resolver_cache.py \
	tools/endpoint_broker.py \
	tools/youtube_feed.py \
	tools/yt_dlp_server.py \
	tools/bounded_range_bridge.py \
	vendor/yt-dlp \
	runtime/admission.env; do
	[ -f "$stage/$required" ] && [ ! -L "$stage/$required" ] || {
		printf 'incomplete YouTube release group: %s\n' "$required" >&2
		exit 1
	}
done

if [ -e "$release" ] || [ -L "$release" ]; then
	[ -d "$release" ] && [ ! -L "$release" ] || {
		printf 'unsafe release root: %s\n' "$release" >&2
		exit 2
	}
	previous=$(mktemp -d "$out/.release-root.previous.XXXXXXXX")
	rmdir "$previous"
	mv -- "$release" "$previous"
fi
mv "$stage" "$release"
stage=
if [ -n "$previous" ]; then
	find "$previous" -mindepth 1 -delete
	rmdir "$previous"
	previous=
fi
trap - EXIT
trap - HUP INT TERM

file "$out/rg40xxv-youtube-player-probe"
file "$out/rg40xxv-youtube-native"
file "$out/rg40xxv-youtube-sdl-texture-scene"
aarch64-linux-gnu-readelf -d "$out/rg40xxv-youtube-player-probe" |
	sed -n '/NEEDED/p'
aarch64-linux-gnu-readelf -d "$out/rg40xxv-youtube-native" |
	sed -n '/NEEDED/p'
aarch64-linux-gnu-readelf -d "$out/rg40xxv-youtube-sdl-texture-scene" |
	sed -n '/NEEDED/p'
sha256sum "$out/rg40xxv-youtube-player-probe"
sha256sum "$out/rg40xxv-youtube-native"
sha256sum "$out/rg40xxv-youtube-sdl-texture-scene"
sha256sum "$release/manifest/SHA256SUMS"
printf '%s\n' 'YOUTUBE_H700_RELEASE_BUILD PASS frontend=texture+native player=probe tools=6 wrappers=2 manifest=SHA256SUMS'
