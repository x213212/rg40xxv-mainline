#!/bin/sh
set -eu

project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
wrapper=$project/bin/rg40xxv-youtube-texture-live-smoke
fixture=$(mktemp -d /tmp/rg40xxv-youtube-texture-wrapper.XXXXXXXX)
trap 'find "$fixture" -mindepth 1 -delete 2>/dev/null || true; rmdir "$fixture" 2>/dev/null || true' EXIT HUP INT TERM
root=$fixture/component
state=$fixture/state-home
runtime=$fixture/runtime
shader=$fixture/shader-cache
marker=$fixture/marker
ensure_marker=$fixture/ensure-marker
install -d -m 0755 "$root/bin" "$root/tools"

cat >"$root/bin/rg40xxv-youtube-sdl-texture-scene" <<'EOF'
#!/bin/sh
set -eu
{
	printf 'ARG=%s\n' "$@"
	printf 'HOME=%s\n' "$HOME"
	printf 'XDG_CACHE_HOME=%s\n' "$XDG_CACHE_HOME"
	printf 'XDG_RUNTIME_DIR=%s\n' "$XDG_RUNTIME_DIR"
	printf 'FEED_CACHE=%s\n' "$RG_YOUTUBE_FEED_CACHE"
	printf 'RESOLVER_CACHE=%s\n' "$RG_YOUTUBE_CACHE_DIR"
	printf 'BROKER_RUNTIME=%s\n' "$RG_YOUTUBE_RUNTIME_ROOT"
	printf 'YTDLP_SOCKET=%s\n' "$RG_YOUTUBE_YTDLP_SOCKET"
	printf 'YTDLP_CACHE=%s\n' "$RG_YOUTUBE_YTDLP_CACHE"
} >"$RG_YOUTUBE_TEST_MARKER"
EOF
chmod 0755 "$root/bin/rg40xxv-youtube-sdl-texture-scene"

cat >"$root/bin/rg40xxv-youtube-resolver-ensure" <<'EOF'
#!/bin/sh
set -eu
{
	printf 'SOCKET_ENV=%s\n' "$RG_YOUTUBE_YTDLP_SOCKET"
	printf 'CACHE_ENV=%s\n' "$RG_YOUTUBE_YTDLP_CACHE"
	printf 'ROOT_ENV=%s\n' "$RG_YOUTUBE_TEXTURE_ROOT"
} >"$RG_YOUTUBE_TEST_ENSURE_MARKER"
EOF
chmod 0755 "$root/bin/rg40xxv-youtube-resolver-ensure"
export RG_YOUTUBE_TEST_ENSURE_MARKER=$ensure_marker

RG_YOUTUBE_TEXTURE_ROOT=$root \
RG_YOUTUBE_STATE_HOME=$state \
RG_YOUTUBE_RUNTIME_ROOT=$runtime \
RG_YOUTUBE_SHADER_CACHE_DIR=$shader \
RG_YOUTUBE_TEST_MARKER=$marker \
	"$wrapper" --broker /run/request.json 'https://youtu.be/GwtNiL9eEYk' \
	2>/dev/null

cat >"$fixture/expected" <<EOF
ARG=--broker
ARG=/run/request.json
ARG=https://youtu.be/GwtNiL9eEYk
HOME=$state
XDG_CACHE_HOME=$state/.cache
XDG_RUNTIME_DIR=$runtime/xdg
FEED_CACHE=$state/.cache/rg40xxv-youtube/feed
RESOLVER_CACHE=$state/.cache/rg40xxv-youtube/resolver
BROKER_RUNTIME=$runtime
YTDLP_SOCKET=$runtime/yt-dlp.sock
YTDLP_CACHE=$state/.cache/rg40xxv-youtube/yt-dlp
EOF
cmp "$fixture/expected" "$marker"
grep -Fqx "SOCKET_ENV=$runtime/yt-dlp.sock" "$ensure_marker"
grep -Fqx "CACHE_ENV=$state/.cache/rg40xxv-youtube/yt-dlp" "$ensure_marker"
grep -Fqx "ROOT_ENV=$root" "$ensure_marker"

