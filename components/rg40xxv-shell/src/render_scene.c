#include "ui.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const SDL_Color primary = { 238, 238, 238, 255 };
static const SDL_Color secondary = { 157, 157, 157, 255 };
static const SDL_Color focus = { 220, 220, 220, 255 };
static const SDL_Color line = { 164, 164, 164, 42 };

static void draw_transition_text(struct ui *ui, int font, const char *value,
				 int y, SDL_Color color)
{
	const int left = 96;
	const int width = UI_WIDTH - left * 2;
	SDL_Rect clip = { left, y, width, 40 };
	int text_x = UI_WIDTH / 2 - text_width(ui, font, value, color) / 2;

	if (text_x < left)
		text_x = left;
	(void)SDL_RenderSetClipRect(ui->renderer, &clip);
	text_draw(ui, font, value, text_x, y, color);
	(void)SDL_RenderSetClipRect(ui->renderer, NULL);
}

static void draw_transition_title(struct ui *ui, const char *value, int y)
{
	const int left = 96;
	const int width = UI_WIDTH - left * 2;
	char fitted[TEXT_VALUE_MAX];
	size_t length;
	bool truncated;

	(void)snprintf(fitted, sizeof(fitted), "%s", value);
	length = strlen(fitted);
	truncated = text_width(ui, 2, fitted, focus) > width;
	while (truncated && length > 0U) {
		do {
			--length;
		} while (length > 0U &&
			 ((unsigned char)fitted[length] & 0xc0U) == 0x80U);
		fitted[length] = '\0';
		if (length + strlen("…") < sizeof(fitted))
			(void)strcat(fitted, "…");
		if (text_width(ui, 2, fitted, focus) <= width)
			break;
		fitted[length] = '\0';
	}
	draw_transition_text(ui, 2, fitted, y, focus);
}

static void draw_transition_wave(struct ui *ui, int y, uint32_t now)
{
	SDL_Point points[49];
	const int left = 128;
	const int step = 8;
	const double phase = (double)now / 115.0;

	for (size_t i = 0; i < sizeof(points) / sizeof(points[0]); ++i) {
		double envelope = sin((double)i * 3.141592653589793 / 48.0);

		points[i].x = left + (int)i * step;
		points[i].y = y + (int)lrint(sin(phase + (double)i * 0.48) *
					      7.0 * envelope);
	}
	render_set_color(ui->renderer, (SDL_Color){ 220, 220, 220, 218 });
	(void)SDL_RenderDrawLines(ui->renderer, points,
				  (int)(sizeof(points) / sizeof(points[0])));
	render_fill_rect(ui->renderer, left, y + 15, 384, 1,
			 (SDL_Color){ 220, 220, 220, 32 });
}

static void draw_launch_error(struct ui *ui)
{
	const int x = 82;
	const int y = 126;
	const int width = 476;
	const int height = ui->launch.diagnostics_expanded ? 230 : 204;

	render_fill_rect(ui->renderer, 0, 36, UI_WIDTH, UI_HEIGHT - 72,
		(SDL_Color){ 5, 5, 5, 176 });
	render_fill_round_rect(ui->renderer, x + 4, y + 8, width, height, 22,
		(SDL_Color){ 0, 0, 0, 94 });
	render_fill_round_rect(ui->renderer, x, y, width, height, 22,
		(SDL_Color){ 28, 28, 28, 246 });
	render_outline_round_rect(ui->renderer, x, y, width, height, 22,
		(SDL_Color){ 226, 164, 157, 112 });
	text_draw(ui, 2, tr(ui, "launch_error_title"), x + 28, y + 24,
		(SDL_Color){ 247, 224, 220, 255 });
	draw_transition_text(ui, 1, ui->launch.game_title, y + 63, primary);
	draw_transition_text(ui, 0, ui->launch.transition_detail, y + 94,
		secondary);
	if (ui->launch.diagnostics_expanded)
		draw_transition_text(ui, 0, ui->launch.diagnostics, y + 127,
			(SDL_Color){ 189, 199, 211, 255 });
	draw_transition_text(ui, 0,
		tr(ui, ui->launch.diagnostics_expanded ?
			"launch_hide_diagnostics" : "launch_show_diagnostics"),
		y + height - 35, secondary);
}

