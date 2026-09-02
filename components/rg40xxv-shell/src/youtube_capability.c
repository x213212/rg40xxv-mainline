#define _POSIX_C_SOURCE 200809L

#include "ui.h"
#include "youtube_capability.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum {
	YOUTUBE_ADMISSION_MAX = 8192,
};

enum youtube_admission_field {
	YT_SCHEMA,
	YT_NATIVE_ROUTE,
	YT_NATIVE_LAUNCHER,
	YT_EVIDENCE_SCOPE,
	YT_NATIVE_CONTROLLER,
	YT_URL_RESOLVER,
	YT_RANGE_BRIDGE,
	YT_H264_DECODE,
	YT_AAC_DECODE,
	YT_DRM_KMS,
	YT_ALSA_AUDIO,
	YT_INPUT,
	YT_SESSION_RETURN,
	YT_MEMORY_BUDGET,
	YT_FIELD_COUNT,
};

static const char *const admission_keys[YT_FIELD_COUNT] = {
	[YT_SCHEMA] = "schema",
	[YT_NATIVE_ROUTE] = "native_route",
	[YT_NATIVE_LAUNCHER] = "native_launcher",
	[YT_EVIDENCE_SCOPE] = "evidence_scope",
	[YT_NATIVE_CONTROLLER] = "native_controller_ui",
	[YT_URL_RESOLVER] = "url_resolver",
	[YT_RANGE_BRIDGE] = "range_bridge",
	[YT_H264_DECODE] = "h264_decode",
	[YT_AAC_DECODE] = "aac_decode",
	[YT_DRM_KMS] = "drm_kms_display",
	[YT_ALSA_AUDIO] = "alsa_audio",
	[YT_INPUT] = "input",
	[YT_SESSION_RETURN] = "session_return",
	[YT_MEMORY_BUDGET] = "memory_budget",
};

static bool secure_file(const struct stat *status, bool executable)
{
	if (!S_ISREG(status->st_mode) || status->st_uid != geteuid() ||
	    status->st_nlink == 0 || (status->st_mode & (S_IWGRP | S_IWOTH)) != 0)
		return false;
	/* Release materialization intentionally hard-links identical immutable
	 * payloads across generations.  Link count is therefore not a mutability
	 * signal; ownership, mode and the O_NOFOLLOW descriptor are the security
	 * boundary. */
	return !executable || (status->st_mode & S_IXUSR) != 0;
}

static int secure_open(const char *path, bool executable, struct stat *status)
{
	int fd;

	if (path == NULL || path[0] != '/')
		return -1;
	fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return -1;
	if (fstat(fd, status) != 0 || !secure_file(status, executable)) {
		(void)close(fd);
		errno = EPERM;
		return -1;
	}
	return fd;
}

static int read_admission(const char *path, char *buffer, size_t size)
{
	struct stat status;
	ssize_t count;
	ssize_t total = 0;
	int fd = secure_open(path, false, &status);

	if (fd < 0)
		return -1;
	if (status.st_size < 0 || (uint64_t)status.st_size >= size ||
	    (uint64_t)status.st_size > YOUTUBE_ADMISSION_MAX) {
		(void)close(fd);
		errno = EFBIG;
		return -1;
	}
	while ((size_t)total + 1U < size) {
		count = read(fd, buffer + total, size - (size_t)total - 1U);
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			break;
		total += count;
	}
	if (close(fd) != 0 || total != status.st_size) {
		errno = EIO;
		return -1;
	}
	buffer[total] = '\0';
	return 0;
}

static int field_for_key(const char *key)
{
	for (int field = 0; field < YT_FIELD_COUNT; ++field) {
		if (strcmp(key, admission_keys[field]) == 0)
			return field;
	}
	return -1;
}

static bool exact_pass(char *const values[YT_FIELD_COUNT], int field)
{
	return values[field] != NULL && strcmp(values[field], "PASS") == 0;
}

static bool valid_gate(char *const values[YT_FIELD_COUNT], int field)
{
	return values[field] != NULL &&
		(strcmp(values[field], "PASS") == 0 ||
		 strcmp(values[field], "UNVERIFIED") == 0 ||
		 strcmp(values[field], "FAIL") == 0);
}

static bool all_fields_present(char *const values[YT_FIELD_COUNT])
{
	for (int field = 0; field < YT_FIELD_COUNT; ++field) {
		if (values[field] == NULL)
			return false;
	}
	return true;
}

static bool secure_launcher(const char *path)
{
	struct stat status;
	int fd = secure_open(path, true, &status);

	if (fd < 0)
		return false;
	(void)close(fd);
	return true;
}

