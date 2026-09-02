#ifndef RG40XXV_YOUTUBE_HOME_VIEW_HPP
#define RG40XXV_YOUTUBE_HOME_VIEW_HPP

#include <SDL.h>
#include "sdl_ttf_compat.h"
#include "home_catalog.hpp"

namespace rg40xxv_youtube {

struct HomeViewText {
	SDL_Texture *texture = nullptr;
	int width = 0;
	int height = 0;
};

struct HomeView {
	SDL_Texture *thumbnails[kHomeCardLimit] {};
	HomeViewText titles[kHomeCardLimit] {};
	HomeViewText metadata[kHomeCardLimit] {};
	HomeViewText heading {};
	HomeViewText quality {};
	HomeViewText search {};
	HomeViewText hint {};
	HomeViewText status {};
	HomeViewText system_status {};
	HomeViewText selector_heading {};
	HomeViewText selector_items[kHomeChannelCount + 1] {};
	TTF_Font *heading_font = nullptr;
	TTF_Font *title_font = nullptr;
	TTF_Font *small_font = nullptr;
	char rendered_video_ids[kHomeCardLimit][12] {};
	char rendered_titles[kHomeCardLimit][kHomeTitleBytes] {};
	char rendered_metadata[kHomeCardLimit]
		[kHomeChannelBytes + kHomePublishedBytes + 48] {};
	char rendered_thumbnail_paths[kHomeCardLimit]
		[kHomeThumbnailPathBytes] {};
	char system_status_value[96] {};
	char status_value[128] {};
	unsigned long catalog_revision = 0;
	std::size_t query_preset = static_cast<std::size_t>(-1);
	std::size_t active_channel = static_cast<std::size_t>(-1);
	bool image_initialized = false;
};

bool home_view_init(HomeView *view, SDL_Renderer *renderer,
		    const char *font_path);
bool home_view_sync(HomeView *view, SDL_Renderer *renderer,
		    const HomeCatalog *catalog);
void home_view_draw(HomeView *view, SDL_Renderer *renderer,
		    const HomeCatalog *catalog);
bool home_view_set_system_status(HomeView *view, SDL_Renderer *renderer,
				 const char *value);
void home_view_destroy(HomeView *view);

} // namespace rg40xxv_youtube

#endif