static void draw_launch_transition(struct ui *ui, uint32_t now, bool full)
{
	const char *title = ui->launch.transition == LAUNCH_TRANSITION_STARTING ?
		tr(ui, "launch_starting") : tr(ui, "launch_restoring");
	int panel_y = full ? 124 : 145;

	if (ui->launch.transition == LAUNCH_TRANSITION_ERROR) {
		draw_launch_error(ui);
		return;
	}

	if (full) {
		for (int y = 0; y < UI_HEIGHT; y += 4) {
			uint8_t shade = (uint8_t)(10 + y * 14 / UI_HEIGHT);

			render_fill_rect(ui->renderer, 0, y, UI_WIDTH, 4,
				(SDL_Color){ shade, shade, shade, 255 });
		}
		render_fill_round_rect(ui->renderer, 72, 78, 496, 300, 96,
			(SDL_Color){ 188, 188, 188, 18 });
	} else {
		render_fill_rect(ui->renderer, 0, 36, UI_WIDTH, UI_HEIGHT - 72,
				 (SDL_Color){ 5, 5, 5, 170 });
	}
	render_fill_round_rect(ui->renderer, 87, panel_y + 8, 466, 220, 24,
		(SDL_Color){ 0, 0, 0, 92 });
	render_fill_round_rect(ui->renderer, 87, panel_y, 466, 220, 24,
		(SDL_Color){ 24, 24, 24, 240 });
	render_outline_round_rect(ui->renderer, 87, panel_y, 466, 220, 24,
		(SDL_Color){ 180, 180, 180, 76 });
	material_icon_draw(ui, MATERIAL_ICON_PLAY, 302, panel_y + 19, 36,
		(SDL_Color){ 220, 220, 220, 255 }, true, true);
	draw_transition_text(ui, 0, title, panel_y + 61, secondary);
	draw_transition_title(ui, ui->launch.game_title, panel_y + 91);
	draw_transition_wave(ui, panel_y + 151, now);
}

static void draw_chip(struct ui *ui, int x, int y, int w, int h, bool active)
{
	render_fill_round_rect(ui->renderer, x, y, w, h, 10,
			 active ? (SDL_Color){ 29, 29, 29, 242 } :
			 (SDL_Color){ 25, 25, 25, 210 });
	render_outline_round_rect(ui->renderer, x, y, w, h, 10,
		active ? (SDL_Color){ 220, 220, 220, 180 } :
		(SDL_Color){ 164, 164, 164, 58 });
}

static void draw_clipped_text(struct ui *ui, int font, const char *text,
			      int x, int y, int w, SDL_Color color)
{
	SDL_Rect clip = { x, y, w, 26 };

	(void)SDL_RenderSetClipRect(ui->renderer, &clip);
	text_draw(ui, font, text, x, y, color);
	(void)SDL_RenderSetClipRect(ui->renderer, NULL);
}

static size_t carousel_position(size_t selected, int offset, size_t count)
{
	long long position = (long long)selected + offset;

	while (position < 0)
		position += (long long)count;
	return (size_t)(position % (long long)count);
}

static void update_carousel(struct ui *ui, uint32_t now)
{
	if (ui->carousel_position == ui->carousel_target)
		return;
	double progress = (double)(now - ui->carousel_started) / 120.0;

	if (progress >= 1.0)
		ui->carousel_position = ui->carousel_target;
	else {
		double inverse = 1.0 - fmax(0.0, progress);
		double eased = 1.0 - inverse * inverse * inverse;

		ui->carousel_position = ui->carousel_from +
			(ui->carousel_target - ui->carousel_from) * eased;
	}
}