RG_YOUTUBE_TEXTURE_ROOT=$root \
RG_YOUTUBE_STATE_HOME=$state \
RG_YOUTUBE_RUNTIME_ROOT=$runtime \
RG_YOUTUBE_SHADER_CACHE_DIR=$shader \
RG_YOUTUBE_TEST_MARKER=$marker \
	"$wrapper" 2>/dev/null
grep -Fqx 'ARG=--home' "$marker"

for private_directory in "$state" "$state/.cache" \
	"$state/.cache/rg40xxv-youtube" \
	"$state/.cache/rg40xxv-youtube/feed" \
	"$state/.cache/rg40xxv-youtube/resolver" \
	"$state/.cache/rg40xxv-youtube/yt-dlp" "$runtime" "$runtime/xdg" \
	"$shader"; do
	[ -d "$private_directory" ] && [ ! -L "$private_directory" ]
	[ "$(stat -c %a "$private_directory")" -eq 700 ]
done

bad_state=$fixture/bad-state
ln -s "$state" "$bad_state"
if RG_YOUTUBE_TEXTURE_ROOT=$root RG_YOUTUBE_STATE_HOME=$bad_state \
	RG_YOUTUBE_RUNTIME_ROOT=$fixture/bad-runtime \
	RG_YOUTUBE_SHADER_CACHE_DIR=$shader \
	RG_YOUTUBE_TEST_MARKER=$marker "$wrapper" --home \
	>"$fixture/bad.out" 2>"$fixture/bad.err"; then
	printf '%s\n' 'wrapper accepted a symlinked private state root' >&2
	exit 1
fi
grep -Fq 'reason=private-directory-symlink' "$fixture/bad.err"

bad_shader=$fixture/bad-shader
ln -s "$shader" "$bad_shader"
if RG_YOUTUBE_TEXTURE_ROOT=$root RG_YOUTUBE_STATE_HOME=$state \
	RG_YOUTUBE_RUNTIME_ROOT=$runtime RG_YOUTUBE_SHADER_CACHE_DIR=$bad_shader \
	RG_YOUTUBE_TEST_MARKER=$marker "$wrapper" --home \
	>"$fixture/bad-shader.out" 2>"$fixture/bad-shader.err"; then
	printf '%s\n' 'wrapper accepted a symlinked shader-cache directory' >&2
	exit 1
fi
grep -Fq 'reason=private-directory-symlink' "$fixture/bad-shader.err"

grep -Fq 'self=$(readlink -f -- "$0")' "$wrapper"
grep -Fq 'root=${RG_YOUTUBE_TEXTURE_ROOT:-$self_root}' "$wrapper"
grep -Fq 'RG_YOUTUBE_YTDLP_SOCKET=$yt_dlp_socket' "$wrapper"
grep -Fq 'RG_YOUTUBE_YTDLP_CACHE=$yt_dlp_cache' "$wrapper"
grep -Fq 'resolver_ensure=${RG_YOUTUBE_RESOLVER_ENSURE:-$root/bin/rg40xxv-youtube-resolver-ensure}' "$wrapper"
grep -Fq '"$resolver_ensure"' "$wrapper"
if grep -Fq -- '--workers 2 </dev/null &' "$wrapper"; then
	printf '%s\n' 'wrapper still owns a resolver child' >&2
	exit 1
fi
grep -Fq 'exec "$root/bin/rg40xxv-youtube-sdl-texture-scene" "$@"' "$wrapper"

printf '%s\n' 'YOUTUBE_TEXTURE_WRAPPER_TEST PASS argv=PASSTHROUGH default=HOME cache=PERSISTENT_PRIVATE resolver=SYSTEMD_SINGLETON broker=/run symlink=REJECTED'
