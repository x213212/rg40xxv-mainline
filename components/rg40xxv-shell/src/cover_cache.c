#define _POSIX_C_SOURCE 200809L

#include "ui.h"
#include "cover_limits.h"

#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

static SDL_Surface *thumbnail_surface(SDL_Surface *source)
{
	SDL_Surface *thumbnail;
	SDL_Rect destination = {
		0, 0, COVER_THUMBNAIL_MAX_WIDTH, COVER_THUMBNAIL_MAX_HEIGHT,
	};
	double scale;

	if (source == NULL ||
	    (source->w <= COVER_THUMBNAIL_MAX_WIDTH &&
	     source->h <= COVER_THUMBNAIL_MAX_HEIGHT))
		return source;
	scale = fmin((double)COVER_THUMBNAIL_MAX_WIDTH / source->w,
		     (double)COVER_THUMBNAIL_MAX_HEIGHT / source->h);
	destination.w = (int)(source->w * scale);
	destination.h = (int)(source->h * scale);
	thumbnail = SDL_CreateRGBSurfaceWithFormat(0, destination.w, destination.h,
						   32, SDL_PIXELFORMAT_ARGB8888);
	if (thumbnail == NULL || SDL_BlitScaled(source, NULL, thumbnail,
							&destination) != 0) {
		if (thumbnail != NULL)
			SDL_FreeSurface(thumbnail);
		SDL_FreeSurface(source);
		return NULL;
	}
	SDL_FreeSurface(source);
	return thumbnail;
}

static SDL_Surface *load_cover_bounded(const char *path,
				       uint64_t *decode_bytes)
{
	struct cover_dimensions dimensions;
	struct stat status;
	SDL_Surface *surface = NULL;
	SDL_RWops *rw;
	FILE *stream;
	uint64_t actual_bytes;
	int fd;

	*decode_bytes = 0;
	if (path == NULL)
		return NULL;
	fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return NULL;
	if (fstat(fd, &status) != 0 || !S_ISREG(status.st_mode) ||
	    status.st_size <= 0 ||
	    cover_probe_fd(fd, (uint64_t)status.st_size, &dimensions) !=
		COVER_PROBE_OK) {
		(void)close(fd);
		return NULL;
	}
	stream = fdopen(fd, "rb");
	if (stream == NULL) {
		(void)close(fd);
		return NULL;
	}
	rw = SDL_RWFromFP(stream, SDL_TRUE);
	if (rw == NULL) {
		(void)fclose(stream);
		return NULL;
	}
	surface = IMG_Load_RW(rw, 1);
	if (surface == NULL)
		return NULL;
	if (surface->w <= 0 || surface->h <= 0 || surface->pitch <= 0 ||
	    (uint32_t)surface->w != dimensions.width ||
	    (uint32_t)surface->h != dimensions.height) {
		SDL_FreeSurface(surface);
		return NULL;
	}
	actual_bytes = (uint64_t)(unsigned int)surface->pitch *
		(uint64_t)(unsigned int)surface->h;
	if (actual_bytes > COVER_DECODE_MAX_BYTES) {
		SDL_FreeSurface(surface);
		return NULL;
	}
	*decode_bytes = actual_bytes > dimensions.decode_bytes ? actual_bytes :
		dimensions.decode_bytes;
	return thumbnail_surface(surface);
}

static struct cover_item *find_item(struct ui *ui, size_t game_id)
{
	for (size_t i = 0; i < COVER_CACHE_MAX; ++i) {
		if (ui->covers[i].occupied && ui->covers[i].game_id == game_id)
			return &ui->covers[i];
	}
	return NULL;
}

static const int visible_priority_offsets[COVER_CACHE_MAX] = {
	0, -1, 1, -2, 2,
};

static size_t visible_game_id(const struct ui *ui, int offset)
{
	long long position;
	size_t count = ui->catalog.visible_count;

	if (count == 0)
		return SIZE_MAX;
	position = (long long)ui->game_index + offset;
	while (position < 0)
		position += (long long)count;
	position %= (long long)count;
	return catalog_visible_id(ui, (size_t)position);
}

static size_t current_visible_ids(const struct ui *ui,
				  size_t ids[COVER_CACHE_MAX])
{
	size_t count = 0;

	for (size_t priority = 0; priority < COVER_CACHE_MAX; ++priority) {
		size_t game_id = visible_game_id(ui,
						 visible_priority_offsets[priority]);
		bool duplicate = false;

		if (game_id == SIZE_MAX)
			continue;
		for (size_t i = 0; i < count; ++i) {
			if (ids[i] == game_id) {
				duplicate = true;
				break;
			}
		}
		if (!duplicate)
			ids[count++] = game_id;
	}
	return count;
}

