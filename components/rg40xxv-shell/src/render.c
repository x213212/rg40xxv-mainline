#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void render_set_color(SDL_Renderer *renderer, SDL_Color color)
{
	(void)SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
}

void render_fill_rect(SDL_Renderer *renderer, int x, int y, int w, int h,
			      SDL_Color color)
{
	SDL_Rect rect = { x, y, w, h };

	render_set_color(renderer, color);
	(void)SDL_RenderFillRect(renderer, &rect);
}

void render_outline_rect(SDL_Renderer *renderer, int x, int y, int w, int h,
				 SDL_Color color)
{
	SDL_Rect rect = { x, y, w, h };

	render_set_color(renderer, color);
	(void)SDL_RenderDrawRect(renderer, &rect);
}

void render_fill_round_rect(SDL_Renderer *renderer, int x, int y, int w, int h,
			    int radius, SDL_Color color)
{
	if (w <= 0 || h <= 0)
		return;
	if (radius < 1) {
		render_fill_rect(renderer, x, y, w, h, color);
		return;
	}
	if (radius * 2 > w)
		radius = w / 2;
	if (radius * 2 > h)
		radius = h / 2;
	render_set_color(renderer, color);
	{
		SDL_Rect middle = { x, y + radius, w, h - radius * 2 };
		SDL_Rect horizontal = { x + radius, y, w - radius * 2, h };

		(void)SDL_RenderFillRect(renderer, &middle);
		(void)SDL_RenderFillRect(renderer, &horizontal);
	}
	for (int row = 0; row < radius; ++row) {
		double dy = (double)radius - (double)row - 0.5;
		int inset = radius - (int)floor(sqrt((double)radius * radius -
			dy * dy));
		int left = x + inset;
		int right = x + w - inset - 1;

		(void)SDL_RenderDrawLine(renderer, left, y + row, right, y + row);
		(void)SDL_RenderDrawLine(renderer, left, y + h - row - 1,
			right, y + h - row - 1);
	}
}

void render_outline_round_rect(SDL_Renderer *renderer, int x, int y, int w,
			       int h, int radius, SDL_Color color)
{
	if (radius < 1) {
		render_outline_rect(renderer, x, y, w, h, color);
		return;
	}
	if (radius * 2 > w)
		radius = w / 2;
	if (radius * 2 > h)
		radius = h / 2;
	render_set_color(renderer, color);
	(void)SDL_RenderDrawLine(renderer, x + radius, y, x + w - radius - 1, y);
	(void)SDL_RenderDrawLine(renderer, x + radius, y + h - 1,
		x + w - radius - 1, y + h - 1);
	(void)SDL_RenderDrawLine(renderer, x, y + radius, x, y + h - radius - 1);
	(void)SDL_RenderDrawLine(renderer, x + w - 1, y + radius,
		x + w - 1, y + h - radius - 1);
	for (int degree = 0; degree <= 90; degree += 3) {
		double angle = (double)degree * 3.141592653589793 / 180.0;
		int dx = (int)lrint(cos(angle) * (radius - 1));
		int dy = (int)lrint(sin(angle) * (radius - 1));

		(void)SDL_RenderDrawPoint(renderer, x + radius - 1 - dx,
			y + radius - 1 - dy);
		(void)SDL_RenderDrawPoint(renderer, x + w - radius + dx,
			y + radius - 1 - dy);
		(void)SDL_RenderDrawPoint(renderer, x + radius - 1 - dx,
			y + h - radius + dy);
		(void)SDL_RenderDrawPoint(renderer, x + w - radius + dx,
			y + h - radius + dy);
	}
}

void render_glass_panel(SDL_Renderer *renderer, int x, int y, int w, int h,
			       bool focused)
{
	SDL_Color edge = focused ? (SDL_Color){ 220, 220, 220, 138 }
				 : (SDL_Color){ 164, 164, 164, 72 };

	render_fill_round_rect(renderer, x + 2, y + 6, w, h, 13,
		(SDL_Color){ 0, 0, 0, 82 });
	render_fill_round_rect(renderer, x - 1, y - 1, w + 2, h + 2, 14,
		focused ? (SDL_Color){ 220, 220, 220, 24 } :
		(SDL_Color){ 190, 190, 190, 10 });
	render_fill_round_rect(renderer, x, y, w, h, 12,
		focused ? (SDL_Color){ 28, 28, 28, 232 } :
		(SDL_Color){ 18, 18, 18, 218 });
	render_outline_round_rect(renderer, x, y, w, h, 12, edge);
	render_fill_round_rect(renderer, x + 8, y + 2, w - 16, 2, 1,
		focused ? (SDL_Color){ 220, 220, 220, 54 } :
		(SDL_Color){ 220, 220, 220, 24 });
}

