#!/bin/sh
set -eu
umask 077

project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
workspace=$(CDPATH= cd -- "$project/../../../.." && pwd -P)
rootfs=$workspace/firmware/mnt/rootfs
binary=$project/build/rg40xxv-youtube-sdl-texture-scene

"$project/build-sdl-texture-scene.sh" >/dev/null
libs=/usr/lib:/lib/aarch64-linux-gnu:/usr/lib/aarch64-linux-gnu:/usr/lib/aarch64-linux-gnu/pulseaudio:/usr/lib/aarch64-linux-gnu/blas:/usr/lib/aarch64-linux-gnu/lapack
contract=$(qemu-aarch64-static -L "$rootfs" -E LD_LIBRARY_PATH="$libs" \
	"$binary" --contract)
printf '%s\n' "$contract" | grep -Fqx \
	'PREFETCH	focus-stable-ms=500	action=resolver-cache-selected+neighbours	max-coordinators=1	max-resolvers=2	failure-retry=focus-change-only	endpoint=activate-only	broker-grace-ms=1500'

start_ns=$(date +%s%N)
output=$(qemu-aarch64-static -L "$rootfs" -E LD_LIBRARY_PATH="$libs" \
	"$binary" --prefetch-lifecycle-self-test)
end_ns=$(date +%s%N)
elapsed_ms=$(( (end_ns - start_ns) / 1000000 ))
printf '%s\n' "$output" | grep -Fqx \
	'YOUTUBE_TEXTURE_PREFETCH_LIFECYCLE_TEST PASS rapid_moves=40 focus_cache_decisions=1 focus_broker_decisions=0 focus_bridge_decisions=0 max_cache_inflight=1 activation_broker_decisions=1 device=PENDING'

python3 - "$project/src/sdl_texture_scene.cpp" <<'PY'
from pathlib import Path
import sys

source = Path(sys.argv[1]).read_text(encoding="utf-8")

def body(signature: str) -> str:
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening:index + 1]
    raise AssertionError(signature)

focus = body("void prefetch_selected(App *a)")
activate = body("void activate_home(App *a)")
loop = body("int main(int argc, char **argv)")
destroy = body("void destroy(App *a)")
assert focus.count("start_prefetch_for_selection") == 1
assert "catalog.selected - 1" in focus
assert "catalog.selected + 1" in focus
assert "start_broker_for_url" not in focus
assert activate.count("start_broker_for_url") == 1
assert "--prefetch-url" not in source
assert loop.index("input(&a)") < loop.index("prefetch_selected(&a)")
assert "POSIX_SPAWN_SETPGROUP" in source
assert "kBrokerGracePolls = 150" in source
assert destroy.index("stop_prefetch(a)") < destroy.index("stop_broker(a)")
PY

printf '%s\n' \
	"YOUTUBE_PREFETCH_BENCHMARK_CONTRACT PASS rapid_moves=40 debounce_ms=500 focus_cache_start_calls=1 focus_broker_start_calls=0 focus_bridge_start_calls=0 activation_broker_start_calls=1 prefetch_max_inflight=1 broker_grace_ms=1500 host_policy_ms=$elapsed_ms device_latency=PENDING"
