#define _POSIX_C_SOURCE 200809L

#include "ui.h"

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

enum {
	INCREMENTAL_MAX_PLATFORMS = 4096,
	INCREMENTAL_MAX_GAMES = 65536,
};

struct platform_directory {
	char *name;
	char path[PATH_MAX];
	struct stat metadata;
};

static uint64_t monotonic_ns(void)
{
	struct timespec value;

	if (clock_gettime(CLOCK_MONOTONIC, &value) != 0)
		return 0U;
	return (uint64_t)value.tv_sec * 1000000000ULL +
		(uint64_t)value.tv_nsec;
}

static uint64_t stat_time_ns(time_t seconds, long nanoseconds)
{
	return (uint64_t)seconds * 1000000000ULL + (uint64_t)nanoseconds;
}

static int compare_directory(const void *left, const void *right)
{
	const struct platform_directory *a = left;
	const struct platform_directory *b = right;

	return strcmp(a->name, b->name);
}

static void free_directories(struct platform_directory *items, size_t count)
{
	for (size_t index = 0; index < count; ++index)
		free(items[index].name);
	free(items);
}

static int list_directories(const char *root, struct platform_directory **output,
			    size_t *output_count)
{
	struct platform_directory *items = NULL;
	size_t count = 0U;
	size_t capacity = 0U;
	DIR *stream = opendir(root);
	struct dirent *entry;
	int result = -1;

	if (stream == NULL)
		return -1;
	while ((entry = readdir(stream)) != NULL) {
		struct platform_directory item = { 0 };
		int length;

		if (entry->d_name[0] == '.' || strcasecmp(entry->d_name, "Imgs") == 0)
			continue;
		length = snprintf(item.path, sizeof(item.path), "%s/%s", root,
			entry->d_name);
		if (length < 0 || (size_t)length >= sizeof(item.path) ||
		    lstat(item.path, &item.metadata) != 0 ||
		    !S_ISDIR(item.metadata.st_mode) || S_ISLNK(item.metadata.st_mode))
			continue;
		if (count >= INCREMENTAL_MAX_PLATFORMS)
			goto out;
		if (count >= capacity) {
			size_t next = capacity == 0U ? 32U : capacity * 2U;
			struct platform_directory *grown = realloc(items,
				next * sizeof(*grown));

			if (grown == NULL)
				goto out;
			items = grown;
			capacity = next;
		}
		item.name = strdup(entry->d_name);
		if (item.name == NULL)
			goto out;
		items[count++] = item;
	}
	qsort(items, count, sizeof(*items), compare_directory);
	*output = items;
	*output_count = count;
	items = NULL;
	count = 0U;
	result = 0;

out:
	(void)closedir(stream);
	free_directories(items, count);
	return result;
}

static struct catalog_platform_record *find_record(struct catalog_state *catalog,
						    const char *name)
{
	for (size_t index = 0; index < catalog->platform_count; ++index) {
		if (catalog->platforms[index].name != NULL &&
		    strcmp(catalog->platforms[index].name, name) == 0)
			return &catalog->platforms[index];
	}
	return NULL;
}

static bool record_matches(const struct catalog_platform_record *record,
			   const struct stat *metadata)
{
	return record != NULL && record->device == (uint64_t)metadata->st_dev &&
		record->inode == (uint64_t)metadata->st_ino &&
		record->mtime_ns == stat_time_ns(metadata->st_mtim.tv_sec,
			metadata->st_mtim.tv_nsec) &&
		record->ctime_ns == stat_time_ns(metadata->st_ctim.tv_sec,
			metadata->st_ctim.tv_nsec);
}

static int grow_values(char ***values, size_t *count, size_t *capacity,
		       const char *value)
{
	char **grown;

	for (size_t index = 0; index < *count; ++index) {
		if (strcmp((*values)[index], value) == 0)
			return 0;
	}
	if (*count >= *capacity) {
		size_t next = *capacity == 0U ? 16U : *capacity * 2U;

		grown = realloc(*values, next * sizeof(*grown));
		if (grown == NULL)
			return -1;
		*values = grown;
		*capacity = next;
	}
	(*values)[*count] = strdup(value);
	if ((*values)[*count] == NULL)
		return -1;
	++*count;
	return 0;
}

