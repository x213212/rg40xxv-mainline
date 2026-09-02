#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const SDL_Color text = { 232, 232, 232, 255 };
static const SDL_Color muted = { 154, 154, 154, 255 };
static const SDL_Color line = { 164, 164, 164, 54 };

struct render_clip_state {
	SDL_Rect previous;
	bool enabled;
};

static void push_clip(SDL_Renderer *renderer, const SDL_Rect *requested,
			      struct render_clip_state *state)
{
	SDL_Rect clipped = *requested;

	state->enabled = SDL_RenderIsClipEnabled(renderer) == SDL_TRUE;
	if (state->enabled) {
		SDL_RenderGetClipRect(renderer, &state->previous);
		if (SDL_IntersectRect(&state->previous, requested, &clipped) !=
		    SDL_TRUE)
			clipped = (SDL_Rect){ 0, 0, 0, 0 };
	}
	(void)SDL_RenderSetClipRect(renderer, &clipped);
}

static void pop_clip(SDL_Renderer *renderer,
			     const struct render_clip_state *state)
{
	(void)SDL_RenderSetClipRect(renderer,
		state->enabled ? &state->previous : NULL);
}

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
	struct render_clip_state saved;

	text_draw(ui, 0, label, x, y, muted);
	push_clip(ui->renderer, &clip, &saved);
	text_draw(ui, 0, value, x + 105, y, text);
	pop_clip(ui->renderer, &saved);
	render_fill_rect(ui->renderer, x, y + 21, width, 1, line);
}

static void draw_setting_card(struct ui *ui, int index, int x, int y,
			      int width, const char *label, const char *value)
{
	bool selected = ui->focus_region == UI_FOCUS_CONTENT &&
		ui->settings_index == index;
	bool editing = selected && ui->settings_detail_active;
	SDL_Color edge = editing ? (SDL_Color){ 238, 238, 238, 232 } :
		selected ? (SDL_Color){ 220, 220, 220, 164 } : line;
	SDL_Rect clip = { x + 9, y + 22, width - 18, 20 };
	struct render_clip_state saved;

	render_fill_round_rect(ui->renderer, x + 2, y + 4, width, 44, 10,
		(SDL_Color){ 0, 0, 0, 58 });
	render_fill_round_rect(ui->renderer, x, y, width, 44, 10,
		editing ? (SDL_Color){ 66, 66, 66, 246 } :
		selected ? (SDL_Color){ 52, 52, 52, 242 } :
		(SDL_Color){ 24, 24, 24, 226 });
	render_outline_round_rect(ui->renderer, x, y, width, 44, 10, edge);
	if (editing)
		render_fill_round_rect(ui->renderer, x + 4, y + 8, 3, 28, 1,
			(SDL_Color){ 238, 238, 238, 220 });
	text_draw(ui, 0, label, x + 9, y + 3, muted);
	push_clip(ui->renderer, &clip, &saved);
	text_draw(ui, 1, value, x + 9, y + 20, text);
	pop_clip(ui->renderer, &saved);
}

static void format_auto_screen_off(struct ui *ui, char *buffer, size_t size)
{
	int minutes = ui->settings.preferences.auto_screen_off_minutes;

	if (minutes <= 0)
		(void)snprintf(buffer, size, "%s", tr(ui, "disabled"));
	else
		(void)snprintf(buffer, size, "%d %s", minutes,
			tr(ui, "minutes_short"));
}

