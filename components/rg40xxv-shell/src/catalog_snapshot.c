#define _POSIX_C_SOURCE 200809L

#include "ui.h"

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SNAPSHOT_SCHEMA "RG40XXV-CATALOG-V3"
#define SNAPSHOT_MAX_BYTES (32U * 1024U * 1024U)
#define SNAPSHOT_MAX_GAMES 65536U
#define SNAPSHOT_MAX_PLATFORMS 4096U
#define SNAPSHOT_MAX_LINE (PATH_MAX * 6U)

struct platform_directory {
	char *name;
	char path[PATH_MAX];
	struct stat metadata;
};

static uint64_t monotonic_ns(void)
{
	struct timespec value;

	if (clock_gettime(CLOCK_MONOTONIC, &value) != 0)
		return 0;
	return (uint64_t)value.tv_sec * 1000000000ULL + (uint64_t)value.tv_nsec;
}

static int route_fingerprint(const struct ui *ui, uint64_t *mtime_ns,
			     uint64_t *size)
{
	struct stat metadata;

	if (ui->routes.source_path[0] == '\0' ||
	    stat(ui->routes.source_path, &metadata) != 0 ||
	    !S_ISREG(metadata.st_mode))
		return -1;
	*mtime_ns = (uint64_t)metadata.st_mtim.tv_sec * 1000000000ULL +
		(uint64_t)metadata.st_mtim.tv_nsec;
	*size = (uint64_t)metadata.st_size;
	return 0;
}

static int grow_games(struct catalog_state *catalog)
{
	size_t capacity;
	struct game_entry *games;

	if (catalog->game_count < catalog->game_capacity)
		return 0;
	capacity = catalog->game_capacity == 0U ? 256U :
		catalog->game_capacity * 2U;
	if (capacity > SNAPSHOT_MAX_GAMES)
		capacity = SNAPSHOT_MAX_GAMES;
	if (capacity <= catalog->game_capacity)
		return -1;
	games = realloc(catalog->games, capacity * sizeof(*games));
	if (games == NULL)
		return -1;
	catalog->games = games;
	catalog->game_capacity = capacity;
	return 0;
}