static uint8_t mix_channel(unsigned int first, unsigned int second,
			   unsigned int amount, unsigned int scale)
{
	return (uint8_t)((first * (scale - amount) + second * amount) / scale);
}

void render_backdrop(struct ui *ui)
{
	const SDL_Color top = { 8, 8, 8, 255 };
	const SDL_Color middle = { 20, 20, 20, 255 };
	const SDL_Color bottom = { 6, 6, 6, 255 };
	const int band = 4;

	for (int y = 0; y < UI_LAYOUT_CONTROLS_Y; y += band) {
		SDL_Color color;
		if (y < UI_LAYOUT_CONTROLS_Y / 2) {
			unsigned int amount = (unsigned int)y;
			unsigned int scale = UI_LAYOUT_CONTROLS_Y / 2;

			color = (SDL_Color) {
				mix_channel(top.r, middle.r, amount, scale),
				mix_channel(top.g, middle.g, amount, scale),
				mix_channel(top.b, middle.b, amount, scale), 255,
			};
		} else {
			unsigned int amount = (unsigned int)(y -
				UI_LAYOUT_CONTROLS_Y / 2);
			unsigned int scale = UI_LAYOUT_CONTROLS_Y -
				UI_LAYOUT_CONTROLS_Y / 2;

			color = (SDL_Color) {
				mix_channel(middle.r, bottom.r, amount, scale),
				mix_channel(middle.g, bottom.g, amount, scale),
				mix_channel(middle.b, bottom.b, amount, scale), 255,
			};
		}
		render_fill_rect(ui->renderer, 0, y, UI_WIDTH, band, color);
	}
	if (ui->nav_index <= NAV_PAGE_FAVORITES ||
	    ui->nav_index == NAV_PAGE_APPS) {
		size_t game_id = catalog_visible_id(ui, ui->game_index);
		int width = 0;
		int height = 0;
		SDL_Texture *texture = game_id == SIZE_MAX ? NULL :
			cover_cache_get(ui, game_id, &width, &height);

		if (texture != NULL && width > 0 && height > 0) {
			SDL_Rect destination = { -18, 30, UI_WIDTH + 36,
				UI_LAYOUT_CONTROLS_Y - 20 };

			(void)SDL_SetTextureColorMod(texture, 158, 158, 158);
			(void)SDL_SetTextureAlphaMod(texture, 30);
			(void)SDL_RenderCopy(ui->renderer, texture, NULL, &destination);
			(void)SDL_SetTextureAlphaMod(texture, 255);
			(void)SDL_SetTextureColorMod(texture, 255, 255, 255);
			render_fill_rect(ui->renderer, 0, 0, UI_WIDTH,
				UI_LAYOUT_CONTROLS_Y,
				(SDL_Color){ 5, 5, 5, 132 });
		}
	}
	render_fill_round_rect(ui->renderer, 44, 104, 552, 230, 110,
		(SDL_Color){ 172, 172, 172, 12 });
}

static void draw_wifi_icon(struct ui *ui, int x, int y, int percent)
{
	for (int bar = 0; bar < 4; ++bar) {
		int height = 3 + bar * 3;
		SDL_Color color = percent >= (bar + 1) * 25 ?
			(SDL_Color){ 210, 210, 210, 255 } :
			(SDL_Color){ 76, 76, 76, 255 };

		render_outline_rect(ui->renderer, x + bar * 4, y + 12 - height,
				    3, height, color);
	}
}