static size_t visible_priority(const size_t *ids, size_t count,
			       size_t game_id)
{
	for (size_t i = 0; i < count; ++i) {
		if (ids[i] == game_id)
			return i;
	}
	return SIZE_MAX;
}

static bool game_id_is_current_visible(const struct ui *ui, size_t game_id)
{
	size_t ids[COVER_CACHE_MAX];
	size_t count = current_visible_ids(ui, ids);

	return visible_priority(ids, count, game_id) != SIZE_MAX;
}

static struct cover_item *replacement_item(struct ui *ui)
{
	struct cover_item *oldest = NULL;

	for (size_t i = 0; i < COVER_CACHE_MAX; ++i) {
		if (!ui->covers[i].occupied)
			return &ui->covers[i];
		if (game_id_is_current_visible(ui, ui->covers[i].game_id))
			continue;
		if (oldest == NULL ||
		    ui->covers[i].last_used < oldest->last_used)
			oldest = &ui->covers[i];
	}
	return oldest;
}

static size_t worker_visible_priority_locked(const struct cover_worker *worker,
					     size_t game_id)
{
	return visible_priority(worker->visible_game_ids,
				worker->visible_game_count, game_id);
}

static int worker_main(void *argument)
{
	struct ui *ui = argument;
	struct cover_worker *worker = &ui->cover_worker;

	(void)setpriority(PRIO_PROCESS, 0, 5);
	(void)SDL_LockMutex(worker->mutex);
	while (worker->running) {
		struct cover_job *job = NULL;
		size_t game_id;
		char *path;
		SDL_Surface *surface;
		uint64_t decode_bytes;

		for (size_t i = 0; i < COVER_CACHE_MAX; ++i) {
			if (worker->jobs[i].occupied && !worker->jobs[i].loading &&
			    !worker->jobs[i].ready &&
			    (job == NULL || worker->jobs[i].priority < job->priority)) {
				job = &worker->jobs[i];
			}
		}
		if (job == NULL) {
			(void)SDL_CondWait(worker->condition, worker->mutex);
			continue;
		}
		job->loading = true;
		game_id = job->game_id;
		path = game_id < ui->catalog.game_count ?
			strdup(ui->catalog.games[game_id].cover_path) : NULL;
		(void)SDL_UnlockMutex(worker->mutex);
		surface = load_cover_bounded(path, &decode_bytes);
		free(path);
		(void)SDL_LockMutex(worker->mutex);
		if (job->occupied && job->game_id == game_id) {
			if (surface == NULL)
				++worker->rejected_count;
			else {
				++worker->decoded_count;
				if (decode_bytes > worker->peak_decode_bytes)
					worker->peak_decode_bytes = decode_bytes;
			}
			if (worker_visible_priority_locked(worker, game_id) == SIZE_MAX) {
				if (surface != NULL)
					SDL_FreeSurface(surface);
				++worker->stale_dropped_count;
				memset(job, 0, sizeof(*job));
			} else {
				job->surface = surface;
				job->loading = false;
				job->ready = true;
			}
		} else if (surface != NULL)
			SDL_FreeSurface(surface);
	}
	(void)SDL_UnlockMutex(worker->mutex);
	return 0;
}

int cover_cache_init(struct ui *ui)
{
	struct cover_worker *worker = &ui->cover_worker;

	memset(worker, 0, sizeof(*worker));
	worker->mutex = SDL_CreateMutex();
	worker->condition = SDL_CreateCond();
	if (worker->mutex == NULL || worker->condition == NULL)
		goto fail;
	worker->running = true;
	worker->thread = SDL_CreateThread(worker_main, "cover-decode", ui);
	if (worker->thread == NULL)
		goto fail;
	return 0;

fail:
	worker->running = false;
	if (worker->condition != NULL)
		SDL_DestroyCond(worker->condition);
	if (worker->mutex != NULL)
		SDL_DestroyMutex(worker->mutex);
	memset(worker, 0, sizeof(*worker));
	return -1;
}

