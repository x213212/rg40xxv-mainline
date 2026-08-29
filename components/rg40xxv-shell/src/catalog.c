#include "ui.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static bool contains_text(const char *text, const char *query)
{
	size_t query_length;

	if (query[0] == '\0')
		return true;
	if (strstr(text, query) != NULL)
		return true;
	query_length = strlen(query);
	for (const char *start = text; *start != '\0'; ++start) {
		size_t offset = 0;

		while (offset < query_length && start[offset] != '\0' &&
		       tolower((unsigned char)start[offset]) ==
		       tolower((unsigned char)query[offset]))
			++offset;
		if (offset == query_length)
			return true;
	}
	return false;
}

static bool game_is_visible(const struct catalog_state *catalog,
			    const struct game_entry *game)
{
	if (catalog->apps_only)
		return strcasecmp(game->system, "APPS") == 0;
	if (catalog->system_filter > 0 &&
	    !(catalog->search_all_systems && catalog->query[0] != '\0') &&
	    strcmp(game->system, catalog->systems[catalog->system_filter - 1]) != 0)
		return false;
	if (catalog->core_filter > 0 &&
	    strcmp(game->core, catalog->cores[catalog->core_filter - 1]) != 0)
		return false;
	if (catalog->favorites_only && !game->favorite)
		return false;
	if (catalog->recent_only && !game->recent)
		return false;
	return contains_text(game->title, catalog->query);
}

static void clamp_filter_indices(struct catalog_state *catalog)
{
	if (catalog->system_filter > catalog->system_count)
		catalog->system_filter = 0;
	if (catalog->core_filter > catalog->core_count)
		catalog->core_filter = 0;
}

static void sort_visible_recent(struct catalog_state *catalog)
{
	size_t count = catalog->visible_count;
	size_t *temporary;
	size_t *source;
	size_t *destination;

	if (count < 2 || count > SIZE_MAX / sizeof(*temporary))
		return;
	temporary = malloc(count * sizeof(*temporary));
	if (temporary == NULL)
		return;
	source = catalog->visible;
	destination = temporary;
	for (size_t width = 1; width < count;) {
		for (size_t left = 0; left < count; left += width * 2) {
			size_t middle = left + width < count ? left + width : count;
			size_t right = middle + width < count ? middle + width : count;
			size_t first = left;
			size_t second = middle;
			size_t output = left;

			while (first < middle && second < right) {
				time_t a = catalog->games[source[first]].last_played;
				time_t b = catalog->games[source[second]].last_played;

				destination[output++] = a >= b ?
					source[first++] : source[second++];
			}
			while (first < middle)
				destination[output++] = source[first++];
			while (second < right)
				destination[output++] = source[second++];
		}
		{
			size_t *swap = source;

			source = destination;
			destination = swap;
		}
		if (width > count / 2)
			break;
		width *= 2;
	}
	if (source != catalog->visible)
		memcpy(catalog->visible, source, count * sizeof(*source));
	free(temporary);
}

void catalog_set_apps_view(struct ui *ui, bool enabled)
{
	if (ui->catalog.apps_only == enabled)
		return;
	ui->catalog.apps_only = enabled;
	catalog_apply_filters(ui);
}

void catalog_apply_filters(struct ui *ui)
{
	struct catalog_state *catalog = &ui->catalog;

	clamp_filter_indices(catalog);
	if (catalog->visible_capacity < catalog->game_count) {
		size_t *visible = realloc(catalog->visible,
					  catalog->game_count * sizeof(*visible));

		if (visible == NULL && catalog->game_count > 0) {
			catalog->visible_count = 0;
			return;
		}
		catalog->visible = visible;
		catalog->visible_capacity = catalog->game_count;
	}
	catalog->visible_count = 0;
	for (size_t i = 0; i < catalog->game_count; ++i) {
		if (game_is_visible(catalog, &catalog->games[i]))
			catalog->visible[catalog->visible_count++] = i;
	}
	if (catalog->recent_only)
		sort_visible_recent(catalog);
	ui->game_index = 0;
	ui->carousel_position = 0.0;
	ui->carousel_from = 0.0;
	ui->carousel_target = 0.0;
	ui->carousel_started = SDL_GetTicks();
}