int youtube_capability_load(struct youtube_capability *capability,
			    const char *path)
{
	char buffer[YOUTUBE_ADMISSION_MAX + 1U];
	char *values[YT_FIELD_COUNT] = { 0 };
	char *cursor;
	char *line;
	bool malformed = false;
	bool complete;
	bool route_ready;

	if (capability == NULL) {
		errno = EINVAL;
		return -1;
	}
	memset(capability, 0, sizeof(*capability));
	if (read_admission(path, buffer, sizeof(buffer)) != 0)
		return -1;
	for (line = strtok_r(buffer, "\n", &cursor); line != NULL;
	     line = strtok_r(NULL, "\n", &cursor)) {
		char *separator;
		int field;

		if (line[0] == '\0' || line[0] == '#')
			continue;
		if (strchr(line, '\r') != NULL || strchr(line, '\t') != NULL ||
		    line[0] == ' ' || line[strlen(line) - 1U] == ' ') {
			malformed = true;
			break;
		}
		separator = strchr(line, '=');
		if (separator == NULL || separator == line || separator[1] == '\0' ||
		    strchr(separator + 1, '=') != NULL) {
			malformed = true;
			break;
		}
		*separator = '\0';
		field = field_for_key(line);
		if (field < 0 || values[field] != NULL) {
			malformed = true;
			break;
		}
		values[field] = separator + 1;
	}
	complete = !malformed && all_fields_present(values) &&
		strcmp(values[YT_SCHEMA],
		       "rg40xxv-youtube-ui-admission-v3") == 0 &&
		strcmp(values[YT_NATIVE_ROUTE], "native-texture") == 0 &&
		strcmp(values[YT_EVIDENCE_SCOPE], "COMPONENT_GATE") == 0 &&
		valid_gate(values, YT_NATIVE_CONTROLLER) &&
		valid_gate(values, YT_URL_RESOLVER) &&
		valid_gate(values, YT_RANGE_BRIDGE) &&
		valid_gate(values, YT_H264_DECODE) &&
		valid_gate(values, YT_AAC_DECODE) &&
		valid_gate(values, YT_DRM_KMS) &&
		valid_gate(values, YT_ALSA_AUDIO) &&
		valid_gate(values, YT_INPUT) &&
		valid_gate(values, YT_SESSION_RETURN) &&
		valid_gate(values, YT_MEMORY_BUDGET);
	route_ready = complete &&
	    exact_pass(values, YT_NATIVE_CONTROLLER) &&
	    exact_pass(values, YT_URL_RESOLVER) &&
	    exact_pass(values, YT_RANGE_BRIDGE) &&
	    exact_pass(values, YT_H264_DECODE) &&
	    exact_pass(values, YT_AAC_DECODE) &&
	    exact_pass(values, YT_DRM_KMS) && exact_pass(values, YT_ALSA_AUDIO) &&
	    exact_pass(values, YT_INPUT) && exact_pass(values, YT_SESSION_RETURN) &&
	    secure_launcher(values[YT_NATIVE_LAUNCHER]);
	if (route_ready) {
		(void)snprintf(capability->native_launcher,
			sizeof(capability->native_launcher), "%s",
			values[YT_NATIVE_LAUNCHER]);
		capability->native_available = true;
		capability->native_launchable =
			strcmp(values[YT_MEMORY_BUDGET], "FAIL") != 0;
		/* COMPONENT_GATE admits a candidate for its acceptance run.  It never
		 * promotes that exact binary to device-verified; the release contract
		 * and separately bound live evidence own that assertion. */
		capability->native_device_verified = false;
	}
	if (malformed) {
		memset(capability, 0, sizeof(*capability));
		errno = EINVAL;
		return -1;
	}
	return capability->native_available ? 0 : -1;
}

static int catalog_add_unique(char ***items, size_t *count, size_t *capacity,
			      const char *value)
{
	for (size_t item = 0; item < *count; ++item) {
		if (strcmp((*items)[item], value) == 0)
			return 0;
	}
	if (*count >= *capacity) {
		size_t next = *capacity == 0U ? 8U : *capacity * 2U;
		char **grown = realloc(*items, next * sizeof(*grown));

		if (grown == NULL)
			return -1;
		*items = grown;
		*capacity = next;
	}
	(*items)[*count] = strdup(value);
	if ((*items)[*count] == NULL)
		return -1;
	++*count;
	return 0;
}

static void game_destroy(struct game_entry *game)
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

static bool tile_matches(const struct game_entry *game, const char *prefix)
{
	size_t length = strlen(prefix);

	return game->system != NULL && game->system_label != NULL &&
		game->title != NULL && strcmp(game->system, "APPS") == 0 &&
		strcmp(game->system_label, "YouTube") == 0 &&
		strncmp(game->title, prefix, length) == 0 &&
		(game->title[length] == '\0' || game->title[length] == ' ');
}