static void store_surface(struct ui *ui, size_t game_id, SDL_Surface *surface)
{
	struct cover_item *item = find_item(ui, game_id);

	/* A ready decode can become stale between adjacent render frames. */
	if (!game_id_is_current_visible(ui, game_id)) {
		if (surface != NULL)
			SDL_FreeSurface(surface);
		if (ui->cover_worker.mutex != NULL) {
			(void)SDL_LockMutex(ui->cover_worker.mutex);
			++ui->cover_worker.stale_dropped_count;
			(void)SDL_UnlockMutex(ui->cover_worker.mutex);
		}
		return;
	}
	if (item == NULL)
		item = replacement_item(ui);
	/* All five cache entries may already be visible; never evict one. */
	if (item == NULL) {
		if (surface != NULL)
			SDL_FreeSurface(surface);
		return;
	}
	if (item->texture != NULL)
		SDL_DestroyTexture(item->texture);
	memset(item, 0, sizeof(*item));
	item->occupied = true;
	item->game_id = game_id;
	item->last_used = ++ui->cover_tick;
	if (surface != NULL) {
		item->width = surface->w;
		item->height = surface->h;
		item->texture = SDL_CreateTextureFromSurface(ui->renderer, surface);
		SDL_FreeSurface(surface);
	}
}

static void refresh_worker_visible(struct ui *ui, const size_t *ids,
				   size_t count)
{
	struct cover_worker *worker = &ui->cover_worker;

	if (worker->mutex == NULL)
		return;
	(void)SDL_LockMutex(worker->mutex);
	worker->visible_game_count = count;
	memcpy(worker->visible_game_ids, ids, count * sizeof(*ids));
	for (size_t i = 0; i < COVER_CACHE_MAX; ++i) {
		struct cover_job *job = &worker->jobs[i];
		size_t priority;

		if (!job->occupied)
			continue;
		priority = visible_priority(ids, count, job->game_id);
		if (priority != SIZE_MAX) {
			job->priority = priority;
			continue;
		}
		if (job->loading)
			continue;
		if (job->surface != NULL)
			SDL_FreeSurface(job->surface);
		++worker->stale_dropped_count;
		if (!job->ready)
			++worker->queued_cancelled_count;
		memset(job, 0, sizeof(*job));
	}
	(void)SDL_UnlockMutex(worker->mutex);
}

static void collect_ready(struct ui *ui, const size_t *ids, size_t count)
{
	struct cover_worker *worker = &ui->cover_worker;
	SDL_Surface *surface = NULL;
	size_t game_id = SIZE_MAX;
	size_t best_priority = SIZE_MAX;
	struct cover_job *best = NULL;

	if (worker->mutex == NULL)
		return;
	(void)SDL_LockMutex(worker->mutex);
	for (size_t i = 0; i < COVER_CACHE_MAX; ++i) {
		struct cover_job *job = &worker->jobs[i];
		size_t priority;

		if (!job->occupied || !job->ready)
			continue;
		priority = visible_priority(ids, count, job->game_id);
		if (priority == SIZE_MAX) {
			if (job->surface != NULL)
				SDL_FreeSurface(job->surface);
			++worker->stale_dropped_count;
			memset(job, 0, sizeof(*job));
			continue;
		}
		if (priority < best_priority) {
			best_priority = priority;
			best = job;
		}
	}
	if (best != NULL) {
		game_id = best->game_id;
		surface = best->surface;
		memset(best, 0, sizeof(*best));
	}
	(void)SDL_UnlockMutex(worker->mutex);
	if (game_id != SIZE_MAX)
		store_surface(ui, game_id, surface);
}

static struct cover_job *find_job(struct cover_worker *worker, size_t game_id)
{
	for (size_t i = 0; i < COVER_CACHE_MAX; ++i) {
		if (worker->jobs[i].occupied && worker->jobs[i].game_id == game_id)
			return &worker->jobs[i];
	}
	return NULL;
}

static void request_game(struct ui *ui, size_t game_id, size_t priority)
{
	struct cover_worker *worker = &ui->cover_worker;

	if (find_item(ui, game_id) != NULL || worker->mutex == NULL)
		return;
	(void)SDL_LockMutex(worker->mutex);
	{
		struct cover_job *job = find_job(worker, game_id);

		if (job != NULL) {
			job->priority = priority;
			(void)SDL_UnlockMutex(worker->mutex);
			return;
		}
	}
	{
		for (size_t i = 0; i < COVER_CACHE_MAX; ++i) {
			if (!worker->jobs[i].occupied) {
				worker->jobs[i].occupied = true;
				worker->jobs[i].game_id = game_id;
				worker->jobs[i].priority = priority;
				(void)SDL_CondSignal(worker->condition);
				break;
			}
		}
	}
	(void)SDL_UnlockMutex(worker->mutex);
}

