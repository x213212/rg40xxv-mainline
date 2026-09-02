#define _POSIX_C_SOURCE 200809L

#include "ui.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

static int write_all(int fd, const char *payload)
{
	size_t left = strlen(payload);

	while (left > 0) {
		ssize_t written = write(fd, payload, left);

		if (written < 0 && errno == EINTR)
			continue;
		if (written <= 0)
			return -1;
		payload += written;
		left -= (size_t)written;
	}
	return 0;
}

static void sync_parent(const char *path)
{
	char directory[PATH_MAX];
	char *slash;
	int fd;

	if (snprintf(directory, sizeof(directory), "%s", path) < 0)
		return;
	slash = strrchr(directory, '/');
	if (slash == NULL)
		(void)snprintf(directory, sizeof(directory), ".");
	else if (slash == directory)
		directory[1] = '\0';
	else
		*slash = '\0';
	fd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd >= 0) {
		(void)fsync(fd);
		(void)close(fd);
	}
}

static bool payload_matches_file(const char *path, const char *payload,
				 size_t payload_length)
{
	char buffer[4096];
	struct stat metadata;
	size_t offset = 0U;
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	bool matches = false;

	if (fd < 0)
		return false;
	if (fstat(fd, &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
	    metadata.st_uid != geteuid() || metadata.st_nlink != 1 ||
	    metadata.st_size < 0 || (uint64_t)metadata.st_size != payload_length)
		goto out;
	while (offset < payload_length) {
		size_t requested = payload_length - offset;
		ssize_t count;

		if (requested > sizeof(buffer))
			requested = sizeof(buffer);
		do {
			count = read(fd, buffer, requested);
		} while (count < 0 && errno == EINTR);
		if (count <= 0 ||
		    memcmp(buffer, payload + offset, (size_t)count) != 0)
			goto out;
		offset += (size_t)count;
	}
	matches = true;

out:
	(void)close(fd);
	return matches;
}

static int atomic_write(const char *path, const char *payload)
{
	char temporary[PATH_MAX];
	size_t payload_length = strlen(payload);
	int fd;
	int length;
	int result = -1;

	length = snprintf(temporary, sizeof(temporary), "%s.tmp", path);
	if (path[0] == '\0' || length < 0 || (size_t)length >= sizeof(temporary))
		return -1;
	/* Avoid an fsync+rename cycle when the durable bytes already match. */
	if (payload_matches_file(path, payload, payload_length))
		return 0;
	fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0)
		return -1;
	if (write_all(fd, payload) == 0 && fsync(fd) == 0 && close(fd) == 0) {
		fd = -1;
		if (rename(temporary, path) == 0) {
			sync_parent(path);
			result = 0;
		}
	}
	if (fd >= 0)
		(void)close(fd);
	if (result != 0)
		(void)remove(temporary);
	return result;
}

static int persistence_thread(void *argument)
{
	struct persistence_worker *worker = argument;

	(void)setpriority(PRIO_PROCESS, 0, 10);
	for (;;) {
		struct persistence_item pending[PERSISTENCE_ITEM_COUNT] = { 0 };

		(void)SDL_LockMutex(worker->mutex);
		while (worker->running && !worker->dirty)
			(void)SDL_CondWait(worker->condition, worker->mutex);
		if (!worker->running && !worker->dirty) {
			(void)SDL_UnlockMutex(worker->mutex);
			break;
		}
		if (worker->running && !SDL_TICKS_PASSED(SDL_GetTicks(),
							 worker->deadline)) {
			uint32_t wait = worker->deadline - SDL_GetTicks();

			(void)SDL_CondWaitTimeout(worker->condition, worker->mutex, wait);
			(void)SDL_UnlockMutex(worker->mutex);
			continue;
		}
		for (size_t i = 0; i < PERSISTENCE_ITEM_COUNT; ++i) {
			pending[i] = worker->items[i];
			memset(&worker->items[i], 0, sizeof(worker->items[i]));
		}
		worker->dirty = false;
		(void)SDL_UnlockMutex(worker->mutex);
		for (size_t i = 0; i < PERSISTENCE_ITEM_COUNT; ++i) {
			if (pending[i].payload != NULL &&
			    atomic_write(pending[i].path, pending[i].payload) != 0) {
				(void)SDL_LockMutex(worker->mutex);
				++worker->write_failures;
				(void)SDL_UnlockMutex(worker->mutex);
			}
			free(pending[i].payload);
		}
	}
	return 0;
}

int persistence_start(struct ui *ui)
{
	struct persistence_worker *worker = &ui->persistence;

	memset(worker, 0, sizeof(*worker));
	worker->mutex = SDL_CreateMutex();
	worker->condition = SDL_CreateCond();
	if (worker->mutex == NULL || worker->condition == NULL)
		goto fail;
	worker->running = true;
	worker->thread = SDL_CreateThread(persistence_thread, "state-writer", worker);
	if (worker->thread == NULL)
		goto fail;
	return 0;
fail:
	if (worker->condition != NULL)
		SDL_DestroyCond(worker->condition);
	if (worker->mutex != NULL)
		SDL_DestroyMutex(worker->mutex);
	memset(worker, 0, sizeof(*worker));
	return -1;
}

static void queue_payload(struct ui *ui, enum persistence_item_id id,
			  const char *path, char *payload)
{
	struct persistence_worker *worker = &ui->persistence;

	if (payload == NULL || path[0] == '\0' || worker->mutex == NULL) {
		free(payload);
		return;
	}
	(void)SDL_LockMutex(worker->mutex);
	free(worker->items[id].payload);
	worker->items[id].payload = payload;
	(void)snprintf(worker->items[id].path, sizeof(worker->items[id].path),
		       "%s", path);
	worker->dirty = true;
	worker->deadline = SDL_GetTicks() + 200U;
	(void)SDL_CondSignal(worker->condition);
	(void)SDL_UnlockMutex(worker->mutex);
}

