#define _POSIX_C_SOURCE 200809L

#include "ui.h"
#include "cover_limits.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum {
	GAME_COUNT = 37,
	LIFECYCLE_COUNT = 24,
	RAPID_SCROLL_COUNT = 80,
	FIXTURE_WIDTH = 1500,
	FIXTURE_HEIGHT = 1500,
};

static unsigned int checks;

#define CHECK(expression) do { \
	++checks; \
	if (!(expression)) { \
		(void)fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
			      __FILE__, __LINE__, #expression); \
		exit(1); \
	} \
} while (0)

size_t catalog_visible_id(const struct ui *ui, size_t visible_index)
{
	if (visible_index >= ui->catalog.visible_count)
		return SIZE_MAX;
	return ui->catalog.visible[visible_index];
}

static void create_large_bmp(const char *path)
{
	SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(
		0, FIXTURE_WIDTH, FIXTURE_HEIGHT, 32, SDL_PIXELFORMAT_ARGB8888);

	CHECK(surface != NULL);
	CHECK(SDL_FillRect(surface, NULL,
			   SDL_MapRGB(surface->format, 43, 117, 181)) == 0);
	CHECK(SDL_SaveBMP(surface, path) == 0);
	SDL_FreeSurface(surface);
}

static bool worker_has_started(struct ui *ui)
{
	bool started = false;

	(void)SDL_LockMutex(ui->cover_worker.mutex);
	for (size_t i = 0; i < COVER_CACHE_MAX; ++i) {
		if (ui->cover_worker.jobs[i].loading ||
		    ui->cover_worker.jobs[i].ready) {
			started = true;
			break;
		}
	}
	if (ui->cover_worker.decoded_count > 0U)
		started = true;
	(void)SDL_UnlockMutex(ui->cover_worker.mutex);
	return started;
}

static void wait_until_worker_loading(struct ui *ui)
{
	for (unsigned int attempt = 0; attempt < 500; ++attempt) {
		if (worker_has_started(ui))
			return;
		SDL_Delay(1U);
	}
	CHECK(false);
}

static void settle_visible_textures(struct ui *ui)
{
	ui->metrics.input_counter = 0U;
	for (unsigned int attempt = 0; attempt < 3000; ++attempt) {
		if (cover_cache_visible_settled(ui))
			break;
		SDL_Delay(1U);
	}
	CHECK(cover_cache_visible_settled(ui));
	CHECK(cover_cache_texture_count(ui) == COVER_CACHE_MAX);
	CHECK(cover_cache_texture_bytes(ui) > 0U);
	CHECK(cover_cache_texture_bytes(ui) <= COVER_THUMBNAIL_CACHE_MAX_BYTES);
}

static void exercise_worker_lifecycle(struct ui *ui)
{
	size_t total_stale = 0;
	size_t total_cancelled = 0;
	size_t total_disk_hits = 0;
	size_t total_disk_misses = 0;
	size_t total_disk_writes = 0;

	for (size_t lifecycle = 0; lifecycle < LIFECYCLE_COUNT; ++lifecycle) {
		CHECK(cover_cache_init(ui) == 0);
		ui->metrics.input_counter = 1U;
		ui->game_index = lifecycle % GAME_COUNT;
		cover_cache_sync_visible(ui);
		wait_until_worker_loading(ui);

		for (size_t step = 0; step < RAPID_SCROLL_COUNT; ++step) {
			ui->game_index = (lifecycle * 13U + step * 11U) %
				GAME_COUNT;
			cover_cache_sync_visible(ui);
			if (step % 5U == 0U)
				SDL_Delay(1U);
		}
		total_stale += cover_cache_stale_dropped_count(ui);
		total_cancelled += cover_cache_queued_cancelled_count(ui);
		SDL_Delay(20U);
		total_disk_hits += cover_cache_disk_hit_count(ui);
		total_disk_misses += cover_cache_disk_miss_count(ui);
		total_disk_writes += cover_cache_disk_write_count(ui);
		CHECK(cover_cache_visible_eviction_count(ui) == 0);
		settle_visible_textures(ui);

		/* This intentionally joins a worker that may still be decoding. */
		cover_cache_destroy(ui);
		CHECK(ui->cover_worker.thread == NULL);
		CHECK(ui->cover_worker.mutex == NULL);
		CHECK(ui->cover_worker.condition == NULL);
		CHECK(cover_cache_texture_count(ui) == 0);
		CHECK(cover_cache_texture_bytes(ui) == 0U);
		CHECK(cover_cache_texture_create_count(ui) ==
		      cover_cache_texture_destroy_count(ui));
	}
	CHECK(total_stale > 0);
	CHECK(total_cancelled > 0);
	if (ui->cover_cache_dir[0] != '\0') {
		CHECK(total_disk_hits > 0);
		CHECK(total_disk_misses > 0);
		CHECK(total_disk_writes > 0);
	}
	(void)printf("COVER_WORKER_LIFECYCLE_TEST PASS checks=%u cycles=%u stale=%zu cancelled=%zu disk_hits=%zu disk_misses=%zu disk_writes=%zu texture_create=%zu texture_destroy=%zu texture_peak_bytes=%llu peak_rss_kib=%llu\n",
		     checks, LIFECYCLE_COUNT, total_stale, total_cancelled,
		     total_disk_hits, total_disk_misses, total_disk_writes,
		     cover_cache_texture_create_count(ui),
		     cover_cache_texture_destroy_count(ui),
		     (unsigned long long)cover_cache_texture_peak_bytes(ui),
		     (unsigned long long)cover_cache_peak_rss_kib());
}

int main(int argc, char **argv)
{
	struct ui ui;
	struct game_entry games[GAME_COUNT];
	size_t visible[GAME_COUNT];
	char temporary_path[] = "/tmp/rg40xxv-cover-worker-XXXXXX";
	const char *fixture_path;
	bool remove_fixture;
	int fd = -1;

	CHECK(argc >= 1 && argc <= 3);
	memset(&ui, 0, sizeof(ui));
	memset(games, 0, sizeof(games));
	CHECK(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_TIMER) == 0);
	ui.window = SDL_CreateWindow("cover-worker-test", 0, 0, 640, 480,
		SDL_WINDOW_HIDDEN);
	CHECK(ui.window != NULL);
	ui.renderer = SDL_CreateRenderer(ui.window, -1, SDL_RENDERER_SOFTWARE);
	CHECK(ui.renderer != NULL);
	if (argc >= 2) {
		fixture_path = argv[1];
		remove_fixture = false;
	} else {
		fd = mkstemp(temporary_path);
		CHECK(fd >= 0);
		CHECK(close(fd) == 0);
		fixture_path = temporary_path;
		remove_fixture = true;
	}
	create_large_bmp(fixture_path);
	if (argc == 3)
		CHECK(cover_cache_configure(&ui, argv[2]) == 0);

	for (size_t i = 0; i < GAME_COUNT; ++i) {
		visible[i] = i;
		games[i].cover_path = (char *)fixture_path;
	}
	ui.catalog.games = games;
	ui.catalog.game_count = GAME_COUNT;
	ui.catalog.visible = visible;
	ui.catalog.visible_count = GAME_COUNT;
	ui.catalog.visible_capacity = GAME_COUNT;
	exercise_worker_lifecycle(&ui);

	if (remove_fixture)
		CHECK(unlink(fixture_path) == 0);
	SDL_DestroyRenderer(ui.renderer);
	SDL_DestroyWindow(ui.window);
	SDL_Quit();
	return 0;
}