static int move_game(struct ui *destination, struct game_entry *game)
{
	struct catalog_state *catalog = &destination->catalog;
	struct game_entry *grown;

	if (catalog->game_count >= INCREMENTAL_MAX_GAMES)
		return -1;
	if (catalog->game_count >= catalog->game_capacity) {
		size_t next = catalog->game_capacity == 0U ? 256U :
			catalog->game_capacity * 2U;

		if (next > INCREMENTAL_MAX_GAMES)
			next = INCREMENTAL_MAX_GAMES;
		grown = realloc(catalog->games, next * sizeof(*grown));
		if (grown == NULL)
			return -1;
		catalog->games = grown;
		catalog->game_capacity = next;
	}
	if (grow_values(&catalog->systems, &catalog->system_count,
			&catalog->system_capacity, game->system) != 0 ||
	    grow_values(&catalog->cores, &catalog->core_count,
			&catalog->core_capacity, game->core) != 0)
		return -1;
	catalog->games[catalog->game_count++] = *game;
	memset(game, 0, sizeof(*game));
	return 0;
}

static int move_games(struct ui *destination, struct ui *source,
		      const char *system)
{
	for (size_t index = 0; index < source->catalog.game_count; ++index) {
		struct game_entry *game = &source->catalog.games[index];

		if (game->system != NULL && strcmp(game->system, system) == 0 &&
		    move_game(destination, game) != 0)
			return -1;
	}
	return 0;
}

static int move_record(struct catalog_state *destination,
		       struct catalog_platform_record *record)
{
	struct catalog_platform_record *grown;

	if (record == NULL)
		return 0;
	if (destination->platform_count >= destination->platform_capacity) {
		size_t next = destination->platform_capacity == 0U ? 32U :
			destination->platform_capacity * 2U;

		grown = realloc(destination->platforms, next * sizeof(*grown));
		if (grown == NULL)
			return -1;
		destination->platforms = grown;
		destination->platform_capacity = next;
	}
	destination->platforms[destination->platform_count++] = *record;
	memset(record, 0, sizeof(*record));
	return 0;
}

static int retain_old(struct ui *next, struct ui *old,
		      struct catalog_platform_record *record, const char *name)
{
	return move_games(next, old, name) != 0 ||
		move_record(&next->catalog, record) != 0 ? -1 : 0;
}

static int add_fresh_record(struct catalog_state *catalog, const char *name,
			    const struct stat *metadata, uint64_t entries)
{
	struct catalog_platform_record record = {
		.name = strdup(name),
		.device = (uint64_t)metadata->st_dev,
		.inode = (uint64_t)metadata->st_ino,
		.mtime_ns = stat_time_ns(metadata->st_mtim.tv_sec,
			metadata->st_mtim.tv_nsec),
		.ctime_ns = stat_time_ns(metadata->st_ctim.tv_sec,
			metadata->st_ctim.tv_nsec),
		.entry_count = entries,
	};
	int result;

	if (record.name == NULL)
		return -1;
	result = move_record(catalog, &record);
	free(record.name);
	return result;
}

static bool metadata_unchanged(const struct stat *before,
			       const struct stat *after)
{
	return before->st_dev == after->st_dev && before->st_ino == after->st_ino &&
		before->st_mtim.tv_sec == after->st_mtim.tv_sec &&
		before->st_mtim.tv_nsec == after->st_mtim.tv_nsec &&
		before->st_ctim.tv_sec == after->st_ctim.tv_sec &&
		before->st_ctim.tv_nsec == after->st_ctim.tv_nsec;
}

bool catalog_snapshot_needs_refresh(const struct ui *ui, const char *rom_root)
{
	struct platform_directory *directories = NULL;
	size_t count = 0U;
	bool needed = true;

	if (list_directories(rom_root, &directories, &count) != 0)
		return true;
	if (count != ui->catalog.platform_count)
		goto out;
	for (size_t index = 0; index < count; ++index) {
		struct catalog_platform_record *record = find_record(
			(struct catalog_state *)&ui->catalog, directories[index].name);

		if (!record_matches(record, &directories[index].metadata))
			goto out;
	}
	needed = false;

out:
	free_directories(directories, count);
	return needed;
}