static int tile_update(struct game_entry *game, const char *title,
		       const char *launcher, const char *frontend,
		       const char *runtime, bool playable)
{
	char *next_title = strdup(title);
	char *next_path = strdup(launcher);
	char *next_frontend = strdup(frontend);
	char *next_runtime = strdup(runtime);

	if (next_title == NULL || next_path == NULL || next_frontend == NULL ||
	    next_runtime == NULL) {
		free(next_title);
		free(next_path);
		free(next_frontend);
		free(next_runtime);
		return -1;
	}
	free(game->title);
	free(game->path);
	free(game->frontend);
	free(game->runtime);
	game->title = next_title;
	game->path = next_path;
	game->frontend = next_frontend;
	game->runtime = next_runtime;
	game->playable = playable;
	return 0;
}

static int add_tile(struct ui *ui, const char *prefix, const char *title,
		    const char *launcher, const char *frontend,
		    const char *runtime, bool playable)
{
	struct catalog_state *catalog = &ui->catalog;
	struct game_entry game = { 0 };
	size_t insert_at = catalog->game_count;

	for (size_t item = 0; item < catalog->game_count; ++item) {
		if (tile_matches(&catalog->games[item], prefix))
			return tile_update(&catalog->games[item], title, launcher,
				frontend, runtime, playable);
		/* Release-owned applications are synthetic and are not part of the
		 * on-disk catalog snapshot.  Put YouTube at the start of Applications
		 * instead of silently appending it after every scanned utility. */
		if (insert_at == catalog->game_count &&
		    catalog->games[item].system != NULL &&
		    strcmp(catalog->games[item].system, "APPS") == 0)
			insert_at = item;
	}
	game.title = strdup(title);
	game.path = strdup(launcher);
	game.cover_path = strdup("");
	game.system = strdup("APPS");
	game.system_label = strdup("YouTube");
	game.core = strdup("native");
	game.frontend = strdup(frontend);
	game.runtime = strdup(runtime);
	game.fallback = strdup("");
	game.playable = playable;
	if (game.title == NULL || game.path == NULL || game.cover_path == NULL ||
	    game.system == NULL || game.system_label == NULL || game.core == NULL ||
	    game.frontend == NULL || game.runtime == NULL || game.fallback == NULL)
		goto fail;
	if (catalog->game_count >= catalog->game_capacity) {
		size_t next = catalog->game_capacity == 0U ? 16U :
			catalog->game_capacity * 2U;
		struct game_entry *grown = realloc(catalog->games,
			next * sizeof(*grown));

		if (grown == NULL)
			goto fail;
		catalog->games = grown;
		catalog->game_capacity = next;
	}
	if (catalog_add_unique(&catalog->systems, &catalog->system_count,
		&catalog->system_capacity, "APPS") != 0 ||
	    catalog_add_unique(&catalog->cores, &catalog->core_count,
		&catalog->core_capacity, "native") != 0)
		goto fail;
	if (insert_at < catalog->game_count)
		memmove(&catalog->games[insert_at + 1U],
			&catalog->games[insert_at],
			(catalog->game_count - insert_at) *
				sizeof(*catalog->games));
	catalog->games[insert_at] = game;
	++catalog->game_count;
	return 0;

fail:
	game_destroy(&game);
	return -1;
}

int youtube_catalog_add(struct ui *ui, const struct youtube_capability *capability)
{
	static const char canonical_native_launcher[] =
		"/opt/rg40xxv/youtube/bin/rg40xxv-youtube-native";
	bool native_available = capability != NULL &&
		capability->native_available;
	bool native_launchable = native_available && capability->native_launchable;
	bool native_device_verified = native_available &&
		capability->native_device_verified;

	if (ui == NULL) {
		errno = EINVAL;
		return -1;
	}
	/* The release-owned texture launcher is the sole active route.  A pending
	 * memory budget stays visible and launchable for its explicit device test;
	 * an observed failure remains visible but blocked. */
	if (add_tile(ui,
		"YouTube",
		native_launchable ? "YouTube" : "YouTube · UNAVAILABLE",
		native_available ? capability->native_launcher :
			canonical_native_launcher,
		native_available ? "Native texture player" :
			"Native texture route unavailable",
		native_device_verified ? "Native texture · device verified" :
			native_launchable ?
			"Native texture candidate · device acceptance pending" :
			native_available ?
			"Native texture · UNAVAILABLE: recorded memory failure" :
			"Native texture · UNAVAILABLE: admission/launcher route missing",
		native_launchable) != 0)
		return -1;
	catalog_apply_filters(ui);
	return 0;
}
