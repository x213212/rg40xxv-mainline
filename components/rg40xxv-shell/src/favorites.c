#define _POSIX_C_SOURCE 200809L

#include "ui.h"

#include <stdio.h>
#include <string.h>

static void strip_line_end(char *text)
{
	text[strcspn(text, "\r\n")] = '\0';
}

static bool mark_path(struct ui *ui, const char *path, const char *hash)
{
	for (size_t i = 0; i < ui->catalog.game_count; ++i) {
		struct game_entry *game = &ui->catalog.games[i];

		if (strcmp(game->path, path) == 0 ||
		    (hash[0] != '\0' && game->content_hash[0] != '\0' &&
		     strcmp(game->content_hash, hash) == 0)) {
			game->favorite = true;
			return true;
		}
	}
	return false;
}

static void load_canonical(struct ui *ui, const char *path)
{
	FILE *stream = fopen(path, "r");
	char line[PATH_MAX + 80];

	if (stream == NULL)
		return;
	while (fgets(line, sizeof(line), stream) != NULL) {
		char *hash = strchr(line, '\t');

		strip_line_end(line);
		if (hash == NULL)
			hash = "";
		else {
			*hash = '\0';
			++hash;
		}
		(void)mark_path(ui, line, hash);
	}
	fclose(stream);
}

static void mark_stock_entry(struct ui *ui, const char *name,
			     const char *system)
{
	for (size_t i = 0; i < ui->catalog.game_count; ++i) {
		struct game_entry *game = &ui->catalog.games[i];
		const char *basename = strrchr(game->path, '/');

		basename = basename == NULL ? game->path : basename + 1;
		if (strcmp(game->system, system) == 0 && strcmp(basename, name) == 0) {
			game->favorite = true;
			return;
		}
	}
}

static void import_stock(struct ui *ui, const char *path)
{
	FILE *stream = fopen(path, "rb");
	char line[1024];

	if (stream == NULL)
		return;
	while (fgets(line, sizeof(line), stream) != NULL) {
		char *first;
		char *second;

		if (strncmp(line, "Version=", 8) == 0)
			continue;
		first = strchr(line, ':');
		if (first == NULL)
			continue;
		*first++ = '\0';
		second = strchr(first, ':');
		if (second == NULL)
			continue;
		*second = '\0';
		mark_stock_entry(ui, line, first);
	}
	fclose(stream);
}

void favorites_load(struct ui *ui, const char *favorites_path,
		    const char *stock_path)
{
	(void)snprintf(ui->catalog.favorites_path,
		       sizeof(ui->catalog.favorites_path), "%s", favorites_path);
	(void)snprintf(ui->catalog.stock_favorites_path,
		       sizeof(ui->catalog.stock_favorites_path), "%s", stock_path);
	load_canonical(ui, favorites_path);
	import_stock(ui, stock_path);
	catalog_apply_filters(ui);
}
