#!/bin/sh
set -eu

project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
output=${TMPDIR:-/tmp}/rg40xxv-power-state-test

cc -std=c11 -O2 -Wall -Wextra -Werror -I"$project/include" \
	"$project/src/power.c" "$project/tests/power_state_test.c" -o "$output"
"$output"
