#!/bin/sh
set -eu

project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
release="$project/build/release-root"
manifest="$release/manifest/SHA256SUMS"
expected=$(mktemp /tmp/rg40xxv-youtube-package-expected.XXXXXXXX)
actual=$(mktemp /tmp/rg40xxv-youtube-package-actual.XXXXXXXX)
expected_tree=$(mktemp /tmp/rg40xxv-youtube-package-tree-expected.XXXXXXXX)
actual_tree=$(mktemp /tmp/rg40xxv-youtube-package-tree-actual.XXXXXXXX)
trap 'rm -f -- "$expected" "$actual" "$expected_tree" "$actual_tree"' EXIT HUP INT TERM

"$project/build.sh"

printf '%s\n' \
	bin/rg40xxv-youtube-native \
	bin/rg40xxv-youtube-native-legacy \
	bin/rg40xxv-youtube-native-session \
	bin/rg40xxv-youtube-player \
	bin/rg40xxv-youtube-resolver-ensure \
	bin/rg40xxv-youtube-sdl-texture-scene \
	runtime/admission.env \
	tools/bounded_range_bridge.py \
	tools/endpoint_broker.py \
	tools/resolve_youtube.py \
	tools/resolver_cache.py \
	tools/youtube_feed.py \
	tools/yt_dlp_server.py \
	vendor/yt-dlp >"$expected"

[ -f "$manifest" ] && [ ! -L "$manifest" ]
awk '{ print $2 }' "$manifest" >"$actual"
cmp "$expected" "$actual"
(
	cd "$release"
	sha256sum -c manifest/SHA256SUMS >/dev/null
)

cmp "$project/build/rg40xxv-youtube-native" \
	"$release/bin/rg40xxv-youtube-native-legacy"
cmp "$project/build/rg40xxv-youtube-player-probe" \
	"$release/bin/rg40xxv-youtube-player"
cmp "$project/build/rg40xxv-youtube-sdl-texture-scene" \
	"$release/bin/rg40xxv-youtube-sdl-texture-scene"
cmp "$project/bin/rg40xxv-youtube-native-session" \
	"$release/bin/rg40xxv-youtube-native-session"
cmp "$project/bin/rg40xxv-youtube-resolver-ensure" \
	"$release/bin/rg40xxv-youtube-resolver-ensure"
cmp "$project/bin/rg40xxv-youtube-texture-live-smoke" \
	"$release/bin/rg40xxv-youtube-native"
cmp "$project/manifest/runtime-admission.env" \
	"$release/runtime/admission.env"

for tool in resolve_youtube.py resolver_cache.py endpoint_broker.py \
	youtube_feed.py yt_dlp_server.py bounded_range_bridge.py; do
	cmp "$project/tools/$tool" "$release/tools/$tool"
done
cmp "$project/vendor/yt-dlp" "$release/vendor/yt-dlp"

while IFS= read -r path; do
	[ "$path" != runtime/admission.env ] || continue
	[ -f "$release/$path" ] && [ ! -L "$release/$path" ] && \
		[ -x "$release/$path" ]
done <"$expected"

[ "$(stat -c %a "$release")" -eq 755 ]
[ "$(stat -c %a "$manifest")" -eq 644 ]
[ "$(stat -c %a "$release/runtime/admission.env")" -eq 644 ]
for directory in bin manifest runtime tools vendor; do
	[ "$(stat -c %a "$release/$directory")" -eq 755 ]
done

printf '%s\n' \
	d:bin \
	d:manifest \
	d:runtime \
	d:tools \
	d:vendor \
	f:bin/rg40xxv-youtube-native \
	f:bin/rg40xxv-youtube-native-legacy \
	f:bin/rg40xxv-youtube-native-session \
	f:bin/rg40xxv-youtube-resolver-ensure \
	f:bin/rg40xxv-youtube-player \
	f:bin/rg40xxv-youtube-sdl-texture-scene \
	f:manifest/SHA256SUMS \
	f:runtime/admission.env \
	f:tools/bounded_range_bridge.py \
	f:tools/endpoint_broker.py \
	f:tools/resolve_youtube.py \
	f:tools/resolver_cache.py \
	f:tools/youtube_feed.py \
	f:tools/yt_dlp_server.py \
	f:vendor/yt-dlp | LC_ALL=C sort >"$expected_tree"
find "$release" -mindepth 1 -printf '%y:%P\n' | LC_ALL=C sort >"$actual_tree"
cmp "$expected_tree" "$actual_tree"

[ "$(find "$release" -type f | wc -l)" -eq 15 ]
[ -z "$(find "$release" -type l -print -quit)" ]
[ -z "$(find "$release" -type f \( -iname '*.img' -o -iname '*.dtb' -o \
	-iname '*.dtbo' -o -iname 'Image' -o -iname 'zImage' -o \
	-iname 'uImage' -o -iname '*u-boot*' \) -print -quit)" ]

printf '%s\n' 'YOUTUBE_H700_PACKAGE_TEST PASS payload=14 manifest=VALID frontend=texture canonical=wrapper resolver=SYSTEMD_SINGLETON legacy=diagnostic player=probe tools=6 p8_payload=NONE device=UNVERIFIED'
