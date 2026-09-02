#!/bin/sh
set -eu

project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
workspace=${RG40XXV_WORKSPACE:-$(CDPATH= cd -- "$project/../../../.." && pwd -P)}
rootfs="$workspace/firmware/mnt/rootfs"
temporary=$(mktemp -d)
trap 'status=$?; trap - EXIT; if test "$status" -ne 0; then cat "$temporary/stdout" "$temporary/stderr" 2>/dev/null || :; fi; rm -rf -- "$temporary"; exit "$status"' \
	EXIT HUP INT TERM
binary="$temporary/stream-backend-test"
state="$temporary/state"
fixture="$temporary/discovery.fixture"
runner="$temporary/stream-runner"
capture="$temporary/runner.args"
cc=${HOST_CC:-cc}

cp "$project/tests/stream-discovery.fixture" "$fixture"
cp "$project/tests/fake-stream-runner.fixture" "$runner"
mkdir -m 0700 "$state"
chmod 0600 "$fixture"
chmod 0755 "$runner"

"$cc" -std=c11 -O1 -g -Wall -Wextra -Werror \
	-DRG40XXV_STREAM_BACKEND_TESTING \
	-fno-omit-frame-pointer -fsanitize=address,undefined \
	-fno-sanitize-recover=all -pthread \
	-I"$project/include" -I"$workspace/services/netstream/include" \
	-idirafter "$rootfs/usr/include" -I"$rootfs/usr/include/SDL2" \
	"$project/tests/stream_backend_test.c" \
	"$project/src/stream_backend.c" \
	"$workspace/services/netstream/src/netstream.c" \
	-Wl,-l:libSDL2-2.0.so.0 \
	-o "$binary"

ASAN_OPTIONS=detect_leaks=0:halt_on_error=1:abort_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
SDL_VIDEODRIVER=dummy FAKE_STREAM_CAPTURE="$capture" \
	"$binary" "$state" "$fixture" "$runner" \
	>"$temporary/stdout" 2>"$temporary/stderr"
grep -Fq 'STREAM_BACKEND_TEST PASS' "$temporary/stdout"
grep -Eq '^<[0-9]{4}>$' "$capture"
if grep -Eq 'ERROR: AddressSanitizer|runtime error:' \
	"$temporary/stdout" "$temporary/stderr"; then
	printf '%s\n' 'stream backend sanitizer finding' >&2
	exit 1
fi
cat "$temporary/stdout"
