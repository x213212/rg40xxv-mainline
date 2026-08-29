#define _POSIX_C_SOURCE 200809L

#include "ui.h"

#include <stdio.h>
#include <string.h>

static size_t named_filter(char **values, size_t count, const char *name)
{
	for (size_t i = 0; i < count; ++i) {
		if (strcmp(values[i], name) == 0)
			return i + 1;
	}
	return 0;
}

void filter_state_load(struct ui *ui, const char *path)
{
	FILE *stream;
	char line[256];

	(void)snprintf(ui->catalog.filter_state_path,
		       sizeof(ui->catalog.filter_state_path), "%s", path);
	stream = fopen(path, "r");
	if (stream == NULL)
		return;
	while (fgets(line, sizeof(line), stream) != NULL) {
		line[strcspn(line, "\r\n")] = '\0';
		if (strncmp(line, "system=", 7) == 0)
			ui->catalog.system_filter = named_filter(ui->catalog.systems,
				ui->catalog.system_count, line + 7);
		else if (strncmp(line, "core=", 5) == 0)
			ui->catalog.core_filter = named_filter(ui->catalog.cores,
				ui->catalog.core_count, line + 5);
		else if (strcmp(line, "favorites=1") == 0)
			ui->catalog.favorites_only = true;
		else if (strcmp(line, "recent=1") == 0)
			ui->catalog.recent_only = true;
		else if (strcmp(line, "search_scope=all") == 0)
			ui->catalog.search_all_systems = true;
	}
	fclose(stream);
	catalog_apply_filters(ui);
}
