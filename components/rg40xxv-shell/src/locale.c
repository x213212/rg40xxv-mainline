#define _POSIX_C_SOURCE 200809L

#include "ui.h"

#include <stdio.h>
#include <string.h>

struct translation {
	const char *key;
	const char *english;
	const char *zh_tw;
};

static const struct translation translations[] = {
	{ "nav_recent", "Recent", "最近遊玩" },
	{ "nav_library", "Library", "遊戲庫" },
	{ "nav_favorites", "Favorites", "收藏" },
	{ "nav_streaming", "Streaming", "串流" },
	{ "nav_apps", "Apps", "應用程式" },
	{ "nav_network", "Network", "網路" },
	{ "nav_settings", "Settings", "設定" },
	{ "library", "Game library", "遊戲庫" },
	{ "search_placeholder", "Search games...", "搜尋遊戲…" },
	{ "search_prefix", "Search: ", "搜尋：" },
	{ "system", "System", "系統" },
	{ "all", "All", "全部" },
	{ "recent", "Recent", "最近" },
	{ "on", "On", "開" },
	{ "core", "Core", "核心" },
	{ "auto", "Auto", "自動" },
	{ "indexed", "Indexed", "已索引" },
	{ "shown", "Shown", "顯示" },
	{ "favorites_only", "Favorites only", "僅收藏" },
	{ "no_rom", "No ROMs found", "尚未找到 ROM" },
	{ "check_rom_root", "Check --rom-root and file types",
	  "請確認 --rom-root 路徑與檔案格式" },
	{ "no_match", "No games match", "找不到符合條件的遊戲" },
	{ "adjust_search", "Adjust search text or combined filters",
	  "請調整搜尋文字或組合篩選條件" },
	{ "apps_title", "Applications", "應用程式" },
	{ "apps_subtitle", "Only launchers discovered under APPS are shown",
	  "只顯示從 APPS 掃描到的啟動項目" },
	{ "apps_categories", "Media  ·  Network  ·  Tools",
	  "媒體　·　網路　·　工具" },
	{ "apps_scanned", "Found", "已找到" },
	{ "no_apps", "No applications found", "尚未找到應用程式" },
	{ "check_apps_root", "Add an APPS launcher or app/launch.sh",
	  "請加入 APPS 啟動腳本或應用程式／launch.sh" },
	{ "favorite_mark", "Favorite", "已收藏" },
	{ "platform", "Platform", "平台" },
	{ "frontend", "Frontend", "前端" },
	{ "runtime", "Core/runtime", "核心／執行環境" },
	{ "runtime_short", "Runtime", "核心／環境" },
	{ "fallback", "Switchable fallback", "可切換備援" },
	{ "route_locked", "Existing save: route locked", "已有存檔：路由已鎖定" },
	{ "quick_title", "Combined filters", "組合篩選" },
	{ "quick_subtitle", "System / favorites / recent / core combine",
	  "系統／收藏／最近／核心可同時套用" },
	{ "filter_system", "System", "系統" },
	{ "filter_favorites", "Favorites", "收藏" },
	{ "filter_recent", "Recent", "最近" },
	{ "filter_core", "Core", "核心" },
	{ "filter_reset", "Reset all filters", "重設所有篩選" },
	{ "filter_language", "Language", "語言" },
	{ "only", "Only", "僅限" },
	{ "zh_tw", "Traditional Chinese", "繁體中文" },
	{ "english", "English", "English" },
	{ "quick_controls", "A / left-right Change  B Close",
	  "A／左右 變更　B 關閉" },
	{ "launch", "Launch", "啟動" },
	{ "change", "Change", "變更" },
	{ "select", "Select", "選擇" },
	{ "adjust", "Adjust", "調整" },
	{ "back", "Back", "返回" },
	{ "close", "Close", "關閉" },
	{ "switch_tab", "Switch tab", "切換頁籤" },
	{ "enter_content", "Enter page", "進入內容" },
	{ "top_nav", "Top tabs", "頂部頁籤" },
	{ "search", "Search", "搜尋" },
	{ "filter", "Filter", "篩選" },
	{ "enter", "Enter", "輸入" },
	{ "delete", "Delete", "刪除" },
	{ "page", "Page", "字頁" },
	{ "shift", "Shift", "大寫" },
	{ "layout", "Layout", "排列" },
	{ "keyboard_common", "On-screen keyboard - common",
	  "螢幕鍵盤 · 常用字" },
	{ "keyboard_alnum", "On-screen keyboard - letters and numbers",
	  "螢幕鍵盤 · 英數字" },
	{ "switch_page", "Y Switch page", "Y 切換字頁" },
	{ "scope_current", "X Current system", "X 目前平台" },
	{ "scope_all", "X Full library", "X 完整遊戲庫" },
	{ "scope_all_short", "Sel All", "Sel 全部" },
	{ "scope_current_short", "Sel System", "Sel 平台" },
	{ "keyboard_controls", "A Enter  B Delete / close",
	  "A 輸入　B 刪除／關閉" },
	{ "keyboard_english", "English", "英文" },
	{ "keyboard_english_shift", "English · Shift", "英文 · 大寫" },
	{ "keyboard_numbers", "Numbers", "數字" },
	{ "keyboard_symbols", "Symbols", "符號" },
	{ "keyboard_new_chewing", "New Chewing Zhuyin", "新酷音注音" },
	{ "space", "Space", "空白" },
	{ "new_chewing_unavailable", "New Chewing unavailable · English fallback",
	  "新酷音未安裝 · 僅英文備援" },
	{ "new_chewing_ready", "New Chewing ready", "新酷音可用" },
	{ "ime_english_short", "IME English only", "IME 僅英文" },
	{ "keyboard_controls_new", "A Input  B Delete  X Shift  Y Layout  Start Done",
	  "A 輸入　B 關閉　X 大寫　Y 排列　Start 完成" },
	{ "input_method", "Input method", "輸入法" },
	{ "new_chewing_english", "New Chewing / English",
	  "新酷音／英文" },
	{ "preparing", "Preparing game", "正在準備遊戲" },
	{ "launch_starting", "Launching", "正在啟動" },
	{ "launch_preview_title", "Selected game", "選定遊戲" },
	{ "launch_preview_platform", "Game platform", "遊戲平台" },
	{ "launch_error_title", "The game could not continue",
	  "遊戲無法繼續執行" },
	{ "launch_show_diagnostics", "A Show diagnostics  ·  B Close",
	  "A 顯示診斷資訊　·　B 關閉" },
	{ "launch_hide_diagnostics", "A Hide diagnostics  ·  B Close",
	  "A 隱藏診斷資訊　·　B 關閉" },
	{ "launch_restoring", "Returned - restoring interface",
	  "已返回 · 正在恢復介面" },
	{ "launch_exit_code", "Exit code", "結束代碼" },
	{ "launch_exit_signal", "Terminated by signal", "因訊號結束" },
	{ "launch_error_code", "Error", "錯誤" },
	{ "launch_abnormal_exit", "Game ended unexpectedly",
	  "遊戲異常結束" },
	{ "not_playable", "No playable runtime", "目前沒有可用的執行環境" },
	{ "launcher_missing", "Launcher is not installed", "啟動器尚未安裝" },
	{ "launch_failed", "Game launch failed - see launch log",
	  "遊戲啟動失敗 · 請查看啟動紀錄" },
	{ "launch_returned", "Returned to library", "已返回遊戲庫" },
	{ "stream_title", "Sunshine / Moonlight hosts",
	  "Sunshine／Moonlight 主機" },
	{ "stream_safe_store", "Secure host store · Wi-Fi secrets are never shown",
	  "安全主機設定 · 不讀取也不顯示 Wi-Fi 密碼" },
	{ "stream_paired", "Paired", "已配對" },
	{ "stream_not_paired", "Not paired", "尚未配對" },
	{ "stream_display", "Display", "顯示" },
	{ "stream_resolution", "Resolution", "解析度" },
	{ "stream_frame_rate", "Frame rate", "畫面更新率" },
	{ "stream_aspect", "Aspect", "畫面比例" },
	{ "stream_transport", "Stream", "串流參數" },
	{ "stream_codec", "Codec", "編碼格式" },
	{ "stream_bitrate", "Bitrate", "位元率" },
	{ "stream_packet", "Packet size", "封包大小" },
	{ "stream_aspect_fit", "Fit", "等比例完整顯示" },
	{ "stream_aspect_fill", "Fill", "等比例填滿裁切" },
	{ "stream_aspect_stretch", "Stretch", "拉伸填滿" },
	{ "stream_not_deployed", "Moonlight production runner is not deployed",
	  "Moonlight 正式執行器尚未部署" },
	{ "stream_pair_required", "Pair this Moonlight host before launching",
	  "請先完成這台 Moonlight 主機的配對" },
	{ "stream_invalid_host", "Streaming host settings are invalid",
	  "串流主機設定無效" },
	{ "stream_codec_unavailable", "This runner currently supports H.264 only",
	  "目前串流執行器僅支援 H.264" },
	{ "stream_launch_ready", "Press A to start streaming",
	  "按 A 開始串流" },
	{ "stream_fixed_argv", "Fixed arguments · no shell command assembly",
	  "固定參數啟動 · 不使用 shell 指令拼接" },
	{ "stream_launch_failed", "Streaming launch failed - see launch log",
	  "串流啟動失敗 · 請查看啟動紀錄" },
	{ "stream_abnormal_exit", "Streaming ended unexpectedly",
	  "串流異常結束" },
	{ "stream_no_hosts", "No streaming hosts configured",
	  "尚未設定串流主機" },
	{ "stream_add_host", "Add a Sunshine host with netstreamctl",
	  "請使用 netstreamctl 新增 Sunshine 主機" },
	{ "stream_load_failed", "Host settings could not be loaded",
	  "無法載入串流主機設定" },
	{ "stream_reloaded", "Streaming hosts reloaded", "已重新載入串流主機" },
	{ "stream_start", "Start", "啟動" },
	{ "stream_reload", "Reload", "重讀" },
	{ "status", "Status", "狀態" },
	{ "previous", "Previous", "上一台" },
	{ "next", "Next", "下一台" },
	{ "wifi", "Wi-Fi", "Wi-Fi" },
	{ "bluetooth_title", "Bluetooth", "藍牙" },
	{ "bluetooth_action", "Connect", "連線" },
	{ "bluetooth_forget", "Forget", "移除" },
	{ "bluetooth_adapter_on", "Adapter on", "介面卡已開啟" },
	{ "bluetooth_adapter_off", "Adapter off", "介面卡已關閉" },
	{ "bluetooth_adapter_missing", "No Bluetooth adapter",
	  "找不到藍牙介面卡" },
	{ "bluetooth_adapter_missing_detail",
	  "hci0 is not present; the adapter did not come up this boot",
	  "系統中沒有 hci0，本次開機介面卡沒有起來" },
	{ "bluetooth_adapter_off_detail", "Press A to power the adapter on",
	  "按 A 開啟介面卡" },
	{ "bluetooth_scanning", "Scanning", "掃描中" },
	{ "bluetooth_scanning_detail", "Looking for nearby devices",
	  "正在尋找附近的裝置" },
	{ "bluetooth_no_devices", "No devices found · press A to scan",
	  "沒有找到裝置 · 按 A 掃描" },
	{ "bluetooth_no_selection", "No device selected", "沒有選取裝置" },
	{ "bluetooth_state_connected", "Connected", "已連線" },
	{ "bluetooth_state_paired", "Paired", "已配對" },
	{ "bluetooth_state_available", "Available", "可用" },
	{ "bluetooth_hint", "A connect · Y forget · B back",
	  "A 連線 · Y 移除 · B 返回" },
	{ "bluetooth_powering_on", "Powering adapter on", "正在開啟介面卡" },
	{ "bluetooth_scan_queued", "Scan requested", "已要求掃描" },
	{ "bluetooth_connect_queued", "Connecting", "連線中" },
	{ "bluetooth_disconnect_queued", "Disconnecting", "中斷連線中" },
	{ "bluetooth_pair_queued", "Pairing", "配對中" },
	{ "bluetooth_forget_queued", "Removing device", "正在移除裝置" },
	{ "bluetooth_backend_required", "Bluetooth backend unavailable",
	  "藍牙後端無法使用" },
	{ "bluetooth_action_failed", "Bluetooth action failed",
	  "藍牙操作失敗" },
	{ "network_title", "Network", "網路" },
	{ "network_subtitle", "Wi-Fi and hotspot controls",
	  "Wi-Fi 與個人熱點控制" },
	{ "network_wifi_mode", "Wi-Fi", "Wi-Fi" },
	{ "network_hotspot_mode", "Hotspot", "個人熱點" },
	{ "network_wifi_detail", "Networks · exclusive with hotspot",
	  "無線網路 · 與熱點互斥" },
	{ "network_hotspot_detail", "Share connection · exclusive with Wi-Fi",
	  "分享連線 · 與 Wi-Fi 互斥" },
	{ "network_usb_detail", "USB debugging may coexist",
	  "USB 偵錯可同時使用" },
	{ "not_configured", "Not configured", "尚未設定" },
	{ "network_backend_reserved", "Independent Wi-Fi/hotspot backend API reserved",
	  "已預留獨立 Wi-Fi／個人熱點後端介面" },
	{ "network_backend_required", "Network control backend is not connected yet",
	  "網路控制後端尚未接妥" },
	{ "network_action_failed", "Network action failed",
	  "網路操作失敗" },
	{ "network_action_queued", "Network action queued",
	  "網路操作已排入佇列" },
	{ "wifi_signal", "Signal", "訊號" },
	{ "volume", "Volume", "音量" },
	{ "connected", "Connected", "已連線" },
	{ "disconnected", "Disconnected", "未連線" },
	{ "network_dormant", "Dormant", "待機" },
	{ "unknown", "Unknown", "未知" },
	{ "muted", "Muted", "靜音" },
	{ "charging", "Charging", "充電中" },
	{ "discharging", "Discharging", "使用電池" },
	{ "battery_full", "Full", "已充飽" },
	{ "not_charging", "Not charging", "未充電" },
	{ "offline", "Offline", "離線" },
	{ "syncing", "Syncing", "同步中" },
	{ "synced", "Synced", "已同步" },
	{ "system_info", "System information", "系統資訊" },
	{ "taiwan_time", "Taiwan time", "台灣時間" },
	{ "kernel", "Kernel", "核心版本" },
	{ "kernel_build", "Kernel build", "核心建置" },
	{ "cpu", "CPU", "處理器" },
	{ "cpu_voltage", "CPU voltage", "處理器電壓" },
	{ "voltage_regulator", "regulator reading", "穩壓器實測" },
	{ "voltage_hwmon", "hwmon reading", "hwmon 實測" },
	{ "memory", "Memory", "記憶體" },
	{ "cache_swap", "Cache / swap", "快取／交換空間" },
	{ "renderer", "Renderer", "繪圖器" },
	{ "storage", "Storage", "儲存空間" },
	{ "battery", "Battery", "電池" },
	{ "backlight", "Backlight", "螢幕背光" },
	{ "boot_slot", "Boot slot", "開機分割區" },
	{ "unavailable", "unavailable", "無法取得" },
	{ "brightness_safe", "Display minimum stays at a panel-safe level; turning the screen off is a separate action and is never emulated with brightness",
	  "數值 0 仍保留面板安全亮度；關閉螢幕是獨立動作，不會用亮度值模擬關閉螢幕" },
	{ "power_lock", "Power & lock", "電源與鎖定" },
	{ "screen_lock", "Screen lock", "螢幕鎖定" },
	{ "display_mode", "Display mode", "畫面模式" },
	{ "display_mode_value", "Full screen · Original aspect",
	  "全螢幕 · 原比例" },
	{ "display_mode_backend_required", "Display mode needs an emulator/frontend backend; the current global default is full-screen stretch",
	  "畫面模式需由模擬器／前端後端接妥後才可套用；目前全域預設為全螢幕填滿" },
	{ "game_volume_osd", "Volume 0-100", "音量 0–100" },
	{ "volume_osd_backend_required", "The UI suspends while a game runs; volume hotkeys and the 0-100 OSD require a resident system hotkey/OSD backend",
	  "遊戲執行時介面會暫停；音量鍵與 0–100 OSD 必須由常駐系統層快捷鍵／OSD 後端提供" },
	{ "mute_toggle_queued", "Mute toggle applying", "正在切換靜音" },
	{ "restart_mode", "Restart mode", "重新啟動模式" },
	{ "restart_normal", "Normal reboot", "一般重啟" },
	{ "restart_confirm", "Press A again within 5 seconds to confirm normal reboot",
	  "五秒內再按一次 A，確認一般重啟" },
	{ "restart_queued", "Normal reboot requested",
	  "已送出一般重啟要求" },
	{ "restart_busy", "A normal reboot request is already running",
	  "一般重啟要求正在執行" },
	{ "enabled", "On", "開" },
	{ "disabled", "Off", "關" },
	{ "setting_controls", "Left / right / A  Change",
	  "左右／A　變更" },
	{ "settings_controls", "Up/down Select · left/right/A Change · B Back",
	  "上下選擇 · 左右／A 變更 · B 返回" },
	{ "joystick_light", "Joystick light", "搖桿燈" },
	{ "usb_debug", "USB debug", "USB 偵錯" },
	{ "screen_power", "Display power", "螢幕電源" },
	{ "orderly_shutdown", "Orderly shutdown", "正常關機" },
	{ "hardware_control", "Hardware control", "硬體控制" },
	{ "hardware_queued", "Applying", "正在套用" },
	{ "hardware_applied", "Applied", "已套用" },
	{ "hardware_failed", "Failed", "執行失敗" },
	{ "debug_logs", "Debug log export / clear", "偵錯紀錄匯出／清除" },
	{ "debug_logs_unavailable", "Debug log export and clearing stay unavailable until the helper interface is stable, preventing accidental system-log deletion",
	  "偵錯紀錄匯出與清除目前無法使用；輔助程式介面穩定後才會開放，以免誤刪系統紀錄" },
	{ "lock_title", "Locked", "已鎖定" },
	{ "lock_instruction", "Press the same button three times",
	  "同一顆按鍵連按三次解鎖" },
	{ "shutdown_countdown", "Power off countdown", "關機倒數" },
	{ "shutdown_cancel", "Release power or press B to cancel",
	  "放開電源鍵或按 B 取消" },
	{ "lock_on", "Screen lock enabled", "螢幕鎖定已開啟" },
	{ "lock_off", "Screen lock disabled", "螢幕鎖定已關閉" },
};

