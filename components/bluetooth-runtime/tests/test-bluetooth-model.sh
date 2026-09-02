#!/bin/sh
set -eu

project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
temporary=$(mktemp -d)
trap 'status=$?; trap - EXIT; rm -rf -- "$temporary"; exit "$status"' \
	EXIT HUP INT TERM
cc=${HOST_CC:-cc}

"$cc" -std=c11 -O1 -g -Wall -Wextra -Werror -Wformat=2 -Wshadow \
	-fno-omit-frame-pointer -fsanitize=address,undefined \
	-fno-sanitize-recover=all -I"$project/src" \
	"$project/tests/bluetooth_model_test.c" \
	"$project/src/bluetooth_model.c" -o "$temporary/bluetooth-model-test"

sanitizer_stderr=$temporary/sanitizer.stderr
if ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:abort_on_error=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	"$temporary/bluetooth-model-test" 2>"$sanitizer_stderr"; then
	test ! -s "$sanitizer_stderr" || cat "$sanitizer_stderr" >&2
elif grep -Fq 'LeakSanitizer does not work under ptrace' \
		"$sanitizer_stderr"; then
	# Some managed CI runners trace each newly executed child even though the
	# shell's own TracerPid is zero.  Retry only this documented unsupported
	# LSAN case; ASan and UBSan stay fully enabled.  Any real sanitizer failure
	# above remains fatal and is never converted into a pass.
	ASAN_OPTIONS=detect_leaks=0:halt_on_error=1:abort_on_error=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
		"$temporary/bluetooth-model-test"
else
	cat "$sanitizer_stderr" >&2
	exit 1
fi
