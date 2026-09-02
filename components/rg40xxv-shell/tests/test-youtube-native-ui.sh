#!/bin/sh
set -eu

# The formal YouTube UI route is native texture only and has no Web fallback.
project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
workspace=${RG40XXV_WORKSPACE:-$(CDPATH= cd -- "$project/../../../.." && pwd -P)}
ui_binary=${RG40XXV_UI_BINARY:-$project/build/rg40xxv-shell}
temporary=$(mktemp -d)
trap 'status=$?; trap - EXIT HUP INT TERM; rm -rf -- "$temporary"; exit "$status"' \
	EXIT HUP INT TERM

rom_root=$temporary/Roms
native_launcher=$temporary/rg40xxv-youtube-native
admission=$temporary/admission.env
verified_admission=$temporary/admission-verified.env
failed_admission=$temporary/admission-failed.env
launcher=$temporary/fake-launcher.sh
capture=$temporary/arguments
output=$temporary/stdout
errors=$temporary/stderr
screenshot=$temporary/youtube-native.bmp

mkdir -p "$rom_root"
printf '%s\n' '#!/bin/sh' 'exit 0' >"$native_launcher"
chmod 0700 "$native_launcher"
cat >"$admission" <<EOF
schema=rg40xxv-youtube-ui-admission-v3
native_route=native-texture
native_launcher=$native_launcher
evidence_scope=COMPONENT_GATE
native_controller_ui=PASS
url_resolver=PASS
range_bridge=PASS
h264_decode=PASS
aac_decode=PASS
drm_kms_display=PASS
alsa_audio=PASS
input=PASS
session_return=PASS
memory_budget=UNVERIFIED
EOF
sed 's/^memory_budget=UNVERIFIED$/memory_budget=PASS/' "$admission" \
	>"$verified_admission"
sed 's/^memory_budget=UNVERIFIED$/memory_budget=FAIL/' "$admission" \
	>"$failed_admission"
chmod 0600 "$admission" "$verified_admission" "$failed_admission"
cp "$project/tests/fake-launcher.fixture" "$launcher"
chmod 0755 "$launcher"

# A truthful pending memory gate is selectable only for the explicit
# user-driven verification run; the UI must not label the device route PASS.
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
	RG40XXV_UI_YOUTUBE_CAPABILITY="$admission" \
	LAUNCH_CAPTURE="$capture" \
	qemu-aarch64-static -L "$workspace/firmware/mnt/rootfs" \
	"$ui_binary" \
	--windowed --font "$project/assets/RG40XXV-UI-Sans.otf" \
	--rom-root "$rom_root" --launcher "$launcher" \
	--launch-log "$temporary/launch.log" \
	--history-file "$temporary/history.tsv" \
	--favorites-file "$temporary/favorites.tsv" \
	--settings-file "$temporary/settings.conf" \
	--apps-preview --launch-once --screenshot "$screenshot" --demo-ms 1400 \
	>"$output" 2>"$errors"

test "$(sed -n '1p' "$capture")" = '<-->'
test "$(sed -n '2p' "$capture")" = '<APPS>'
test "$(sed -n '3p' "$capture")" = "<$native_launcher>"
grep -Fq 'roms=1 visible=1' "$output"
grep -Fq 'apps_view=yes' "$output"
grep -Fq \
	'YOUTUBE_CAPABILITY native_route=READY native_device=UNVERIFIED' \
	"$errors"
grep -Fq "$native_launcher" "$temporary/history.tsv"
test -s "$screenshot"

# A complete component admission remains a candidate and may not claim final
# device acceptance without separately bound exact-binary live evidence.
verified_capture=$temporary/verified-arguments
verified_errors=$temporary/verified-stderr
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
	RG40XXV_UI_YOUTUBE_CAPABILITY="$verified_admission" \
	LAUNCH_CAPTURE="$verified_capture" \
	qemu-aarch64-static -L "$workspace/firmware/mnt/rootfs" \
	"$ui_binary" \
	--windowed --font "$project/assets/RG40XXV-UI-Sans.otf" \
	--rom-root "$rom_root" --launcher "$launcher" \
	--launch-log "$temporary/verified-launch.log" \
	--history-file "$temporary/verified-history.tsv" \
	--favorites-file "$temporary/verified-favorites.tsv" \
	--settings-file "$temporary/verified-settings.conf" \
	--apps-preview --launch-once --demo-ms 1400 \
	>/dev/null 2>"$verified_errors"
test "$(sed -n '3p' "$verified_capture")" = "<$native_launcher>"
grep -Fq 'YOUTUBE_CAPABILITY native_route=READY native_device=UNVERIFIED' \
	"$verified_errors"

# A recorded memory failure remains visible for diagnosis but cannot reach the
# resident launch handoff.
failed_capture=$temporary/failed-arguments
failed_output=$temporary/failed-stdout
failed_errors=$temporary/failed-stderr
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
	RG40XXV_UI_YOUTUBE_CAPABILITY="$failed_admission" \
	LAUNCH_CAPTURE="$failed_capture" \
	qemu-aarch64-static -L "$workspace/firmware/mnt/rootfs" \
	"$ui_binary" \
	--windowed --font "$project/assets/RG40XXV-UI-Sans.otf" \
	--rom-root "$rom_root" --launcher "$launcher" \
	--launch-log "$temporary/failed-launch.log" \
	--history-file "$temporary/failed-history.tsv" \
	--favorites-file "$temporary/failed-favorites.tsv" \
	--settings-file "$temporary/failed-settings.conf" \
	--apps-preview --launch-once --demo-ms 900 \
	>"$failed_output" 2>"$failed_errors"
test ! -e "$failed_capture"
grep -Fq 'roms=1 visible=1' "$failed_output"
grep -Fq 'YOUTUBE_CAPABILITY native_route=READY native_device=FAIL' \
	"$failed_errors"

# Native admission is release-owned and available on the first read.  No p1
# installer polling or Web tile remains in the active UI route.
! grep -Fq 'YOUTUBE_CAPABILITY_REFRESH' "$project/src/main.c"
! grep -Fq 'youtube.web_' "$project/src/main.c"
! grep -Fq 'YouTube · Web' "$project/src/youtube_capability.c"

printf '%s\n' \
	'YOUTUBE_NATIVE_UI_TEST PASS route=native-texture memory-unverified=launchable memory-fail=blocked web=absent refresh=none'