void cover_cache_sync_visible(struct ui *ui)
{
	size_t ids[COVER_CACHE_MAX];
	size_t count = current_visible_ids(ui, ids);

	refresh_worker_visible(ui, ids, count);
	/* Keep texture upload out of the input-to-present critical frame. */
	if (ui->metrics.input_counter == 0U)
		collect_ready(ui, ids, count);
	for (size_t priority = 0; priority < count; ++priority) {
		size_t game_id = ids[priority];
		struct cover_item *item = find_item(ui, game_id);

		if (item != NULL)
			item->last_used = ++ui->cover_tick;
		else
			request_game(ui, game_id, priority);
	}
}

SDL_Texture *cover_cache_get(struct ui *ui, size_t game_id, int *width,
			     int *height)
{
	struct cover_item *item = find_item(ui, game_id);
	size_t ids[COVER_CACHE_MAX];
	size_t count;
	size_t priority;

	if (item == NULL) {
		count = current_visible_ids(ui, ids);
		priority = visible_priority(ids, count, game_id);
		if (priority != SIZE_MAX)
			request_game(ui, game_id, priority);
		return NULL;
	}
	item->last_used = ++ui->cover_tick;
	if (width != NULL)
		*width = item->width;
	if (height != NULL)
		*height = item->height;
	return item->texture;
}

bool cover_cache_visible_settled(struct ui *ui)
{
	size_t ids[COVER_CACHE_MAX];
	size_t count = current_visible_ids(ui, ids);

	refresh_worker_visible(ui, ids, count);
	collect_ready(ui, ids, count);
	for (size_t i = 0; i < count; ++i) {
		if (find_item(ui, ids[i]) == NULL)
			return false;
	}
	return true;
}

size_t cover_cache_texture_count(const struct ui *ui)
{
	size_t count = 0;

	for (size_t i = 0; i < COVER_CACHE_MAX; ++i) {
		if (ui->covers[i].texture != NULL)
			++count;
	}
	return count;
}

size_t cover_cache_rejected_count(struct ui *ui)
{
	struct cover_worker *worker = &ui->cover_worker;
	size_t count;

	if (worker->mutex == NULL)
		return worker->rejected_count;
	(void)SDL_LockMutex(worker->mutex);
	count = worker->rejected_count;
	(void)SDL_UnlockMutex(worker->mutex);
	return count;
}

static size_t worker_counter(struct cover_worker *worker, const size_t *value)
{
	size_t count;

	if (worker->mutex == NULL)
		return *value;
	(void)SDL_LockMutex(worker->mutex);
	count = *value;
	(void)SDL_UnlockMutex(worker->mutex);
	return count;
}

size_t cover_cache_stale_dropped_count(struct ui *ui)
{
	return worker_counter(&ui->cover_worker,
			      &ui->cover_worker.stale_dropped_count);
}

size_t cover_cache_queued_cancelled_count(struct ui *ui)
{
	return worker_counter(&ui->cover_worker,
			      &ui->cover_worker.queued_cancelled_count);
}

size_t cover_cache_visible_eviction_count(struct ui *ui)
{
	return worker_counter(&ui->cover_worker,
			      &ui->cover_worker.visible_eviction_count);
}

uint64_t cover_cache_peak_decode_bytes(struct ui *ui)
{
	struct cover_worker *worker = &ui->cover_worker;
	uint64_t bytes;

	if (worker->mutex == NULL)
		return worker->peak_decode_bytes;
	(void)SDL_LockMutex(worker->mutex);
	bytes = worker->peak_decode_bytes;
	(void)SDL_UnlockMutex(worker->mutex);
	return bytes;
}

void cover_cache_destroy(struct ui *ui)
{
	struct cover_worker *worker = &ui->cover_worker;

	if (worker->mutex != NULL) {
		(void)SDL_LockMutex(worker->mutex);
		worker->running = false;
		(void)SDL_CondSignal(worker->condition);
		(void)SDL_UnlockMutex(worker->mutex);
	}
	if (worker->thread != NULL)
		SDL_WaitThread(worker->thread, NULL);
	for (size_t i = 0; i < COVER_CACHE_MAX; ++i) {
		if (worker->jobs[i].surface != NULL)
			SDL_FreeSurface(worker->jobs[i].surface);
		if (ui->covers[i].texture != NULL)
			SDL_DestroyTexture(ui->covers[i].texture);
	}
	if (worker->condition != NULL)
		SDL_DestroyCond(worker->condition);
	if (worker->mutex != NULL)
		SDL_DestroyMutex(worker->mutex);
	memset(worker, 0, sizeof(*worker));
	memset(ui->covers, 0, sizeof(ui->covers));
	ui->cover_tick = 0;
}
