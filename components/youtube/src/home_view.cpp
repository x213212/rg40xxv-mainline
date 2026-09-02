#include "home_view.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

extern "C" {
int IMG_Init(int flags);
void IMG_Quit(void);
SDL_Surface *IMG_Load(const char *file);
}

namespace rg40xxv_youtube {
namespace {

constexpr int kImageJpg = 0x1;
constexpr SDL_Color kWhite {244, 245, 248, 255};
constexpr SDL_Color kLight {194, 198, 208, 255};

void destroy_text(HomeViewText *text)
{
	if (text->texture != nullptr)
		SDL_DestroyTexture(text->texture);
	*text = HomeViewText {};
}

bool make_text(SDL_Renderer *renderer, TTF_Font *font, const char *value,
	       SDL_Color color, HomeViewText *result)
{
	destroy_text(result);
	SDL_Surface *surface = TTF_RenderUTF8_Blended(font,
		value != nullptr ? value : "", color);
	if (surface == nullptr)
		return false;
	result->texture = SDL_CreateTextureFromSurface(renderer, surface);
	result->width = surface->w;
	result->height = surface->h;
	SDL_FreeSurface(surface);
	return result->texture != nullptr;
}

void draw_text(SDL_Renderer *renderer, const HomeViewText &text, int x, int y,
	       int max_width)
{
	if (text.texture == nullptr || max_width <= 0)
		return;
	const int width = std::min(text.width, max_width);
	SDL_Rect source {0, 0, width, text.height};
	SDL_Rect destination {x, y, width, text.height};
	(void)SDL_RenderCopy(renderer, text.texture, &source, &destination);
}

void format_duration(unsigned seconds, char *output, std::size_t size)
{
	if (seconds == 0) {
		std::snprintf(output, size, "--:--");
		return;
	}
	const unsigned hours = seconds / 3600U;
	const unsigned minutes = (seconds / 60U) % 60U;
	if (hours != 0)
		std::snprintf(output, size, "%u:%02u:%02u", hours, minutes,
			      seconds % 60U);
	else
		std::snprintf(output, size, "%u:%02u", minutes, seconds % 60U);
}

void copy_value(char *destination, std::size_t size, const char *source)
{
	if (size == 0)
		return;
	std::snprintf(destination, size, "%s", source != nullptr ? source : "");
}

void clear_card(HomeView *view, std::size_t index)
{
	if (view->thumbnails[index] != nullptr) {
		SDL_DestroyTexture(view->thumbnails[index]);
		view->thumbnails[index] = nullptr;
	}
	destroy_text(&view->titles[index]);
	destroy_text(&view->metadata[index]);
	view->rendered_video_ids[index][0] = 0;
	view->rendered_titles[index][0] = 0;
	view->rendered_metadata[index][0] = 0;
	view->rendered_thumbnail_paths[index][0] = 0;
}

bool sync_card(HomeView *view, SDL_Renderer *renderer, std::size_t index,
	       const HomeCard &card)
{
	bool okay = true;
	char duration[24] {};
	char metadata[kHomeChannelBytes + kHomePublishedBytes + 48] {};
	format_duration(card.duration_seconds, duration, sizeof(duration));
	std::snprintf(metadata, sizeof(metadata), "%s · %s · %s", card.channel,
		      card.published, duration);
	const bool identity_changed = std::strcmp(
		view->rendered_video_ids[index], card.video_id) != 0;
	if (identity_changed || std::strcmp(view->rendered_titles[index],
					    card.title) != 0) {
		if (make_text(renderer, view->title_font, card.title, kWhite,
			      &view->titles[index]))
			copy_value(view->rendered_titles[index],
				   sizeof(view->rendered_titles[index]), card.title);
		else
			okay = false;
	}
	if (identity_changed || std::strcmp(view->rendered_metadata[index],
					    metadata) != 0) {
		if (make_text(renderer, view->small_font, metadata, kLight,
			      &view->metadata[index]))
			copy_value(view->rendered_metadata[index],
				   sizeof(view->rendered_metadata[index]), metadata);
		else
			okay = false;
	}
	if (identity_changed && view->thumbnails[index] != nullptr) {
		SDL_DestroyTexture(view->thumbnails[index]);
		view->thumbnails[index] = nullptr;
		view->rendered_thumbnail_paths[index][0] = 0;
	}
	// A metadata refresh emits ITEM records before its progressive THUMB
	// records.  For the same video ID, an empty incoming path means "not here
	// yet", not "destroy the warm texture".  Build a replacement first and
	// swap only after decode/upload succeeds.
	if (card.thumbnail_path[0] != 0 &&
	    (identity_changed || std::strcmp(view->rendered_thumbnail_paths[index],
					      card.thumbnail_path) != 0)) {
		SDL_Texture *replacement = nullptr;
			SDL_Surface *surface = IMG_Load(card.thumbnail_path);
			if (surface != nullptr) {
				replacement = SDL_CreateTextureFromSurface(renderer, surface);
				SDL_FreeSurface(surface);
			}
			if (replacement == nullptr)
				okay = false;
			else {
				if (view->thumbnails[index] != nullptr)
					SDL_DestroyTexture(view->thumbnails[index]);
				view->thumbnails[index] = replacement;
				copy_value(view->rendered_thumbnail_paths[index],
					   sizeof(view->rendered_thumbnail_paths[index]),
					   card.thumbnail_path);
			}
	}
	copy_value(view->rendered_video_ids[index],
		   sizeof(view->rendered_video_ids[index]), card.video_id);
	return okay;
}

bool update_search(HomeView *view, SDL_Renderer *renderer,
		   const HomeCatalog *catalog)
{
	char label[256] {};
	if (home_catalog_channel_active(catalog))
		std::snprintf(label, sizeof(label), "CHANNEL   %s",
			      home_catalog_source_name(catalog));
	else
		std::snprintf(label, sizeof(label), "SEARCH   <  %s  >",
			      home_catalog_query(catalog));
	view->query_preset = catalog->query_preset;
	view->active_channel = catalog->active_channel;
	return make_text(renderer, view->title_font, label, kWhite, &view->search);
}

} // namespace

bool home_view_init(HomeView *view, SDL_Renderer *renderer,
		    const char *font_path)
{
	if (view == nullptr || renderer == nullptr || font_path == nullptr ||
	    font_path[0] != '/')
		return false;
	*view = HomeView {};
	view->image_initialized = (IMG_Init(kImageJpg) & kImageJpg) != 0;
	if (!view->image_initialized)
		return false;
	view->heading_font = TTF_OpenFont(font_path, 28);
	view->title_font = TTF_OpenFont(font_path, 18);
	view->small_font = TTF_OpenFont(font_path, 14);
	if (view->heading_font == nullptr || view->title_font == nullptr ||
	    view->small_font == nullptr)
		return false;
	bool okay = make_text(renderer, view->heading_font, "YouTube", kWhite,
			      &view->heading) &&
		make_text(renderer, view->small_font, "360P", kLight,
			  &view->quality) &&
		make_text(renderer, view->small_font,
			  "D-PAD MOVE   A SELECT   B BACK   X CHANNELS", kLight,
			  &view->hint) &&
		make_text(renderer, view->heading_font, "CHANNELS", kWhite,
			  &view->selector_heading) &&
		make_text(renderer, view->title_font, "總覽推薦",
			  kWhite, &view->selector_items[0]);
	for (std::size_t index = 0; index < kHomeChannelCount; ++index) {
		const HomeChannel *channel = home_catalog_channel(index);
		okay = channel != nullptr &&
			make_text(renderer, view->title_font, channel->name, kWhite,
				  &view->selector_items[index + 1]) && okay;
	}
	return okay;
}

bool home_view_set_system_status(HomeView *view, SDL_Renderer *renderer,
				 const char *value)
{
	if (view == nullptr || renderer == nullptr || value == nullptr)
		return false;
	if (std::strcmp(view->system_status_value, value) == 0)
		return true;
	if (!make_text(renderer, view->small_font, value, kLight,
		       &view->system_status))
		return false;
	copy_value(view->system_status_value, sizeof(view->system_status_value),
		   value);
	return true;
}

bool home_view_sync(HomeView *view, SDL_Renderer *renderer,
		    const HomeCatalog *catalog)
{
	if (view == nullptr || renderer == nullptr || catalog == nullptr)
		return false;
	bool okay = true;
	if (view->query_preset != catalog->query_preset ||
	    view->active_channel != catalog->active_channel)
		okay = update_search(view, renderer, catalog) && okay;
	if (view->catalog_revision == catalog->revision)
		return okay;
	// The 640x480 layout draws three cards.  Materialize those plus two rows
	// on either side; never synchronously decode an entire 96-card catalog.
	constexpr std::size_t visible = 3;
	constexpr std::size_t lookahead = 2;
	std::size_t first = 0;
	if (catalog->focus == HomeFocus::card && catalog->selected > 1)
		first = catalog->selected - 1;
	if (catalog->count > visible && first + visible > catalog->count)
		first = catalog->count - visible;
	const std::size_t materialize_begin = first > lookahead ? first - lookahead : 0;
	const std::size_t materialize_end = std::min(
		catalog->count, first + visible + lookahead);
	for (std::size_t index = materialize_begin; index < materialize_end; ++index)
		okay = sync_card(view, renderer, index, catalog->cards[index]) && okay;
	for (std::size_t index = 0; index < kHomeCardLimit; ++index) {
		if (index >= materialize_begin && index < materialize_end)
			continue;
		if (view->rendered_video_ids[index][0] != 0 ||
		    view->thumbnails[index] != nullptr ||
		    view->titles[index].texture != nullptr ||
		    view->metadata[index].texture != nullptr)
			clear_card(view, index);
	}
	char status[128] {};
	if (!catalog->network_ready && catalog->loading)
		std::snprintf(status, sizeof(status), "Loading %s...",
			      home_catalog_source_name(catalog));
	else if (catalog->available_count > catalog->count)
		std::snprintf(status, sizeof(status),
			      "%zu/%zu shown - more below", catalog->count,
			      catalog->available_count);
	else if (catalog->loading)
		std::snprintf(status, sizeof(status), "Loading more videos... %zu shown",
			      catalog->count);
	else if (catalog->network_ready && catalog->thumbnails_loading)
		std::snprintf(status, sizeof(status),
			      "%zu ready - loading images", catalog->count);
	else if (catalog->network_ready && catalog->cache_stale)
		std::snprintf(status, sizeof(status), "%zu cached videos ready",
			      catalog->count);
	else if (catalog->network_ready)
		std::snprintf(status, sizeof(status), "%s - %zu videos ready",
			      home_catalog_source_name(catalog), catalog->count);
	else
		std::snprintf(status, sizeof(status), "Offline feed");
	if (std::strcmp(view->status_value, status) != 0) {
		okay = make_text(renderer, view->small_font, status, kLight,
				 &view->status) && okay;
		if (view->status.texture != nullptr)
			copy_value(view->status_value, sizeof(view->status_value), status);
	}
	view->catalog_revision = catalog->revision;
	return okay;
}

void home_view_draw(HomeView *view, SDL_Renderer *renderer,
		    const HomeCatalog *catalog)
{
	if (view == nullptr || renderer == nullptr || catalog == nullptr)
		return;
	(void)home_view_sync(view, renderer, catalog);
	SDL_SetRenderDrawColor(renderer, 14, 16, 22, 255);
	(void)SDL_RenderClear(renderer);
	SDL_Rect red_bar {0, 0, 640, 7};
	SDL_SetRenderDrawColor(renderer, 210, 26, 42, 255);
	(void)SDL_RenderFillRect(renderer, &red_bar);
	draw_text(renderer, view->heading, 20, 15, 180);
	draw_text(renderer, view->quality, 390, 22, 70);
	if (view->system_status.texture != nullptr)
		draw_text(renderer, view->system_status,
			  620 - view->system_status.width, 22, 210);

	SDL_Rect search_box {20, 54, 600, 48};
	SDL_SetRenderDrawColor(renderer, 34, 37, 46, 255);
	(void)SDL_RenderFillRect(renderer, &search_box);
	SDL_SetRenderDrawColor(renderer,
		catalog->focus == HomeFocus::search &&
			!home_catalog_channel_active(catalog) ? 235 : 88,
		catalog->focus == HomeFocus::search &&
			!home_catalog_channel_active(catalog) ? 42 : 92,
		catalog->focus == HomeFocus::search &&
			!home_catalog_channel_active(catalog) ? 56 : 104, 255);
	(void)SDL_RenderDrawRect(renderer, &search_box);
	draw_text(renderer, view->search, 38, 66, 560);

	constexpr std::size_t visible = 3;
	std::size_t first = 0;
	if (catalog->focus == HomeFocus::card && catalog->selected > 1)
		first = catalog->selected - 1;
	if (catalog->count > visible && first + visible > catalog->count)
		first = catalog->count - visible;
	for (std::size_t row = 0; row < visible && first + row < catalog->count;
	     ++row) {
		const std::size_t index = first + row;
		const int y = 116 + static_cast<int>(row) * 96;
		SDL_Rect card {20, y, 600, 88};
		SDL_SetRenderDrawColor(renderer, 27, 30, 38, 255);
		(void)SDL_RenderFillRect(renderer, &card);
		const bool selected = catalog->focus == HomeFocus::card &&
			index == catalog->selected;
		SDL_SetRenderDrawColor(renderer, selected ? 235 : 62,
			selected ? 42 : 65, selected ? 56 : 76, 255);
		(void)SDL_RenderDrawRect(renderer, &card);
		SDL_Rect thumb {26, y + 5, 128, 72};
		if (view->thumbnails[index] != nullptr)
			(void)SDL_RenderCopy(renderer, view->thumbnails[index], nullptr, &thumb);
		else {
			SDL_SetRenderDrawColor(renderer, 50, 53, 62, 255);
			(void)SDL_RenderFillRect(renderer, &thumb);
		}
		draw_text(renderer, view->titles[index], 166, y + 13, 438);
		draw_text(renderer, view->metadata[index], 166, y + 56, 438);
	}
	draw_text(renderer, view->status, 20, 415, 250);
	draw_text(renderer, view->hint, 225, 446, 395);

	if (catalog->channel_selector_open) {
		SDL_Rect panel {45, 27, 550, 420};
		SDL_SetRenderDrawColor(renderer, 10, 12, 18, 255);
		(void)SDL_RenderFillRect(renderer, &panel);
		SDL_SetRenderDrawColor(renderer, 210, 26, 42, 255);
		(void)SDL_RenderDrawRect(renderer, &panel);
		draw_text(renderer, view->selector_heading, 68, 40, 500);
		for (std::size_t index = 0; index <= kHomeChannelCount; ++index) {
			const int y = 82 + static_cast<int>(index) * 35;
			SDL_Rect row {65, y, 510, 31};
			const bool selected = index == catalog->channel_selector_selected;
			SDL_SetRenderDrawColor(renderer, selected ? 210 : 27,
				selected ? 26 : 30, selected ? 42 : 38, 255);
			(void)SDL_RenderFillRect(renderer, &row);
			draw_text(renderer, view->selector_items[index], 78, y + 5, 480);
		}
	}
	SDL_RenderPresent(renderer);
}

void home_view_destroy(HomeView *view)
{
	if (view == nullptr)
		return;
	for (std::size_t index = 0; index < kHomeCardLimit; ++index) {
		if (view->thumbnails[index] != nullptr)
			SDL_DestroyTexture(view->thumbnails[index]);
		destroy_text(&view->titles[index]);
		destroy_text(&view->metadata[index]);
	}
	destroy_text(&view->status);
	destroy_text(&view->system_status);
	destroy_text(&view->hint);
	destroy_text(&view->search);
	destroy_text(&view->quality);
	destroy_text(&view->heading);
	destroy_text(&view->selector_heading);
	for (auto &item : view->selector_items)
		destroy_text(&item);
	if (view->small_font != nullptr)
		TTF_CloseFont(view->small_font);
	if (view->title_font != nullptr)
		TTF_CloseFont(view->title_font);
	if (view->heading_font != nullptr)
		TTF_CloseFont(view->heading_font);
	if (view->image_initialized)
		IMG_Quit();
	*view = HomeView {};
}

} // namespace rg40xxv_youtube