static void draw_empty_state(struct ui *ui)
{
	const char *title;
	const char *hint;

	if (ui->catalog.apps_only) {
		title = tr(ui, "no_apps");
		hint = tr(ui, "check_apps_root");
	} else {
		title = ui->catalog.game_count == 0 ?
			tr(ui, "no_rom") : tr(ui, "no_match");
		hint = ui->catalog.game_count == 0 ?
			tr(ui, "check_rom_root") : tr(ui, "adjust_search");
	}

	text_draw(ui, 2, title, UI_WIDTH / 2 -
		  text_width(ui, 2, title, primary) / 2, 214, primary);
	text_draw(ui, 0, hint, UI_WIDTH / 2 -
		  text_width(ui, 0, hint, secondary) / 2, 248, secondary);
}

static void draw_library(struct ui *ui, uint32_t now)
{
	struct catalog_state *catalog = &ui->catalog;
	const struct game_entry *selected;
	char text[192];
	size_t count = catalog->visible_count;
	const char *page_title = ui->nav_index == NAV_PAGE_RECENT ?
		tr(ui, "nav_recent") :
		ui->nav_index == NAV_PAGE_FAVORITES ? tr(ui, "nav_favorites") :
		catalog->apps_only ? tr(ui, "apps_title") : tr(ui, "library");
	bool content_focused = ui->focus_region == UI_FOCUS_CONTENT;

	render_glass_panel(ui->renderer, UI_LAYOUT_CONTENT_X, UI_LAYOUT_CONTENT_Y,
		UI_LAYOUT_CONTENT_WIDTH, UI_LAYOUT_CONTENT_HEIGHT, content_focused);
	text_draw(ui, 1, page_title, 28, 96,
		content_focused ? primary : secondary);
	if (catalog->apps_only) {
		draw_clipped_text(ui, 0, tr(ui, "apps_categories"), 414, 99,
			198, secondary);
	} else {
		draw_chip(ui, 352, 93, 260, 36, ui->search_active);
		(void)snprintf(text, sizeof(text), "%s%s",
			       ui->search_active ? tr(ui, "search_prefix") :
			       tr(ui, "search_placeholder"),
			       ui->search_active ? catalog->query : "");
		draw_clipped_text(ui, 0, text, 364, 101, 236,
				  ui->search_active ? primary : secondary);
	}
	render_fill_rect(ui->renderer, 27, 139, 586, 1, line);
	if (count == 0) {
		draw_empty_state(ui);
		return;
	}
	update_carousel(ui, now);
	int first_offset = count >= VISIBLE_COVER_COUNT ? -2 :
		-(int)((count - 1) / 2);
	int last_offset = count >= VISIBLE_COVER_COUNT ? 2 :
		(int)(count / 2);
	for (int offset = first_offset; offset <= last_offset; ++offset) {
		size_t position = carousel_position(ui->game_index, offset, count);
		double distance = (double)position - ui->carousel_position;
		struct ui_layout_cover cover;

		while (distance <= -(double)count / 2.0)
			distance += (double)count;
		while (distance > (double)count / 2.0)
			distance -= (double)count;
		cover = ui_layout_cover_at(distance);
		render_cover(ui, cover.x, cover.y, cover.width, cover.height,
			position, content_focused && fabs(distance) < 0.34);
	}
	selected = catalog_visible_game(ui, ui->game_index);
	render_fill_round_rect(ui->renderer, UI_LAYOUT_LIBRARY_INFO_X + 2,
		UI_LAYOUT_LIBRARY_INFO_Y + 4, UI_LAYOUT_LIBRARY_INFO_WIDTH,
		UI_LAYOUT_LIBRARY_INFO_HEIGHT, 10,
		(SDL_Color){ 0, 0, 0, 58 });
	render_fill_round_rect(ui->renderer, UI_LAYOUT_LIBRARY_INFO_X,
		UI_LAYOUT_LIBRARY_INFO_Y, UI_LAYOUT_LIBRARY_INFO_WIDTH,
		UI_LAYOUT_LIBRARY_INFO_HEIGHT, 10,
			 (SDL_Color){ 24, 24, 24, 196 });
	draw_clipped_text(ui, 1, selected->title,
		UI_LAYOUT_LIBRARY_INFO_X + 10, UI_LAYOUT_LIBRARY_INFO_Y + 1,
		UI_LAYOUT_LIBRARY_INFO_WIDTH - 20, primary);
	draw_clipped_text(ui, 0, selected->system_label,
		UI_LAYOUT_LIBRARY_INFO_X + 10, UI_LAYOUT_LIBRARY_INFO_Y + 20,
		UI_LAYOUT_LIBRARY_INFO_WIDTH - 20, secondary);
}