static int add_unique(char ***values, size_t *count, size_t *capacity,
		      const char *value)
{
	char **grown;
	size_t next;

	for (size_t index = 0; index < *count; ++index) {
		if (strcmp((*values)[index], value) == 0)
			return 0;
	}
	if (*count >= *capacity) {
		next = *capacity == 0U ? 16U : *capacity * 2U;
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

static void free_game(struct game_entry *game)
{
	free(game->title);
	free(game->path);
	free(game->cover_path);
	free(game->system);
	free(game->system_label);
	free(game->core);
	free(game->frontend);
	free(game->runtime);
	free(game->fallback);
	memset(game, 0, sizeof(*game));
}

static int compare_text(const void *left, const void *right)
{
	const char *const *a = left;
	const char *const *b = right;

	return strcmp(*a, *b);
}

static int compare_game(const void *left, const void *right)
{
	const struct game_entry *a = left;
	const struct game_entry *b = right;
	int system = strcmp(a->system, b->system);

	return system != 0 ? system : strcmp(a->title, b->title);
}

static int hex_value(char value)
{
	if (value >= '0' && value <= '9')
		return value - '0';
	if (value >= 'a' && value <= 'f')
		return value - 'a' + 10;
	if (value >= 'A' && value <= 'F')
		return value - 'A' + 10;
	return -1;
}

static char *decode_token(const char *token, size_t maximum)
{
	size_t encoded;
	size_t length;
	char *value;

	if (strcmp(token, "-") == 0)
		return strdup("");
	encoded = strlen(token);
	if (encoded == 0U || (encoded & 1U) != 0U || encoded / 2U > maximum)
		return NULL;
	length = encoded / 2U;
	value = malloc(length + 1U);
	if (value == NULL)
		return NULL;
	for (size_t index = 0; index < length; ++index) {
		int high = hex_value(token[index * 2U]);
		int low = hex_value(token[index * 2U + 1U]);

		if (high < 0 || low < 0) {
			free(value);
			return NULL;
		}
		value[index] = (char)((unsigned int)high * 16U + (unsigned int)low);
		if (value[index] == '\0') {
			free(value);
			return NULL;
		}
	}
	value[length] = '\0';
	return value;
}

static int write_token(FILE *stream, const char *value)
{
	static const char digits[] = "0123456789abcdef";
	const unsigned char *cursor = (const unsigned char *)value;

	if (*cursor == '\0')
		return fputc('-', stream) == EOF ? -1 : 0;
	while (*cursor != '\0') {
		if (fputc(digits[*cursor >> 4U], stream) == EOF ||
		    fputc(digits[*cursor & 0x0fU], stream) == EOF)
			return -1;
		++cursor;
	}
	return 0;
}

static int add_snapshot_game(struct ui *ui, struct game_entry *game)
{
	struct catalog_state *catalog = &ui->catalog;

	if (catalog->game_count >= SNAPSHOT_MAX_GAMES || grow_games(catalog) != 0 ||
	    add_unique(&catalog->systems, &catalog->system_count,
		       &catalog->system_capacity, game->system) != 0 ||
	    add_unique(&catalog->cores, &catalog->core_count,
		       &catalog->core_capacity, game->core) != 0)
		return -1;
	catalog->games[catalog->game_count++] = *game;
	memset(game, 0, sizeof(*game));
	return 0;
}

static int add_platform_record(struct catalog_state *catalog,
			       struct catalog_platform_record *record)
{
	struct catalog_platform_record *grown;
	size_t next;

	if (catalog->platform_count >= SNAPSHOT_MAX_PLATFORMS)
		return -1;
	if (catalog->platform_count >= catalog->platform_capacity) {
		next = catalog->platform_capacity == 0U ? 32U :
			catalog->platform_capacity * 2U;
		grown = realloc(catalog->platforms, next * sizeof(*grown));
		if (grown == NULL)
			return -1;
		catalog->platforms = grown;
		catalog->platform_capacity = next;
	}
	catalog->platforms[catalog->platform_count++] = *record;
	memset(record, 0, sizeof(*record));
	return 0;
}

static int parse_platform_line(struct catalog_state *catalog, char *line)
{
	struct catalog_platform_record record = { 0 };
	unsigned long long values[5];
	char *save = NULL;
	char *token = strtok_r(line, " ", &save);
	char *end;

	if (token == NULL || strcmp(token, "platform") != 0)
		return -1;
	for (size_t index = 0; index < 5U; ++index) {
		token = strtok_r(NULL, " ", &save);
		if (token == NULL)
			goto fail;
		errno = 0;
		values[index] = strtoull(token, &end, 10);
		if (errno != 0 || end == token || *end != '\0')
			goto fail;
	}
	token = strtok_r(NULL, " ", &save);
	if (token == NULL || strtok_r(NULL, " ", &save) != NULL ||
	    (record.name = decode_token(token, NAME_MAX)) == NULL ||
	    record.name[0] == '\0' || strchr(record.name, '/') != NULL)
		goto fail;
	record.device = values[0];
	record.inode = values[1];
	record.mtime_ns = values[2];
	record.ctime_ns = values[3];
	record.entry_count = values[4];
	if (add_platform_record(catalog, &record) != 0)
		goto fail;
	return 0;

fail:
	free(record.name);
	return -1;
}

static int parse_game_line(struct ui *ui, char *line)
{
	static const size_t limits[] = {
		PATH_MAX - 1U, PATH_MAX - 1U, PATH_MAX - 1U, 64U, 128U,
		96U, 96U, 96U, 96U,
	};
	struct game_entry game = { 0 };
	char *save = NULL;
	char *token;
	char *end;
	long long modified;
	long playable;
	char **fields[] = {
		&game.title, &game.path, &game.cover_path, &game.system,
		&game.system_label, &game.core, &game.frontend, &game.runtime,
		&game.fallback,
	};

	token = strtok_r(line, " ", &save);
	if (token == NULL || strcmp(token, "game") != 0)
		return -1;
	token = strtok_r(NULL, " ", &save);
	if (token == NULL)
		return -1;
	errno = 0;
	modified = strtoll(token, &end, 10);
	if (errno != 0 || *end != '\0')
		return -1;
	token = strtok_r(NULL, " ", &save);
	if (token == NULL)
		return -1;
	errno = 0;
	playable = strtol(token, &end, 10);
	if (errno != 0 || *end != '\0' || (playable != 0 && playable != 1))
		return -1;
	for (size_t index = 0; index < sizeof(fields) / sizeof(fields[0]); ++index) {
		token = strtok_r(NULL, " ", &save);
		if (token == NULL || (*fields[index] =
		    decode_token(token, limits[index])) == NULL)
			goto fail;
	}
	if (strtok_r(NULL, " ", &save) != NULL || game.path[0] != '/' ||
	    game.cover_path[0] != '/' || game.system[0] == '\0' ||
	    game.core[0] == '\0')
		goto fail;
	game.modified = (time_t)modified;
	game.playable = playable != 0;
	if (add_snapshot_game(ui, &game) != 0)
		goto fail;
	return 0;

fail:
	free_game(&game);
	return -1;
}

int catalog_snapshot_load(struct ui *ui, const char *path,
			  const char *rom_root, bool *complete)
{
	struct stat metadata;
	FILE *stream = NULL;
	char *line = NULL;
	size_t capacity = 0U;
	ssize_t length;
	char *decoded_root = NULL;
	char *decoded_cursor = NULL;
	uint64_t expected_mtime;
	uint64_t expected_size;
	unsigned long long stored_mtime;
	unsigned long long stored_size;
	size_t stored_routes;
	unsigned int stored_rpg_truth;
	int stored_rpg_launchable;
	size_t expected_count = 0U;
	size_t stored_platforms = 0U;
	size_t expected_platforms = 0U;
	int stored_complete;
	int result = -1;
	bool found_end = false;
	int fd;

	if (complete != NULL)
		*complete = false;
	if (path == NULL || path[0] != '/' || rom_root == NULL)
		return -1;
	fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return -1;
	if (fstat(fd, &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
	    metadata.st_uid != geteuid() || metadata.st_nlink != 1 ||
	    (metadata.st_mode & 0022) != 0 || metadata.st_size <= 0 ||
	    (uint64_t)metadata.st_size > SNAPSHOT_MAX_BYTES) {
		(void)close(fd);
		return -1;
	}
	stream = fdopen(fd, "r");
	if (stream == NULL) {
		(void)close(fd);
		return -1;
	}
	length = getline(&line, &capacity, stream);
	if (length <= 0 || (size_t)length > SNAPSHOT_MAX_LINE ||
	    strcmp(line, SNAPSHOT_SCHEMA "\n") != 0)
		goto out;
	length = getline(&line, &capacity, stream);
	if (length <= 6 || strncmp(line, "root ", 5) != 0)
		goto out;
	line[strcspn(line, "\r\n")] = '\0';
	decoded_root = decode_token(line + 5, PATH_MAX - 1U);
	if (decoded_root == NULL || strcmp(decoded_root, rom_root) != 0)
		goto out;
	if (route_fingerprint(ui, &expected_mtime, &expected_size) != 0)
		goto out;
	length = getline(&line, &capacity, stream);
	if (length <= 0 || sscanf(line, "routes %zu %llu %llu", &stored_routes,
					   &stored_mtime, &stored_size) != 3 ||
		    stored_routes != ui->routes.count || stored_mtime != expected_mtime ||
		    stored_size != expected_size)
		goto out;
	length = getline(&line, &capacity, stream);
	if (length <= 0 ||
	    sscanf(line, "rpg-capability %u %d", &stored_rpg_truth,
		   &stored_rpg_launchable) != 2 ||
	    stored_rpg_truth > 4U ||
	    (stored_rpg_launchable != 0 && stored_rpg_launchable != 1) ||
	    stored_rpg_truth != ui->routes.rpg_truth_level ||
	    (stored_rpg_launchable != 0) != ui->routes.rpg_launchable)
		goto out;
	length = getline(&line, &capacity, stream);
	if (length <= 0 || sscanf(line, "complete %d", &stored_complete) != 1 ||
	    (stored_complete != 0 && stored_complete != 1))
		goto out;
	length = getline(&line, &capacity, stream);
	if (length <= 7 || strncmp(line, "cursor ", 7) != 0)
		goto out;
	line[strcspn(line, "\r\n")] = '\0';
	decoded_cursor = decode_token(line + 7, NAME_MAX);
	if (decoded_cursor == NULL || strchr(decoded_cursor, '/') != NULL)
		goto out;
	length = getline(&line, &capacity, stream);
	if (length <= 0 || sscanf(line, "platforms %zu", &stored_platforms) != 1 ||
	    stored_platforms > SNAPSHOT_MAX_PLATFORMS)
		goto out;
	catalog_destroy(ui);
	if (snprintf(ui->catalog.rom_root, sizeof(ui->catalog.rom_root), "%s",
		     rom_root) < 0 || strlen(rom_root) >= sizeof(ui->catalog.rom_root))
		goto fail_catalog;
	(void)snprintf(ui->catalog.refresh_cursor,
		sizeof(ui->catalog.refresh_cursor), "%s", decoded_cursor);
	for (size_t index = 0; index < stored_platforms; ++index) {
		length = getline(&line, &capacity, stream);
		if (length <= 0 || (size_t)length > SNAPSHOT_MAX_LINE)
			goto fail_catalog;
		line[strcspn(line, "\r\n")] = '\0';
		if (parse_platform_line(&ui->catalog, line) != 0)
			goto fail_catalog;
	}
	while ((length = getline(&line, &capacity, stream)) > 0) {
		if ((size_t)length > SNAPSHOT_MAX_LINE)
			goto fail_catalog;
		line[strcspn(line, "\r\n")] = '\0';
		if (strncmp(line, "end ", 4) == 0) {
			if (sscanf(line, "end %zu %zu", &expected_count,
				   &expected_platforms) != 2 ||
			    expected_count != ui->catalog.game_count ||
			    expected_platforms != ui->catalog.platform_count)
				goto fail_catalog;
			found_end = true;
			break;
		}
		if (parse_game_line(ui, line) != 0)
			goto fail_catalog;
	}
	if (!found_end || getline(&line, &capacity, stream) >= 0)
		goto fail_catalog;
	{
			qsort(ui->catalog.games, ui->catalog.game_count,
			      sizeof(*ui->catalog.games), compare_game);
			qsort(ui->catalog.systems, ui->catalog.system_count,
			      sizeof(*ui->catalog.systems), compare_text);
			qsort(ui->catalog.cores, ui->catalog.core_count,
			      sizeof(*ui->catalog.cores), compare_text);
			catalog_apply_filters(ui);
			if (complete != NULL)
				*complete = stored_complete != 0;
			result = 0;
			goto out;
	}

fail_catalog:
	catalog_destroy(ui);
out:
	free(decoded_root);
	free(decoded_cursor);
	free(line);
	(void)fclose(stream);
	return result;
}

static int write_snapshot_stream(FILE *stream, const struct ui *ui,
				 const char *rom_root, bool complete)
{
	uint64_t route_mtime;
	uint64_t route_size;

	if (route_fingerprint(ui, &route_mtime, &route_size) != 0 ||
	    fprintf(stream, SNAPSHOT_SCHEMA "\nroot ") < 0 ||
	    write_token(stream, rom_root) != 0 ||
	    fprintf(stream,
		    "\nroutes %zu %llu %llu\nrpg-capability %u %d\ncomplete %d\ncursor ",
			    ui->routes.count, (unsigned long long)route_mtime,
			    (unsigned long long)route_size,
			    ui->routes.rpg_truth_level,
			    ui->routes.rpg_launchable ? 1 : 0,
			    complete ? 1 : 0) < 0 ||
	    write_token(stream, ui->catalog.refresh_cursor) != 0 ||
	    fprintf(stream, "\nplatforms %zu\n", ui->catalog.platform_count) < 0)
		return -1;
	for (size_t index = 0; index < ui->catalog.platform_count; ++index) {
		const struct catalog_platform_record *record =
			&ui->catalog.platforms[index];

		if (fprintf(stream, "platform %llu %llu %llu %llu %llu ",
			(unsigned long long)record->device,
			(unsigned long long)record->inode,
			(unsigned long long)record->mtime_ns,
			(unsigned long long)record->ctime_ns,
			(unsigned long long)record->entry_count) < 0 ||
		    write_token(stream, record->name) != 0 ||
		    fputc('\n', stream) == EOF)
			return -1;
	}
	for (size_t index = 0; index < ui->catalog.game_count; ++index) {
		const struct game_entry *game = &ui->catalog.games[index];
		const char *fields[] = {
			game->title, game->path, game->cover_path, game->system,
			game->system_label, game->core, game->frontend, game->runtime,
			game->fallback,
		};

		if (fprintf(stream, "game %lld %d", (long long)game->modified,
			    game->playable ? 1 : 0) < 0)
			return -1;
		for (size_t field = 0; field < sizeof(fields) / sizeof(fields[0]);
		     ++field) {
			if (fputc(' ', stream) == EOF ||
			    write_token(stream, fields[field]) != 0)
				return -1;
		}
		if (fputc('\n', stream) == EOF)
			return -1;
	}
	return fprintf(stream, "end %zu %zu\n", ui->catalog.game_count,
		ui->catalog.platform_count) < 0 ? -1 : 0;
}

int catalog_snapshot_write(const struct ui *ui, const char *path,
			   const char *rom_root, bool complete)
{
	char directory[PATH_MAX];
	char temporary[PATH_MAX];
	const char *separator;
	struct stat metadata;
	FILE *stream = NULL;
	int fd = -1;
	int directory_fd = -1;
	int result = -1;
	int temporary_length;
	size_t directory_length;

	if (path == NULL || path[0] != '/' || rom_root == NULL)
		return -1;
	separator = strrchr(path, '/');
	if (separator == NULL || separator[1] == '\0')
		return -1;
	directory_length = separator == path ? 1U : (size_t)(separator - path);
	if (directory_length >= sizeof(directory))
		return -1;
	memcpy(directory, path, directory_length);
	directory[directory_length] = '\0';
	if (lstat(directory, &metadata) != 0 || !S_ISDIR(metadata.st_mode) ||
	    S_ISLNK(metadata.st_mode) || metadata.st_uid != geteuid())
		return -1;
	if (lstat(path, &metadata) == 0) {
		if (!S_ISREG(metadata.st_mode) || S_ISLNK(metadata.st_mode) ||
		    metadata.st_uid != geteuid())
			return -1;
	} else if (errno != ENOENT) {
		return -1;
	}
	temporary_length = snprintf(temporary, sizeof(temporary),
		"%s.tmp.%ld.%llu", path, (long)getpid(),
		(unsigned long long)monotonic_ns());
	if (temporary_length < 0 || (size_t)temporary_length >= sizeof(temporary))
		return -1;
	fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
		  0600);
	if (fd < 0)
		goto out;
	stream = fdopen(fd, "w");
	if (stream == NULL)
		goto out;
	fd = -1;
	if (write_snapshot_stream(stream, ui, rom_root, complete) != 0 ||
	    fflush(stream) != 0 || fsync(fileno(stream)) != 0 ||
	    fclose(stream) != 0) {
		stream = NULL;
		goto out;
	}
	stream = NULL;
	if (rename(temporary, path) != 0)
		goto out;
	temporary[0] = '\0';
	directory_fd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC |
			    O_NOFOLLOW);
	if (directory_fd < 0 || fsync(directory_fd) != 0)
		goto out;
	result = 0;

out:
	if (stream != NULL)
		(void)fclose(stream);
	else if (fd >= 0)
		(void)close(fd);
	if (directory_fd >= 0)
		(void)close(directory_fd);
	if (temporary[0] != '\0')
		(void)unlink(temporary);
	return result;
}
