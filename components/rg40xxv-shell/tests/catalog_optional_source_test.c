#define _POSIX_C_SOURCE 200809L

#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

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

const struct platform_route *platform_route_find(const struct ui *ui,
						  const char *system)
{
	(void)ui;
	(void)system;
	return NULL;
}

const char *platform_route_frontend(const char *selector)
{
	return selector;
}

const char *platform_route_runtime(const char *selector)
{
	return selector;
}

static bool path_below(const char *path, const char *root)
{
	size_t length = strlen(root);

	return strncmp(path, root, length) == 0 && path[length] == '/';
}

static size_t count_paths_below(const struct ui *ui, const char *root)
{
	size_t count = 0U;

	for (size_t index = 0; index < ui->catalog.game_count; ++index) {
		if (path_below(ui->catalog.games[index].path, root))
			++count;
	}
	return count;
}

static const struct game_entry *find_title(const struct ui *ui,
					   const char *title, size_t *matches)
{
	const struct game_entry *found = NULL;

	*matches = 0U;
	for (size_t index = 0; index < ui->catalog.game_count; ++index) {
		if (strcasecmp(ui->catalog.games[index].title, title) == 0) {
			found = &ui->catalog.games[index];
			++*matches;
		}
	}
	return found;
}

int main(int argc, char **argv)
{
	struct ui ui = { 0 };
	struct ui cached = { 0 };
	struct stat tf1_before;
	struct stat tf2_before;
	struct stat after;
	char tf1_duplicate[PATH_MAX];
	char tf2_duplicate[PATH_MAX];
	char tf2_mount[PATH_MAX];
	char *separator;
	char *tf1_real;
	char *tf2_real;
	const struct game_entry *duplicate;
	size_t matches;

	CHECK(argc == 4);
	tf1_real = realpath(argv[1], NULL);
	tf2_real = realpath(argv[2], NULL);
	CHECK(tf1_real != NULL);
	CHECK(tf2_real != NULL);
	CHECK(snprintf(tf2_mount, sizeof(tf2_mount), "%s", argv[2]) > 0);
	separator = strrchr(tf2_mount, '/');
	CHECK(separator != NULL && separator != tf2_mount);
	*separator = '\0';
	/* Merely leaving a Roms directory behind is not a mounted TF2. */
	CHECK(!catalog_optional_source_available(tf2_mount, argv[2]));
	CHECK(snprintf(tf1_duplicate, sizeof(tf1_duplicate),
		"%s/GBA/shared/Duplicate Game.gba", argv[1]) > 0);
	CHECK(snprintf(tf2_duplicate, sizeof(tf2_duplicate),
		"%s/gba/shared/duplicate game.GBA", argv[2]) > 0);
	CHECK(stat(tf1_duplicate, &tf1_before) == 0);
	CHECK(stat(tf2_duplicate, &tf2_before) == 0);

	/* TF1 is a complete library by itself. */
	CHECK(catalog_scan(&ui, argv[1]) == 0);
	CHECK(ui.catalog.game_count == 3U);
	CHECK(count_paths_below(&ui, tf1_real) == 3U);
	CHECK(count_paths_below(&ui, tf2_real) == 0U);

	/* A present TF2 is appended, and case variants of one platform merge. */
	CHECK(catalog_scan(&cached, argv[2]) == 0);
	CHECK(catalog_append_cached_source(&ui, &cached, argv[2], argv[1]) == 0);
	CHECK(ui.catalog.game_count == 5U);
	CHECK(catalog_system_game_count(&ui, "GBA") == 3U);
	CHECK(ui.catalog.system_count == 3U);
	CHECK(count_paths_below(&ui, tf1_real) == 3U);
	CHECK(count_paths_below(&ui, tf2_real) == 2U);

	/* The same relative game path exists on both cards; TF1 wins. */
	duplicate = find_title(&ui, "duplicate game", &matches);
	CHECK(matches == 1U);
	CHECK(duplicate != NULL && path_below(duplicate->path, tf1_real));
	CHECK(stat(tf1_duplicate, &after) == 0 &&
	      after.st_dev == tf1_before.st_dev && after.st_ino == tf1_before.st_ino);
	CHECK(stat(tf2_duplicate, &after) == 0 &&
	      after.st_dev == tf2_before.st_dev && after.st_ino == tf2_before.st_ino);

	/* A rescan after TF2 removal cannot retain stale TF2 catalog entries. */
	CHECK(rename(argv[2], argv[3]) == 0);
	CHECK(catalog_scan(&ui, argv[1]) == 0);
	CHECK(ui.catalog.game_count == 3U);
	CHECK(count_paths_below(&ui, tf1_real) == 3U);
	CHECK(catalog_append_source_bounded(&ui, argv[2], argv[1], 0U, 0U) < 0);
	CHECK(ui.catalog.game_count == 3U);

	catalog_destroy(&ui);
	catalog_destroy(&cached);
	free(tf1_real);
	free(tf2_real);
	(void)printf(
		"CATALOG_OPTIONAL_SOURCE_TEST PASS checks=%u tf1_only=3 merged=5 tf1_wins=1 tf2_removed=3\n",
		checks);
	return 0;
}
