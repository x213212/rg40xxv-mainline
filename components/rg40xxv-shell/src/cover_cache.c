#define _POSIX_C_SOURCE 200809L

#include "ui.h"
#include "cover_limits.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

enum {
	COVER_DISK_CACHE_MAX_ENTRIES = 256,
	COVER_DISK_CACHE_MAX_SCAN = 4096,
	COVER_DISK_CACHE_PRUNE_INTERVAL = 32,
};

#define COVER_DISK_CACHE_MAX_BYTES (32ULL * 1024ULL * 1024ULL)
#define COVER_DISK_CACHE_FREE_FLOOR (128ULL * 1024ULL * 1024ULL)

static const unsigned char cover_disk_magic[8] = {
	'R', 'G', 'C', 'O', 'V', 'R', '0', '1',
};

struct cover_disk_header {
	unsigned char magic[8];
	uint64_t key_a;
	uint64_t key_b;
	uint64_t source_mtime_ns;
	uint64_t source_size;
	uint32_t width;
	uint32_t height;
	uint32_t pixel_format;
	uint32_t reserved;
};

struct cover_load_result {
	SDL_Surface *surface;
	uint64_t decode_bytes;
	bool disk_hit;
	bool disk_miss;
	bool disk_write;
};

struct cover_disk_candidate {
	char *name;
	uint64_t bytes;
	struct timespec modified;
};

static uint64_t monotonic_ns(void)
{
	struct timespec value;

	if (clock_gettime(CLOCK_MONOTONIC, &value) != 0)
		return 0;
	return (uint64_t)value.tv_sec * 1000000000ULL +
		(uint64_t)value.tv_nsec;
}

static bool cover_disk_cache_name(const char *name)
{
	if (name == NULL || strlen(name) != 37U || name[16] != '-' ||
	    strcmp(name + 33, ".rgc") != 0)
		return false;
	for (size_t index = 0U; index < 33U; ++index) {
		char value = name[index];

		if (index == 16U)
			continue;
		if (!((value >= '0' && value <= '9') ||
		      (value >= 'a' && value <= 'f')))
			return false;
	}
	return true;
}

static bool cover_disk_cache_temporary_name(const char *name)
{
	const char *suffix;

	if (name == NULL || strlen(name) <= 42U)
		return false;
	suffix = strstr(name, ".rgc.tmp.");
	if (suffix == NULL || (size_t)(suffix - name) != 33U)
		return false;
	for (size_t index = 0U; index < 33U; ++index) {
		char value = name[index];

		if (index == 16U) {
			if (value != '-')
				return false;
			continue;
		}
		if (!((value >= '0' && value <= '9') ||
		      (value >= 'a' && value <= 'f')))
			return false;
	}
	suffix += strlen(".rgc.tmp.");
	if (*suffix < '0' || *suffix > '9')
		return false;
	for (; *suffix != '\0'; ++suffix) {
		if ((*suffix < '0' || *suffix > '9') && *suffix != '.')
			return false;
	}
	return true;
}

static int compare_cover_disk_candidate(const void *left, const void *right)
{
	const struct cover_disk_candidate *a = left;
	const struct cover_disk_candidate *b = right;

	if (a->modified.tv_sec != b->modified.tv_sec)
		return a->modified.tv_sec < b->modified.tv_sec ? -1 : 1;
	if (a->modified.tv_nsec != b->modified.tv_nsec)
		return a->modified.tv_nsec < b->modified.tv_nsec ? -1 : 1;
	return strcmp(a->name, b->name);
}

