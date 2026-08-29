#include "ui.h"

#include <stdio.h>
#include <string.h>

static const SDL_Color text = { 232, 232, 232, 255 };
static const SDL_Color muted = { 154, 154, 154, 255 };
static const SDL_Color line = { 164, 164, 164, 54 };

static const char *value_or_unknown(struct ui *ui, const char *value)
{
	return value[0] == '\0' || value[0] == '-' ||
		strcmp(value, HARDWARE_TEXT_UNAVAILABLE) == 0 ?
		tr(ui, "unavailable") : value;
}

static void format_bytes(char *buffer, size_t size, int64_t bytes)
{
	if (bytes < 0)
		(void)snprintf(buffer, size, "--");
	else if (bytes >= INT64_C(1073741824))
		(void)snprintf(buffer, size, "%.1f GiB",
			       (double)bytes / 1073741824.0);
	else
		(void)snprintf(buffer, size, "%.0f MiB",
			       (double)bytes / 1048576.0);
}

static void draw_row(struct ui *ui, int x, int y, int width,
		     const char *label, const char *value)
{
	SDL_Rect clip = { x + 105, y, width - 105, 23 };

	text_draw(ui, 0, label, x, y, muted);
	(void)SDL_RenderSetClipRect(ui->renderer, &clip);
	text_draw(ui, 0, value, x + 105, y, text);
	(void)SDL_RenderSetClipRect(ui->renderer, NULL);
	render_fill_rect(ui->renderer, x, y + 21, width, 1, line);
}

static void draw_setting_card(struct ui *ui, int index, int x, int y,
			      int width, const char *label, const char *value)
{
	bool selected = ui->focus_region == UI_FOCUS_CONTENT &&
		ui->settings_index == index;
	SDL_Color edge = selected ? (SDL_Color){ 220, 220, 220, 164 } : line;
	SDL_Rect clip = { x + 9, y + 22, width - 18, 20 };

	render_fill_round_rect(ui->renderer, x + 2, y + 4, width, 44, 10,
		(SDL_Color){ 0, 0, 0, 58 });
	render_fill_round_rect(ui->renderer, x, y, width, 44, 10,
		selected ? (SDL_Color){ 52, 52, 52, 242 } :
		(SDL_Color){ 24, 24, 24, 226 });
	render_outline_round_rect(ui->renderer, x, y, width, 44, 10, edge);
	text_draw(ui, 0, label, x + 9, y + 3, muted);
	(void)SDL_RenderSetClipRect(ui->renderer, &clip);
	text_draw(ui, 1, value, x + 9, y + 20, text);
	(void)SDL_RenderSetClipRect(ui->renderer, NULL);
}

static void draw_volume_setting_card(struct ui *ui, int x, int y, int width,
				     int percent, int muted_state)
{
	bool selected = ui->focus_region == UI_FOCUS_CONTENT &&
		ui->settings_index == 5;
	int clamped = percent < 0 ? 0 : percent > 100 ? 100 : percent;
	char value[32];

	(void)snprintf(value, sizeof(value), percent < 0 ? "--" : "%d%%%s",
		clamped, muted_state > 0 ? " M" : "");
	render_fill_round_rect(ui->renderer, x + 2, y + 4, width, 44, 10,
		(SDL_Color){ 0, 0, 0, 58 });
	render_fill_round_rect(ui->renderer, x, y, width, 44, 10,
		selected ? (SDL_Color){ 52, 52, 52, 242 } :
		(SDL_Color){ 24, 24, 24, 226 });
	render_outline_round_rect(ui->renderer, x, y, width, 44, 10,
		selected ? (SDL_Color){ 220, 220, 220, 164 } : line);
	text_draw(ui, 0, tr(ui, "game_volume_osd"), x + 9, y + 3, muted);
	text_draw(ui, 0, value, x + width - 49, y + 3, text);
	render_fill_round_rect(ui->renderer, x + 9, y + 29, width - 18, 6, 3,
		(SDL_Color){ 10, 10, 10, 210 });
	if (percent >= 0 && clamped > 0)
		render_fill_round_rect(ui->renderer, x + 9, y + 29,
			(width - 18) * clamped / 100, 6, 3,
			(SDL_Color){ 220, 220, 220, 226 });
}

