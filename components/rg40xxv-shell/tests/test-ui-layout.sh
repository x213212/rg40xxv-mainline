#!/bin/sh
set -eu

project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
temporary=$(mktemp -d)
trap 'status=$?; trap - EXIT; rm -rf -- "$temporary"; exit "$status"' \
	EXIT HUP INT TERM

cc -std=c11 -O2 -Wall -Wextra -Werror \
	-I"$project/include" \
	"$project/src/ui_layout.c" "$project/tests/ui_layout_test.c" \
	-lm -o "$temporary/ui-layout-test"
"$temporary/ui-layout-test"
