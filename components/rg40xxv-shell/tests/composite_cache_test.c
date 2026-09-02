#include "ui.h"

#include <stdio.h>
#include <stdlib.h>

enum { TEST_WIDTH = 96, TEST_HEIGHT = 64 };

static int set_target(SDL_Renderer *renderer, SDL_Texture *target,
		      SDL_Color clear)
{
	if (SDL_SetRenderTarget(renderer, target) != 0 ||
	    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND) != 0 ||
	    SDL_SetRenderDrawColor(renderer, clear.r, clear.g, clear.b,
		clear.a) != 0 || SDL_RenderClear(renderer) != 0)
		return -1;
	return 0;
}

static void draw_translucent_scene(SDL_Renderer *renderer)
{
	SDL_Rect first = { 6, 7, 72, 43 };
	SDL_Rect second = { 21, 16, 67, 39 };
	SDL_Rect edge = { 13, 11, 71, 47 };

	(void)SDL_SetRenderDrawColor(renderer, 24, 24, 24, 226);
	(void)SDL_RenderFillRect(renderer, &first);
	(void)SDL_SetRenderDrawColor(renderer, 220, 220, 220, 54);
	(void)SDL_RenderFillRect(renderer, &second);
	(void)SDL_SetRenderDrawColor(renderer, 170, 170, 170, 90);
	(void)SDL_RenderDrawRect(renderer, &edge);
	(void)SDL_SetRenderDrawColor(renderer, 238, 238, 238, 232);
	(void)SDL_RenderDrawLine(renderer, 17, 31, 82, 31);
}

static int read_target(SDL_Renderer *renderer, SDL_Texture *target,
		       uint32_t *pixels)
{
	if (SDL_SetRenderTarget(renderer, target) != 0)
		return -1;
	return SDL_RenderReadPixels(renderer, NULL, SDL_PIXELFORMAT_ARGB8888,
		pixels, TEST_WIDTH * (int)sizeof(*pixels));
}

static unsigned int channel_delta(unsigned int first, unsigned int second)
{
	return first > second ? first - second : second - first;
}

int main(void)
{
	SDL_Window *window = NULL;
	SDL_Renderer *renderer = NULL;
	SDL_Texture *direct = NULL;
	SDL_Texture *cached = NULL;
	SDL_Texture *layer = NULL;
	uint32_t *direct_pixels = NULL;
	uint32_t *cached_pixels = NULL;
	unsigned int maximum_delta = 0U;
	unsigned int differing_pixels = 0U;
	const char *stage = "SDL_Init";
	int status = 1;

	if (SDL_Init(SDL_INIT_VIDEO) != 0)
		goto done;
	stage = "SDL_CreateWindow";
	window = SDL_CreateWindow("composite-cache-test", 0, 0,
		TEST_WIDTH, TEST_HEIGHT, SDL_WINDOW_HIDDEN);
	if (window == NULL)
		goto done;
	stage = "SDL_CreateRenderer";
	renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
	if (renderer == NULL)
		goto done;
	stage = "SDL_CreateTexture";
	direct = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
		SDL_TEXTUREACCESS_TARGET, TEST_WIDTH, TEST_HEIGHT);
	cached = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
		SDL_TEXTUREACCESS_TARGET, TEST_WIDTH, TEST_HEIGHT);
	layer = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
		SDL_TEXTUREACCESS_TARGET, TEST_WIDTH, TEST_HEIGHT);
	if (direct == NULL || cached == NULL || layer == NULL)
		goto done;
	stage = "direct render";
	if (set_target(renderer, direct, (SDL_Color){ 7, 9, 13, 255 }) != 0)
		goto done;
	draw_translucent_scene(renderer);
	stage = "layer render";
	if (set_target(renderer, layer, (SDL_Color){ 7, 9, 13, 255 }) != 0)
		goto done;
	draw_translucent_scene(renderer);
	stage = "premultiplied blend";
	if (!render_set_opaque_cache_blend(layer))
		goto done;
	stage = "cached render";
	if (set_target(renderer, cached, (SDL_Color){ 7, 9, 13, 255 }) != 0 ||
	    SDL_RenderCopy(renderer, layer, NULL, NULL) != 0)
		goto done;
	stage = "readback";
	direct_pixels = calloc(TEST_WIDTH * TEST_HEIGHT,
		sizeof(*direct_pixels));
	cached_pixels = calloc(TEST_WIDTH * TEST_HEIGHT,
		sizeof(*cached_pixels));
	if (direct_pixels == NULL || cached_pixels == NULL ||
	    read_target(renderer, direct, direct_pixels) != 0 ||
	    read_target(renderer, cached, cached_pixels) != 0)
		goto done;
	for (size_t i = 0; i < TEST_WIDTH * TEST_HEIGHT; ++i) {
		uint32_t first = direct_pixels[i];
		uint32_t second = cached_pixels[i];
		unsigned int pixel_delta = 0U;

		for (unsigned int shift = 0U; shift <= 24U; shift += 8U) {
			unsigned int delta = channel_delta((first >> shift) & 0xffU,
				(second >> shift) & 0xffU);

			if (delta > pixel_delta)
				pixel_delta = delta;
		}
		if (pixel_delta != 0U)
			++differing_pixels;
		if (pixel_delta > maximum_delta)
			maximum_delta = pixel_delta;
	}
	if (maximum_delta != 0U) {
		(void)fprintf(stderr,
			"composite cache differs: pixels=%u maximum_delta=%u\n",
			differing_pixels, maximum_delta);
		goto done;
	}
	(void)printf("COMPOSITE_CACHE_TEST PASS differing_pixels=%u max_delta=%u\n",
		differing_pixels, maximum_delta);
	status = 0;

done:
	if (status != 0)
		(void)fprintf(stderr, "COMPOSITE_CACHE_TEST FAIL stage=%s error=%s\n",
			stage, SDL_GetError());
	free(cached_pixels);
	free(direct_pixels);
	if (layer != NULL)
		SDL_DestroyTexture(layer);
	if (cached != NULL)
		SDL_DestroyTexture(cached);
	if (direct != NULL)
		SDL_DestroyTexture(direct);
	if (renderer != NULL)
		SDL_DestroyRenderer(renderer);
	if (window != NULL)
		SDL_DestroyWindow(window);
	SDL_Quit();
	return status;
}
