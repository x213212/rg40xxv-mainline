#!/bin/sh
set -eu

project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
workspace=$(CDPATH= cd -- "$project/../.." && pwd -P)
rootfs="$workspace/firmware/mnt/rootfs"
loader="$rootfs/usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1"
library_path="$rootfs/usr/lib/aarch64-linux-gnu:$rootfs/usr/lib"
temporary=$(mktemp -d)
trap 'status=$?; trap - EXIT; rm -rf -- "$temporary"; exit "$status"' \
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
brightness=8
joystick_rgb=0
usb_debug=0
EOF

SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
	qemu-aarch64-static "$loader" --library-path "$library_path" \
	"$project/build/rg40xxv-shell" \
	--windowed --font "$project/assets/RG40XXV-UI-Sans.otf" \
	--rom-root "$rom_root" --state-dir "$state_dir" \
	--settings-file "$settings_file" \
	--filter-state "$temporary/filters.conf" \
	--history-file "$temporary/history.tsv" \
	--favorites-file "$temporary/favorites.tsv" \
	--hardwarectl "$helper" --hardwarectl-log "$hardware_log" \
	--settings-preview --benchmark --demo-ms 900 \
	>"$stdout" 2>"$stderr"

grep -Fq 'UI_RESULT PASS' "$stdout"
grep -Fq 'FRAME_METRICS ' "$stdout"
awk '
	/^FRAME_METRICS / {
		for (field = 1; field <= NF; ++field) {
			if ($field ~ /^p95_ms=/) {
				split($field, value, "=")
				if ((value[2] + 0) >= 40.0) exit 1
				found = 1
			}
		}
	}
	END { if (!found) exit 1 }
' "$stdout"

# Benchmark inputs are RIGHT, LEFT, RIGHT on the selected brightness row.
awk '
	$0 == "BEGIN" { command++; token = 0; next }
	$0 == "END" { next }
	{ token++; values[command "," token] = $0 }
	END {
		if (command != 3) exit 1
		if (values["1,1"] != "<brightness>" || values["1,2"] != "<13>") exit 1
		if (values["2,1"] != "<brightness>" || values["2,2"] != "<8>") exit 1
		if (values["3,1"] != "<brightness>" || values["3,2"] != "<13>") exit 1
	}
' "${helper}.capture"

test ! -e "${helper}.env-leak"
test ! -e "${helper}.concurrent"
grep -Fq 'UI_HARDWARECTL_RESULT command=2 value=13 spawn_error=0 exit=7 signal=0' \
	"$stderr"
grep -Fq 'UI_HARDWARECTL_STATUS 螢幕背光 · 執行失敗 · 結束代碼 7' \
	"$stderr"
grep -Fq 'brightness=13' "$settings_file"
grep -Fq 'usb_debug=0' "$settings_file"
grep -aFq '數值 0 仍保留面板安全亮度；關閉螢幕是獨立動作' \
	"$project/build/rg40xxv-shell"
grep -aFq '偵錯紀錄匯出與清除目前無法使用' \
	"$project/build/rg40xxv-shell"

printf '%s\n' 'Nonblocking localized settings UI integration: PASS'
