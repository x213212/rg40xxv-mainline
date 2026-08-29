#!/bin/sh
set -eu

project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
atlas="$project/assets/RG40XXV-Material-Icons.png"

test -s "$atlas"
python3 - "$atlas" <<'PY'
import sys
from PIL import Image

image = Image.open(sys.argv[1]).convert("RGBA")
assert image.size == (13 * 32, 2 * 32), image.size
assert any(pixel[3] for pixel in image.getdata())
assert len(image.tobytes()) == 13 * 32 * 2 * 32 * 4
print("ICON_ATLAS_TEST PASS icons=13 variants=2 size=%dx%d" % image.size)
PY
