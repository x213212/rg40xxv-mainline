#include "ui.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static bool same_color(SDL_Color a, SDL_Color b)
{
	return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

void text_cache_collect_retired(struct ui *ui)
{
	for (size_t i = 0; i < ui->retired_texture_count; ++i)
		SDL_DestroyTexture(ui->retired_textures[i]);
	ui->retired_texture_count = 0;
}

static void retire_texture(struct ui *ui, SDL_Texture *texture)
{
	if (texture == NULL)
		return;
	if (ui->retired_texture_count == TEXT_CACHE_MAX) {
		/* Finish queued copies before reclaiming textures from this frame. */
		(void)SDL_RenderFlush(ui->renderer);
		text_cache_collect_retired(ui);
	}
	ui->retired_textures[ui->retired_texture_count++] = texture;
}

static struct text_item *text_texture(struct ui *ui, int font_index,
					      const char *text, SDL_Color color)
{
	SDL_Surface *surface;
	SDL_Texture *texture;
	struct text_item *item;

	for (size_t i = 0; i < ui->cache_count; ++i) {
		item = &ui->cache[i];
		if (item->font_index == font_index && same_color(item->color, color) &&
		    strcmp(item->text, text) == 0) {
			item->last_used = ++ui->text_tick;
			return item;
		}
	}
	surface = TTF_RenderUTF8_Blended(ui->fonts[font_index], text, color);
	if (surface == NULL)
		return NULL;
	texture = SDL_CreateTextureFromSurface(ui->renderer, surface);
	if (texture == NULL) {
		SDL_FreeSurface(surface);
		return NULL;
	}
	if (ui->cache_count < TEXT_CACHE_MAX)
		item = &ui->cache[ui->cache_count++];
	else {
		item = &ui->cache[0];
		for (size_t i = 1; i < ui->cache_count; ++i) {
			if (ui->cache[i].last_used < item->last_used)
				item = &ui->cache[i];
		}
		/* The renderer may still have a queued copy using this texture. */
		retire_texture(ui, item->texture);
	}
	memset(item, 0, sizeof(*item));
	(void)snprintf(item->text, sizeof(item->text), "%s", text);
	item->font_index = font_index;
	item->color = color;
	item->width = surface->w;
	item->height = surface->h;
	item->last_used = ++ui->text_tick;
	item->texture = texture;
	SDL_FreeSurface(surface);
	return item;
}

void text_draw(struct ui *ui, int font_index, const char *text, int x, int y,
	       SDL_Color color)
{
	struct text_item *item = text_texture(ui, font_index, text, color);
	SDL_Rect destination;

	if (item == NULL)
		return;
	destination = (SDL_Rect){ x, y, item->width, item->height };
	(void)SDL_RenderCopy(ui->renderer, item->texture, NULL, &destination);
}

int text_width(struct ui *ui, int font_index, const char *text, SDL_Color color)
{
	struct text_item *item = text_texture(ui, font_index, text, color);

	return item != NULL ? item->width : 0;
}

static size_t utf8_glyph_size(const char *text)
{
	unsigned char lead = (unsigned char)text[0];

	if (lead < 0x80U)
		return 1U;
	if ((lead & 0xe0U) == 0xc0U)
		return 2U;
	if ((lead & 0xf0U) == 0xe0U)
		return 3U;
	if ((lead & 0xf8U) == 0xf0U)
		return 4U;
	return 1U;
}

static void text_prewarm_glyphs(struct ui *ui, int font_index,
				const char *text, SDL_Color color)
{
	char glyph[5];

	while (*text != '\0') {
		size_t remaining = strlen(text);
		size_t length = utf8_glyph_size(text);

		if (length > remaining)
			length = 1U;
		memcpy(glyph, text, length);
		glyph[length] = '\0';
		(void)text_width(ui, font_index, glyph, color);
		text += length;
	}
}

void text_prewarm_visible(struct ui *ui)
{
	static const SDL_Color primary = { 238, 238, 238, 255 };
	static const SDL_Color secondary = { 157, 157, 157, 255 };
	static const SDL_Color focus = { 220, 220, 220, 255 };
	static const SDL_Color cover_text = { 234, 234, 234, 255 };
	char fallback[192];
	size_t count = ui->catalog.visible_count;
	const char *starting = tr(ui, "launch_starting");
	const char *restoring = tr(ui, "launch_restoring");

	/* Launch transitions animate cached glyph textures independently.  Warm
	 * the bounded locale strings here so the first A-to-overlay frame never
	 * performs per-character TTF rasterization. */
	text_prewarm_glyphs(ui, 0, starting, secondary);
	text_prewarm_glyphs(ui, 0, restoring, secondary);
	(void)text_width(ui, 0, tr(ui, "launch_cancelled"), secondary);

	if (count == 0)
		return;
	for (int offset = -2; offset <= 2; ++offset) {
		long long position = (long long)ui->game_index + offset;
		const struct game_entry *game;

		while (position < 0)
			position += (long long)count;
		position %= (long long)count;
		game = catalog_visible_game(ui, (size_t)position);
		if (game == NULL)
			continue;
		(void)text_width(ui, 2, game->title, primary);
		(void)text_width(ui, 1, game->system, cover_text);
		(void)text_width(ui, 0, game->system_label, focus);
		(void)text_width(ui, 0, game->system, primary);
		(void)text_width(ui, 0, game->frontend, primary);
		(void)text_width(ui, 0, game->runtime, primary);
		if (game->fallback[0] != '\0') {
			(void)snprintf(fallback, sizeof(fallback), "%s：%s",
				tr(ui, "fallback"), game->fallback);
			(void)text_width(ui, 0, fallback, secondary);
		}
	}
}

void text_cache_destroy(struct ui *ui)
{
	for (size_t i = 0; i < ui->cache_count; ++i)
		SDL_DestroyTexture(ui->cache[i].texture);
	text_cache_collect_retired(ui);
	ui->cache_count = 0;
	ui->text_tick = 0;
}
