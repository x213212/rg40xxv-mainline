#!/bin/sh
set -eu

project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
temporary=$(mktemp -d)
trap 'status=$?; trap - EXIT; rm -rf -- "$temporary"; exit "$status"' \
	EXIT HUP INT TERM

cc -std=c11 -O1 -g -Wall -Wextra -Werror \
	-fno-omit-frame-pointer \
	-fsanitize=address,undefined -fno-sanitize-recover=all \
	-I"$project/include" \
	"$project/src/input_latch.c" \
	"$project/src/input_latch_snapshot.c" \
	"$project/src/input_navigation.c" \
	"$project/tests/input_latch_test.c" \
	-o "$temporary/input-latch-test"

ASAN_OPTIONS=detect_leaks=0:halt_on_error=1:abort_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	"$temporary/input-latch-test"