static void draw_volume_row(struct ui *ui, int x, int y, int width,
			    int percent)
{
	const int bar_x = x + 111;
	const int bar_width = 112;
	int clamped = percent < 0 ? 0 : percent > 100 ? 100 : percent;

	text_draw(ui, 0, tr(ui, "volume"), x, y, muted);
	text_draw(ui, 0, "0", x + 94, y, muted);
	render_fill_round_rect(ui->renderer, bar_x, y + 7, bar_width, 8, 4,
		(SDL_Color){ 8, 8, 8, 255 });
	if (percent >= 0 && clamped > 0)
		render_fill_round_rect(ui->renderer, bar_x + 1, y + 8,
			(bar_width - 2) * clamped / 100, 6, 3,
			(SDL_Color){ 220, 220, 220, 230 });
	text_draw(ui, 0, "100", bar_x + bar_width + 5, y, muted);
	render_fill_rect(ui->renderer, x, y + 21, width, 1, line);
}

static void draw_marquee(struct ui *ui, const char *value, int y,
			 uint32_t now)
{
	const int x = UI_LAYOUT_SETTINGS_NOTE_X;
	const int width = UI_LAYOUT_SETTINGS_NOTE_WIDTH;
	SDL_Rect clip = { x, y, width, UI_LAYOUT_SETTINGS_NOTE_HEIGHT };
	int measured = text_width(ui, 0, value, muted);
	int offset = ui_layout_marquee_offset(measured, width, now);

	(void)SDL_RenderSetClipRect(ui->renderer, &clip);
	text_draw(ui, 0, value, x + offset, y, muted);
	(void)SDL_RenderSetClipRect(ui->renderer, NULL);
}

static const char *network_text(struct ui *ui,
				enum hardware_network_state state)
{
	if (state == HARDWARE_NETWORK_UP)
		return tr(ui, "connected");
	if (state == HARDWARE_NETWORK_DOWN)
		return tr(ui, "disconnected");
	if (state == HARDWARE_NETWORK_DORMANT)
		return tr(ui, "network_dormant");
	if (state == HARDWARE_NETWORK_UNKNOWN)
		return tr(ui, "unknown");
	return tr(ui, "unavailable");
}

static const char *battery_text(struct ui *ui,
				enum hardware_battery_status status)
{
	if (status == HARDWARE_BATTERY_CHARGING)
		return tr(ui, "charging");
	if (status == HARDWARE_BATTERY_DISCHARGING)
		return tr(ui, "discharging");
	if (status == HARDWARE_BATTERY_FULL)
		return tr(ui, "battery_full");
	if (status == HARDWARE_BATTERY_NOT_CHARGING)
		return tr(ui, "not_charging");
	if (status == HARDWARE_BATTERY_UNKNOWN)
		return tr(ui, "unknown");
	return tr(ui, "unavailable");
}

static const char *voltage_source_text(struct ui *ui,
				       enum hardware_voltage_source source)
{
	if (source == HARDWARE_VOLTAGE_MEASURED_REGULATOR)
		return tr(ui, "voltage_regulator");
	if (source == HARDWARE_VOLTAGE_MEASURED_HWMON)
		return tr(ui, "voltage_hwmon");
	return tr(ui, "unavailable");
}

static void format_datetime(char *buffer, size_t size,
			    const struct hardware_datetime *datetime)
{
	if (datetime->available <= 0) {
		buffer[0] = '\0';
		return;
	}
	if (strftime(buffer, size, "%Y/%m/%d %H:%M", &datetime->local) == 0)
		buffer[0] = '\0';
}

