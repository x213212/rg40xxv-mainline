#!/bin/sh
set -eu

project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
output=${TMPDIR:-/tmp}/rg40xxv-input-method-test

cc -std=c11 -O2 -Wall -Wextra -Werror -I"$project/include" \
	"$project/src/input_method.c" "$project/tests/input_method_test.c" \
	-o "$output"
"$output"
