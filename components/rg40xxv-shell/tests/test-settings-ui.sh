#!/bin/sh
set -eu

project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
workspace=${RG40XXV_WORKSPACE:-$(CDPATH= cd -- "$project/../../../.." && pwd -P)}
rootfs="$workspace/firmware/mnt/rootfs"
loader="$rootfs/usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1"
library_path="$rootfs/usr/lib/aarch64-linux-gnu:$rootfs/usr/lib"
temporary=$(mktemp -d)
trap 'status=$?; trap - EXIT; if test "$status" -ne 0; then cat "$stdout" "$stderr" 2>/dev/null || :; fi; rm -rf -- "$temporary"; exit "$status"' \
	EXIT HUP INT TERM

helper="$temporary/fake-hardwarectl"
hardware_log="$temporary/hardwarectl.log"
settings_file="$temporary/settings.conf"
rom_root="$temporary/roms"
state_dir="$temporary/netstream"
stdout="$temporary/stdout"
stderr="$temporary/stderr"

mkdir -m 0700 "$rom_root" "$state_dir"
cp "$project/tests/fake-hardwarectl.fixture" "$helper"
chmod 0755 "$helper"
cat >"$settings_file" <<'EOF'
language=zh_TW
screen_lock=1
auto_screen_off=3
brightness=8
joystick_rgb=0
usb_debug=0
EOF

SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
	qemu-aarch64-static "$loader" --library-path "$library_path" \
	"$project/build/rg40xxv-shell" \
	--windowed --font "$project/assets/RG40XXV-UI-Sans.otf" \
	--icon-atlas "$project/assets/RG40XXV-Material-Icons.png" \
	--rom-root "$rom_root" --state-dir "$state_dir" \
	--settings-file "$settings_file" \
	--filter-state "$temporary/filters.conf" \
	--history-file "$temporary/history.tsv" \
	--favorites-file "$temporary/favorites.tsv" \
	--hardwarectl "$helper" --hardwarectl-log "$hardware_log" \
	--settings-preview --benchmark --demo-ms 3000 \
	>"$stdout" 2>"$stderr"

grep -Fq 'UI_RESULT PASS' "$stdout"
grep -Fq 'FRAME_METRICS ' "$stdout"
if grep -Fq 'material icon atlas unavailable' "$stderr"; then
	printf '%s\n' 'settings fixture did not load the release icon atlas' >&2
	exit 1
fi
awk '
	/^FRAME_METRICS / {
		samples = "unknown"
		for (field = 1; field <= NF; ++field) {
			if ($field ~ /^samples=/) {
				split($field, sample_value, "=")
				samples = sample_value[2]
			}
			if ($field ~ /^p95_ms=/) {
				split($field, value, "=")
				p95 = value[2]
				found = 1
				if ((value[2] + 0) >= 40.0) {
					printf "settings benchmark p95_ms=%s samples=%s exceeds 40.0ms\n", \
						value[2], samples > "/dev/stderr"
					failed = 1
				}
			}
		}
		if (samples == "unknown" || (samples + 0) < 20) {
			printf "settings benchmark samples=%s is below 20\n", \
				samples > "/dev/stderr"
			failed = 1
		}
	}
	END {
		if (found)
			printf "SETTINGS_FRAME_METRICS p95_ms=%s samples=%s\n", p95, samples
		if (!found || failed)
			exit 1
	}
' "$stdout"

# A enters detail before any write.  B returns to row selection, where RIGHT
# and F3 must be inert; the second A re-enters before the final adjustments.
awk '
	$0 == "BEGIN" { command++; token = 0; next }
	$0 == "END" { next }
	{ token++; values[command "," token] = $0 }
	END {
		valid = command == 3 &&
			values["1,1"] == "<brightness>" && values["1,2"] == "<13>" &&
			values["2,1"] == "<brightness>" && values["2,2"] == "<3>" &&
			values["3,1"] == "<brightness>" && values["3,2"] == "<8>"
		if (!valid) {
			printf "unexpected hardware argv: commands=%d values=%s,%s,%s\n",
				command, values["1,2"], values["2,2"], values["3,2"] > "/dev/stderr"
			exit 1
		}
	}
' "${helper}.capture"

test ! -e "${helper}.env-leak"
test ! -e "${helper}.concurrent"
grep -Eq 'UI_HARDWARECTL_RESULT request=[0-9]+ command=2 value=13 spawn_error=0 exit=7 signal=0' \
	"$stderr"
grep -Fq 'UI_HARDWARECTL_STATUS 螢幕背光 · 執行失敗 · 結束代碼 7' \
	"$stderr"
test "$(grep -Fc 'UI_SETTINGS_DETAIL ENTER index=0' "$stderr")" -eq 2
grep -Fq 'UI_SETTINGS_DETAIL LEAVE reason=back index=0' "$stderr"
test ! -s "$temporary/favorites.tsv"
# The first requested 13% write exits 7.  A failed helper must not leave the
# persisted preference claiming hardware state that was never applied.
grep -Fq 'brightness=8' "$settings_file"
grep -Fq 'usb_debug=0' "$settings_file"
grep -Fq 'boot_target=custom' "$settings_file"
grep -Fq 'auto_screen_off=3' "$settings_file"
grep -aFq '數值 0 仍保留面板安全亮度；關閉螢幕是獨立動作' \
	"$project/build/rg40xxv-shell"
grep -aFq '偵錯紀錄匯出與清除目前無法使用' \
	"$project/build/rg40xxv-shell"

printf '%s\n' 'Nonblocking localized settings UI integration: PASS'
