#!/bin/sh
set -eu

project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
workspace=${RG40XXV_WORKSPACE:-$(CDPATH= cd -- "$project/../../../.." && pwd -P)}
temporary=$(mktemp -d)
trap 'status=$?; trap - EXIT; rm -rf -- "$temporary"; exit "$status"' \
	EXIT HUP INT TERM

tf1=$temporary/tf1/Roms
tf2=$temporary/tf2/Roms
removed=$temporary/tf2/Roms.removed
binary=$temporary/catalog-optional-source-test

mkdir -p "$tf1/GBA/shared" "$tf1/FC" "$tf2/gba/shared" "$tf2/SFC"
: >"$tf1/GBA/Primary Only.gba"
: >"$tf1/GBA/shared/Duplicate Game.gba"
: >"$tf1/FC/First.nes"
: >"$tf2/gba/TF2 Only.gba"
: >"$tf2/gba/shared/duplicate game.GBA"
: >"$tf2/SFC/Second.sfc"

cc -std=c11 -O2 -Wall -Wextra -Werror \
	-I"$project/include" \
	-I"$workspace/services/netstream/include" \
	-I"$workspace/firmware/mnt/rootfs/usr/include/SDL2" \
	"$project/tests/catalog_optional_source_test.c" \
	"$project/src/catalog.c" "$project/src/catalog_scan.c" \
	-o "$binary"

"$binary" "$tf1" "$tf2" "$removed"
