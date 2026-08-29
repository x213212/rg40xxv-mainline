#!/bin/sh
set -eu

project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
temporary=$(mktemp -d)
trap 'rm -rf "$temporary"' EXIT HUP INT TERM

${CC:-cc} -std=c11 -O2 -Wall -Wextra -Werror \
	-I"$project/include" \
	"$project/src/input_focus.c" \
	"$project/tests/input_focus_test.c" \
	-o "$temporary/input-focus-test"
"$temporary/input-focus-test"
