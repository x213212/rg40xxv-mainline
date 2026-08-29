#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
	GAME_COUNT = 17,
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

static const int priority_offsets[COVER_CACHE_MAX] = { 0, -1, 1, -2, 2 };

static size_t expected_id(const struct ui *ui, size_t priority)
{
	long long position = (long long)ui->game_index +
		priority_offsets[priority];

	while (position < 0)
		position += (long long)ui->catalog.visible_count;
	position %= (long long)ui->catalog.visible_count;
	return ui->catalog.visible[position];
}

static bool is_expected(const struct ui *ui, size_t game_id)
{
	for (size_t priority = 0; priority < COVER_CACHE_MAX; ++priority) {
		if (expected_id(ui, priority) == game_id)
			return true;
	}
	return false;
}

static struct cover_job *job_for(struct ui *ui, size_t game_id)
{
	for (size_t i = 0; i < COVER_CACHE_MAX; ++i) {
		if (ui->cover_worker.jobs[i].occupied &&
		    ui->cover_worker.jobs[i].game_id == game_id)
			return &ui->cover_worker.jobs[i];
	}
	return NULL;
}

static struct cover_item *item_for(struct ui *ui, size_t game_id)
{
	for (size_t i = 0; i < COVER_CACHE_MAX; ++i) {
		if (ui->covers[i].occupied && ui->covers[i].game_id == game_id)
			return &ui->covers[i];
	}
	return NULL;
}

static void check_queue_matches_visible(struct ui *ui)
{
	size_t occupied = 0;

	for (size_t i = 0; i < COVER_CACHE_MAX; ++i) {
		struct cover_job *job = &ui->cover_worker.jobs[i];

		if (!job->occupied)
			continue;
		++occupied;
		CHECK(!job->loading);
		CHECK(!job->ready);
		CHECK(is_expected(ui, job->game_id));
	}
	CHECK(occupied == COVER_CACHE_MAX);
	for (size_t priority = 0; priority < COVER_CACHE_MAX; ++priority) {
		struct cover_job *job = job_for(ui, expected_id(ui, priority));

		CHECK(job != NULL);
		CHECK(job->priority == priority);
	}
}

static void clear_jobs(struct ui *ui)
{
	for (size_t i = 0; i < COVER_CACHE_MAX; ++i) {
		if (ui->cover_worker.jobs[i].surface != NULL)
			SDL_FreeSurface(ui->cover_worker.jobs[i].surface);
		memset(&ui->cover_worker.jobs[i], 0,
		       sizeof(ui->cover_worker.jobs[i]));
	}
}

static void test_priority_and_rapid_scroll(struct ui *ui)
{
	ui->metrics.input_counter = 1U;
	ui->game_index = 5;
	cover_cache_sync_visible(ui);
	check_queue_matches_visible(ui);

	for (size_t step = 0; step < 200; ++step) {
		ui->game_index = (step * 7U + 3U) % GAME_COUNT;
		cover_cache_sync_visible(ui);
		check_queue_matches_visible(ui);
	}
	CHECK(cover_cache_queued_cancelled_count(ui) > 0);
	CHECK(cover_cache_stale_dropped_count(ui) >=
	      cover_cache_queued_cancelled_count(ui));
}

static void test_stale_ready_is_freed_not_stored(struct ui *ui)
{
	struct cover_job *stale;
	size_t stale_before = cover_cache_stale_dropped_count(ui);

	clear_jobs(ui);
	ui->game_index = 0;
	ui->metrics.input_counter = 1U;
	cover_cache_sync_visible(ui);
	stale = job_for(ui, 0);
	CHECK(stale != NULL);
	stale->surface = SDL_CreateRGBSurfaceWithFormat(0, 4, 4, 32,
							SDL_PIXELFORMAT_ARGB8888);
	CHECK(stale->surface != NULL);
	stale->ready = true;

	/* Index 8 has a disjoint +/-2 set from index 0. */
	ui->game_index = 8;
	cover_cache_sync_visible(ui);
	CHECK(item_for(ui, 0) == NULL);
	CHECK(job_for(ui, 0) == NULL);
	CHECK(cover_cache_stale_dropped_count(ui) > stale_before);
	check_queue_matches_visible(ui);
}

static void test_ready_priority_and_nonvisible_eviction(struct ui *ui)
{
	static const size_t old_visible[COVER_CACHE_MAX] = { 0, 16, 1, 15, 2 };
	struct cover_job *center;

	clear_jobs(ui);
	memset(ui->covers, 0, sizeof(ui->covers));
	for (size_t i = 0; i < COVER_CACHE_MAX; ++i) {
		ui->covers[i].occupied = true;
		ui->covers[i].game_id = old_visible[i];
		ui->covers[i].last_used = i + 1U;
	}

	/* New set is 1,0,2,16,3: only game 15 is safe to replace. */
	ui->game_index = 1;
	ui->metrics.input_counter = 1U;
	cover_cache_sync_visible(ui);
	center = job_for(ui, 3);
	CHECK(center != NULL);
	center->ready = true;
	ui->metrics.input_counter = 0U;
	cover_cache_sync_visible(ui);

	CHECK(item_for(ui, 15) == NULL);
	CHECK(item_for(ui, 0) != NULL);
	CHECK(item_for(ui, 1) != NULL);
	CHECK(item_for(ui, 2) != NULL);
	CHECK(item_for(ui, 3) != NULL);
	CHECK(item_for(ui, 16) != NULL);
	CHECK(cover_cache_visible_eviction_count(ui) == 0);
}

int main(void)
{
	struct ui ui;
	size_t visible[GAME_COUNT];
	size_t stale;
	size_t cancelled;
	size_t visible_evictions;

	memset(&ui, 0, sizeof(ui));
	for (size_t i = 0; i < GAME_COUNT; ++i)
		visible[i] = i;
	ui.catalog.visible = visible;
	ui.catalog.visible_count = GAME_COUNT;
	ui.catalog.visible_capacity = GAME_COUNT;
	ui.catalog.game_count = GAME_COUNT;
	CHECK(SDL_Init(0) == 0);
	ui.cover_worker.mutex = SDL_CreateMutex();
	ui.cover_worker.condition = SDL_CreateCond();
	CHECK(ui.cover_worker.mutex != NULL);
	CHECK(ui.cover_worker.condition != NULL);

	test_priority_and_rapid_scroll(&ui);
	test_stale_ready_is_freed_not_stored(&ui);
	test_ready_priority_and_nonvisible_eviction(&ui);
	stale = cover_cache_stale_dropped_count(&ui);
	cancelled = cover_cache_queued_cancelled_count(&ui);
	visible_evictions = cover_cache_visible_eviction_count(&ui);
	cover_cache_destroy(&ui);
	SDL_Quit();
	(void)printf("COVER_CACHE_STALE_TEST PASS checks=%u stale=%zu cancelled=%zu visible_evictions=%zu\n",
		     checks, stale, cancelled, visible_evictions);
	return 0;
}
