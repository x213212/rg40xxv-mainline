#ifndef RG40XXV_SDL_IMAGE_COMPAT_H
#define RG40XXV_SDL_IMAGE_COMPAT_H

#include <SDL.h>

enum {
	IMG_INIT_JPG = 0x00000001,
	IMG_INIT_PNG = 0x00000002,
	IMG_INIT_TIF = 0x00000004,
	IMG_INIT_WEBP = 0x00000008,
};

int IMG_Init(int flags);
void IMG_Quit(void);
SDL_Surface *IMG_Load(const char *file);
SDL_Surface *IMG_Load_RW(SDL_RWops *source, int free_source);

#endif
