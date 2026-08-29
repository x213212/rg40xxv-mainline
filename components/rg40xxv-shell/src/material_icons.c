#include "ui.h"

#include <stdio.h>
#include <string.h>

enum {
	MATERIAL_ICON_CELL = 32,
	MATERIAL_ICON_VARIANTS = 2,
};

int material_icons_load(struct ui *ui, const char *path)
{
	SDL_Surface *surface;
	SDL_Texture *texture;
	char saved_path[PATH_MAX];

	if (path == NULL)
		return -1;
	(void)snprintf(saved_path, sizeof(saved_path), "%s", path);
	surface = IMG_Load(saved_path);
	if (surface == NULL || surface->w != MATERIAL_ICON_COUNT *
		MATERIAL_ICON_CELL || surface->h != MATERIAL_ICON_VARIANTS *
		MATERIAL_ICON_CELL) {
		if (surface != NULL)
			SDL_FreeSurface(surface);
		return -1;
	}
	texture = SDL_CreateTextureFromSurface(ui->renderer, surface);
	SDL_FreeSurface(surface);
	if (texture == NULL)
		return -1;
	(void)SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
	if (ui->icon_atlas != NULL)
		SDL_DestroyTexture(ui->icon_atlas);
	ui->icon_atlas = texture;
	(void)snprintf(ui->icon_atlas_path, sizeof(ui->icon_atlas_path), "%s",
		saved_path);
	return 0;
}

const char *material_icons_find(const char *requested)
{
	static const char *const candidates[] = {
		"/run/rg40xxv-ui/RG40XXV-Material-Icons.png",
		"/usr/share/rg40xxv-ui/RG40XXV-Material-Icons.png",
		"assets/RG40XXV-Material-Icons.png",
	};
	FILE *stream;

	if (requested != NULL)
		return requested;
	for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
		stream = fopen(candidates[i], "r");
		if (stream != NULL) {
			(void)fclose(stream);
			return candidates[i];
		}
	}
	return NULL;
}

void material_icon_draw(struct ui *ui, enum material_icon_id icon,
			int x, int y, int size, SDL_Color color, bool filled,
			bool glow)
{
	SDL_Rect source;
	SDL_Rect destination = { x, y, size, size };
	uint8_t alpha = color.a;

	if (ui->icon_atlas == NULL || icon < 0 || icon >= MATERIAL_ICON_COUNT)
		return;
	source = (SDL_Rect) {
		.x = icon * MATERIAL_ICON_CELL,
		.y = filled ? MATERIAL_ICON_CELL : 0,
		.w = MATERIAL_ICON_CELL,
		.h = MATERIAL_ICON_CELL,
	};
	(void)SDL_SetTextureColorMod(ui->icon_atlas, color.r, color.g, color.b);
	if (glow) {
		SDL_Rect halo = { x - 2, y - 2, size + 4, size + 4 };

		(void)SDL_SetTextureAlphaMod(ui->icon_atlas,
			(uint8_t)((unsigned int)alpha * 28U / 100U));
		(void)SDL_RenderCopy(ui->renderer, ui->icon_atlas, &source, &halo);
	}
	(void)SDL_SetTextureAlphaMod(ui->icon_atlas, alpha);
	(void)SDL_RenderCopy(ui->renderer, ui->icon_atlas, &source,
		&destination);
	(void)SDL_SetTextureAlphaMod(ui->icon_atlas, 255);
	(void)SDL_SetTextureColorMod(ui->icon_atlas, 255, 255, 255);
}
