#define _POSIX_C_SOURCE 200809L

#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned int checks;
static size_t launched_game_id = SIZE_MAX;
static unsigned int history_requests;

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
	return 100U;
}

const char *SDLCALL SDL_GetError(void)
{
	return "test error";
}

const char *ns_aspect_name(NsAspect aspect)
{
	(void)aspect;
	return "fit";
}

const char *tr(const struct ui *ui, const char *key)
{
	(void)ui;
	return key;
}

void audio_play_chime(struct ui *ui, double frequency)
{
	(void)ui;
	(void)frequency;
}

void render_activate(struct ui *ui, const char *text, uint32_t now)
{
	(void)ui;
	(void)text;
	(void)now;
}

void persistence_request_filters(struct ui *ui)
{
	(void)ui;
}

void persistence_request_favorites(struct ui *ui)
{
	(void)ui;
}

void persistence_request_history(struct ui *ui)
{
	(void)ui;
	++history_requests;
}

int persistence_start(struct ui *ui)
{
	(void)ui;
	return 0;
}

void persistence_stop(struct ui *ui)
{
	(void)ui;
}

void history_mark_launched(struct ui *ui, size_t game_id)
{
	(void)ui;
	launched_game_id = game_id;
}

void lifecycle_session_suspend(struct ui *ui)
{
	ui->launch.session_suspended = true;
}

int lifecycle_session_resume(struct ui *ui)
{
	ui->launch.session_suspended = false;
	return 0;
}

static int handoff_matches(const char *path, const char *content)
{
	FILE *stream = fopen(path, "r");
	char line[PATH_MAX + 32];
	char expected[PATH_MAX + 32];

	if (stream == NULL)
		return 0;
	if (fgets(line, sizeof(line), stream) == NULL ||
	    strcmp(line, "schema=rg40xxv-launch-handoff-v1\n") != 0 ||
	    fgets(line, sizeof(line), stream) == NULL ||
	    strcmp(line, "route=\n") != 0 ||
	    fgets(line, sizeof(line), stream) == NULL ||
	    strcmp(line, "platform=APPS\n") != 0 ||
	    fgets(line, sizeof(line), stream) == NULL ||
	    snprintf(expected, sizeof(expected), "content=%s\n", content) < 0 ||
	    strcmp(line, expected) != 0 || fgets(line, sizeof(line), stream) != NULL) {
		(void)fclose(stream);
		return 0;
	}
	return fclose(stream) == 0;
}

int main(int argc, char **argv)
{
	struct ui ui = { 0 };
	struct game_entry games[] = {
		{
			.title = "PortMaster",
			.path = NULL,
			.system = "APPS",
			.system_label = "Applications",
			.core = "native",
			.frontend = "Native script",
			.runtime = "AArch64/Linux",
			.fallback = "",
			.playable = true,
		},
		{
			.title = "Terminal",
			.path = NULL,
			.system = "APPS",
			.system_label = "Applications",
			.core = "native",
			.frontend = "Native script",
			.runtime = "AArch64/Linux",
			.fallback = "",
			.playable = true,
		},
	};

	CHECK(argc == 5);
	games[0].path = argv[3];
	games[1].path = argv[4];
	ui.catalog.games = games;
	ui.catalog.game_count = sizeof(games) / sizeof(games[0]);
	ui.catalog.apps_only = true;
	catalog_apply_filters(&ui);
	CHECK(ui.catalog.visible_count == 2U);
	CHECK(catalog_visible_id(&ui, 0U) == 0U);
	CHECK(catalog_visible_id(&ui, 1U) == 1U);

	/* This is the runtime failure sequence: the user moves off PortMaster, then
	 * a delayed capability update re-applies the unchanged Apps view. */
	ui.game_index = 1U;
	ui.carousel_position = 1.0;
	ui.carousel_from = 1.0;
	ui.carousel_target = 1.0;
	for (unsigned int refresh = 0U; refresh < 64U; ++refresh)
		catalog_apply_filters(&ui);
	CHECK(ui.game_index == 1U);
	CHECK(catalog_visible_id(&ui, ui.game_index) == 1U);
	CHECK(strcmp(catalog_visible_game(&ui, ui.game_index)->path, argv[4]) == 0);

	launch_configure(&ui, argv[1], "", argv[2], "", true);
	ui.resident = true;
	ui.running = true;
	launch_queue_selected(&ui, 100U);
	CHECK(ui.launch.pending);
	CHECK(ui.launch.pending_game_id == 1U);
	CHECK(strcmp(ui.launch.game_title, "Terminal") == 0);
	CHECK(launch_cancel_pending(&ui, 100U));
	CHECK(!ui.launch.pending);
	CHECK(ui.launch.transition == LAUNCH_TRANSITION_NONE);
	CHECK(ui.running);
	CHECK(!ui.launch.supervisor_handoff);
	CHECK(!launch_cancel_pending(&ui, 100U));

	/* A new activation remains possible after a local cancellation. */
	launch_queue_selected(&ui, 100U);
	CHECK(ui.launch.pending);
	CHECK(ui.launch.pending_game_id == 1U);

	/* Once activation snapshots the game ID, another refresh must not retarget
	 * the pending resident request either. */
	catalog_apply_filters(&ui);
	launch_transition_presented(&ui);
	launch_update(&ui, 100U);
	CHECK(!ui.running);
	CHECK(ui.launch.supervisor_handoff);
	CHECK(launched_game_id == 1U);
	CHECK(history_requests == 1U);
	CHECK(handoff_matches(argv[2], argv[4]));
	free(ui.catalog.visible);

	(void)printf("APPS_SELECTION_HANDOFF_TEST PASS checks=%u selected=Terminal portmaster=not-selected refreshes=65\n",
		checks);
	return 0;
}
