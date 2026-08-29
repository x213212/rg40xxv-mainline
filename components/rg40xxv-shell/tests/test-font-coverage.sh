#!/bin/sh
set -eu

project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
font="$project/assets/RG40XXV-UI-Sans.otf"

test -s "$font"
test "$(stat -c '%s' "$font")" -le 12582912
python3 "$project/tests/font_coverage_test.py" "$font"