static void draw_battery_icon(struct ui *ui, int x, int y, int percent)
{
	const SDL_Color edge = { 180, 180, 180, 255 };

	render_outline_rect(ui->renderer, x, y, 20, 10, edge);
	render_fill_rect(ui->renderer, x + 20, y + 3, 2, 4, edge);
	if (percent > 0) {
		int width = (percent * 16 + 99) / 100;

		render_fill_rect(ui->renderer, x + 2, y + 2, width, 6,
				 (SDL_Color){ 185, 185, 185, 255 });
	}
}

static void draw_volume_icon(struct ui *ui, int x, int y, int percent)
{
	const SDL_Color edge = { 180, 180, 180, 255 };

	render_outline_rect(ui->renderer, x, y, 13, 12, edge);
	if (percent > 0) {
		int height = (percent * 8 + 99) / 100;

		render_fill_rect(ui->renderer, x + 3, y + 9 - height, 7, height, edge);
	}
}

void render_status(struct ui *ui)
{
	const SDL_Color primary = { 235, 235, 235, 255 };
	const SDL_Color secondary = { 158, 158, 158, 255 };
	const struct hardware_snapshot *status = &ui->hardware;
	char datetime[40] = "--/-- --:--";
	char wifi[20] = "--";
	char battery[20] = "--";
	char volume[20] = "--";
	const char *sync;

	if (status->datetime.available > 0) {
		if (ui->locale.language == UI_LANGUAGE_ZH_TW)
			(void)snprintf(datetime, sizeof(datetime), "%d月%d日 %02d:%02d",
				status->datetime.local.tm_mon + 1,
				status->datetime.local.tm_mday,
				status->datetime.local.tm_hour,
				status->datetime.local.tm_min);
		else
			(void)snprintf(datetime, sizeof(datetime), "%02d/%02d %02d:%02d",
				status->datetime.local.tm_mon + 1,
				status->datetime.local.tm_mday,
				status->datetime.local.tm_hour,
				status->datetime.local.tm_min);
	}
	if (status->wifi.operstate == HARDWARE_NETWORK_UP &&
	    status->wifi.link_quality >= 0)
		(void)snprintf(wifi, sizeof(wifi), "%d%%", status->wifi.link_quality);
	if (status->battery.percent >= 0)
		(void)snprintf(battery, sizeof(battery), "%d%%%s",
			status->battery.percent,
			status->battery.status == HARDWARE_BATTERY_CHARGING ? "+" :
			status->battery.percent <= 15 ? "!" : "");
	if (status->audio.volume_percent >= 0)
		(void)snprintf(volume, sizeof(volume), "%s%d%%",
			status->audio.muted > 0 ? "M " : "", status->audio.volume_percent);
	if (status->wifi.operstate != HARDWARE_NETWORK_UP)
		sync = tr(ui, "offline");
	else if (status->datetime.time_synced > 0)
		sync = tr(ui, "synced");
	else
		sync = tr(ui, "syncing");
	render_fill_round_rect(ui->renderer, 8, 4, UI_WIDTH - 16, 28, 11,
		(SDL_Color){ 18, 18, 18, 196 });
	render_outline_round_rect(ui->renderer, 8, 4, UI_WIDTH - 16, 28, 11,
		(SDL_Color){ 170, 170, 170, 36 });
	text_draw(ui, 0, datetime, 18, 8, primary);
	text_draw(ui, 0, sync, 143, 8, secondary);
	if (ui->icon_atlas != NULL) {
		material_icon_draw(ui, MATERIAL_ICON_WIFI, 348, 7, 22, primary,
			status->wifi.operstate == HARDWARE_NETWORK_UP, false);
		material_icon_draw(ui, MATERIAL_ICON_BATTERY, 437, 7, 22,
			status->battery.percent <= 15 ?
			(SDL_Color){ 242, 139, 130, 255 } : primary,
			status->battery.status == HARDWARE_BATTERY_CHARGING, false);
		material_icon_draw(ui, MATERIAL_ICON_VOLUME, 542, 7, 22, primary,
			status->audio.muted <= 0, false);
	} else {
		draw_wifi_icon(ui, 354, 11, status->wifi.link_quality);
		draw_battery_icon(ui, 443, 12, status->battery.percent);
		draw_volume_icon(ui, 548, 11, status->audio.volume_percent);
	}
	text_draw(ui, 0, wifi, 373, 8, secondary);
	text_draw(ui, 0, battery, 470, 8, secondary);
	text_draw(ui, 0, volume, 567, 8, secondary);
}

