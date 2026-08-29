#ifndef RG40XXV_SDL_TTF_COMPAT_H
#define RG40XXV_SDL_TTF_COMPAT_H

#include <SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _TTF_Font TTF_Font;

int TTF_Init(void);
void TTF_Quit(void);
TTF_Font *TTF_OpenFont(const char *file, int ptsize);
void TTF_CloseFont(TTF_Font *font);
SDL_Surface *TTF_RenderUTF8_Blended(TTF_Font *font, const char *text,
				    SDL_Color foreground);
void TTF_SetFontHinting(TTF_Font *font, int hinting);
void TTF_SetFontKerning(TTF_Font *font, int allowed);

enum {
	TTF_HINTING_NORMAL = 0,
	TTF_HINTING_LIGHT = 1,
};

#define TTF_GetError SDL_GetError

#ifdef __cplusplus
}
#endif

#endif
