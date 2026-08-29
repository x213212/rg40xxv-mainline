#!/bin/sh
set -eu

project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
locale="$project/src/locale.c"

for mainland_term in 配置 默認 默認值 數據 視頻 軟件 信息 屏幕 文件夾 內存 網絡 運行 後備 當前 全庫 關屏; do
	if grep -Fq "$mainland_term" "$locale"; then
		printf 'non-zh_TW term remains: %s\n' "$mainland_term" >&2
		exit 1
	fi
done
grep -Fq '{ "restart_mode", "Restart mode", "重新啟動模式" }' "$locale"
grep -Fq '{ "restart_normal", "Normal reboot", "一般重啟" }' "$locale"
grep -Fq '"五秒內再按一次 A，確認一般重啟"' "$locale"
grep -Fq '{ "apps_categories", "Media  ·  Network  ·  Tools",' "$locale"
if grep -Eq 'resident_(keep|ignore)' "$locale"; then
	printf '%s\n' 'resident status text must not be user-visible' >&2
	exit 1
fi
if grep -Fq '版面預留' "$locale"; then
	printf '%s\n' 'normal reboot must not remain labelled as a layout stub' >&2
	exit 1
fi
if grep -Eq 'restart_(stock|fastboot)|Fastboot／維修' "$locale"; then
	printf '%s\n' 'normal reboot UI must not expose alternate reboot targets' >&2
	exit 1
fi
printf '%s\n' 'LOCALE_UI_TEST PASS zh_TW terminology and real normal reboot strings'
