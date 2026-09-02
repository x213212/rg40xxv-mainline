#!/bin/sh
set -eu

project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
workspace=${RG40XXV_WORKSPACE:-$(CDPATH= cd -- "$project/../../../.." && pwd -P)}
rootfs="$workspace/firmware/mnt/rootfs"
temporary=$(mktemp -d)
trap 'status=$?; trap - EXIT; if test "$status" -ne 0; then cat "$temporary/test.stdout" "$temporary/test.stderr" "$temporary/test.valgrind" 2>/dev/null || :; fi; rm -rf -- "$temporary"; exit "$status"' \
	EXIT HUP INT TERM
cc=${CC:-gcc}

# shellcheck source=memory-safety-gate.sh
. "$project/tests/memory-safety-gate.sh"

test -f "$rootfs/usr/include/SDL2/SDL.h"
command -v "$cc" >/dev/null 2>&1
command -v python3 >/dev/null 2>&1

"$cc" -std=c11 -O1 -g -Wall -Wextra -Werror \
	-fsanitize=address,undefined -fno-sanitize-recover=all \
	-fno-omit-frame-pointer \
	-I"$project/include" \
	-I"$workspace/services/netstream/include" \
	-I"$rootfs/usr/include/SDL2" \
	"$project/tests/text_texture_lifetime_test.c" \
	"$project/src/text.c" \
	-o "$temporary/text-texture-lifetime"
rg40xxv_select_leak_backend "$cc" "$temporary" \
	"$project/tests/memory_safety_probe.c"

case $RG40XXV_LEAK_BACKEND in
lsan) detect_leaks=1 ;;
valgrind|skip) detect_leaks=0 ;;
*) printf 'unknown leak backend: %s\n' "$RG40XXV_LEAK_BACKEND" >&2; exit 1 ;;
esac
ASAN_OPTIONS=detect_leaks=$detect_leaks:halt_on_error=1:abort_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	"$temporary/text-texture-lifetime" \
	>"$temporary/test.stdout" 2>"$temporary/test.stderr"
grep -Fq 'TEXT_TEXTURE_LIFETIME_TEST PASS' "$temporary/test.stdout"
if grep -Eq 'ERROR: AddressSanitizer|runtime error:' \
	"$temporary/test.stdout" "$temporary/test.stderr"; then
	printf '%s\n' 'texture lifetime sanitizer finding' >&2
	exit 1
fi

if test "$RG40XXV_LEAK_BACKEND" = valgrind; then
	"$cc" -std=c11 -O1 -g -Wall -Wextra -Werror \
		-fno-omit-frame-pointer \
		-I"$project/include" \
		-I"$workspace/services/netstream/include" \
		-I"$rootfs/usr/include/SDL2" \
		"$project/tests/text_texture_lifetime_test.c" \
		"$project/src/text.c" \
		-o "$temporary/text-texture-lifetime-plain"
	rg40xxv_valgrind_run "$temporary/test.valgrind" \
		"$temporary/text-texture-lifetime-plain" \
		>"$temporary/valgrind.stdout"
	grep -Fq 'TEXT_TEXTURE_LIFETIME_TEST PASS' \
		"$temporary/valgrind.stdout"
fi

# Guard the two ordering contracts the isolated text test cannot observe:
# every normal frame collects after Present, and renderer teardown flushes
# before destroying icon/cache/text textures.
python3 - "$project/src/render_scene.c" "$project/src/lifecycle.c" <<'PY'
from pathlib import Path
import re
import sys

scene = Path(sys.argv[1]).read_text()
lifecycle = Path(sys.argv[2]).read_text()

presents = len(re.findall(r"SDL_RenderPresent\(ui->renderer\);", scene))
paired = len(re.findall(
    r"SDL_RenderPresent\(ui->renderer\);\s*text_cache_collect_retired\(ui\);",
    scene,
))
if presents == 0 or paired != presents:
    raise SystemExit(f"unpaired frame retirement: presents={presents} paired={paired}")

start = lifecycle.index("void render_destroy(struct ui *ui)")
end = lifecycle.index("\n}\n", start)
destroy = lifecycle[start:end]
flush = destroy.find("SDL_RenderFlush(ui->renderer)")
first_texture_destroy = destroy.find("SDL_DestroyTexture")
text_destroy = destroy.find("text_cache_destroy(ui)")
if flush < 0 or first_texture_destroy < 0 or text_destroy < 0:
    raise SystemExit("render_destroy lifetime operations are missing")
if not (flush < first_texture_destroy and flush < text_destroy):
    raise SystemExit("render_destroy does not flush before texture teardown")
PY

cat "$temporary/test.stdout"
printf '%s\n' 'TEXTURE_LIFETIME_SOURCE_ORDER PASS'
if test "$RG40XXV_LEAK_BACKEND" = skip; then
	printf 'TEXTURE_MEMORY_SAFETY PASS asan=PASS ubsan=PASS allocator-lifecycle=PASS leak=SKIP reason=%s\n' \
		"$RG40XXV_LEAK_SKIP_REASON"
else
	printf 'TEXTURE_MEMORY_SAFETY PASS asan=PASS ubsan=PASS allocator-lifecycle=PASS leak=%s\n' \
		"$RG40XXV_LEAK_BACKEND"
fi