int catalog_snapshot_refresh(struct ui *ui, const char *path,
			     const char *rom_root, uint32_t max_ms)
{
	struct platform_directory *directories = NULL;
	size_t directory_count = 0U;
	struct ui old = { 0 };
	struct ui next = { 0 };
	bool old_complete = false;
	bool loaded;
	bool warm_unchanged = true;
	bool blocked = false;
	uint64_t deadline;
	size_t scanned = 0U;
	size_t reused = 0U;
	int result = -1;

	loaded = catalog_snapshot_load(ui, path, rom_root, &old_complete) == 0;
	if (!loaded)
		catalog_destroy(ui);
	if (list_directories(rom_root, &directories, &directory_count) != 0)
		goto out;
	if (!loaded || ui->catalog.platform_count != directory_count)
		warm_unchanged = false;
	for (size_t index = 0; warm_unchanged && index < directory_count; ++index) {
		if (!record_matches(find_record(&ui->catalog, directories[index].name),
			&directories[index].metadata))
			warm_unchanged = false;
	}
	if (warm_unchanged && old_complete) {
		(void)fprintf(stderr,
			"CATALOG_INCREMENTAL mode=warm-unchanged platforms=%zu scanned=0 reused=%zu\n",
			directory_count, directory_count);
		result = 0;
		goto out;
	}

	old.catalog = ui->catalog;
	memset(&ui->catalog, 0, sizeof(ui->catalog));
	next.routes = ui->routes;
	(void)snprintf(next.catalog.rom_root, sizeof(next.catalog.rom_root), "%s",
		rom_root);
	deadline = monotonic_ns() + (uint64_t)max_ms * 1000000ULL;
	for (size_t index = 0; index < directory_count; ++index) {
		struct platform_directory *directory = &directories[index];
		struct catalog_platform_record *record =
			find_record(&old.catalog, directory->name);

		if (record_matches(record, &directory->metadata)) {
			if (retain_old(&next, &old, record, directory->name) != 0)
				goto rebuild_fail;
			++reused;
			continue;
		}
		if (!blocked && monotonic_ns() < deadline) {
			struct ui scanned_ui = { 0 };
			struct stat after;
			uint64_t entries = 0U;
			uint64_t now = monotonic_ns();
			uint32_t remaining = now < deadline ?
				(uint32_t)((deadline - now) / 1000000ULL) : 0U;
			int scan_result;

			scanned_ui.routes = ui->routes;
			scan_result = remaining == 0U ? 1 :
				catalog_scan_platform(&scanned_ui, directory->path,
					directory->name, 0U, remaining, &entries);
			if (scan_result == 0 && lstat(directory->path, &after) == 0 &&
			    metadata_unchanged(&directory->metadata, &after)) {
				if (move_games(&next, &scanned_ui, directory->name) != 0 ||
				    add_fresh_record(&next.catalog, directory->name, &after,
					entries) != 0) {
					catalog_destroy(&scanned_ui);
					goto rebuild_fail;
				}
				++scanned;
			} else {
				blocked = true;
			}
			catalog_destroy(&scanned_ui);
		} else {
			blocked = true;
		}
		if (blocked) {
			if (next.catalog.refresh_cursor[0] == '\0')
				(void)snprintf(next.catalog.refresh_cursor,
					sizeof(next.catalog.refresh_cursor), "%s",
					directory->name);
			if (retain_old(&next, &old, record, directory->name) != 0)
				goto rebuild_fail;
		}
	}
	catalog_destroy(&old);
	ui->catalog = next.catalog;
	memset(&next.catalog, 0, sizeof(next.catalog));
	catalog_apply_filters(ui);
	if (catalog_snapshot_write(ui, path, rom_root, !blocked) != 0)
		goto out;
	(void)fprintf(stderr,
		"CATALOG_INCREMENTAL mode=%s platforms=%zu scanned=%zu reused=%zu cursor=%s\n",
		loaded ? "merge" : "cold", directory_count, scanned, reused,
		ui->catalog.refresh_cursor[0] == '\0' ? "none" :
		ui->catalog.refresh_cursor);
	result = blocked ? 1 : 0;
	goto out;

rebuild_fail:
	catalog_destroy(&old);
	catalog_destroy(&next);
out:
	free_directories(directories, directory_count);
	return result;
}