static void draw_volume_setting_card(struct ui *ui, int x, int y, int width,
				     int percent, int muted_state)
{
	bool selected = ui->focus_region == UI_FOCUS_CONTENT &&
		ui->settings_index == 5;
	bool editing = selected && ui->settings_detail_active;
	int clamped = percent < 0 ? 0 : percent > 100 ? 100 : percent;
	char value[32];

	(void)snprintf(value, sizeof(value), percent < 0 ? "--" : "%d%%%s",
		clamped, muted_state > 0 ? " M" : "");
	render_fill_round_rect(ui->renderer, x + 2, y + 4, width, 44, 10,
		(SDL_Color){ 0, 0, 0, 58 });
	render_fill_round_rect(ui->renderer, x, y, width, 44, 10,
		editing ? (SDL_Color){ 66, 66, 66, 246 } :
		selected ? (SDL_Color){ 52, 52, 52, 242 } :
		(SDL_Color){ 24, 24, 24, 226 });
	render_outline_round_rect(ui->renderer, x, y, width, 44, 10,
		editing ? (SDL_Color){ 238, 238, 238, 232 } :
		selected ? (SDL_Color){ 220, 220, 220, 164 } : line);
	if (editing)
		render_fill_round_rect(ui->renderer, x + 4, y + 8, 3, 28, 1,
			(SDL_Color){ 238, 238, 238, 220 });
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
	struct render_clip_state saved;
	int measured = text_width(ui, 0, value, muted);
	int offset = ui_layout_marquee_offset(measured, width, now);

	if (measured > width)
		ui->settings_marquee_active = true;
	push_clip(ui->renderer, &clip, &saved);
	text_draw(ui, 0, value, x + offset, y, muted);
	pop_clip(ui->renderer, &saved);
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

enum settings_paint_part {
	SETTINGS_PAINT_TOP = 1U << 0,
	SETTINGS_PAINT_INSTRUCTION = 1U << 1,
	SETTINGS_PAINT_CARDS = 1U << 2,
	SETTINGS_PAINT_NOTES = 1U << 3,
	SETTINGS_PAINT_ALL = (1U << 4) - 1U,
};

static void render_system_info_parts(struct ui *ui, uint32_t now,
				     unsigned int parts)
{
	static int profile = -1;
	uint64_t ticks[6] = { 0 };
	uint64_t frequency = 0;
	const struct hardware_snapshot *data = &ui->hardware;
	SDL_RendererInfo renderer;
	char value[256];
	char first[32];
	char second[32];
	int y;
	if (profile < 0)
		profile = getenv("RG40XXV_RENDER_PROFILE") != NULL;
	if (profile && parts == SETTINGS_PAINT_ALL) {
		frequency = SDL_GetPerformanceFrequency();
		ticks[0] = SDL_GetPerformanceCounter();
	}

	if ((parts & SETTINGS_PAINT_TOP) != 0U) {
	text_draw(ui, 2, tr(ui, "system_info"), 28, 91,
		ui->focus_region == UI_FOCUS_CONTENT ? text : muted);
	render_fill_rect(ui->renderer, 27, 120, 586, 1, line);
	if (profile)
		ticks[1] = SDL_GetPerformanceCounter();
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
	if (profile)
		ticks[2] = SDL_GetPerformanceCounter();

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
	if (profile && parts == SETTINGS_PAINT_ALL)
		ticks[3] = SDL_GetPerformanceCounter();
	}

	if ((parts & SETTINGS_PAINT_INSTRUCTION) != 0U) {
	render_fill_rect(ui->renderer, 27, 263, 586, 1, line);
	text_draw(ui, 0, tr(ui, ui->settings_detail_active ?
		"settings_detail_controls" : "settings_controls"), 28, 267,
		muted);
	}
	if ((parts & SETTINGS_PAINT_CARDS) != 0U) {
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
	draw_setting_card(ui, 4, 28, 334, 188, tr(ui, "restart_mode"),
		tr(ui, "restart_custom_system"));
	draw_volume_setting_card(ui, 224, 334, 188,
		ui->settings.volume_target >= 0 ? ui->settings.volume_target :
		data->audio.volume_percent, data->audio.muted);
	format_auto_screen_off(ui, first, sizeof(first));
	draw_setting_card(ui, 6, 420, 334, 192, tr(ui, "auto_screen_off"), first);
	if (profile && parts == SETTINGS_PAINT_ALL)
		ticks[4] = SDL_GetPerformanceCounter();
	}
	if ((parts & SETTINGS_PAINT_NOTES) != 0U) {
	ui->settings_marquee_active = false;
	draw_marquee(ui, tr(ui, "brightness_safe"),
		UI_LAYOUT_SETTINGS_NOTE_FIRST_Y, now);
	draw_marquee(ui, tr(ui, "volume_osd_backend_required"),
		UI_LAYOUT_SETTINGS_NOTE_SECOND_Y, now);
	}
	if (profile && parts == SETTINGS_PAINT_ALL) {
		ticks[5] = SDL_GetPerformanceCounter();
		(void)fprintf(stderr,
			"SETTINGS_PROFILE panel=%.3f left_rows=%.3f right_rows=%.3f cards=%.3f notes=%.3f total=%.3f\n",
			(double)(ticks[1] - ticks[0]) * 1000.0 / (double)frequency,
			(double)(ticks[2] - ticks[1]) * 1000.0 / (double)frequency,
			(double)(ticks[3] - ticks[2]) * 1000.0 / (double)frequency,
			(double)(ticks[4] - ticks[3]) * 1000.0 / (double)frequency,
			(double)(ticks[5] - ticks[4]) * 1000.0 / (double)frequency,
			(double)(ticks[5] - ticks[0]) * 1000.0 / (double)frequency);
	}
}

void render_system_info(struct ui *ui, uint32_t now)
{
	render_system_info_parts(ui, now, SETTINGS_PAINT_ALL);
}

static void settings_render_key(const struct ui *ui,
				struct settings_render_key *key)
{
	memset(key, 0, sizeof(*key));
	key->hardware = ui->hardware;
	key->preferences = ui->settings.preferences;
	key->volume_target = ui->settings.volume_target;
	key->usb_debug_available = ui->settings.usb_debug_available;
	key->settings_index = ui->settings_index;
	key->focus_region = ui->focus_region;
	key->language = ui->locale.language;
	key->detail_active = ui->settings_detail_active;
	key->backend_available = settings_backend_available(&ui->settings);
}

static bool hardware_rows_equal(const struct hardware_snapshot *left,
				const struct hardware_snapshot *right)
{
	if (left->datetime.available != right->datetime.available)
		return false;
	if (left->datetime.available > 0 &&
	    (left->datetime.local.tm_year != right->datetime.local.tm_year ||
	     left->datetime.local.tm_mon != right->datetime.local.tm_mon ||
	     left->datetime.local.tm_mday != right->datetime.local.tm_mday ||
	     left->datetime.local.tm_hour != right->datetime.local.tm_hour ||
	     left->datetime.local.tm_min != right->datetime.local.tm_min))
		return false;
	return strcmp(left->system.kernel_release,
			right->system.kernel_release) == 0 &&
		left->memory.used_bytes == right->memory.used_bytes &&
		left->memory.total_bytes == right->memory.total_bytes &&
		left->cpu.frequency_khz == right->cpu.frequency_khz &&
		left->cpu.temperature_millic == right->cpu.temperature_millic &&
		left->cpu.voltage_uv == right->cpu.voltage_uv &&
		left->cpu.voltage_source == right->cpu.voltage_source &&
		strcmp(left->wifi.interface, right->wifi.interface) == 0 &&
		left->wifi.operstate == right->wifi.operstate &&
		left->wifi.link_quality == right->wifi.link_quality &&
		left->wifi.signal_dbm == right->wifi.signal_dbm &&
		left->battery.percent == right->battery.percent &&
		left->battery.status == right->battery.status &&
		left->backlight.percent == right->backlight.percent &&
		left->audio.volume_percent == right->audio.volume_percent &&
		left->storage.used_bytes == right->storage.used_bytes &&
		left->storage.total_bytes == right->storage.total_bytes;
}

static SDL_Rect settings_card_region(int index)
{
	static const SDL_Rect regions[] = {
		{ 28, 284, 142, 48 }, { 176, 284, 142, 48 },
		{ 324, 284, 142, 48 }, { 472, 284, 142, 48 },
		{ 28, 334, 190, 48 }, { 224, 334, 190, 48 },
		{ 420, 334, 194, 48 },
	};

	return index >= 0 && index < (int)(sizeof(regions) / sizeof(regions[0])) ?
		regions[index] : (SDL_Rect){ 0, 0, 0, 0 };
}

static void add_dirty_region(SDL_Rect *dirty, bool *present, SDL_Rect added)
{
	if (added.w <= 0 || added.h <= 0)
		return;
	if (!*present) {
		*dirty = added;
		*present = true;
		return;
	}
	SDL_UnionRect(dirty, &added, dirty);
}

static void redraw_settings_region(struct ui *ui, uint32_t now,
				   const SDL_Rect *region,
				   unsigned int parts)
{
	(void)SDL_RenderSetClipRect(ui->renderer, region);
	render_settings_background(ui,
		ui->focus_region == UI_FOCUS_CONTENT);
	render_system_info_parts(ui, now, parts);
	(void)SDL_RenderSetClipRect(ui->renderer, NULL);
}

void render_settings_page(struct ui *ui, uint32_t now)
{
	struct settings_render_key key;
	SDL_Texture *previous;
	bool full_redraw;
	bool top_dirty = false;
	bool instruction_dirty = false;
	bool cards_dirty = false;
	bool notes_dirty = false;
	SDL_Rect cards = { 0, 0, 0, 0 };

	settings_render_key(ui, &key);
	full_redraw = !ui->settings_page_cache_valid ||
		ui->settings_page_cache_key.language != key.language ||
		ui->settings_page_cache_key.focus_region != key.focus_region;
	if (!full_redraw) {
		const struct settings_render_key *old =
			&ui->settings_page_cache_key;

		top_dirty = !hardware_rows_equal(&old->hardware, &key.hardware);
		if (old->detail_active != key.detail_active) {
			instruction_dirty = true;
			add_dirty_region(&cards, &cards_dirty,
				settings_card_region(key.settings_index));
		}
		if (old->settings_index != key.settings_index) {
			add_dirty_region(&cards, &cards_dirty,
				settings_card_region(old->settings_index));
			add_dirty_region(&cards, &cards_dirty,
				settings_card_region(key.settings_index));
		}
		if (old->backend_available != key.backend_available) {
			add_dirty_region(&cards, &cards_dirty,
				settings_card_region(0));
			add_dirty_region(&cards, &cards_dirty,
				settings_card_region(1));
		}
		if (old->preferences.backlight_percent !=
		    key.preferences.backlight_percent)
			add_dirty_region(&cards, &cards_dirty,
				settings_card_region(0));
		if (old->preferences.joystick_rgb_brightness !=
		    key.preferences.joystick_rgb_brightness)
			add_dirty_region(&cards, &cards_dirty,
				settings_card_region(1));
		if (old->usb_debug_available != key.usb_debug_available ||
		    old->preferences.usb_debug_enabled !=
			key.preferences.usb_debug_enabled)
			add_dirty_region(&cards, &cards_dirty,
				settings_card_region(2));
		if (old->preferences.screen_lock_enabled !=
		    key.preferences.screen_lock_enabled)
			add_dirty_region(&cards, &cards_dirty,
				settings_card_region(3));
		if (old->preferences.auto_screen_off_minutes !=
		    key.preferences.auto_screen_off_minutes)
			add_dirty_region(&cards, &cards_dirty,
				settings_card_region(6));
		if (old->volume_target != key.volume_target ||
		    old->hardware.audio.volume_percent !=
			key.hardware.audio.volume_percent ||
		    old->hardware.audio.muted != key.hardware.audio.muted)
			add_dirty_region(&cards, &cards_dirty,
				settings_card_region(5));
		notes_dirty = ui->settings_marquee_active;
	}
	if (full_redraw || top_dirty || instruction_dirty || cards_dirty ||
	    notes_dirty) {
		bool created = false;

		if (ui->settings_page_cache == NULL) {
			ui->settings_page_cache = SDL_CreateTexture(ui->renderer,
				SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET,
				UI_WIDTH, UI_HEIGHT);
			created = ui->settings_page_cache != NULL;
		}
		previous = SDL_GetRenderTarget(ui->renderer);
		if (ui->settings_page_cache == NULL ||
		    SDL_SetRenderTarget(ui->renderer,
			ui->settings_page_cache) != 0) {
			if (created && ui->settings_page_cache != NULL) {
				SDL_DestroyTexture(ui->settings_page_cache);
				ui->settings_page_cache = NULL;
			}
			(void)SDL_SetRenderTarget(ui->renderer, previous);
			ui->settings_page_cache_valid = false;
			render_settings_background(ui,
				ui->focus_region == UI_FOCUS_CONTENT);
			render_system_info(ui, now);
			return;
		}
		if (full_redraw) {
			render_settings_background(ui,
				ui->focus_region == UI_FOCUS_CONTENT);
			render_system_info(ui, now);
		} else {
			if (top_dirty)
				redraw_settings_region(ui, now,
					&(SDL_Rect){ 20, 88, 600, 175 },
					SETTINGS_PAINT_TOP);
			if (instruction_dirty)
				redraw_settings_region(ui, now,
					&(SDL_Rect){ 27, 263, 586, 21 },
					SETTINGS_PAINT_INSTRUCTION);
			if (cards_dirty)
				redraw_settings_region(ui, now, &cards,
					SETTINGS_PAINT_CARDS);
			if (notes_dirty)
				redraw_settings_region(ui, now,
					&(SDL_Rect){ 28, 388, 584, 37 },
					SETTINGS_PAINT_NOTES);
		}
		(void)SDL_SetRenderTarget(ui->renderer, previous);
		(void)SDL_SetTextureBlendMode(ui->settings_page_cache,
			SDL_BLENDMODE_NONE);
		ui->settings_page_cache_key = key;
		ui->settings_page_cache_valid = true;
	}
	if (ui->settings_page_cache_valid)
		ui->settings_page_cache_key = key;
	(void)SDL_RenderCopy(ui->renderer, ui->settings_page_cache, NULL, NULL);
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
