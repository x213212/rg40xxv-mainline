#!/bin/sh
set -eu

project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
output=$(mktemp)
trap 'rm -f -- "$output"' EXIT HUP INT TERM

cc -std=c11 -O2 -Wall -Wextra -Werror -Wpedantic \
	-I"$project/include" \
	"$project/src/power_input_filter.c" \
	"$project/tests/power_input_filter_test.c" \
	-o "$output"
"$output"