const struct game_entry *catalog_visible_game(const struct ui *ui,
					       size_t visible_index)
{
	size_t game_id = catalog_visible_id(ui, visible_index);

	return game_id == SIZE_MAX ? NULL : &ui->catalog.games[game_id];
}

size_t catalog_visible_id(const struct ui *ui, size_t visible_index)
{
	if (visible_index >= ui->catalog.visible_count)
		return SIZE_MAX;
	return ui->catalog.visible[visible_index];
}

static size_t cycle_index(size_t current, size_t count, int direction)
{
	if (count == 0)
		return 0;
	if (direction < 0)
		return current == 0 ? count - 1 : current - 1;
	return (current + 1) % count;
}

void catalog_cycle_filter(struct ui *ui, int filter, int direction)
{
	struct catalog_state *catalog = &ui->catalog;

	clamp_filter_indices(catalog);
	switch (filter) {
	case 0:
		catalog->system_filter = cycle_index(catalog->system_filter,
						     catalog->system_count + 1,
						     direction);
		break;
	case 1:
		catalog->favorites_only = !catalog->favorites_only;
		break;
	case 2:
		catalog->recent_only = !catalog->recent_only;
		break;
	case 3:
		catalog->core_filter = cycle_index(catalog->core_filter,
						   catalog->core_count + 1,
						   direction);
		break;
	case 4:
		catalog->system_filter = 0;
		catalog->core_filter = 0;
		catalog->favorites_only = false;
		catalog->recent_only = false;
		catalog->query[0] = '\0';
		break;
	default:
		return;
	}
	catalog_apply_filters(ui);
	persistence_request_filters(ui);
}

void catalog_filter_text(const struct ui *ui, int filter, char *buffer,
			 size_t size)
{
	const struct catalog_state *catalog = &ui->catalog;

	switch (filter) {
	case 0:
		if (catalog->system_filter == 0 ||
		    catalog->system_filter > catalog->system_count)
			(void)snprintf(buffer, size, "%s　%s (%zu)",
				tr(ui, "filter_system"), tr(ui, "all"),
				catalog->visible_count);
		else {
			const char *system = catalog->systems[catalog->system_filter - 1];

			(void)snprintf(buffer, size, "%s　%s (%zu)",
				tr(ui, "filter_system"), system,
				catalog->visible_count);
		}
		break;
	case 1:
		(void)snprintf(buffer, size, "%s　%s", tr(ui, "filter_favorites"),
			catalog->favorites_only ? tr(ui, "favorites_only") :
			tr(ui, "all"));
		break;
	case 2:
		(void)snprintf(buffer, size, "%s　%s", tr(ui, "filter_recent"),
			catalog->recent_only ? tr(ui, "only") : tr(ui, "all"));
		break;
	case 3:
		(void)snprintf(buffer, size, "%s　%s", tr(ui, "filter_core"),
			catalog->core_filter == 0 ||
			catalog->core_filter > catalog->core_count ? tr(ui, "auto") :
			catalog->cores[catalog->core_filter - 1]);
		break;
	case 4:
		(void)snprintf(buffer, size, "%s", tr(ui, "filter_reset"));
		break;
	default:
		(void)snprintf(buffer, size, "%s　%s", tr(ui, "filter_language"),
			ui->locale.language == UI_LANGUAGE_ZH_TW ?
			tr(ui, "zh_tw") : tr(ui, "english"));
		break;
	}
}

size_t catalog_system_game_count(const struct ui *ui, const char *system)
{
	size_t count = 0;

	for (size_t i = 0; i < ui->catalog.game_count; ++i) {
		if (strcmp(ui->catalog.games[i].system, system) == 0)
			++count;
	}
	return count;
}

void catalog_toggle_favorite(struct ui *ui)
{
	size_t game_id = catalog_visible_id(ui, ui->game_index);

	if (game_id == SIZE_MAX)
		return;
	ui->catalog.games[game_id].favorite =
		!ui->catalog.games[game_id].favorite;
	persistence_request_favorites(ui);
	if (ui->catalog.favorites_only)
		catalog_apply_filters(ui);
}
