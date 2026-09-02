#!/bin/sh
set -eu
project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
fixture=$(mktemp -d /tmp/rg40xxv-home-catalog.XXXXXXXX)
trap 'find "$fixture" -type f -delete 2>/dev/null || true; find "$fixture" -depth -type d -empty -delete 2>/dev/null || true' EXIT HUP INT TERM
chmod 0700 "$fixture"
index=1
while test "$index" -le 96; do
	id=$(printf 'HomeCard%03d' "$index")
	: >"$fixture/$id.jpg"
	chmod 0600 "$fixture/$id.jpg"
	index=$((index + 1))
done
cp "$project/tests/fake-youtube-feed.sh" "$fixture/feed"
chmod 0700 "$fixture/feed"
g++ -std=c++17 -O2 -Wall -Wextra -Werror -fno-exceptions -fno-rtti \
	-I"$project/src" "$project/tests/home_catalog_unit.cpp" \
	"$project/src/home_catalog.cpp" -o "$fixture/unit"
RG_YOUTUBE_HOME_TEST_ROOT=$fixture "$fixture/unit" "$fixture/feed"
