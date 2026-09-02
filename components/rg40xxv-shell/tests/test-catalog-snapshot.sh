#!/bin/sh
set -eu

project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
workspace=${RG40XXV_WORKSPACE:-$(CDPATH= cd -- "$project/../../../.." && pwd -P)}
rootfs="$workspace/firmware/mnt/rootfs"
temporary=$(mktemp -d)
trap 'status=$?; trap - EXIT; rm -rf -- "$temporary"; exit "$status"' \
	EXIT HUP INT TERM

rom_root="$temporary/roms"
snapshot="$temporary/catalog.v3"
routes="$temporary/platform-routes.json"
export RG40XXV_UI_RPGMAKER_CAPABILITY="$temporary/missing-admission.env"
mkdir -m 0700 "$rom_root" "$temporary/state" "$temporary/hardware" \
	"$temporary/cover-cache"
cp "$project/tests/platform-routes.fixture.json" "$routes"
mkdir "$rom_root/GBA" "$rom_root/FC" "$rom_root/SFC"

index=0
while test "$index" -lt 300; do
	platform=GBA
	extension=gba
	case $((index % 3)) in
		1) platform=FC; extension=nes ;;
		2) platform=SFC; extension=sfc ;;
	esac
	: >"$rom_root/$platform/Game-$index.$extension"
	index=$((index + 1))
done

refresh()
{
	label=$1
	shift
	set +e
	qemu-aarch64-static -L "$rootfs" "$project/build/rg40xxv-shell" \
		--catalog-refresh-only --rom-root "$rom_root" \
		--platform-routes "$routes" --catalog-snapshot "$snapshot" \
		"$@" >"$temporary/$label.stdout" 2>"$temporary/$label.stderr"
	status=$?
	set -e
	printf '%s\n' "$status" >"$temporary/$label.status"
}

refresh cold
test "$(cat "$temporary/cold.status")" -eq 0
grep -Fq 'CATALOG_INCREMENTAL mode=cold platforms=3 scanned=3 reused=0 cursor=none' \
	"$temporary/cold.stderr"
grep -Fq 'CATALOG_REFRESH_RESULT result=complete games=300' \
	"$temporary/cold.stderr"
grep -Fq 'RG40XXV-CATALOG-V3' "$snapshot"
test "$(grep -c '^platform ' "$snapshot")" -eq 3

refresh warm
test "$(cat "$temporary/warm.status")" -eq 0
grep -Fq 'mode=warm-unchanged platforms=3 scanned=0 reused=3' \
	"$temporary/warm.stderr"

# Catalog entries cache the UI's playable bit.  A prior DEVICE_PASS snapshot
# must be rejected after admission disappears or is downgraded, even when the
# platform route JSON is unchanged.
sed -i 's/^rpg-capability 0 0$/rpg-capability 4 1/' "$snapshot"
grep -Fq 'rpg-capability 4 1' "$snapshot"
refresh admission-downgrade
test "$(cat "$temporary/admission-downgrade.status")" -eq 0
grep -Fq 'CATALOG_INCREMENTAL mode=cold platforms=3 scanned=3 reused=0 cursor=none' \
	"$temporary/admission-downgrade.stderr"
grep -Fq 'rpg-capability 0 0' "$snapshot"

: >"$rom_root/GBA/Changed.gba"
refresh changed
test "$(cat "$temporary/changed.status")" -eq 0
grep -Fq 'mode=merge platforms=3 scanned=1 reused=2 cursor=none' \
	"$temporary/changed.stderr"
grep -Fq 'CATALOG_REFRESH_RESULT result=complete games=301' \
	"$temporary/changed.stderr"

# A one-millisecond cold budget must publish a valid partial snapshot with a
# cursor. A later bounded run resumes by reusing completed platform records.
partial_root="$temporary/partial-roms"
partial_snapshot="$temporary/partial.v3"
mkdir "$partial_root" "$partial_root/GBA" "$partial_root/FC"
index=0
while test "$index" -lt 1200; do
	: >"$partial_root/GBA/Cold-$index.gba"
	index=$((index + 1))
done
set +e
qemu-aarch64-static -L "$rootfs" "$project/build/rg40xxv-shell" \
	--catalog-refresh-only --catalog-refresh-budget-ms 1 \
	--rom-root "$partial_root" --platform-routes "$routes" \
	--catalog-snapshot "$partial_snapshot" \
	>"$temporary/partial.stdout" 2>"$temporary/partial.stderr"
partial_status=$?
set -e
test "$partial_status" -eq 75
grep -Fq 'complete 0' "$partial_snapshot"
grep -Eq '^cursor [^-]' "$partial_snapshot"
qemu-aarch64-static -L "$rootfs" "$project/build/rg40xxv-shell" \
	--catalog-refresh-only --rom-root "$partial_root" \
	--platform-routes "$routes" --catalog-snapshot "$partial_snapshot" \
	>"$temporary/resume.stdout" 2>"$temporary/resume.stderr"
grep -Fq 'CATALOG_REFRESH_RESULT result=complete games=1200' \
	"$temporary/resume.stderr"

# Measure the true render-present timestamp, not process start or a stale
# ready marker. Five samples keep the p95 assertion deterministic.
sample=1
while test "$sample" -le 5; do
	exit_ns=$(awk '{ printf "%.0f", $1 * 1000000000 }' /proc/uptime)
	RG_GAME_EXIT_MONOTONIC_NS=$exit_ns \
	SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
	qemu-aarch64-static -L "$rootfs" "$project/build/rg40xxv-shell" \
		--windowed --font "$project/assets/RG40XXV-UI-Sans.otf" \
		--rom-root "$rom_root" --platform-routes "$routes" \
		--catalog-snapshot "$snapshot" \
		--cover-cache-dir "$temporary/cover-cache" \
		--state-dir "$temporary/state" --hardware-root "$temporary/hardware" \
		--hardwarectl /not-present/ui-hardwarectl --demo-ms 180 \
		>"$temporary/ui-$sample.stdout" 2>"$temporary/ui-$sample.stderr"
	grep -Fq 'catalog_source=snapshot-complete' "$temporary/ui-$sample.stderr"
	grep -Fq 'CATALOG_REFRESH skipped=unchanged' \
		"$temporary/ui-$sample.stderr"
	sed -n 's/.*game_exit_to_present_ms=\([0-9][0-9]*\).*/\1/p' \
		"$temporary/ui-$sample.stderr" >>"$temporary/latencies"
	sample=$((sample + 1))
done
test "$(wc -l <"$temporary/latencies")" -eq 5
sort -n "$temporary/latencies" >"$temporary/latencies.sorted"
p95=$(sed -n '5p' "$temporary/latencies.sorted")
test "$p95" -lt 1000

cold_ms=$(sed -n 's/.*scan_ms=\([0-9][0-9]*\).*/\1/p' \
	"$temporary/cold.stderr")
warm_ms=$(sed -n 's/.*scan_ms=\([0-9][0-9]*\).*/\1/p' \
	"$temporary/warm.stderr")
printf 'CATALOG_SNAPSHOT_TEST PASS games=301 cold_ms=%s warm_ms=%s first_frame_p95_ms=%s\n' \
	"$cold_ms" "$warm_ms" "$p95"
