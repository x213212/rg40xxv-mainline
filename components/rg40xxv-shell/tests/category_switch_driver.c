#define _POSIX_C_SOURCE 200809L

#include <SDL.h>

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*
 * Keep the production entry point unchanged.  The sanitizer integration test
 * links this translation unit instead of src/main.c, then drives real SDL
 * key events from a second thread while the normal UI loop is running.
 */
#define frame_scheduler_init rg40xxv_test_frame_scheduler_init
#define main rg40xxv_ui_main
#include "../src/main.c"
#undef main
#undef frame_scheduler_init

enum {
	CATEGORY_SWITCH_COUNT = 73,
	INPUT_READY_GRACE_MS = 200,
	KEY_DOWN_MS = 10,
	KEY_UP_MS = 20,
};

static atomic_bool ui_loop_ready;
static atomic_bool empty_rpg_page_rendered;
static atomic_uint rendered_pages;
static atomic_uint rpg_filter_failures;

struct category_switch_result {
	unsigned int key_downs;
	unsigned int push_failures;
};

static void pause_ms(long milliseconds)
{
	struct timespec delay = {
		.tv_sec = milliseconds / 1000,
		.tv_nsec = milliseconds % 1000 * 1000000L,
	};

	while (nanosleep(&delay, &delay) != 0)
		;
}

/* Signal after production input_init and immediately before the UI loop. */
extern void frame_scheduler_init(struct frame_scheduler *scheduler,
				 uint32_t now);

void rg40xxv_test_frame_scheduler_init(struct frame_scheduler *scheduler,
				       uint32_t now)
{
	frame_scheduler_init(scheduler, now);
	atomic_store_explicit(&ui_loop_ready, true, memory_order_release);
}

extern void __real_render_scene(struct ui *ui, uint32_t now);

void __wrap_render_scene(struct ui *ui, uint32_t now)
{
	if (ui->nav_index >= 0 && ui->nav_index < NAV_COUNT)
		(void)atomic_fetch_or_explicit(&rendered_pages,
			1U << (unsigned int)ui->nav_index, memory_order_relaxed);
	if (ui->nav_index == NAV_PAGE_RPG) {
		bool valid = ui->catalog.rpg_only && !ui->catalog.apps_only;

		for (size_t index = 0; index < ui->catalog.visible_count; ++index) {
			const struct game_entry *game =
				catalog_visible_game(ui, index);

			if (game == NULL || !catalog_system_is_rpg(game->system))
				valid = false;
		}
		if (!valid)
			(void)atomic_fetch_add_explicit(&rpg_filter_failures, 1U,
				memory_order_relaxed);
		if (ui->catalog.game_count > 0U &&
		    ui->catalog.visible_count == 0U)
			atomic_store_explicit(&empty_rpg_page_rendered, true,
				memory_order_relaxed);
	}
	__real_render_scene(ui, now);
}

static void *inject_category_switches(void *argument)
{
	struct category_switch_result *result = argument;
	SDL_Event event;

	while (!atomic_load_explicit(&ui_loop_ready, memory_order_acquire))
		pause_ms(10);
	/* Let the post-input_init neutral latch complete before the first key. */
	pause_ms(INPUT_READY_GRACE_MS);
	for (unsigned int i = 0; i < CATEGORY_SWITCH_COUNT; ++i) {
		memset(&event, 0, sizeof(event));
		event.type = SDL_KEYDOWN;
		event.key.state = SDL_PRESSED;
		event.key.repeat = 0;
		event.key.keysym.sym = SDLK_RIGHT;
		if (SDL_PushEvent(&event) == 1)
			++result->key_downs;
		else
			++result->push_failures;
		pause_ms(KEY_DOWN_MS);
		event.type = SDL_KEYUP;
		event.key.state = SDL_RELEASED;
		if (SDL_PushEvent(&event) != 1)
			++result->push_failures;
		pause_ms(KEY_UP_MS);
	}
	return NULL;
}

int main(int argc, char **argv)
{
	const char *enabled = getenv("RG40XXV_TEST_CATEGORY_SWITCH");
	struct category_switch_result result = { 0 };
	pthread_t thread;
	bool inject = enabled != NULL && strcmp(enabled, "1") == 0;
	const unsigned int all_pages = (1U << NAV_COUNT) - 1U;
	int status;

	if (inject && pthread_create(&thread, NULL, inject_category_switches,
				     &result) != 0) {
		(void)fprintf(stderr, "category switch injector: pthread_create\n");
		return 1;
	}
	status = rg40xxv_ui_main(argc, argv);
	if (!inject)
		return status;
	(void)pthread_join(thread, NULL);
	if (status != 0 || result.key_downs != CATEGORY_SWITCH_COUNT ||
	    result.push_failures != 0U ||
	    atomic_load_explicit(&rendered_pages, memory_order_relaxed) !=
		all_pages ||
	    !atomic_load_explicit(&empty_rpg_page_rendered,
		memory_order_relaxed) ||
	    atomic_load_explicit(&rpg_filter_failures,
		memory_order_relaxed) != 0U) {
		(void)fprintf(stderr,
			"category switch injector failed: status=%d key_downs=%u failures=%u pages=0x%02x rpg_empty=%d rpg_filter_failures=%u\n",
			status, result.key_downs, result.push_failures,
			atomic_load_explicit(&rendered_pages,
				memory_order_relaxed),
			atomic_load_explicit(&empty_rpg_page_rendered,
				memory_order_relaxed),
			atomic_load_explicit(&rpg_filter_failures,
				memory_order_relaxed));
		return 1;
	}
	(void)printf("CATEGORY_SWITCH_DRIVER PASS switches=%u pages=0x%02x rpg_empty=yes rpg_filter=PASS\n",
		result.key_downs, all_pages);
	return 0;
}
