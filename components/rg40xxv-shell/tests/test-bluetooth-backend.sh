#!/bin/sh
set -eu

project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
workspace=${RG40XXV_WORKSPACE:-$(CDPATH= cd -- "$project/../../../.." && pwd -P)}
rootfs="$workspace/firmware/mnt/rootfs"
temporary=$(mktemp -d)
trap 'status=$?; trap - EXIT; if test "$status" -ne 0; then cat "$temporary/stdout" "$temporary/stderr" 2>/dev/null || :; fi; rm -rf -- "$temporary"; exit "$status"' \
	EXIT HUP INT TERM
helper="$temporary/bluetooth-helper"
binary="$temporary/bluetooth-backend-test"
pass_capability="$temporary/pass.env"
pending_capability="$temporary/pending.env"
malformed_capability="$temporary/malformed.env"
unsafe_capability="$temporary/unsafe.env"
linked_capability="$temporary/linked.env"
missing_capability="$temporary/missing.env"
capture="$helper.capture"
cc=${HOST_CC:-cc}

printf '%s\n%s\n' \
	'schema=rg40xxv-bluetooth-runtime-admission-v1' \
	'status=PASS' >"$pass_capability"
printf '%s\n%s\n' \
	'schema=rg40xxv-bluetooth-runtime-admission-v1' \
	'status=PENDING' >"$pending_capability"
printf '%s\n%s\n%s\n' \
	'schema=rg40xxv-bluetooth-runtime-admission-v1' \
	'status=PASS' 'unexpected=true' >"$malformed_capability"
cp "$pass_capability" "$unsafe_capability"
chmod 0600 "$pass_capability" "$pending_capability" "$malformed_capability"
chmod 0666 "$unsafe_capability"
ln -s "$pass_capability" "$linked_capability"

"$cc" -std=c11 -O1 -g -Wall -Wextra -Werror \
	-DBLUETOOTH_FIXTURE_HELPER \
	"$project/tests/bluetooth_backend_test.c" \
	-o "$helper"
chmod 0700 "$helper"

"$cc" -std=c11 -O1 -g -Wall -Wextra -Werror \
	-DRG40XXV_BLUETOOTH_BACKEND_TESTING \
	-fno-omit-frame-pointer -fsanitize=address,undefined \
	-fno-sanitize-recover=all -pthread \
	-I"$project/include" -idirafter "$rootfs/usr/include" \
	-I"$rootfs/usr/include/SDL2" \
	"$project/tests/bluetooth_backend_test.c" \
	"$project/src/bluetooth_backend.c" \
	-Wl,-l:libSDL2-2.0.so.0 \
	-o "$binary"

ASAN_OPTIONS=detect_leaks=0:halt_on_error=1:abort_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
SDL_VIDEODRIVER=dummy \
	"$binary" "$helper" "$pass_capability" "$pending_capability" \
	"$malformed_capability" "$unsafe_capability" "$missing_capability" \
	"$linked_capability" \
	>"$temporary/stdout" 2>"$temporary/stderr"

grep -Fq 'BLUETOOTH_BACKEND_TEST PASS' "$temporary/stdout"
grep -Fxq 'status' "$capture"
grep -Fxq 'scan' "$capture"
grep -Fxq 'power	off' "$capture"
grep -Fxq 'power	on' "$capture"
grep -Fxq 'pair	AA:BB:CC:DD:EE:FF' "$capture"
grep -Fxq 'connect	AA:BB:CC:DD:EE:FF' "$capture"
grep -Fxq 'disconnect	AA:BB:CC:DD:EE:FF' "$capture"
grep -Fxq 'forget	AA:BB:CC:DD:EE:FF' "$capture"
test ! -e "$helper.env-leak"
if grep -Eq 'ERROR: AddressSanitizer|runtime error:' \
	"$temporary/stdout" "$temporary/stderr"; then
	printf '%s\n' 'bluetooth backend sanitizer finding' >&2
	exit 1
fi
cat "$temporary/stdout"
