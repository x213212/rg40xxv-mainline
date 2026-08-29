#!/bin/sh
set -eu

project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
temporary=$(mktemp -d)
trap 'status=$?; trap - EXIT; rm -rf -- "$temporary"; exit "$status"' \
	EXIT HUP INT TERM

cc -std=c11 -O2 -Wall -Wextra -Werror \
	-I"$project/include" \
	"$project/src/input_navigation.c" \
	"$project/tests/input_navigation_test.c" \
	-o "$temporary/input-navigation-test"
"$temporary/input-navigation-test"
