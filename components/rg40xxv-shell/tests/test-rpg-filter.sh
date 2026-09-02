#!/bin/sh
set -eu

project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
workspace=${RG40XXV_WORKSPACE:-$(CDPATH= cd -- "$project/../../../.." && pwd -P)}
temporary=$(mktemp -d)
trap 'status=$?; trap - EXIT; rm -rf -- "$temporary"; exit "$status"' \
	EXIT HUP INT TERM

cc -std=c11 -O2 -Wall -Wextra -Werror \
	-I"$project/include" \
	-I"$workspace/services/netstream/include" \
	-I"$workspace/firmware/mnt/rootfs/usr/include/SDL2" \
	"$project/tests/rpg_filter_test.c" "$project/src/catalog.c" \
	"$project/src/locale.c" \
	-o "$temporary/rpg-filter-test"

"$temporary/rpg-filter-test"
