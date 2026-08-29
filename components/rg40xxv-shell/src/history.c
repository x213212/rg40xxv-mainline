#define _POSIX_C_SOURCE 200809L

#include "ui.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void mark_history_path(struct ui *ui, const char *path,
			      time_t played_at)
{
	for (size_t i = 0; i < ui->catalog.game_count; ++i) {
		struct game_entry *game = &ui->catalog.games[i];

		if (strcmp(game->path, path) == 0) {
			game->recent = true;
			if (played_at > game->last_played)
				game->last_played = played_at;
			return;
		}
	}
}

void history_load(struct ui *ui, const char *path)
{
	FILE *stream;
	char line[PATH_MAX + 48];

	(void)snprintf(ui->catalog.history_path,
		       sizeof(ui->catalog.history_path), "%s", path);
	stream = fopen(path, "r");
	if (stream == NULL)
		return;
	while (fgets(line, sizeof(line), stream) != NULL) {
		char *separator = strchr(line, '\t');
		char *end;
		long long stamp;

		if (separator == NULL)
			continue;
		*separator++ = '\0';
		separator[strcspn(separator, "\r\n")] = '\0';
		errno = 0;
		stamp = strtoll(line, &end, 10);
		if (errno != 0 || end == line || *end != '\0' || stamp <= 0)
			continue;
		mark_history_path(ui, separator, (time_t)stamp);
	}
	fclose(stream);
	catalog_apply_filters(ui);
}

void history_mark_launched(struct ui *ui, size_t game_id)
{
	struct game_entry *game;

	if (game_id >= ui->catalog.game_count)
		return;
	game = &ui->catalog.games[game_id];
	game->recent = true;
	game->last_played = time(NULL);
}
