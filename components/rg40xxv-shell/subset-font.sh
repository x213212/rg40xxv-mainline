#!/bin/sh
set -eu

project=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
source_font=${1:-/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc}
output=${2:-"$project/assets/RG40XXV-UI-Sans.otf"}
rom_root=${RG40XXV_UI_ROM_ROOT:-}
route_catalog=${RG40XXV_UI_PLATFORM_ROUTES:-}

command -v pyftsubset >/dev/null 2>&1 || {
	printf '%s\n' 'pyftsubset not found (install fonttools)' >&2
	exit 1
}
test -r "$source_font"

# Face 3 is Noto Sans CJK Traditional Chinese. Reading every C source and
# header as one corpus prevents labels added in a split module from being
# omitted from the subset.
corpus=$(mktemp)
trap 'rm -f "$corpus"' EXIT HUP INT TERM
find "$project/src" "$project/include" -type f \( -name '*.c' -o -name '*.h' \) \
	-exec cat {} + > "$corpus"
if [ -n "$rom_root" ] && [ -d "$rom_root" ]; then
	# Cover filenames and PORTS' adjacent engine/data trees are not catalog
	# titles.  Including them bloats the embedded font enough to exceed the
	# Cedrus ACM firmware's 1 MiB asset ceiling.  Keep every displayed ROM
	# filename, plus the directory names used as EasyRPG titles.
	find "$rom_root" -type f \
		! -path '*/Imgs/*' \
		! -path "$rom_root/PORTS/*/*" \
		-printf '%f\n' >> "$corpus"
	if [ -d "$rom_root/EASYRPG" ]; then
		find "$rom_root/EASYRPG" -mindepth 1 -maxdepth 1 -type d \
			-printf '%f\n' >> "$corpus"
	fi
fi
if [ -n "$route_catalog" ] && [ -r "$route_catalog" ]; then
	cat "$route_catalog" >> "$corpus"
fi

# The UI is horizontal-only: retain Latin kerning and native-size hinting but
# omit vertical tables.  Keep the complete basic CJK ranges: ROM and port
# titles are external data and may use Simplified Chinese characters that do
# not occur in the built-in zh_TW strings or in the ROM set present at build
# time.  A title must never turn into tofu merely because it was added later.
pyftsubset "$source_font" \
	--font-number=3 \
	--text-file="$corpus" \
	--unicodes='U+0020-007E,U+00A0-024F,U+2000-206F,U+3000-303F,U+3100-312F,U+31A0-31BF,U+3400-4DBF,U+4E00-9FFF,U+F900-FAFF,U+FF00-FFEF' \
	--layout-features='kern' \
	--flavor= \
	--output-file="$output"

bytes=$(wc -c < "$output")
if [ "$bytes" -gt 12582912 ]; then
	printf 'FONT_RESULT FAIL path=%s bytes=%s limit=12582912\n' \
		"$output" "$bytes" >&2
	exit 1
fi
printf 'FONT_RESULT PASS path=%s bytes=%s limit=12582912\n' \
	"$output" "$bytes"
