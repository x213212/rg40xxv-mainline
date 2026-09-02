#!/bin/sh
set -eu

project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
temporary=$(mktemp -d)
trap 'status=$?; trap - EXIT; rm -rf -- "$temporary"; exit "$status"' \
	EXIT HUP INT TERM

cc -std=c11 -O2 -Wall -Wextra -Werror \
	-I"$project/include" \
	"$project/src/frame_scheduler.c" \
	"$project/tests/frame_scheduler_test.c" \
	-o "$temporary/frame-scheduler-test"
"$temporary/frame-scheduler-test"