void render_system_info(struct ui *ui, uint32_t now)
{
	const struct hardware_snapshot *data = &ui->hardware;
	SDL_RendererInfo renderer;
	char value[256];
	char first[32];
	char second[32];
	int y;

	render_glass_panel(ui->renderer, UI_LAYOUT_CONTENT_X, UI_LAYOUT_CONTENT_Y,
		UI_LAYOUT_CONTENT_WIDTH, UI_LAYOUT_CONTENT_HEIGHT,
		ui->focus_region == UI_FOCUS_CONTENT);
	text_draw(ui, 2, tr(ui, "system_info"), 28, 91,
		ui->focus_region == UI_FOCUS_CONTENT ? text : muted);
	render_fill_rect(ui->renderer, 27, 120, 586, 1, line);
	y = 126;
	format_datetime(value, sizeof(value), &data->datetime);
	draw_row(ui, 28, y, 278, tr(ui, "taiwan_time"),
		 value[0] != '\0' ? value : tr(ui, "unavailable"));
	y += 22;
	draw_row(ui, 28, y, 278, tr(ui, "kernel"),
		 value_or_unknown(ui, data->system.kernel_release));
	y += 22;
	if (data->memory.used_bytes >= 0 && data->memory.total_bytes >= 0) {
		format_bytes(first, sizeof(first), data->memory.used_bytes);
		format_bytes(second, sizeof(second), data->memory.total_bytes);
		(void)snprintf(value, sizeof(value), "%s / %s", first, second);
	} else {
		(void)snprintf(value, sizeof(value), "%s", tr(ui, "unavailable"));
	}
	draw_row(ui, 28, y, 278, tr(ui, "memory"), value);
	y += 22;
	if (data->cpu.frequency_khz >= 0 && data->cpu.temperature_millic >= 0)
		(void)snprintf(value, sizeof(value), "%.0f MHz%s%.1f C",
			(double)data->cpu.frequency_khz / 1000.0,
			" · ", (double)data->cpu.temperature_millic / 1000.0);
	else if (data->cpu.frequency_khz >= 0)
		(void)snprintf(value, sizeof(value), "%.0f MHz",
			(double)data->cpu.frequency_khz / 1000.0);
	else
		(void)snprintf(value, sizeof(value), "%s", tr(ui, "unavailable"));
	draw_row(ui, 28, y, 278, tr(ui, "cpu"), value);
	y += 22;
	if (data->cpu.voltage_uv > 0)
		(void)snprintf(value, sizeof(value), "%.0f mV · %s",
			(double)data->cpu.voltage_uv / 1000.0,
			voltage_source_text(ui, data->cpu.voltage_source));
	else
		(void)snprintf(value, sizeof(value), "%s", tr(ui, "unavailable"));
	draw_row(ui, 28, y, 278, tr(ui, "cpu_voltage"), value);
	y += 22;
	(void)SDL_GetRendererInfo(ui->renderer, &renderer);
	draw_row(ui, 28, y, 278, tr(ui, "renderer"),
		 renderer.name != NULL ? renderer.name : tr(ui, "unavailable"));

	y = 126;
	if (data->wifi.operstate != HARDWARE_NETWORK_UNAVAILABLE)
		(void)snprintf(value, sizeof(value), "%s · %s",
			value_or_unknown(ui, data->wifi.interface),
			network_text(ui, data->wifi.operstate));
	else
		(void)snprintf(value, sizeof(value), "%s", tr(ui, "unavailable"));
	draw_row(ui, 322, y, 290, tr(ui, "wifi"), value);
	y += 22;
	if (data->wifi.link_quality >= 0 && data->wifi.signal_dbm >= -127)
		(void)snprintf(value, sizeof(value), "%d%% · %d dBm",
			data->wifi.link_quality, data->wifi.signal_dbm);
	else if (data->wifi.link_quality >= 0)
		(void)snprintf(value, sizeof(value), "%d%%",
			data->wifi.link_quality);
	else
		(void)snprintf(value, sizeof(value), "%s", tr(ui, "unavailable"));
	draw_row(ui, 322, y, 290, tr(ui, "wifi_signal"), value);
	y += 22;
	if (data->battery.percent >= 0)
		(void)snprintf(value, sizeof(value), "%d%% · %s", data->battery.percent,
			battery_text(ui, data->battery.status));
	else
		(void)snprintf(value, sizeof(value), "%s", tr(ui, "unavailable"));
	draw_row(ui, 322, y, 290, tr(ui, "battery"), value);
	y += 22;
	if (data->backlight.percent >= 0)
		(void)snprintf(value, sizeof(value), "%d%%", data->backlight.percent);
	else
		(void)snprintf(value, sizeof(value), "%s", tr(ui, "unavailable"));
	draw_row(ui, 322, y, 290, tr(ui, "backlight"), value);
	y += 22;
	if (data->audio.volume_percent >= 0)
		(void)snprintf(value, sizeof(value), "%d%%%s%s",
			data->audio.volume_percent, data->audio.muted > 0 ? " · " : "",
			data->audio.muted > 0 ? tr(ui, "muted") : "");
	else
		(void)snprintf(value, sizeof(value), "%s", tr(ui, "unavailable"));
	draw_volume_row(ui, 322, y, 290, data->audio.volume_percent);
	y += 22;
	if (data->storage.used_bytes >= 0 && data->storage.total_bytes >= 0) {
		format_bytes(first, sizeof(first), data->storage.used_bytes);
		format_bytes(second, sizeof(second), data->storage.total_bytes);
		(void)snprintf(value, sizeof(value), "%s / %s", first, second);
	} else {
		(void)snprintf(value, sizeof(value), "%s", tr(ui, "unavailable"));
	}
	draw_row(ui, 322, y, 290, tr(ui, "storage"), value);

	render_fill_rect(ui->renderer, 27, 263, 586, 1, line);
	text_draw(ui, 0, tr(ui, "settings_controls"), 28, 267, muted);
	if (settings_backend_available(&ui->settings)) {
		(void)snprintf(first, sizeof(first), "%d%%",
			ui->settings.preferences.backlight_percent);
		(void)snprintf(second, sizeof(second), "%d%%",
			ui->settings.preferences.joystick_rgb_brightness);
	} else {
		(void)snprintf(first, sizeof(first), "%s", tr(ui, "unavailable"));
		(void)snprintf(second, sizeof(second), "%s", tr(ui, "unavailable"));
	}
	draw_setting_card(ui, 0, 28, 284, 140, tr(ui, "backlight"), first);
	draw_setting_card(ui, 1, 176, 284, 140, tr(ui, "joystick_light"), second);
	draw_setting_card(ui, 2, 324, 284, 140, tr(ui, "usb_debug"),
		ui->settings.usb_debug_available > 0 ?
		tr(ui, ui->settings.preferences.usb_debug_enabled ?
		   "enabled" : "disabled") : tr(ui, "unavailable"));
	draw_setting_card(ui, 3, 472, 284, 140, tr(ui, "screen_lock"),
		tr(ui, ui->settings.preferences.screen_lock_enabled ?
		   "enabled" : "disabled"));
	draw_setting_card(ui, 4, 28, 334, 188, tr(ui, "display_mode"),
		tr(ui, "display_mode_value"));
	draw_volume_setting_card(ui, 224, 334, 188,
		ui->settings.volume_target >= 0 ? ui->settings.volume_target :
		data->audio.volume_percent, data->audio.muted);
	draw_setting_card(ui, 6, 420, 334, 192, tr(ui, "restart_mode"),
		tr(ui, "restart_normal"));
	draw_marquee(ui, tr(ui, "brightness_safe"),
		UI_LAYOUT_SETTINGS_NOTE_FIRST_Y, now);
	draw_marquee(ui, tr(ui, "volume_osd_backend_required"),
		UI_LAYOUT_SETTINGS_NOTE_SECOND_Y, now);
}

void system_info_print_result(const struct ui *ui)
{
	char datetime[32];

	format_datetime(datetime, sizeof(datetime), &ui->hardware.datetime);
	printf("SYSTEM_RESULT PASS taipei=%s kernel=%s ram_used=%lld ram_total=%lld wifi=%s signal=%d battery=%d battery_status=%s backlight=%d volume=%d muted=%d voltage_uv=%lld voltage_source=%s\n",
		datetime[0] != '\0' ? datetime : HARDWARE_TEXT_UNAVAILABLE,
		ui->hardware.system.kernel_release,
		(long long)ui->hardware.memory.used_bytes,
		(long long)ui->hardware.memory.total_bytes,
		hardware_network_state_label(ui->hardware.wifi.operstate),
		ui->hardware.wifi.link_quality, ui->hardware.battery.percent,
		hardware_battery_status_label(ui->hardware.battery.status),
		ui->hardware.backlight.percent, ui->hardware.audio.volume_percent,
		ui->hardware.audio.muted, (long long)ui->hardware.cpu.voltage_uv,
		hardware_voltage_source_label(ui->hardware.cpu.voltage_source));
}
