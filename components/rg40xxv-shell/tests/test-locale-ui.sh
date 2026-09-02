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
grep -Fq '"一般重啟 · 自訂 Linux"' "$locale"
grep -Fq '"自訂 Linux · 固定安全策略"' "$locale"
if grep -Eq 'restart_(modes|layout_only)|原廠 · Fastboot' "$locale"; then
	printf '%s\n' 'normal reboot UI must not expose stock/fastboot targets' >&2
	exit 1
fi
grep -Fq '{ "apps_categories", "Media  ·  Network  ·  Tools",' "$locale"
grep -Fq '{ "syncing", "Setting clock", "校時中" }' "$locale"
grep -Fq '{ "synced", "Clock set", "時間已校準" }' "$locale"
grep -Fq '{ "unsynced", "Clock not set", "時間未校準" }' "$locale"
if grep -Eq 'resident_(keep|ignore)' "$locale"; then
	printf '%s\n' 'resident status text must not be user-visible' >&2
	exit 1
fi
printf '%s\n' 'LOCALE_UI_TEST PASS zh_TW terminology and restart preview strings'
