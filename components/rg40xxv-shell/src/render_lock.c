#include "ui.h"

#include <stdio.h>

static const SDL_Color lock_text = { 235, 235, 235, 255 };
static const SDL_Color lock_muted = { 150, 150, 150, 255 };
static const SDL_Color lock_line = { 90, 90, 90, 255 };

static void centered_text(struct ui *ui, int font, const char *value, int y,
			  SDL_Color color)
{
	text_draw(ui, font, value,
		  UI_WIDTH / 2 - text_width(ui, font, value, color) / 2, y, color);
}

static void draw_unlock_progress(struct ui *ui)
{
	char fraction[16];
	int progress = ui->power.unlock_progress;

	for (int i = 0; i < 3; ++i) {
		int x = 271 + i * 36;

		render_outline_rect(ui->renderer, x, 263, 24, 24,
				    i < progress ? lock_text : lock_line);
		if (i < progress)
			render_fill_rect(ui->renderer, x + 7, 270, 10, 10, lock_text);
	}
	(void)snprintf(fraction, sizeof(fraction), "%d / 3", progress);
	centered_text(ui, 1, fraction, 298, lock_muted);
}

void render_lock_screen(struct ui *ui, uint32_t now)
{
	char seconds[32];

	render_outline_rect(ui->renderer, 171, 125, 298, 228, lock_line);
	render_fill_rect(ui->renderer, 172, 126, 296, 1,
			 (SDL_Color){ 190, 190, 190, 130 });
	if (ui->power.view == POWER_VIEW_SHUTDOWN_COUNTDOWN) {
		uint32_t elapsed = now - ui->power.shutdown_started_at;
		unsigned int remaining = elapsed >= 3000U ? 0U :
			(3000U - elapsed + 999U) / 1000U;

		centered_text(ui, 2, tr(ui, "shutdown_countdown"), 174, lock_text);
		(void)snprintf(seconds, sizeof(seconds), "%u", remaining);
		centered_text(ui, 3, seconds, 224, lock_text);
		centered_text(ui, 0, tr(ui, "shutdown_cancel"), 304, lock_muted);
		return;
	}
	centered_text(ui, 2, tr(ui, "lock_title"), 167, lock_text);
	centered_text(ui, 0, tr(ui, "lock_instruction"), 215, lock_muted);
	draw_unlock_progress(ui);
}
