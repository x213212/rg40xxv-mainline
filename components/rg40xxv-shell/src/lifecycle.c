#include "ui.h"

#include <stdio.h>

static const int font_sizes[FONT_COUNT] = { 14, 17, 22, 30 };

int lifecycle_graphics_init(struct ui *ui, const char *font_path,
			    bool windowed)
{
	uint32_t flags = SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI;

	if (!windowed)
		flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
	ui->window = SDL_CreateWindow("RG40XX V UI", SDL_WINDOWPOS_CENTERED,
		SDL_WINDOWPOS_CENTERED, UI_WIDTH, UI_HEIGHT, flags);
	if (ui->window == NULL)
		return -1;
	ui->renderer = SDL_CreateRenderer(ui->window, -1,
		SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
	if (ui->renderer == NULL)
		ui->renderer = SDL_CreateRenderer(ui->window, -1,
					  SDL_RENDERER_SOFTWARE);
	if (ui->renderer == NULL)
		return -1;
	(void)SDL_SetRenderDrawColor(ui->renderer, 0, 0, 0, 255);
	(void)SDL_RenderClear(ui->renderer);
	SDL_RenderPresent(ui->renderer);
	if (SDL_RenderSetLogicalSize(ui->renderer, UI_WIDTH, UI_HEIGHT) != 0)
		return -1;
	(void)SDL_SetRenderDrawBlendMode(ui->renderer, SDL_BLENDMODE_BLEND);
	for (int i = 0; i < FONT_COUNT; ++i) {
		ui->fonts[i] = TTF_OpenFont(font_path, font_sizes[i]);
		if (ui->fonts[i] == NULL)
			return -1;
		TTF_SetFontHinting(ui->fonts[i], TTF_HINTING_NORMAL);
		TTF_SetFontKerning(ui->fonts[i], 1);
	}
	return 0;
}

const char *render_find_font(const char *requested)
{
	static const char *const candidates[] = {
		"/run/rg40xxv-ui/RG40XXV-UI-Sans.otf",
		"/mnt/vendor/bin/default.ttf",
		"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
		"/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
	};
	FILE *stream;

	if (requested != NULL)
		return requested;
	for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
		stream = fopen(candidates[i], "r");
		if (stream != NULL) {
			fclose(stream);
			return candidates[i];
		}
	}
	return NULL;
}

void render_destroy(struct ui *ui)
{
	/*
	 * SDL may batch RenderCopy commands until Present/Flush.  This teardown is
	 * also used when suspending the resident UI for a game, so submit every
	 * queued copy before releasing any renderer-owned texture.
	 */
	if (ui->renderer != NULL)
		(void)SDL_RenderFlush(ui->renderer);
	if (ui->icon_atlas != NULL)
		SDL_DestroyTexture(ui->icon_atlas);
	if (ui->navigation_cache != NULL)
		SDL_DestroyTexture(ui->navigation_cache);
	if (ui->controls_cache != NULL)
		SDL_DestroyTexture(ui->controls_cache);
	ui->navigation_cache = NULL;
	ui->controls_cache = NULL;
	ui->icon_atlas = NULL;
	text_cache_destroy(ui);
	for (int i = 0; i < FONT_COUNT; ++i) {
		if (ui->fonts[i] != NULL) {
			TTF_CloseFont(ui->fonts[i]);
			ui->fonts[i] = NULL;
		}
	}
}

void lifecycle_session_suspend(struct ui *ui)
{
	if (ui->launch.session_suspended)
		return;
	persistence_stop(ui);
	monitor_stop(ui);
	cover_cache_destroy(ui);
	audio_close(ui);
	input_close(ui);
	render_destroy(ui);
	if (ui->renderer != NULL)
		SDL_DestroyRenderer(ui->renderer);
	if (ui->window != NULL)
		SDL_DestroyWindow(ui->window);
	ui->renderer = NULL;
	ui->window = NULL;
	IMG_Quit();
	TTF_Quit();
	SDL_QuitSubSystem(SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER |
			      SDL_INIT_JOYSTICK | SDL_INIT_VIDEO);
	ui->launch.session_suspended = true;
}

int lifecycle_session_resume(struct ui *ui)
{
	if (!ui->launch.session_suspended)
		return 0;
	if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0)
		return -1;
	if (TTF_Init() != 0)
		return -1;
	(void)SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
	if (lifecycle_graphics_init(ui, ui->launch.font_path,
				    ui->launch.windowed) != 0)
		return -1;
	if ((IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG) & IMG_INIT_PNG) == 0)
		return -1;
	if (ui->icon_atlas_path[0] != '\0' &&
	    material_icons_load(ui, ui->icon_atlas_path) != 0)
		return -1;
	/* Discard the child's stale SDL queue before snapshotting held controls. */
	(void)SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
	(void)hardware_refresh(&ui->hardware_backend, &ui->hardware, 1);
	if (monitor_start(ui) != 0 || persistence_start(ui) != 0)
		return -1;
	audio_init(ui);
	input_init(ui);
	if (cover_cache_init(ui) != 0)
		return -1;
	text_prewarm_visible(ui);
	ui->metrics.last_present = 0U;
	ui->metrics.input_counter = 0U;
	ui->launch.session_suspended = false;
	return 0;
}
