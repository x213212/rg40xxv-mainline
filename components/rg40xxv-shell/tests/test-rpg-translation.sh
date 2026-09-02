#!/bin/sh
set -eu

project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
workspace=${RG40XXV_WORKSPACE:-$(CDPATH= cd -- "$project/../../../.." && pwd -P)}
temporary=$(mktemp -d)
trap 'status=$?; trap - EXIT; rm -rf -- "$temporary"; exit "$status"' \
	EXIT HUP INT TERM

cc -std=c11 -O2 -Wall -Wextra -Werror \
	-I"$project/include" \
	-I"$workspace/firmware/mnt/rootfs/usr/include/SDL2" \
	-I"$workspace/services/netstream/include" \
	"$project/tests/rpg_translation_test.c" \
	"$project/src/rpg_translation.c" \
	-o "$temporary/rpg-translation-test"

"$temporary/rpg-translation-test" "$temporary/translation-mode"
test "$(stat -c %a "$temporary/translation-mode")" = 600

printf '%s\n' 'RPG translation menu state: PASS persistent=off,static RM2k=safe-block'