const char *tr(const struct ui *ui, const char *key)
{
	for (size_t i = 0; i < sizeof(translations) / sizeof(translations[0]); ++i) {
		if (strcmp(translations[i].key, key) == 0) {
			if (ui->locale.language == UI_LANGUAGE_ZH_TW &&
			    translations[i].zh_tw != NULL)
				return translations[i].zh_tw;
			return translations[i].english;
		}
	}
	return key;
}

void locale_init(struct ui *ui, const char *settings_path,
		 const char *requested_language)
{
	char line[64];
	FILE *stream;

	ui->locale.language = UI_LANGUAGE_ZH_TW;
	(void)snprintf(ui->locale.settings_path, sizeof(ui->locale.settings_path),
		       "%s", settings_path);
	stream = fopen(settings_path, "r");
	if (stream != NULL) {
		while (fgets(line, sizeof(line), stream) != NULL) {
			int value;

			if (strncmp(line, "language=en", 11) == 0)
				ui->locale.language = UI_LANGUAGE_ENGLISH;
			else if (strncmp(line, "language=zh_TW", 14) == 0)
				ui->locale.language = UI_LANGUAGE_ZH_TW;
			else if (strncmp(line, "screen_lock=0", 13) == 0)
				ui->settings.preferences.screen_lock_enabled = false;
			else if (strncmp(line, "screen_lock=1", 13) == 0)
				ui->settings.preferences.screen_lock_enabled = true;
			else if (sscanf(line, "brightness=%d", &value) == 1 &&
				 value >= 0 && value <= 100)
				ui->settings.preferences.backlight_percent = value;
			else if (sscanf(line, "joystick_rgb=%d", &value) == 1 &&
				 value >= 0 && value <= 100) {
				ui->settings.preferences.joystick_rgb_brightness = value;
				ui->settings.preferences.joystick_rgb_enabled = value > 0;
			} else if (strncmp(line, "usb_debug=1", 11) == 0)
				ui->settings.preferences.usb_debug_enabled = true;
			else if (strncmp(line, "usb_debug=0", 11) == 0)
				ui->settings.preferences.usb_debug_enabled = false;
		}
		fclose(stream);
	}
	if (requested_language != NULL)
		ui->locale.language = strcmp(requested_language, "en") == 0 ?
			UI_LANGUAGE_ENGLISH : UI_LANGUAGE_ZH_TW;
}

void locale_toggle(struct ui *ui)
{
	ui->locale.language = ui->locale.language == UI_LANGUAGE_ZH_TW ?
		UI_LANGUAGE_ENGLISH : UI_LANGUAGE_ZH_TW;
	persistence_request_locale(ui);
}
