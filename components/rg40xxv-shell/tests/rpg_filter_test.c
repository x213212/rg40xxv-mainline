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

void persistence_request_locale(struct ui *ui)
{
	(void)ui;
}

int main(void)
{
	struct ui ui = { 0 };
	struct ui empty = { 0 };
	struct game_entry games[] = {
		{ .title = "Files", .system = "APPS", .core = "native" },
		{ .title = "Emerald", .system = "GBA", .core = "mGBA",
		  .favorite = true, .playable = true },
		{ .title = "Bahamut", .system = "EASYRPG", .core = "easyrpg",
		  .playable = true },
		{ .title = "Summer", .system = "RPGMV", .core = "wpe",
		  .favorite = true, .playable = false },
		{ .title = "Dungeon", .system = "RPGMZ", .core = "wpe",
		  .playable = false },
		{ .title = "Unavailable", .system = "PS2", .core = "none",
		  .playable = false },
	};
	struct game_entry empty_games[] = {
		{ .title = "Files", .system = "APPS", .core = "native" },
		{ .title = "Emerald", .system = "GBA", .core = "mGBA" },
	};
	char *systems[] = { "APPS", "EASYRPG", "GBA", "PS2", "RPGMV", "RPGMZ" };
	char *cores[] = { "easyrpg", "mGBA", "native", "none", "wpe" };

	CHECK(NAV_COUNT == 8);
	CHECK(NAV_PAGE_RPG == NAV_PAGE_FAVORITES + 1);
	CHECK(catalog_system_is_rpg("EASYRPG"));
	CHECK(catalog_system_is_rpg("rpgmv"));
	CHECK(catalog_system_is_rpg("RPGMZ"));
	CHECK(!catalog_system_is_rpg("GBA"));
	ui.locale.language = UI_LANGUAGE_ENGLISH;
	CHECK(strcmp(tr(&ui, "rpg_pending_entry"),
		"Runtime unavailable · launch blocked") == 0);
	ui.locale.language = UI_LANGUAGE_ZH_TW;
	CHECK(strstr(tr(&ui, "rpg_pending_entry"), "禁止啟動") != NULL);

	ui.catalog.games = games;
	ui.catalog.game_count = sizeof(games) / sizeof(games[0]);
	ui.catalog.systems = systems;
	ui.catalog.system_count = sizeof(systems) / sizeof(systems[0]);
	ui.catalog.cores = cores;
	ui.catalog.core_count = sizeof(cores) / sizeof(cores[0]);
	ui.catalog.system_filter = 3;
	ui.catalog.core_filter = 2;
	ui.catalog.favorites_only = true;
	catalog_apply_filters(&ui);
	CHECK(ui.catalog.visible_count == 1);
	CHECK(catalog_visible_id(&ui, 0) == 1);

	catalog_set_rpg_view(&ui, true);
	CHECK(ui.catalog.rpg_only && !ui.catalog.apps_only);
	CHECK(ui.catalog.visible_count == 1);
	CHECK(catalog_visible_id(&ui, 0) == 3);
	CHECK(ui.catalog.system_filter == 3 && ui.catalog.core_filter == 2);
	ui.catalog.favorites_only = false;
	catalog_apply_filters(&ui);
	CHECK(ui.catalog.visible_count == 3);
	for (size_t index = 0; index < ui.catalog.visible_count; ++index)
		CHECK(catalog_system_is_rpg(
			catalog_visible_game(&ui, index)->system));
	(void)snprintf(ui.catalog.query, sizeof(ui.catalog.query), "%s", "dung");
	catalog_apply_filters(&ui);
	CHECK(ui.catalog.visible_count == 1);
	CHECK(strcmp(catalog_visible_game(&ui, 0)->system, "RPGMZ") == 0);

	CHECK(catalog_game_can_launch(&games[2]));
	CHECK(!catalog_game_can_launch(&games[3]));
	CHECK(!catalog_game_can_launch(&games[4]));
	CHECK(!catalog_game_can_launch(&games[5]));
	CHECK(!catalog_game_can_launch(NULL));

	catalog_set_apps_view(&ui, true);
	CHECK(ui.catalog.apps_only && !ui.catalog.rpg_only);
	CHECK(ui.catalog.visible_count == 1);
	CHECK(catalog_visible_id(&ui, 0) == 0);
	CHECK(ui.catalog.system_filter == 3 && ui.catalog.core_filter == 2);
	ui.catalog.query[0] = '\0';
	catalog_set_apps_view(&ui, false);
	CHECK(!ui.catalog.apps_only && !ui.catalog.rpg_only);
	CHECK(ui.catalog.visible_count == 1);
	CHECK(catalog_visible_id(&ui, 0) == 1);

	empty.catalog.games = empty_games;
	empty.catalog.game_count = sizeof(empty_games) / sizeof(empty_games[0]);
	catalog_set_rpg_view(&empty, true);
	CHECK(empty.catalog.rpg_only);
	CHECK(empty.catalog.visible_count == 0);

	free(ui.catalog.visible);
	free(empty.catalog.visible);
	(void)printf("RPG_FILTER_TEST PASS checks=%u systems=EASYRPG,RPGMV,RPGMZ empty=visible blocked_launch=MV,MZ\n",
		checks);
	return 0;
}