static void draw_quick_menu(struct ui *ui)
{
	char label[96];

	render_fill_rect(ui->renderer, 0, 36, UI_WIDTH, UI_HEIGHT - 36,
			 (SDL_Color){ 0, 0, 0, 165 });
	render_glass_panel(ui->renderer, 350, 45, 278, 382, true);
	text_draw(ui, 2, tr(ui, "quick_title"), 371, 63, primary);
	text_draw(ui, 0, tr(ui, "quick_subtitle"), 371, 92,
		  secondary);
	for (int i = 0; i < QUICK_MENU_COUNT; ++i) {
		int y = 121 + i * 45;
		bool active = i == ui->quick_menu_index;

		catalog_filter_text(ui, i, label, sizeof(label));
		if (active) {
			render_fill_rect(ui->renderer, 367, y - 6, 241, 42,
					 (SDL_Color){ 29, 29, 29, 245 });
			render_outline_rect(ui->renderer, 367, y - 6, 241, 42, focus);
			render_fill_rect(ui->renderer, 368, y + 2, 2, 25, focus);
		}
		draw_clipped_text(ui, 0, label, 381, y, 216, primary);
	}
	text_draw(ui, 0, tr(ui, "quick_controls"), 400, 394, secondary);
}

static void draw_button_hint(struct ui *ui, int x, const char *button,
			     const char *label)
{
	render_fill_round_rect(ui->renderer, x, 443, 28, 28, 9,
			 (SDL_Color){ 27, 27, 27, 238 });
	render_outline_round_rect(ui->renderer, x, 443, 28, 28, 9,
			    (SDL_Color){ 170, 170, 170, 90 });
	text_draw(ui, 1, button, x + 7, 446, primary);
	text_draw(ui, 0, label, x + 36, 448, secondary);
}

static void draw_controls(struct ui *ui)
{
	render_fill_rect(ui->renderer, 20, 435, UI_WIDTH - 40, 1,
			 (SDL_Color){ 160, 160, 160, 42 });
	if (!ui->search_active && ui->focus_region == UI_FOCUS_TOP_NAV) {
		draw_button_hint(ui, 68, "<>", tr(ui, "switch_tab"));
		draw_button_hint(ui, 278, "Av", tr(ui, "enter_content"));
		draw_button_hint(ui, 488, "B", tr(ui, "back"));
		return;
	}
	if (!ui->search_active && ui->nav_index == NAV_PAGE_SETTINGS) {
		draw_button_hint(ui, 14, "^v", tr(ui, "select"));
		draw_button_hint(ui, 170, "<>", tr(ui, "adjust"));
		draw_button_hint(ui, 326, "A", tr(ui, "change"));
		draw_button_hint(ui, 482, "B", tr(ui, "top_nav"));
		return;
	}
	if (!ui->search_active && ui->nav_index == NAV_PAGE_STREAMING) {
		draw_button_hint(ui, 14, "<", tr(ui, "previous"));
		draw_button_hint(ui, 170, ">", tr(ui, "next"));
		draw_button_hint(ui, 326, "A", tr(ui, "status"));
		draw_button_hint(ui, 482, "B", tr(ui, "top_nav"));
		return;
	}
	if (!ui->search_active && ui->nav_index == NAV_PAGE_APPS) {
		draw_button_hint(ui, 170, "A", tr(ui, "launch"));
		draw_button_hint(ui, 326, "B", tr(ui, "top_nav"));
		return;
	}
	if (!ui->search_active && ui->nav_index == NAV_PAGE_NETWORK) {
		if (ui->bluetooth.open) {
			draw_button_hint(ui, 14, "^v", tr(ui, "select"));
			draw_button_hint(ui, 170, "A", tr(ui, "bluetooth_action"));
			draw_button_hint(ui, 326, "Y", tr(ui, "bluetooth_forget"));
			draw_button_hint(ui, 482, "B", tr(ui, "back"));
		} else {
			draw_button_hint(ui, 92, "<>", tr(ui, "select"));
			draw_button_hint(ui, 292, "A", tr(ui, "enter"));
			draw_button_hint(ui, 492, "B", tr(ui, "top_nav"));
		}
		return;
	}
	draw_button_hint(ui, 14, "A", tr(ui, ui->search_active ? "enter" :
			 ui->nav_index == NAV_PAGE_SETTINGS ? "change" : "launch"));
	draw_button_hint(ui, 170, "B", tr(ui, ui->search_active ? "close" : "top_nav"));
	draw_button_hint(ui, 326, "X", tr(ui, ui->search_active ? "shift" : "search"));
	draw_button_hint(ui, 482, "Y", tr(ui, ui->search_active ? "layout" : "filter"));
}

