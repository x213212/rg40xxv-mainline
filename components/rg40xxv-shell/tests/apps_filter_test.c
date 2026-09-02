#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned int checks;

#define CHECK(condition)                                                        \
	do {                                                                      \
		++checks;                                                          \
		if (!(condition)) {                                                \
			(void)fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__,         \
				      __LINE__, #condition);                          \
			return 1;                                                     \
		}                                                                 \
	} while (0)

Uint32 SDLCALL SDL_GetTicks(void)
{
	return 1U;
}

void persistence_request_filters(struct ui *ui)
{
	(void)ui;
}

void persistence_request_favorites(struct ui *ui)
{
	(void)ui;
}

const char *tr(const struct ui *ui, const char *key)
{
	(void)ui;
	return key;
}

int main(void)
{
	struct ui ui = { 0 };
	struct game_entry games[] = {
		{ .title = "File Manager", .system = "APPS", .core = "native" },
		{ .title = "Pokemon Emerald", .system = "GBA", .core = "mGBA",
		  .favorite = true },
		{ .title = "Other Game", .system = "GBA", .core = "mGBA",
		  .favorite = true },
		{ .title = "YouTube", .system = "APPS", .core = "native" },
	};
	char *systems[] = { "APPS", "GBA" };
	char *cores[] = { "mGBA", "native" };
	char filter_text[128];

	ui.catalog.games = games;
	ui.catalog.game_count = sizeof(games) / sizeof(games[0]);
	ui.catalog.systems = systems;
	ui.catalog.system_count = sizeof(systems) / sizeof(systems[0]);
	ui.catalog.cores = cores;
	ui.catalog.core_count = sizeof(cores) / sizeof(cores[0]);
	ui.catalog.system_filter = 2;
	ui.catalog.favorites_only = true;
	(void)snprintf(ui.catalog.query, sizeof(ui.catalog.query), "%s", "Pokemon");
	catalog_apply_filters(&ui);
	CHECK(ui.catalog.visible_count == 1);
	CHECK(catalog_visible_id(&ui, 0) == 1);

	catalog_set_apps_view(&ui, true);
	CHECK(ui.catalog.apps_only);
	CHECK(ui.catalog.visible_count == 2);
	CHECK(catalog_visible_id(&ui, 0) == 0);
	CHECK(strcmp(catalog_visible_game(&ui, 0)->system, "APPS") == 0);
	CHECK(ui.catalog.system_filter == 2);
	CHECK(ui.catalog.favorites_only);
	CHECK(strcmp(ui.catalog.query, "Pokemon") == 0);
	ui.game_index = 1;
	ui.carousel_position = 1.0;
	ui.carousel_from = 1.0;
	ui.carousel_target = 1.0;
	catalog_apply_filters(&ui);
	CHECK(ui.game_index == 1);
	CHECK(catalog_visible_id(&ui, ui.game_index) == 3);
	CHECK(strcmp(catalog_visible_game(&ui, ui.game_index)->title,
		     "YouTube") == 0);
	CHECK(ui.carousel_position == 1.0);
	CHECK(ui.carousel_target == 1.0);

	catalog_set_apps_view(&ui, false);
	CHECK(!ui.catalog.apps_only);
	CHECK(ui.catalog.visible_count == 1);
	CHECK(catalog_visible_id(&ui, 0) == 1);
	CHECK(ui.catalog.system_filter == 2);
	CHECK(ui.catalog.favorites_only);
	CHECK(strcmp(ui.catalog.query, "Pokemon") == 0);

	ui.catalog.system_filter = SIZE_MAX;
	ui.catalog.core_filter = SIZE_MAX;
	catalog_apply_filters(&ui);
	CHECK(ui.catalog.system_filter == 0);
	CHECK(ui.catalog.core_filter == 0);
	catalog_filter_text(&ui, 0, filter_text, sizeof(filter_text));
	CHECK(strstr(filter_text, "filter_system") != NULL);
	catalog_filter_text(&ui, 3, filter_text, sizeof(filter_text));
	CHECK(strstr(filter_text, "auto") != NULL);

	ui.catalog.query[0] = '\0';
	ui.catalog.favorites_only = false;
	ui.catalog.recent_only = true;
	games[0].last_played = 10;
	games[1].last_played = 30;
	games[2].last_played = 20;
	games[0].recent = true;
	games[1].recent = true;
	games[2].recent = true;
	catalog_apply_filters(&ui);
	CHECK(catalog_visible_id(&ui, 0) == 1);
	CHECK(catalog_visible_id(&ui, 1) == 2);
	CHECK(ui.catalog.visible_count == 2);
	free(ui.catalog.visible);
	(void)printf("APPS_FILTER_TEST PASS checks=%u\n", checks);
	return 0;
}