static void draw_navigation_contents(struct ui *ui)
{
	static const char *const nav_keys[NAV_COUNT] = {
		"nav_recent", "nav_library", "nav_favorites", "nav_streaming",
		"nav_apps", "nav_network", "nav_settings",
	};
	static const enum material_icon_id nav_icons[NAV_COUNT] = {
		MATERIAL_ICON_HISTORY, MATERIAL_ICON_LIBRARY,
		MATERIAL_ICON_FAVORITE, MATERIAL_ICON_CAST, MATERIAL_ICON_APPS,
		MATERIAL_ICON_WIFI, MATERIAL_ICON_SETTINGS,
	};
	const SDL_Color normal = { 158, 158, 158, 255 };
	const SDL_Color selected = { 238, 238, 238, 255 };
	const SDL_Color accent = { 220, 220, 220, 255 };

	render_glass_panel(ui->renderer, UI_LAYOUT_NAV_X, UI_LAYOUT_NAV_Y,
		UI_LAYOUT_NAV_WIDTH, UI_LAYOUT_NAV_HEIGHT,
		ui->focus_region == UI_FOCUS_TOP_NAV);
	for (int i = 0; i < NAV_COUNT; ++i) {
		struct ui_layout_rect tab = ui_layout_navigation_tab(i);
		const char *label = tr(ui, nav_keys[i]);
		SDL_Rect clip = { tab.x + 3, tab.y, tab.width - 6, tab.height };
		int label_width = text_width(ui, 0, label,
			i == ui->nav_index ? selected : normal);
		int icon_size = ui->icon_atlas != NULL ? 18 : 0;
		int gap = icon_size > 0 ? 3 : 0;
		int group_width = icon_size + gap + label_width;
		int group_x = tab.x + (tab.width - group_width) / 2;
		int label_x = group_x + icon_size + gap;
		bool active = i == ui->nav_index;
		bool focused = active && ui->focus_region == UI_FOCUS_TOP_NAV;

		if (active) {
			render_fill_round_rect(ui->renderer, tab.x + 2, tab.y,
				tab.width - 4, tab.height, 10, focused ?
				(SDL_Color){ 72, 72, 72, 148 } :
				(SDL_Color){ 52, 52, 52, 116 });
			render_outline_round_rect(ui->renderer, tab.x + 2, tab.y,
				tab.width - 4, tab.height, 10, focused ?
				(SDL_Color){ 220, 220, 220, 162 } :
				(SDL_Color){ 177, 177, 177, 58 });
		}
		if (label_x < clip.x)
			label_x = clip.x;
		(void)SDL_RenderSetClipRect(ui->renderer, &clip);
		if (icon_size > 0)
			material_icon_draw(ui, nav_icons[i], group_x, tab.y + 5,
				icon_size, active ? accent : normal, active,
				focused);
		text_draw(ui, 0, label, label_x, tab.y + 5,
			active ? selected : normal);
		(void)SDL_RenderSetClipRect(ui->renderer, NULL);
	}
}

void render_navigation(struct ui *ui, uint32_t now)
{
	SDL_Rect section = { 0, UI_LAYOUT_STATUS_BOTTOM, UI_WIDTH,
		UI_LAYOUT_CONTENT_Y - UI_LAYOUT_STATUS_BOTTOM };
	bool current = ui->navigation_cache != NULL &&
		ui->navigation_cache_index == ui->nav_index &&
		ui->navigation_cache_focus_region == ui->focus_region &&
		ui->navigation_cache_language == ui->locale.language;

	(void)now;
	if (!current && ui->metrics.input_counter != 0U) {
		draw_navigation_contents(ui);
		return;
	}
	if (!current) {
		SDL_Texture *target = SDL_CreateTexture(ui->renderer,
			SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET,
			UI_WIDTH, UI_HEIGHT);

		if (target == NULL || SDL_SetRenderTarget(ui->renderer, target) != 0) {
			if (target != NULL)
				SDL_DestroyTexture(target);
			draw_navigation_contents(ui);
			return;
		}
		render_set_color(ui->renderer, (SDL_Color){ 0, 0, 0, 0 });
		(void)SDL_RenderClear(ui->renderer);
		draw_navigation_contents(ui);
		(void)SDL_SetRenderTarget(ui->renderer, NULL);
		(void)SDL_SetTextureBlendMode(target, SDL_BLENDMODE_BLEND);
		if (ui->navigation_cache != NULL)
			SDL_DestroyTexture(ui->navigation_cache);
		ui->navigation_cache = target;
		ui->navigation_cache_index = ui->nav_index;
		ui->navigation_cache_focus_region = ui->focus_region;
		ui->navigation_cache_language = ui->locale.language;
	}
	(void)SDL_RenderCopy(ui->renderer, ui->navigation_cache, &section, &section);
}