void persistence_request_favorites(struct ui *ui)
{
	size_t size = 1;
	char *payload;
	char *cursor;

	for (size_t i = 0; i < ui->catalog.game_count; ++i) {
		const struct game_entry *game = &ui->catalog.games[i];

		if (game->favorite) {
			size_t add = strlen(game->path) + strlen(game->content_hash) + 2U;

			if (SIZE_MAX - size < add)
				return;
			size += add;
		}
	}
	payload = malloc(size);
	if (payload == NULL)
		return;
	cursor = payload;
	for (size_t i = 0; i < ui->catalog.game_count; ++i) {
		const struct game_entry *game = &ui->catalog.games[i];

		if (game->favorite) {
			size_t path_length = strlen(game->path);
			size_t hash_length = strlen(game->content_hash);

			memcpy(cursor, game->path, path_length);
			cursor += path_length;
			*cursor++ = '\t';
			memcpy(cursor, game->content_hash, hash_length);
			cursor += hash_length;
			*cursor++ = '\n';
		}
	}
	*cursor = '\0';
	queue_payload(ui, PERSISTENCE_FAVORITES, ui->catalog.favorites_path, payload);
}

void persistence_request_filters(struct ui *ui)
{
	const struct catalog_state *catalog = &ui->catalog;
	const char *system = catalog->system_filter == 0 ||
		catalog->system_filter > catalog->system_count ? "" :
		catalog->systems[catalog->system_filter - 1];
	const char *core = catalog->core_filter == 0 ||
		catalog->core_filter > catalog->core_count ? "" :
		catalog->cores[catalog->core_filter - 1];
	size_t size = strlen(system) + strlen(core) + 96U;
	char *payload = malloc(size);

	if (payload == NULL)
		return;
	(void)snprintf(payload, size,
		"system=%s\ncore=%s\nfavorites=%d\nrecent=%d\nsearch_scope=%s\n",
		system, core, 0, 0,
		catalog->search_all_systems ? "all" : "current");
	queue_payload(ui, PERSISTENCE_FILTERS, catalog->filter_state_path, payload);
}

void persistence_request_locale(struct ui *ui)
{
	char *payload = malloc(224U);

	if (payload != NULL)
		(void)snprintf(payload, 224U,
			"language=%s\nscreen_lock=%d\nauto_screen_off=%d\nbrightness=%d\njoystick_rgb=%d\nusb_debug=%d\nboot_target=custom\n",
			ui->locale.language == UI_LANGUAGE_ZH_TW ? "zh_TW" : "en",
			ui->settings.preferences.screen_lock_enabled ? 1 : 0,
			ui->settings.preferences.auto_screen_off_minutes,
			ui->settings.preferences.backlight_percent,
			ui->settings.preferences.joystick_rgb_brightness,
			ui->settings.preferences.usb_debug_enabled ? 1 : 0);

	queue_payload(ui, PERSISTENCE_LOCALE, ui->locale.settings_path, payload);
}

void persistence_request_history(struct ui *ui)
{
	size_t ids[HISTORY_MAX];
	size_t count = 0;
	size_t size = 1;
	char *payload;
	char *cursor;

	for (size_t game_id = 0; game_id < ui->catalog.game_count; ++game_id) {
		const struct game_entry *game = &ui->catalog.games[game_id];
		size_t position;

		if (!game->recent || game->last_played <= 0)
			continue;
		position = count;
		if (position > HISTORY_MAX - 1)
			position = HISTORY_MAX - 1;
		while (position > 0 &&
		       ui->catalog.games[ids[position - 1]].last_played <
		       game->last_played) {
			if (position < HISTORY_MAX)
				ids[position] = ids[position - 1];
			--position;
		}
		if (position < HISTORY_MAX)
			ids[position] = game_id;
		if (count < HISTORY_MAX)
			++count;
	}
	for (size_t i = 0; i < count; ++i) {
		const struct game_entry *game = &ui->catalog.games[ids[i]];
		size_t add = strlen(game->path) + 32U;

		if (SIZE_MAX - size < add)
			return;
		size += add;
	}
	payload = malloc(size);
	if (payload == NULL)
		return;
	cursor = payload;
	for (size_t i = 0; i < count; ++i) {
		const struct game_entry *game = &ui->catalog.games[ids[i]];
		int written = snprintf(cursor, size - (size_t)(cursor - payload),
				       "%lld\t%s\n",
				       (long long)game->last_played, game->path);

		if (written < 0 || (size_t)written >=
		    size - (size_t)(cursor - payload)) {
			free(payload);
			return;
		}
		cursor += written;
	}
	*cursor = '\0';
	queue_payload(ui, PERSISTENCE_HISTORY, ui->catalog.history_path, payload);
}

void persistence_stop(struct ui *ui)
{
	struct persistence_worker *worker = &ui->persistence;

	if (worker->mutex != NULL) {
		(void)SDL_LockMutex(worker->mutex);
		worker->running = false;
		(void)SDL_CondSignal(worker->condition);
		(void)SDL_UnlockMutex(worker->mutex);
	}
	if (worker->thread != NULL)
		SDL_WaitThread(worker->thread, NULL);
	for (size_t i = 0; i < PERSISTENCE_ITEM_COUNT; ++i)
		free(worker->items[i].payload);
	if (worker->condition != NULL)
		SDL_DestroyCond(worker->condition);
	if (worker->mutex != NULL)
		SDL_DestroyMutex(worker->mutex);
	memset(worker, 0, sizeof(*worker));
}
