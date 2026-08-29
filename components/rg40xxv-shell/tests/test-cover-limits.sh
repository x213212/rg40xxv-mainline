#!/bin/sh
set -eu

project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
temporary=$(mktemp -d)
trap 'status=$?; trap - EXIT; rm -rf -- "$temporary"; exit "$status"' \
	EXIT HUP INT TERM

cc -std=c11 -O2 -Wall -Wextra -Werror \
	-I"$project/include" \
	"$project/tests/cover_limits_test.c" \
	"$project/src/cover_limits.c" \
	-o "$temporary/cover-limits-test"

"$temporary/cover-limits-test"