static void render_controls(struct ui *ui)
{
	SDL_Rect section = { 0, 435, UI_WIDTH, UI_HEIGHT - 435 };
	int mode = ui->search_active ? 1 : ui->focus_region == UI_FOCUS_TOP_NAV ? 10 :
		ui->nav_index == NAV_PAGE_SETTINGS ? 2 :
		ui->nav_index == NAV_PAGE_STREAMING ? 3 :
		ui->nav_index == NAV_PAGE_APPS ? 4 :
		ui->nav_index == NAV_PAGE_NETWORK ?
			(ui->bluetooth.open ? 6 : 5) : 0;
	bool current = ui->controls_cache != NULL &&
		ui->controls_cache_mode == mode &&
		ui->controls_cache_language == ui->locale.language;

	if (!current && ui->metrics.input_counter != 0U) {
		draw_controls(ui);
		return;
	}
	if (!current) {
		SDL_Texture *target = SDL_CreateTexture(ui->renderer,
			SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET,
			UI_WIDTH, UI_HEIGHT);

		if (target == NULL || SDL_SetRenderTarget(ui->renderer, target) != 0) {
			if (target != NULL)
				SDL_DestroyTexture(target);
			draw_controls(ui);
			return;
		}
		render_set_color(ui->renderer, (SDL_Color){ 0, 0, 0, 0 });
		(void)SDL_RenderClear(ui->renderer);
		draw_controls(ui);
		(void)SDL_SetRenderTarget(ui->renderer, NULL);
		(void)SDL_SetTextureBlendMode(target, SDL_BLENDMODE_BLEND);
		if (ui->controls_cache != NULL)
			SDL_DestroyTexture(ui->controls_cache);
		ui->controls_cache = target;
		ui->controls_cache_mode = mode;
		ui->controls_cache_language = ui->locale.language;
	}
	(void)SDL_RenderCopy(ui->renderer, ui->controls_cache, &section, &section);
}

