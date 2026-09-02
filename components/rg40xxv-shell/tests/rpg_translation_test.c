#define _POSIX_C_SOURCE 200809L

#include "ui.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *last_message;

Uint32 SDLCALL SDL_GetTicks(void)
{
	return 1U;
}

const char *tr(const struct ui *ui, const char *key)
{
	(void)ui;
	return key;
}

void render_activate(struct ui *ui, const char *message, uint32_t now)
{
	(void)ui;
	(void)now;
	last_message = message;
}

const struct game_entry *catalog_visible_game(const struct ui *ui,
					      size_t visible_index)
{
	if (visible_index >= ui->catalog.game_count)
		return NULL;
	return &ui->catalog.games[visible_index];
}

bool catalog_system_is_rpg(const char *system)
{
	return system != NULL && (strcmp(system, "EASYRPG") == 0 ||
		strcmp(system, "RPGMV") == 0 || strcmp(system, "RPGMZ") == 0);
}

int main(int argc, char **argv)
{
	struct game_entry game = { .system = "RPGMV" };
	struct ui ui = { 0 };
	char payload[32];
	FILE *stream;

	assert(argc == 2);
	ui.catalog.games = &game;
	ui.catalog.game_count = 1U;
	ui.catalog.rpg_only = true;
	assert(rpg_translation_init(&ui.rpg_translation, argv[1]) == 0);
	assert(ui.rpg_translation.mode == RPG_TRANSLATION_OFF);
	assert(strcmp(getenv("RG_RMUT_TRANSLATION_MODE"), "off") == 0);
	rpg_translation_toggle_selected(&ui, 10U);
	assert(ui.rpg_translation.mode == RPG_TRANSLATION_STATIC);
	assert(strcmp(getenv("RG_RMUT_TRANSLATION_MODE"), "static") == 0);
	assert(strcmp(last_message, "rpg_translation_static_enabled") == 0);
	stream = fopen(argv[1], "r");
	assert(stream != NULL);
	assert(fgets(payload, sizeof(payload), stream) != NULL);
	assert(fclose(stream) == 0);
	assert(strcmp(payload, "static\n") == 0);

	memset(&ui.rpg_translation, 0, sizeof(ui.rpg_translation));
	assert(rpg_translation_init(&ui.rpg_translation, argv[1]) == 0);
	assert(ui.rpg_translation.mode == RPG_TRANSLATION_STATIC);
	rpg_translation_toggle_selected(&ui, 20U);
	assert(ui.rpg_translation.mode == RPG_TRANSLATION_OFF);
	assert(strcmp(getenv("RG_RMUT_TRANSLATION_MODE"), "off") == 0);

	game.system = "EASYRPG";
	rpg_translation_toggle_selected(&ui, 30U);
	assert(ui.rpg_translation.mode == RPG_TRANSLATION_OFF);
	assert(strcmp(last_message, "rpg_translation_rm2k_unsupported") == 0);

	/* Malformed, permissive and linked state never inherits a stale static
	 * request.  Initialization must actively fail closed to off. */
	stream = fopen(argv[1], "w");
	assert(stream != NULL);
	assert(fputs("dynamic\n", stream) >= 0);
	assert(fclose(stream) == 0);
	assert(chmod(argv[1], 0600) == 0);
	assert(setenv("RG_RMUT_TRANSLATION_MODE", "static", 1) == 0);
	memset(&ui.rpg_translation, 0, sizeof(ui.rpg_translation));
	assert(rpg_translation_init(&ui.rpg_translation, argv[1]) != 0);
	assert(ui.rpg_translation.mode == RPG_TRANSLATION_OFF);
	assert(strcmp(getenv("RG_RMUT_TRANSLATION_MODE"), "off") == 0);
	assert(chmod(argv[1], 0666) == 0);
	memset(&ui.rpg_translation, 0, sizeof(ui.rpg_translation));
	assert(rpg_translation_init(&ui.rpg_translation, argv[1]) != 0);
	assert(ui.rpg_translation.mode == RPG_TRANSLATION_OFF);
	assert(unlink(argv[1]) == 0);
	assert(symlink("/etc/passwd", argv[1]) == 0);
	memset(&ui.rpg_translation, 0, sizeof(ui.rpg_translation));
	assert(rpg_translation_init(&ui.rpg_translation, argv[1]) != 0);
	assert(ui.rpg_translation.mode == RPG_TRANSLATION_OFF);
	assert(unlink(argv[1]) == 0);
	stream = fopen(argv[1], "w");
	assert(stream != NULL);
	assert(fputs("off\n", stream) >= 0);
	assert(fclose(stream) == 0);
	assert(chmod(argv[1], 0600) == 0);
	assert(rpg_translation_init(&ui.rpg_translation, "/.") != 0);

	printf("RPG_TRANSLATION_TEST PASS modes=off,static rm2k=blocked malformed=fail-closed\n");
	return 0;
}
