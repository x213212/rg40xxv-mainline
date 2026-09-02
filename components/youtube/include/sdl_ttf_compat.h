#ifndef RG40XXV_YOUTUBE_SDL_TTF_COMPAT_H
#define RG40XXV_YOUTUBE_SDL_TTF_COMPAT_H

#include <SDL.h>

extern "C" {
typedef struct _TTF_Font TTF_Font;
int TTF_Init(void);
void TTF_Quit(void);
TTF_Font *TTF_OpenFont(const char *file, int ptsize);
void TTF_CloseFont(TTF_Font *font);
SDL_Surface *TTF_RenderUTF8_Blended(TTF_Font *font, const char *text,
				    SDL_Color foreground);
}

#define TTF_GetError SDL_GetError

#endif
