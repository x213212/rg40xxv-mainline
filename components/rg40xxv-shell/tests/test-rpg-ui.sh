#!/bin/sh
set -eu

project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
workspace=${RG40XXV_WORKSPACE:-$(CDPATH= cd -- "$project/../../../.." && pwd -P)}
temporary=$(mktemp -d)
trap 'status=$?; trap - EXIT; rm -rf -- "$temporary"; exit "$status"' \
	EXIT HUP INT TERM

content_project=$(CDPATH= cd -- "$project/../rpg-content" && pwd -P)
content_root="$content_project/build/release-root/Roms"
rom_root="$temporary/roms"
mv_game="$rom_root/RPGMV/Runtime Blocked Fixture"
invalid_rm2k="$rom_root/EASYRPG/Missing Database"
symlink_rm2k="$rom_root/EASYRPG/Symlink Database"
launcher="$temporary/fake-launcher.sh"
routes="$workspace/lab/emulators/aarch64-staging/compat/platform-routes.json"
rmut_capability="$project/../rmut-h700/build/release-root/rpgmaker/runtime/admission.env"
unified="$workspace/lab/emulators/aarch64-staging/compat/unified-launch.sh"

# An admitted MV/MZ tile must not retain the blocked-runtime subtitle.
grep -Fq 'catalog_system_is_pending_rpg(selected->system) && !selected->playable' \
	"$project/src/render_scene.c"

# RMUT is the sole production route for both formats. The unfinished WPE
# fallback must not leak into the release catalog.
for system in RPGMV RPGMZ; do
	grep -Eq "\"$system\".*\"routes\"[[:space:]]*:[[:space:]]*\\[[[:space:]]*\"standalone:rmut-nwjs-aarch64\"[[:space:]]*\\].*\"playable\"[[:space:]]*:[[:space:]]*false" \
		"$routes"
done
! grep -Fq 'standalone:wpe-rpgmaker-h700' "$routes"
grep -Fq 'RPGMV|RPGMZ) run_rmut_nwjs "$@" ;;' "$unified"

# The fresh RMUT host admission is loader diagnostics only and must not unlock
# a production tile.
grep -Fqx 'schema=rg40xxv-rmut-runtime-admission-v3' "$rmut_capability"
grep -Fqx 'route=standalone:rmut-nwjs-aarch64' "$rmut_capability"
grep -Fqx 'truth_level=LOADER_PASS' "$rmut_capability"
grep -Fqx 'device_validation=UNVERIFIED' "$rmut_capability"
! grep -Fq 'route_launchable=' "$rmut_capability"

# These are parser fixtures, not evidence promotion. They prove that even the
# highest QEMU stage remains blocked and that only a complete DEVICE_PASS
# contract changes UI playability.
sha_a=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
sha_b=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb
sha_c=cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc
qemu_capability=$temporary/qemu-real-admission.env
sed \
	-e 's/^qemu_render_validation=UNVERIFIED$/qemu_render_validation=PASS/' \
	-e "s/^qemu_render_evidence_sha256=UNVERIFIED$/qemu_render_evidence_sha256=$sha_a/" \
	-e 's/^qemu_real_mv_interactive=UNVERIFIED$/qemu_real_mv_interactive=PASS/' \
	-e 's/^qemu_real_mz_interactive=UNVERIFIED$/qemu_real_mz_interactive=PASS/' \
	-e "s/^qemu_real_evidence_sha256=UNVERIFIED$/qemu_real_evidence_sha256=$sha_b/" \
	-e 's/^truth_level=LOADER_PASS$/truth_level=QEMU_REAL_MV_MZ_INTERACTIVE_PASS/' \
	"$rmut_capability" >"$qemu_capability"
device_capability=$temporary/synthetic-device-admission.env
sed \
	-e 's/^device_validation=UNVERIFIED$/device_validation=PASS/' \
	-e 's/^display_smoke=UNVERIFIED$/display_smoke=PASS/' \
	-e 's/^egl_panfrost_smoke=UNVERIFIED$/egl_panfrost_smoke=PASS/' \
	-e 's/^fullscreen_smoke=UNVERIFIED$/fullscreen_smoke=PASS/' \
	-e 's/^device_mv_interactive=UNVERIFIED$/device_mv_interactive=PASS/' \
	-e 's/^device_mz_interactive=UNVERIFIED$/device_mz_interactive=PASS/' \
	-e 's/^audio_smoke=UNVERIFIED$/audio_smoke=PASS/' \
	-e 's/^h700_gamepad_smoke=UNVERIFIED$/h700_gamepad_smoke=PASS/' \
	-e 's/^menu_start_supervisor_smoke=UNVERIFIED$/menu_start_supervisor_smoke=PASS/' \
	-e 's/^save_restart_smoke=UNVERIFIED$/save_restart_smoke=PASS/' \
	-e 's/^peak_pss_under_640_mib=UNVERIFIED$/peak_pss_under_640_mib=PASS/' \
	-e "s/^device_evidence_sha256=UNVERIFIED$/device_evidence_sha256=$sha_c/" \
	-e 's/^truth_level=QEMU_REAL_MV_MZ_INTERACTIVE_PASS$/truth_level=DEVICE_PASS/' \
	"$qemu_capability" >"$device_capability"

