#!/bin/sh
set -eu

project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
temporary=$(mktemp -d)
trap 'status=$?; trap - EXIT; if test "$status" -ne 0; then cat "$temporary/test.stdout" "$temporary/test.stderr" "$temporary/test.valgrind" 2>/dev/null || :; fi; rm -rf -- "$temporary"; exit "$status"' \
	EXIT HUP INT TERM
cc=${HOST_CC:-${CC:-cc}}

# shellcheck source=memory-safety-gate.sh
. "$project/tests/memory-safety-gate.sh"

"$cc" -std=c11 -O1 -g -Wall -Wextra -Werror \
	-fno-omit-frame-pointer -fsanitize=address,undefined \
	-fno-sanitize-recover=all \
	-I"$project/include" \
	"$project/tests/network_state_test.c" \
	"$project/src/network_state.c" \
	-o "$temporary/network-state-test"
rg40xxv_select_leak_backend "$cc" "$temporary" \
	"$project/tests/memory_safety_probe.c"
snapshot="$temporary/snapshot.v1"
printf '%b\n' \
	'RG40XXV_NETWORK_SNAPSHOT	1' \
	'S	1	1	1	0	550e8400-e29b-41d4-a716-446655440000	AA:BB:CC:DD:EE:FF	Home%3AWiFi	192.168.0.125%2F24	-' \
	'A	AA:BB:CC:DD:EE:FF	78	WPA2	Home%3AWiFi	1	1	550e8400-e29b-41d4-a716-446655440000' \
	'A	11:22:33:44:55:66	64	WPA2	Cafe%20Net	0	0	-' \
	>"$snapshot"
chmod 0600 "$snapshot"
scan="$temporary/scan.v1"
printf '%b\n' \
	'RG40XXV_NETWORK_SNAPSHOT	1' \
	'S	1	1	0	0	-	-	-	-	-' \
	'A	11:22:33:44:55:66	88	WPA2	Cafe%20Net	0	0	-' \
	'A	22:33:44:55:66:77	42	--	Open%20Net	0	0	-' \
	>"$scan"
chmod 0600 "$scan"
unsafe="$temporary/unsafe.v1"
cp "$snapshot" "$unsafe"
chmod 0666 "$unsafe"
linked="$temporary/linked.v1"
ln -s "$snapshot" "$linked"
case $RG40XXV_LEAK_BACKEND in
lsan) detect_leaks=1 ;;
valgrind|skip) detect_leaks=0 ;;
*) printf 'unknown leak backend: %s\n' "$RG40XXV_LEAK_BACKEND" >&2; exit 1 ;;
esac
ASAN_OPTIONS=detect_leaks=$detect_leaks:halt_on_error=1:abort_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	"$temporary/network-state-test" "$snapshot" "$scan" "$unsafe" "$linked" \
	>"$temporary/test.stdout" 2>"$temporary/test.stderr"
grep -Fq 'NETWORK_STATE_TEST PASS' "$temporary/test.stdout"
if grep -Eq 'ERROR: AddressSanitizer|runtime error:' \
	"$temporary/test.stdout" "$temporary/test.stderr"; then
	printf '%s\n' 'network state sanitizer finding' >&2
	exit 1
fi

if test "$RG40XXV_LEAK_BACKEND" = valgrind; then
	"$cc" -std=c11 -O1 -g -Wall -Wextra -Werror \
		-I"$project/include" \
		"$project/tests/network_state_test.c" \
		"$project/src/network_state.c" \
		-o "$temporary/network-state-test-plain"
	rg40xxv_valgrind_run "$temporary/test.valgrind" \
		"$temporary/network-state-test-plain" \
		"$snapshot" "$scan" "$unsafe" "$linked" \
		>"$temporary/valgrind.stdout"
	grep -Fq 'NETWORK_STATE_TEST PASS' "$temporary/valgrind.stdout"
fi

cat "$temporary/test.stdout"
if test "$RG40XXV_LEAK_BACKEND" = skip; then
	printf 'NETWORK_MEMORY_SAFETY PASS asan=PASS ubsan=PASS leak=SKIP reason=%s\n' \
		"$RG40XXV_LEAK_SKIP_REASON"
else
	printf 'NETWORK_MEMORY_SAFETY PASS asan=PASS ubsan=PASS leak=%s\n' \
		"$RG40XXV_LEAK_BACKEND"
fi