static void cover_disk_cache_prune(const char *directory)
{
	struct cover_disk_candidate *items = NULL;
	size_t count = 0U;
	size_t capacity = 0U;
	uint64_t total = 0U;
	DIR *stream = opendir(directory);
	struct dirent *entry;

	if (stream == NULL)
		return;
	while (count < COVER_DISK_CACHE_MAX_SCAN &&
	       (entry = readdir(stream)) != NULL) {
		struct cover_disk_candidate candidate = { 0 };
		char path[PATH_MAX];
		struct stat metadata;
		int length;

		if (cover_disk_cache_temporary_name(entry->d_name)) {
			length = snprintf(path, sizeof(path), "%s/%s", directory,
				entry->d_name);
			if (length >= 0 && (size_t)length < sizeof(path) &&
			    lstat(path, &metadata) == 0 &&
			    S_ISREG(metadata.st_mode) &&
			    metadata.st_uid == geteuid() && metadata.st_nlink == 1)
				(void)unlink(path);
			continue;
		}
		if (!cover_disk_cache_name(entry->d_name))
			continue;
		length = snprintf(path, sizeof(path), "%s/%s", directory,
			entry->d_name);
		if (length < 0 || (size_t)length >= sizeof(path) ||
		    lstat(path, &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
		    metadata.st_uid != geteuid() || metadata.st_nlink != 1 ||
		    metadata.st_size < 0)
			continue;
		if (count == capacity) {
			size_t next = capacity == 0U ? 64U : capacity * 2U;
			struct cover_disk_candidate *grown;

			if (next > COVER_DISK_CACHE_MAX_SCAN)
				next = COVER_DISK_CACHE_MAX_SCAN;
			grown = realloc(items, next * sizeof(*grown));
			if (grown == NULL)
				break;
			items = grown;
			capacity = next;
		}
		candidate.name = strdup(entry->d_name);
		if (candidate.name == NULL)
			break;
		candidate.bytes = (uint64_t)metadata.st_size;
		candidate.modified = metadata.st_mtim;
		items[count++] = candidate;
		if (UINT64_MAX - total < candidate.bytes)
			total = UINT64_MAX;
		else
			total += candidate.bytes;
	}
	(void)closedir(stream);
	/* qsort()'s base argument is declared nonnull by the C library even when
	 * the element count is zero.  An empty cache legitimately leaves items
	 * NULL, so avoid invoking undefined behaviour before the worker starts. */
	if (count > 1U)
		qsort(items, count, sizeof(*items), compare_cover_disk_candidate);
	for (size_t index = 0U;
	     index < count &&
	     (count - index > COVER_DISK_CACHE_MAX_ENTRIES ||
	      total > COVER_DISK_CACHE_MAX_BYTES);
	     ++index) {
		char path[PATH_MAX];
		int length = snprintf(path, sizeof(path), "%s/%s", directory,
			items[index].name);

		if (length >= 0 && (size_t)length < sizeof(path) &&
		    unlink(path) == 0)
			total = total >= items[index].bytes ?
				total - items[index].bytes : 0U;
	}
	for (size_t index = 0U; index < count; ++index)
		free(items[index].name);
	free(items);
}

static bool cover_disk_cache_has_space(const char *directory)
{
	struct statvfs filesystem;
	uint64_t available;

	if (statvfs(directory, &filesystem) != 0 || filesystem.f_frsize == 0U)
		return false;
	if ((uint64_t)filesystem.f_bavail >
	    UINT64_MAX / (uint64_t)filesystem.f_frsize)
		return true;
	available = (uint64_t)filesystem.f_bavail *
		(uint64_t)filesystem.f_frsize;
	return available >= COVER_DISK_CACHE_FREE_FLOOR;
}

static uint64_t hash_bytes(uint64_t hash, const void *data, size_t length)
{
	const unsigned char *bytes = data;

	for (size_t index = 0; index < length; ++index) {
		hash ^= bytes[index];
		hash *= 1099511628211ULL;
	}
	return hash;
}

static uint64_t source_mtime_ns(const struct stat *metadata)
{
	return (uint64_t)metadata->st_mtim.tv_sec * 1000000000ULL +
		(uint64_t)metadata->st_mtim.tv_nsec;
}

static void cover_disk_keys(const char *path, const struct stat *metadata,
			    uint64_t *key_a, uint64_t *key_b)
{
	uint64_t modified = source_mtime_ns(metadata);
	uint64_t size = (uint64_t)metadata->st_size;

	*key_a = hash_bytes(1469598103934665603ULL, path, strlen(path));
	*key_a = hash_bytes(*key_a, &modified, sizeof(modified));
	*key_a = hash_bytes(*key_a, &size, sizeof(size));
	*key_b = hash_bytes(7809847782465536322ULL, path, strlen(path));
	*key_b = hash_bytes(*key_b, &size, sizeof(size));
	*key_b = hash_bytes(*key_b, &modified, sizeof(modified));
}

static int cover_disk_path(const struct ui *ui, const char *source,
			   const struct stat *metadata, char *path, size_t size,
			   uint64_t *key_a, uint64_t *key_b)
{
	int length;

	if (ui->cover_cache_dir[0] == '\0')
		return -1;
	cover_disk_keys(source, metadata, key_a, key_b);
	length = snprintf(path, size, "%s/%016llx-%016llx.rgc",
		ui->cover_cache_dir, (unsigned long long)*key_a,
		(unsigned long long)*key_b);
	return length < 0 || (size_t)length >= size ? -1 : 0;
}

static int read_all(int fd, void *buffer, size_t length)
{
	unsigned char *output = buffer;
	size_t done = 0U;

	while (done < length) {
		ssize_t count = read(fd, output + done, length - done);

		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			return -1;
		done += (size_t)count;
	}
	return 0;
}

static int write_all(int fd, const void *buffer, size_t length)
{
	const unsigned char *input = buffer;
	size_t done = 0U;

	while (done < length) {
		ssize_t count = write(fd, input + done, length - done);

		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			return -1;
		done += (size_t)count;
	}
	return 0;
}

static SDL_Surface *cover_disk_load(const struct ui *ui, const char *source,
				    const struct stat *source_metadata,
				    uint64_t *decode_bytes)
{
	struct cover_disk_header header;
	struct stat metadata;
	SDL_Surface *surface = NULL;
	char path[PATH_MAX];
	uint64_t key_a;
	uint64_t key_b;
	uint64_t pixels;
	uint64_t expected_size;
	int fd;

	if (cover_disk_path(ui, source, source_metadata, path, sizeof(path),
			    &key_a, &key_b) != 0)
		return NULL;
	fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return NULL;
	if (fstat(fd, &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
	    metadata.st_uid != geteuid() || metadata.st_nlink != 1 ||
	    (metadata.st_mode & 0022) != 0 ||
	    read_all(fd, &header, sizeof(header)) != 0 ||
	    memcmp(header.magic, cover_disk_magic, sizeof(header.magic)) != 0 ||
	    header.key_a != key_a || header.key_b != key_b ||
	    header.source_mtime_ns != source_mtime_ns(source_metadata) ||
	    header.source_size != (uint64_t)source_metadata->st_size ||
	    header.width == 0U || header.height == 0U ||
	    header.width > COVER_THUMBNAIL_MAX_WIDTH ||
	    header.height > COVER_THUMBNAIL_MAX_HEIGHT ||
	    header.pixel_format != SDL_PIXELFORMAT_ARGB8888) {
		(void)close(fd);
		return NULL;
	}
	pixels = (uint64_t)header.width * (uint64_t)header.height * 4U;
	expected_size = sizeof(header) + pixels;
	if (pixels > COVER_THUMBNAIL_MAX_BYTES ||
	    (uint64_t)metadata.st_size != expected_size) {
		(void)close(fd);
		return NULL;
	}
	surface = SDL_CreateRGBSurfaceWithFormat(0, (int)header.width,
		(int)header.height, 32, SDL_PIXELFORMAT_ARGB8888);
	if (surface == NULL)
		goto out;
	for (uint32_t row = 0; row < header.height; ++row) {
		unsigned char *destination =
			(unsigned char *)surface->pixels + (size_t)row * surface->pitch;

		if (read_all(fd, destination, (size_t)header.width * 4U) != 0) {
			SDL_FreeSurface(surface);
			surface = NULL;
			break;
		}
	}
out:
	(void)close(fd);
	if (surface != NULL)
		*decode_bytes = pixels;
	return surface;
}

static bool cover_disk_write(struct ui *ui, const char *source,
			     const struct stat *source_metadata,
			     const SDL_Surface *surface)
{
	struct cover_disk_header header = { 0 };
	char path[PATH_MAX];
	char temporary[PATH_MAX];
	uint64_t key_a;
	uint64_t key_b;
	int length;
	int fd;
	bool result = false;

	if (!cover_disk_cache_has_space(ui->cover_cache_dir) || surface == NULL ||
	    surface->w <= 0 || surface->h <= 0 ||
	    surface->w > COVER_THUMBNAIL_MAX_WIDTH ||
	    surface->h > COVER_THUMBNAIL_MAX_HEIGHT ||
	    surface->format == NULL ||
	    surface->format->format != SDL_PIXELFORMAT_ARGB8888 ||
	    cover_disk_path(ui, source, source_metadata, path, sizeof(path),
			    &key_a, &key_b) != 0)
		return false;
	length = snprintf(temporary, sizeof(temporary), "%s.tmp.%ld.%llu", path,
		(long)getpid(), (unsigned long long)monotonic_ns());
	if (length < 0 || (size_t)length >= sizeof(temporary))
		return false;
	fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC |
		  O_NOFOLLOW, 0600);
	if (fd < 0)
		return false;
	memcpy(header.magic, cover_disk_magic, sizeof(header.magic));
	header.key_a = key_a;
	header.key_b = key_b;
	header.source_mtime_ns = source_mtime_ns(source_metadata);
	header.source_size = (uint64_t)source_metadata->st_size;
	header.width = (uint32_t)surface->w;
	header.height = (uint32_t)surface->h;
	header.pixel_format = SDL_PIXELFORMAT_ARGB8888;
	if (write_all(fd, &header, sizeof(header)) != 0)
		goto out;
	for (int row = 0; row < surface->h; ++row) {
		const unsigned char *source_row =
			(const unsigned char *)surface->pixels + (size_t)row * surface->pitch;

		if (write_all(fd, source_row, (size_t)surface->w * 4U) != 0)
			goto out;
	}
	if (close(fd) != 0) {
		fd = -1;
		goto out;
	}
	fd = -1;
	if (rename(temporary, path) != 0)
		goto out;
	result = true;
	if (++ui->cover_worker.disk_writes_since_prune >=
	    COVER_DISK_CACHE_PRUNE_INTERVAL) {
		cover_disk_cache_prune(ui->cover_cache_dir);
		ui->cover_worker.disk_writes_since_prune = 0U;
	}

out:
	if (fd >= 0)
		(void)close(fd);
	if (!result)
		(void)unlink(temporary);
	return result;
}

static SDL_Surface *thumbnail_surface(SDL_Surface *source)
{
	SDL_Surface *thumbnail;
	SDL_Rect destination = { 0, 0, 0, 0 };
	double scale = 1.0;

	if (source == NULL)
		return source;
	if (source->w > COVER_THUMBNAIL_MAX_WIDTH ||
	    source->h > COVER_THUMBNAIL_MAX_HEIGHT)
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

static void load_cover_bounded(struct ui *ui, const char *path,
			       struct cover_load_result *result)
{
	struct cover_dimensions dimensions;
	struct stat status;
	SDL_Surface *surface = NULL;
	SDL_RWops *rw;
	FILE *stream;
	uint64_t actual_bytes;
	int fd;

	memset(result, 0, sizeof(*result));
	if (path == NULL)
		return;
	fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return;
	if (fstat(fd, &status) != 0 || !S_ISREG(status.st_mode) ||
	    status.st_size <= 0 || (uint64_t)status.st_size > COVER_FILE_MAX_BYTES) {
		(void)close(fd);
		return;
	}
	if (ui->cover_cache_dir[0] != '\0') {
		result->surface = cover_disk_load(ui, path, &status,
			&result->decode_bytes);
		if (result->surface != NULL) {
			result->disk_hit = true;
			(void)close(fd);
			return;
		}
		result->disk_miss = true;
	}
	if (
	    cover_probe_fd(fd, (uint64_t)status.st_size, &dimensions) !=
		COVER_PROBE_OK) {
		(void)close(fd);
		return;
	}
	stream = fdopen(fd, "rb");
	if (stream == NULL) {
		(void)close(fd);
		return;
	}
	rw = SDL_RWFromFP(stream, SDL_TRUE);
	if (rw == NULL) {
		(void)fclose(stream);
		return;
	}
	surface = IMG_Load_RW(rw, 1);
	if (surface == NULL)
		return;
	if (surface->w <= 0 || surface->h <= 0 || surface->pitch <= 0 ||
	    (uint32_t)surface->w != dimensions.width ||
	    (uint32_t)surface->h != dimensions.height) {
		SDL_FreeSurface(surface);
		return;
	}
	actual_bytes = (uint64_t)(unsigned int)surface->pitch *
		(uint64_t)(unsigned int)surface->h;
	if (actual_bytes > COVER_DECODE_MAX_BYTES) {
		SDL_FreeSurface(surface);
		return;
	}
	result->decode_bytes = actual_bytes > dimensions.decode_bytes ? actual_bytes :
		dimensions.decode_bytes;
	result->surface = thumbnail_surface(surface);
	if (result->surface != NULL && ui->cover_cache_dir[0] != '\0')
		result->disk_write = cover_disk_write(ui, path, &status,
			result->surface);
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

static void destroy_item_texture(struct ui *ui, struct cover_item *item)
{
	if (item->texture == NULL)
		return;
	SDL_DestroyTexture(item->texture);
	item->texture = NULL;
	++ui->cover_texture_destroy_count;
	if (ui->cover_texture_bytes >= item->texture_bytes)
		ui->cover_texture_bytes -= item->texture_bytes;
	else
		ui->cover_texture_bytes = 0U;
	item->texture_bytes = 0U;
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
		uint64_t generation;
		struct cover_load_result loaded;

		for (size_t i = 0; i < COVER_CACHE_MAX; ++i) {
			if (worker->jobs[i].occupied && !worker->jobs[i].loading &&
			    !worker->jobs[i].ready && !worker->jobs[i].cancelled &&
			    worker->jobs[i].generation ==
				worker->visible_generation &&
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
		generation = job->generation;
		(void)SDL_UnlockMutex(worker->mutex);
		/* job->path is copied by the UI thread before publication and a loading
		 * job is never cleared until this worker reacquires the mutex. */
		load_cover_bounded(ui, job->path, &loaded);
		(void)SDL_LockMutex(worker->mutex);
		if (loaded.disk_hit)
			++worker->disk_hit_count;
		if (loaded.disk_miss)
			++worker->disk_miss_count;
		if (loaded.disk_write)
			++worker->disk_write_count;
		if (job->occupied && job->game_id == game_id &&
		    job->generation == generation) {
			if (loaded.surface == NULL)
				++worker->rejected_count;
			else {
				++worker->decoded_count;
				if (loaded.decode_bytes > worker->peak_decode_bytes)
					worker->peak_decode_bytes = loaded.decode_bytes;
			}
			if (job->cancelled || generation != worker->visible_generation ||
			    worker_visible_priority_locked(worker, game_id) == SIZE_MAX) {
				if (loaded.surface != NULL)
					SDL_FreeSurface(loaded.surface);
				++worker->stale_dropped_count;
				memset(job, 0, sizeof(*job));
			} else {
				job->surface = loaded.surface;
				job->loading = false;
				job->ready = true;
			}
		} else if (loaded.surface != NULL)
			SDL_FreeSurface(loaded.surface);
		/* Wake the event-driven renderer as soon as a decode changes state. */
		{
			SDL_Event event = { .type = SDL_USEREVENT };

			event.user.code = 0x52474356;
			(void)SDL_PushEvent(&event);
		}
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

int cover_cache_configure(struct ui *ui, const char *directory)
{
	struct stat metadata;
	int length;

	if (directory == NULL || directory[0] != '/' ||
	    lstat(directory, &metadata) != 0 || !S_ISDIR(metadata.st_mode) ||
	    S_ISLNK(metadata.st_mode) || metadata.st_uid != geteuid() ||
	    (metadata.st_mode & 0022) != 0)
		return -1;
	length = snprintf(ui->cover_cache_dir, sizeof(ui->cover_cache_dir), "%s",
		directory);
	if (length < 0 || (size_t)length >= sizeof(ui->cover_cache_dir)) {
		ui->cover_cache_dir[0] = '\0';
		return -1;
	}
	cover_disk_cache_prune(ui->cover_cache_dir);
	return 0;
}

static void store_surface(struct ui *ui, size_t game_id, SDL_Surface *surface)
{
	struct cover_item *item = find_item(ui, game_id);
	SDL_Texture *new_texture = NULL;
	uint64_t new_bytes = 0U;
	int width = 0;
	int height = 0;

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
	if (surface != NULL) {
		width = surface->w;
		height = surface->h;
		new_bytes = (uint64_t)(unsigned int)surface->w *
			(uint64_t)(unsigned int)surface->h * 4U;
		new_texture = SDL_CreateTextureFromSurface(ui->renderer, surface);
		SDL_FreeSurface(surface);
		if (new_texture == NULL)
			return;
		++ui->cover_texture_create_count;
	}
	if (item == NULL)
		item = replacement_item(ui);
	/* All five cache entries may already be visible; never evict one. */
	if (item == NULL) {
		if (new_texture != NULL) {
			SDL_DestroyTexture(new_texture);
			++ui->cover_texture_destroy_count;
		}
		return;
	}
	/* This is a metric-backed invariant, not a permanently-zero placeholder. */
	if (item->occupied &&
	    game_id_is_current_visible(ui, item->game_id)) {
		if (ui->cover_worker.mutex != NULL) {
			(void)SDL_LockMutex(ui->cover_worker.mutex);
			++ui->cover_worker.visible_eviction_count;
			(void)SDL_UnlockMutex(ui->cover_worker.mutex);
		}
		if (new_texture != NULL) {
			SDL_DestroyTexture(new_texture);
			++ui->cover_texture_destroy_count;
		}
		return;
	}
	{
		uint64_t retained = ui->cover_texture_bytes >= item->texture_bytes ?
			ui->cover_texture_bytes - item->texture_bytes : 0U;

		if (retained + new_bytes <= COVER_THUMBNAIL_CACHE_MAX_BYTES)
			goto within_budget;
		if (new_texture != NULL) {
			SDL_DestroyTexture(new_texture);
			++ui->cover_texture_destroy_count;
		}
		return;
	}
within_budget:
	/* Upload succeeds before the prior LRU texture is retired: no white gap. */
	destroy_item_texture(ui, item);
	memset(item, 0, sizeof(*item));
	item->occupied = true;
	item->game_id = game_id;
	item->last_used = ++ui->cover_tick;
	if (new_texture != NULL) {
		item->width = width;
		item->height = height;
		item->texture = new_texture;
		item->texture_bytes = new_bytes;
		ui->cover_texture_bytes += new_bytes;
		if (ui->cover_texture_bytes > ui->cover_texture_peak_bytes)
			ui->cover_texture_peak_bytes = ui->cover_texture_bytes;
	}
}

static void refresh_worker_visible(struct ui *ui, const size_t *ids,
				   size_t count)
{
	struct cover_worker *worker = &ui->cover_worker;
	bool changed;

	if (worker->mutex == NULL)
		return;
	(void)SDL_LockMutex(worker->mutex);
	changed = worker->visible_game_count != count ||
		memcmp(worker->visible_game_ids, ids, count * sizeof(*ids)) != 0;
	if (changed) {
		++worker->visible_generation;
		if (worker->visible_generation == 0U)
			worker->visible_generation = 1U;
	}
	worker->visible_game_count = count;
	memcpy(worker->visible_game_ids, ids, count * sizeof(*ids));
	for (size_t i = 0; i < COVER_CACHE_MAX; ++i) {
		struct cover_job *job = &worker->jobs[i];
		size_t priority;

		if (!job->occupied)
			continue;
		if (job->generation != worker->visible_generation) {
			if (job->loading) {
				if (!job->cancelled)
					++worker->queued_cancelled_count;
				job->cancelled = true;
				continue;
			}
			if (job->surface != NULL)
				SDL_FreeSurface(job->surface);
			++worker->stale_dropped_count;
			if (!job->ready)
				++worker->queued_cancelled_count;
			memset(job, 0, sizeof(*job));
			continue;
		}
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

static bool collect_ready(struct ui *ui, const size_t *ids, size_t count)
{
	struct cover_worker *worker = &ui->cover_worker;
	SDL_Surface *surface = NULL;
	size_t game_id = SIZE_MAX;
	size_t best_priority = SIZE_MAX;
	struct cover_job *best = NULL;

	if (worker->mutex == NULL)
		return false;
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
	return game_id != SIZE_MAX;
}

static struct cover_job *find_job(struct cover_worker *worker, size_t game_id)
{
	for (size_t i = 0; i < COVER_CACHE_MAX; ++i) {
		if (worker->jobs[i].occupied && !worker->jobs[i].cancelled &&
		    worker->jobs[i].generation == worker->visible_generation &&
		    worker->jobs[i].game_id == game_id)
			return &worker->jobs[i];
	}
	return NULL;
}

static void request_game(struct ui *ui, size_t game_id, size_t priority)
{
	struct cover_worker *worker = &ui->cover_worker;
	const char *source;
	char path[PATH_MAX];
	int length;

	if (find_item(ui, game_id) != NULL || worker->mutex == NULL ||
	    game_id >= ui->catalog.game_count)
		return;
	/* request_game and catalog mutation both run on the UI thread.  Snapshot
	 * the path before handing the job to the independently scheduled worker. */
	source = ui->catalog.games[game_id].cover_path;
	length = snprintf(path, sizeof(path), "%s", source != NULL ? source : "");
	if (length < 0 || (size_t)length >= sizeof(path))
		path[0] = '\0';
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
				worker->jobs[i].generation =
					worker->visible_generation;
				(void)snprintf(worker->jobs[i].path,
					sizeof(worker->jobs[i].path), "%s", path);
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
	if (ui->metrics.input_counter == 0U) {
		for (size_t uploaded = 0; uploaded < count; ++uploaded) {
			if (!collect_ready(ui, ids, count))
				break;
		}
	}
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
	bool settled = true;

	refresh_worker_visible(ui, ids, count);
	while (collect_ready(ui, ids, count))
		;
	for (size_t i = 0; i < count; ++i) {
		if (find_item(ui, ids[i]) == NULL) {
			/* Cancelled jobs may have occupied every slot at the last sync. */
			request_game(ui, ids[i], i);
			settled = false;
		}
	}
	return settled;
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

size_t cover_cache_disk_hit_count(struct ui *ui)
{
	return worker_counter(&ui->cover_worker,
		&ui->cover_worker.disk_hit_count);
}

size_t cover_cache_disk_miss_count(struct ui *ui)
{
	return worker_counter(&ui->cover_worker,
		&ui->cover_worker.disk_miss_count);
}

size_t cover_cache_disk_write_count(struct ui *ui)
{
	return worker_counter(&ui->cover_worker,
		&ui->cover_worker.disk_write_count);
}

size_t cover_cache_texture_create_count(const struct ui *ui)
{
	return ui->cover_texture_create_count;
}

size_t cover_cache_texture_destroy_count(const struct ui *ui)
{
	return ui->cover_texture_destroy_count;
}

uint64_t cover_cache_texture_bytes(const struct ui *ui)
{
	return ui->cover_texture_bytes;
}

uint64_t cover_cache_texture_peak_bytes(const struct ui *ui)
{
	return ui->cover_texture_peak_bytes;
}

uint64_t cover_cache_peak_rss_kib(void)
{
	FILE *stream = fopen("/proc/self/status", "r");
	char *line = NULL;
	size_t capacity = 0U;
	uint64_t value = 0U;

	if (stream == NULL)
		return 0U;
	while (getline(&line, &capacity, stream) >= 0) {
		unsigned long long parsed;

		if (sscanf(line, "VmHWM: %llu kB", &parsed) == 1) {
			value = (uint64_t)parsed;
			break;
		}
	}
	free(line);
	(void)fclose(stream);
	return value;
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
	/* SDL may still reference cover textures in its queued render commands. */
	if (ui->renderer != NULL)
		(void)SDL_RenderFlush(ui->renderer);
	for (size_t i = 0; i < COVER_CACHE_MAX; ++i) {
		if (worker->jobs[i].surface != NULL)
			SDL_FreeSurface(worker->jobs[i].surface);
		destroy_item_texture(ui, &ui->covers[i]);
	}
	if (worker->condition != NULL)
		SDL_DestroyCond(worker->condition);
	if (worker->mutex != NULL)
		SDL_DestroyMutex(worker->mutex);
	memset(worker, 0, sizeof(*worker));
	memset(ui->covers, 0, sizeof(ui->covers));
	ui->cover_tick = 0;
}
