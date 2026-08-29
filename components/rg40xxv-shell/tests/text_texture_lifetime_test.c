#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct SDL_Renderer {
	int unused;
};

struct SDL_Texture {
	unsigned int id;
	bool queued;
	bool destroyed;
};

struct _TTF_Font {
	int unused;
};

enum {
	TEXTURE_FIXTURE_MAX = TEXT_CACHE_MAX * 3,
};

static SDL_Texture *textures[TEXTURE_FIXTURE_MAX];
static size_t texture_count;
static unsigned int flush_count;
static unsigned int destroy_count;
static unsigned int lifetime_violations;

#define CHECK(condition) do { \
	if (!(condition)) { \
		fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
			__FILE__, __LINE__, #condition); \
		return 1; \
	} \
} while (0)

SDL_Surface *TTF_RenderUTF8_Blended(TTF_Font *font, const char *text,
				    SDL_Color foreground)
{
	SDL_Surface *surface = calloc(1, sizeof(*surface));

	(void)font;
	(void)foreground;
	if (surface != NULL) {
		surface->w = (int)strlen(text) + 1;
		surface->h = 12;
	}
	return surface;
}

SDL_Texture *SDL_CreateTextureFromSurface(SDL_Renderer *renderer,
					 SDL_Surface *surface)
{
	SDL_Texture *texture;

	(void)renderer;
	(void)surface;
	if (texture_count == TEXTURE_FIXTURE_MAX)
		return NULL;
	texture = calloc(1, sizeof(*texture));
	if (texture == NULL)
		return NULL;
	texture->id = (unsigned int)texture_count + 1U;
	textures[texture_count++] = texture;
	return texture;
}

void SDL_FreeSurface(SDL_Surface *surface)
{
	free(surface);
}

int SDL_RenderCopy(SDL_Renderer *renderer, SDL_Texture *texture,
		   const SDL_Rect *source, const SDL_Rect *destination)
{
	(void)renderer;
	(void)source;
	(void)destination;
	if (texture == NULL || texture->destroyed) {
		++lifetime_violations;
		return -1;
	}
	texture->queued = true;
	return 0;
}

int SDL_RenderFlush(SDL_Renderer *renderer)
{
	(void)renderer;
	for (size_t i = 0; i < texture_count; ++i) {
		if (!textures[i]->destroyed)
			textures[i]->queued = false;
	}
	++flush_count;
	return 0;
}

void SDL_DestroyTexture(SDL_Texture *texture)
{
	if (texture == NULL)
		return;
	if (texture->destroyed) {
		++lifetime_violations;
		return;
	}
	if (texture->queued)
		++lifetime_violations;
	texture->destroyed = true;
	++destroy_count;
}

const struct game_entry *catalog_visible_game(const struct ui *ui,
					      size_t visible_index)
{
	(void)ui;
	(void)visible_index;
	return NULL;
}

const char *tr(const struct ui *ui, const char *key)
{
	(void)ui;
	return key;
}

static void draw_unique(struct ui *ui, const char *prefix, size_t index)
{
	char text[64];

	(void)snprintf(text, sizeof(text), "%s-%zu", prefix, index);
	text_draw(ui, 0, text, 0, 0, (SDL_Color){ 255, 255, 255, 255 });
}

int main(void)
{
	struct SDL_Renderer renderer = { 0 };
	struct _TTF_Font font = { 0 };
	struct ui ui = { 0 };
	unsigned int flushes_before_saturation;

	ui.renderer = &renderer;
	ui.fonts[0] = &font;

	/* Fill the cache with textures referenced by this unpresented frame. */
	for (size_t i = 0; i < TEXT_CACHE_MAX; ++i)
		draw_unique(&ui, "base", i);
	CHECK(ui.cache_count == TEXT_CACHE_MAX);
	CHECK(destroy_count == 0);

	/* LRU eviction must retire, not destroy, its still-queued texture. */
	draw_unique(&ui, "first-overflow", 0);
	CHECK(ui.retired_texture_count == 1);
	CHECK(destroy_count == 0);
	CHECK(lifetime_violations == 0);

	/* Model SDL_RenderPresent, then collect resources from the old frame. */
	CHECK(SDL_RenderFlush(ui.renderer) == 0);
	text_cache_collect_retired(&ui);
	CHECK(ui.retired_texture_count == 0);
	CHECK(destroy_count == 1);
	CHECK(lifetime_violations == 0);

	/* Queue every live cache entry again before forcing a full retire queue. */
	for (size_t i = 0; i < ui.cache_count; ++i)
		text_draw(&ui, ui.cache[i].font_index, ui.cache[i].text, 0, 0,
			  ui.cache[i].color);
	for (size_t i = 0; i < TEXT_CACHE_MAX; ++i)
		draw_unique(&ui, "saturation", i);
	CHECK(ui.retired_texture_count == TEXT_CACHE_MAX);
	CHECK(lifetime_violations == 0);

	/* The next eviction must flush before reclaiming the saturated queue. */
	flushes_before_saturation = flush_count;
	draw_unique(&ui, "saturation-overflow", 0);
	CHECK(flush_count == flushes_before_saturation + 1U);
	CHECK(ui.retired_texture_count == 1);
	CHECK(destroy_count == 1U + TEXT_CACHE_MAX);
	CHECK(lifetime_violations == 0);

	/* Finish the final frame and then model renderer teardown. */
	CHECK(SDL_RenderFlush(ui.renderer) == 0);
	text_cache_collect_retired(&ui);
	CHECK(SDL_RenderFlush(ui.renderer) == 0);
	text_cache_destroy(&ui);
	CHECK(ui.cache_count == 0);
	CHECK(ui.retired_texture_count == 0);
	CHECK(destroy_count == texture_count);
	CHECK(lifetime_violations == 0);

	for (size_t i = 0; i < texture_count; ++i)
		free(textures[i]);
	printf("TEXT_TEXTURE_LIFETIME_TEST PASS textures=%zu flushes=%u destroys=%u\n",
	       texture_count, flush_count, destroy_count);
	return 0;
}