static void draw_action_osd(struct ui *ui, uint32_t now)
{
	uint32_t remaining;
	uint8_t alpha;
	SDL_Rect clip = { 120, 389, 400, 24 };
	int measured;
	int x;

	if (ui->action_text == NULL || SDL_TICKS_PASSED(now, ui->action_until))
		return;
	remaining = ui->action_until - now;
	alpha = remaining < 420U ? (uint8_t)(remaining * 255U / 420U) : 255U;
	render_fill_round_rect(ui->renderer, 105, 382, 430, 40, 14,
		(SDL_Color){ 8, 8, 8, (uint8_t)(190U * alpha / 255U) });
	render_outline_round_rect(ui->renderer, 105, 382, 430, 40, 14,
		(SDL_Color){ 170, 170, 170, (uint8_t)(90U * alpha / 255U) });
	material_icon_draw(ui, MATERIAL_ICON_TUNE, 119, 391, 20,
		(SDL_Color){ 220, 220, 220, alpha }, true, false);
	measured = text_width(ui, 0, ui->action_text,
		(SDL_Color){ 232, 232, 232, alpha });
	x = 150 + (370 - measured) / 2;
	if (x < 150)
		x = 150;
	(void)SDL_RenderSetClipRect(ui->renderer, &clip);
	text_draw(ui, 0, ui->action_text, x, 390,
		(SDL_Color){ 232, 232, 232, alpha });
	(void)SDL_RenderSetClipRect(ui->renderer, NULL);
}

void render_scene(struct ui *ui, uint32_t now)
{
	if (ui->launch.transition == LAUNCH_TRANSITION_STARTING) {
		draw_launch_transition(ui, now, true);
		SDL_RenderPresent(ui->renderer);
		text_cache_collect_retired(ui);
		launch_transition_presented(ui);
		return;
	}
	render_set_color(ui->renderer, (SDL_Color){ 5, 5, 5, 255 });
	(void)SDL_RenderClear(ui->renderer);
	if (ui->nav_index <= NAV_PAGE_FAVORITES ||
	    ui->nav_index == NAV_PAGE_APPS)
		cover_cache_sync_visible(ui);
	render_backdrop(ui);
	if (ui->power.view == POWER_VIEW_LOCKED ||
	    ui->power.view == POWER_VIEW_SHUTDOWN_COUNTDOWN) {
		render_status(ui);
		render_lock_screen(ui, now);
		SDL_RenderPresent(ui->renderer);
		text_cache_collect_retired(ui);
		return;
	}
	render_status(ui);
	render_navigation(ui, now);
	if (ui->nav_index == NAV_PAGE_SETTINGS)
		render_system_info(ui, now);
	else if (ui->nav_index == NAV_PAGE_NETWORK) {
		if (ui->bluetooth.open)
			render_bluetooth_page(ui, now);
		else
			render_network_page(ui, now);
	}
	else if (ui->nav_index == NAV_PAGE_STREAMING)
		render_stream_page(ui, now);
	else {
		if (ui->metrics.input_counter == 0U)
			text_prewarm_visible(ui);
		draw_library(ui, now);
	}
	if (ui->search_active && ui->nav_index != NAV_PAGE_SETTINGS &&
	    ui->nav_index != NAV_PAGE_NETWORK &&
	    ui->nav_index != NAV_PAGE_APPS)
		keyboard_render(ui);
	else if (ui->quick_menu_open)
		draw_quick_menu(ui);
	render_controls(ui);
	draw_action_osd(ui, now);
	if (ui->launch.transition == LAUNCH_TRANSITION_RETURNED ||
	    ui->launch.transition == LAUNCH_TRANSITION_ERROR)
		draw_launch_transition(ui, now, false);
	SDL_RenderPresent(ui->renderer);
	text_cache_collect_retired(ui);
	launch_transition_presented(ui);
}

int render_save_screenshot(struct ui *ui, const char *path)
{
	SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(
		0, UI_WIDTH, UI_HEIGHT, 32, SDL_PIXELFORMAT_ARGB8888);
	int result;

	if (surface == NULL)
		return -1;
	result = SDL_RenderReadPixels(ui->renderer, NULL, SDL_PIXELFORMAT_ARGB8888,
				     surface->pixels, surface->pitch);
	if (result == 0)
		result = SDL_SaveBMP(surface, path);
	SDL_FreeSurface(surface);
	return result;
}

void render_activate(struct ui *ui, const char *message, uint32_t now)
{
	ui->action_text = message;
	ui->action_until = now + 1800U;
}