static void draw_cover_texture(SDL_Renderer *renderer, SDL_Texture *texture,
			       int source_width, int source_height,
			       int x, int y, int w, int h)
{
	SDL_Rect source = { 0, 0, source_width, source_height };
	SDL_Rect destination = { x, y, w, h };

	if ((long long)source_width * h > (long long)source_height * w) {
		source.w = source_height * w / h;
		source.x = (source_width - source.w) / 2;
	} else {
		source.h = source_width * h / w;
		source.y = (source_height - source.h) / 2;
	}
	(void)SDL_RenderCopy(renderer, texture, &source, &destination);
}

void render_cover(struct ui *ui, int x, int y, int w, int h,
		  size_t visible_index, bool selected)
{
	const SDL_Color edge = { 164, 164, 164, 58 };
	const SDL_Color focus = { 220, 220, 220, 178 };
	const SDL_Color primary = { 238, 238, 238, 255 };
	const struct game_entry *game = catalog_visible_game(ui, visible_index);
	size_t game_id = catalog_visible_id(ui, visible_index);
	SDL_Texture *texture;
	int texture_width = 0;
	int texture_height = 0;
	int platform_width;

	if (game == NULL || game_id == SIZE_MAX)
		return;
	texture = cover_cache_get(ui, game_id, &texture_width, &texture_height);
	if (selected)
		render_fill_round_rect(ui->renderer, x - 7, y - 7, w + 14, h + 14,
			16, (SDL_Color){ 188, 188, 188, 34 });
	render_fill_round_rect(ui->renderer, x + 4, y + 7, w, h, 13,
		(SDL_Color){ 0, 0, 0, 112 });
	render_fill_round_rect(ui->renderer, x, y, w, h, 12,
		selected ? (SDL_Color){ 46, 46, 46, 255 } :
		(SDL_Color){ 28, 28, 28, 250 });
	if (texture != NULL && texture_width > 0 && texture_height > 0) {
		if (!selected) {
			(void)SDL_SetTextureColorMod(texture, 178, 188, 202);
			(void)SDL_SetTextureAlphaMod(texture, 176);
		}
		draw_cover_texture(ui->renderer, texture, texture_width, texture_height,
				   x + 4, y + 4, w - 8, h - 8);
		if (!selected) {
			(void)SDL_SetTextureAlphaMod(texture, 255);
			(void)SDL_SetTextureColorMod(texture, 255, 255, 255);
		}
	} else {
		uint8_t tone = (uint8_t)(28U + game_id % 5U * 3U);
		SDL_Rect clip = { x + 4, y + 4, w - 8, h - 8 };

		render_fill_round_rect(ui->renderer, x + 4, y + 4, w - 8, h - 8,
			9, (SDL_Color){ tone, tone, tone, 255 });
		render_fill_round_rect(ui->renderer, x + 13, y + 17, w - 26,
			h > 70 ? h / 2 : h - 34, 8,
			(SDL_Color){ 188, 188, 188, 24 });
		platform_width = text_width(ui, 1, game->system, primary);
		(void)SDL_RenderSetClipRect(ui->renderer, &clip);
		text_draw(ui, 1, game->system, x + (w - platform_width) / 2,
			  y + h / 2 - 8, primary);
		(void)SDL_RenderSetClipRect(ui->renderer, NULL);
	}
	render_outline_round_rect(ui->renderer, x, y, w, h, 12,
		selected ? focus : edge);
}