"$content_project/build.sh" >/dev/null
for title in 巴哈姆特 盜人講座; do
	test -f "$content_root/EASYRPG/$title/RPG_RT.ldb"
done
cp "$project/tests/fake-launcher.fixture" "$launcher"
chmod 0755 "$launcher"

for title in 巴哈姆特 盜人講座; do
	capture="$temporary/$title.arguments"
	output="$temporary/$title.stdout"
	SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
		LAUNCH_CAPTURE="$capture" \
		qemu-aarch64-static -L "$workspace/firmware/mnt/rootfs" \
		"$project/build/rg40xxv-shell" \
		--windowed --font "$project/assets/RG40XXV-UI-Sans.otf" \
		--rom-root "$content_root" --search "$title" \
		--platform-routes "$routes" --launcher "$launcher" \
		--launch-log "$temporary/$title.launch.log" \
		--history-file "$temporary/$title.history.tsv" \
		--favorites-file "$temporary/$title.favorites.tsv" \
		--settings-file "$temporary/$title.settings.conf" \
		--rpg-preview --launch-once --demo-ms 1400 >"$output"
	test "$(sed -n '1p' "$capture")" = '<-->'
	test "$(sed -n '2p' "$capture")" = '<EASYRPG>'
	test "$(sed -n '3p' "$capture")" = \
		"<$content_root/EASYRPG/$title>"
	grep -Fq 'visible=1' "$output"
	grep -Fq 'apps_view=no rpg_view=yes' "$output"
	grep -Fq "$content_root/EASYRPG/$title" \
		"$temporary/$title.history.tsv"
done

# MV/MZ inventory remains visible for every truth stage, but only DEVICE_PASS
# may hand off to the production launcher.
mkdir -p "$mv_game"
mkdir -p "$invalid_rm2k" "$symlink_rm2k"
ln -s "$content_root/EASYRPG/巴哈姆特/RPG_RT.ldb" \
	"$symlink_rm2k/RPG_RT.ldb"

# Invalid RPG Maker 2000/2003 directories must not be advertised as playable:
# a project needs a direct, physical RPG_RT.ldb on the same filesystem.
for invalid_title in 'Missing Database' 'Symlink Database'; do
	invalid_capture="$temporary/invalid-$invalid_title.arguments"
	SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
		LAUNCH_CAPTURE="$invalid_capture" \
		qemu-aarch64-static -L "$workspace/firmware/mnt/rootfs" \
		"$project/build/rg40xxv-shell" \
		--windowed --font "$project/assets/RG40XXV-UI-Sans.otf" \
		--rom-root "$rom_root" --search "$invalid_title" \
		--platform-routes "$routes" --launcher "$launcher" \
		--launch-log "$temporary/invalid.launch.log" \
		--history-file "$temporary/invalid.history.tsv" \
		--favorites-file "$temporary/invalid.favorites.tsv" \
		--settings-file "$temporary/invalid.settings.conf" \
		--rpg-preview --launch-once --demo-ms 700 \
		>"$temporary/invalid-$invalid_title.stdout"
	test ! -e "$invalid_capture"
	grep -Fq 'visible=0' "$temporary/invalid-$invalid_title.stdout"
done

run_mv_probe()
{
	name=$1
	capability=$2
	demo_ms=$3
	RG40XXV_UI_RPGMAKER_CAPABILITY="$capability" \
		SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
		LAUNCH_CAPTURE="$temporary/$name.arguments" \
		qemu-aarch64-static -L "$workspace/firmware/mnt/rootfs" \
		"$project/build/rg40xxv-shell" \
		--windowed --font "$project/assets/RG40XXV-UI-Sans.otf" \
		--rom-root "$rom_root" --platform-routes "$routes" \
		--launcher "$launcher" --launch-log "$temporary/$name.launch.log" \
		--history-file "$temporary/$name.history.tsv" \
		--favorites-file "$temporary/$name.favorites.tsv" \
		--settings-file "$temporary/$name.settings.conf" \
		--rpg-preview --launch-once --demo-ms "$demo_ms" \
		>"$temporary/$name.stdout" 2>"$temporary/$name.stderr"
}

run_mv_probe missing "$temporary/missing-admission.env" 700
run_mv_probe loader "$rmut_capability" 700
run_mv_probe qemu "$qemu_capability" 700
for blocked in missing loader qemu; do
	test ! -e "$temporary/$blocked.arguments"
	test ! -s "$temporary/$blocked.history.tsv"
	grep -Fq 'visible=1' "$temporary/$blocked.stdout"
done

# The route-less handoff deliberately exercises unified-launch's RMUT default,
# but only with a complete synthetic DEVICE_PASS parser fixture.
run_mv_probe device "$device_capability" 1400
test "$(sed -n '1p' "$temporary/device.arguments")" = '<-->'
test "$(sed -n '2p' "$temporary/device.arguments")" = '<RPGMV>'
test "$(sed -n '3p' "$temporary/device.arguments")" = "<$mv_game>"
grep -Fq "$mv_game" "$temporary/device.history.tsv"
grep -Fq 'visible=1' "$temporary/device.stdout"

printf '%s\n' \
	'UI RPG integration: PASS EASYRPG=巴哈姆特,盜人講座 invalid-projects=hidden loader=blocked qemu=blocked DEVICE_PASS=launchable'
