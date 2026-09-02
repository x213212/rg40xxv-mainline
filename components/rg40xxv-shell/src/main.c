#define _POSIX_C_SOURCE 200809L
#include "frame_scheduler.h"
#include "ui.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
static volatile sig_atomic_t termination_requested;

static int network_backend_action(void *context,
				  const struct network_ui_request *request)
{
	struct settings_state *settings = context;

	if (request == NULL)
		return EINVAL;
	switch (request->action) {
	case NETWORK_UI_ACTION_SCAN:
		return request->mode == NETWORK_UI_WIFI ?
			settings_request_wifi_scan(settings) : EOPNOTSUPP;
	case NETWORK_UI_ACTION_CONNECT:
		return request->mode == NETWORK_UI_WIFI ?
			settings_request_wifi_connect(settings, request->bssid,
				request->password) : EOPNOTSUPP;
	case NETWORK_UI_ACTION_DISCONNECT:
		return request->mode == NETWORK_UI_WIFI ?
			settings_request_wifi_disconnect(settings) : EOPNOTSUPP;
	case NETWORK_UI_ACTION_FORGET:
		return request->mode == NETWORK_UI_WIFI ?
			settings_request_wifi_forget(settings, request->uuid) :
			EOPNOTSUPP;
	case NETWORK_UI_ACTION_HOTSPOT_SET:
		return request->mode == NETWORK_UI_HOTSPOT ?
			settings_request_hotspot_set(settings, request->enabled) :
			EOPNOTSUPP;
	case NETWORK_UI_ACTION_RECOVER:
		return request->mode == NETWORK_UI_WIFI ?
			settings_request_wifi_recover(settings) : EOPNOTSUPP;
	case NETWORK_UI_ACTION_STATUS:
		return settings_request_network_status(settings);
	}
	return EINVAL;
}

static uint64_t monotonic_ns(void)
{
	struct timespec value;

	if (clock_gettime(CLOCK_MONOTONIC, &value) != 0)
		return 0;
	return (uint64_t)value.tv_sec * 1000000000ULL +
		(uint64_t)value.tv_nsec;
}

static uint64_t game_exit_monotonic_ns(void)
{
	const char *value = getenv("RG_GAME_EXIT_MONOTONIC_NS");
	char *end = NULL;
	unsigned long long parsed;

	if (value == NULL || value[0] == '\0')
		return 0;
	errno = 0;
	parsed = strtoull(value, &end, 10);
	if (errno != 0 || end == value || *end != '\0' || parsed == 0U) {
		(void)fprintf(stderr,
			"UI_FIRST_FRAME warning=invalid-game-exit-monotonic\n");
		return 0;
	}
	return (uint64_t)parsed;
}
static void request_termination(int signal_number)
{
	(void)signal_number;
	termination_requested = 1;
}
static int install_signal_handlers(void)
{
	struct sigaction action;

	memset(&action, 0, sizeof(action));
	action.sa_handler = request_termination;
	(void)sigemptyset(&action.sa_mask);
	if (sigaction(SIGTERM, &action, NULL) != 0 ||
	    sigaction(SIGINT, &action, NULL) != 0 ||
	    sigaction(SIGHUP, &action, NULL) != 0)
		return -1;
	return 0;
}
static int publish_ready_marker(const char *path)
{
	static const char payload[] = "first-frame-presented\n";
	struct stat info;
	int fd;
	ssize_t written;

	if (path == NULL)
		return 0;
	if (strcmp(path, "/run/rg40xxv-ui.ready") != 0)
		return -1;
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW,
		  0600);
	if (fd < 0)
		return -1;
	if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) ||
	    info.st_uid != geteuid()) {
		(void)close(fd);
		return -1;
	}
	written = write(fd, payload, sizeof(payload) - 1U);
	if (close(fd) != 0 || written != (ssize_t)(sizeof(payload) - 1U))
		return -1;
	return 0;
}

static int sooner_timeout(int current, int candidate)
{
	if (candidate < 0)
		return current;
	return current < 0 || candidate < current ? candidate : current;
}

static int deadline_timeout_ms(uint32_t now, uint32_t deadline)
{
	uint32_t remaining;

	if (SDL_TICKS_PASSED(now, deadline))
		return 0;
	remaining = deadline - now;
	return remaining > (uint32_t)INT_MAX ? INT_MAX : (int)remaining;
}

static bool benchmark_input_pending(const struct ui *ui)
{
	enum { SETTINGS_BENCHMARK_INPUT_COUNT = 20 };

	return ui->benchmark &&
		(!ui->settings_preview ||
		 ui->benchmark_input_count < SETTINGS_BENCHMARK_INPUT_COUNT);
}

static SDL_Keycode settings_benchmark_key(unsigned int input)
{
	static const SDL_Keycode sequence[] = {
		SDLK_RETURN, SDLK_RIGHT, SDLK_ESCAPE, SDLK_RIGHT,
		SDLK_F3, SDLK_RETURN, SDLK_LEFT, SDLK_RIGHT,
	};

	return input < sizeof(sequence) / sizeof(sequence[0]) ?
		sequence[input] : SDLK_F3;
}

static int required_wake_ms(const struct ui *ui, uint32_t now,
			    uint32_t started, bool screenshot_delay_seen,
			    bool screenshot_force_seen)
{
	int timeout = power_next_timeout_ms(&ui->power, now);
	int idle_timeout = power_idle_timeout_ms(&ui->power,
		ui->last_user_activity_at,
		(uint32_t)ui->settings.preferences.auto_screen_off_minutes *
			UINT32_C(60000), now);

	timeout = sooner_timeout(timeout, idle_timeout);

	/* Complete the post-start/resume neutral latch before the first key. */
	if (input_latch_waiting(&ui->input_latch) &&
	    ui->input_latch.neutral_since_valid &&
	    input_latch_active_count(&ui->input_latch) == 0U)
		timeout = sooner_timeout(timeout, deadline_timeout_ms(now,
			ui->input_latch.neutral_since +
			ui->input_latch.stable_interval_ms));
	if (benchmark_input_pending(ui))
		timeout = sooner_timeout(timeout,
			deadline_timeout_ms(now, ui->benchmark_next_input));
	if (ui->launch_once && !ui->launch_once_queued)
		timeout = sooner_timeout(timeout,
			deadline_timeout_ms(now, ui->launch_once_at));
	if (ui->demo_deadline != 0U)
		timeout = sooner_timeout(timeout,
			deadline_timeout_ms(now, ui->demo_deadline));
	if (ui->screenshot_path != NULL && !ui->screenshot_done) {
		if (!screenshot_delay_seen)
			timeout = sooner_timeout(timeout, deadline_timeout_ms(now,
				started + ui->screenshot_delay_ms));
		else if (!screenshot_force_seen)
			timeout = sooner_timeout(timeout,
				deadline_timeout_ms(now, started + 3000U));
	}
	return timeout;
}

static struct frame_scheduler_query scheduler_query(const struct ui *ui,
					     uint32_t now,
					     int required_wake)
{
	return (struct frame_scheduler_query) {
		.now = now,
		.action_until = ui->action_until,
		.required_wake_ms = required_wake,
		.visible = power_should_render(&ui->power),
		.animate_60hz = ui->carousel_position != ui->carousel_target ||
			ui->launch.transition == LAUNCH_TRANSITION_STARTING ||
			ui->launch.transition == LAUNCH_TRANSITION_RETURNED,
		.marquee_10hz = ui->nav_index == NAV_PAGE_SETTINGS &&
			ui->settings_marquee_active,
		.action_active = ui->action_text != NULL && ui->action_until != 0U,
		.raw_evdev = ui->input_fd >= 0 || ui->power_input_fd >= 0,
	};
}

static bool raw_evdev_pending(int fd)
{
	struct pollfd input = {
		.fd = fd,
		.events = POLLIN | POLLERR | POLLHUP,
	};

	return fd >= 0 && poll(&input, 1, 0) > 0 &&
		(input.revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL)) != 0;
}

static bool sdl_event_pending(void)
{
	SDL_Event event;

	return SDL_PeepEvents(&event, 1, SDL_PEEKEVENT,
		SDL_FIRSTEVENT, SDL_LASTEVENT) > 0;
}

static bool wait_for_activity(struct ui *ui, int timeout)
{
	struct pollfd inputs[2];
	nfds_t count = 0;
	SDL_Event event;
	int received;

	if (timeout <= 0)
		return false;
	if (sdl_event_pending() || raw_evdev_pending(ui->input_fd) ||
	    raw_evdev_pending(ui->power_input_fd))
		return true;
	if (ui->input_fd >= 0) {
		inputs[count].fd = ui->input_fd;
		inputs[count].events = POLLIN | POLLERR | POLLHUP;
		inputs[count].revents = 0;
		++count;
	}
	if (ui->power_input_fd >= 0 && ui->power_input_fd != ui->input_fd) {
		inputs[count].fd = ui->power_input_fd;
		inputs[count].events = POLLIN | POLLERR | POLLHUP;
		inputs[count].revents = 0;
		++count;
	}
	if (count > 0U) {
		if (timeout > FRAME_SCHEDULER_RAW_EVDEV_MAX_WAIT_MS)
			timeout = FRAME_SCHEDULER_RAW_EVDEV_MAX_WAIT_MS;
		do {
			received = poll(inputs, count, timeout);
		} while (received < 0 && errno == EINTR &&
			 !termination_requested);
		return received > 0 || sdl_event_pending();
	}
	received = SDL_WaitEventTimeout(&event, timeout);
	if (received != 0)
		(void)SDL_PushEvent(&event);
	return received != 0 || raw_evdev_pending(ui->input_fd) ||
		raw_evdev_pending(ui->power_input_fd);
}
static void cleanup_mode(struct ui *ui, bool supervisor_handoff)
{
	if (catalog_refresh_stop(ui) != 0)
		(void)fprintf(stderr, "CATALOG_REFRESH stop=failed\n");
	launch_shutdown(ui);
	settings_backend_stop(&ui->settings);
	search_close(ui);
	persistence_stop(ui);
	monitor_stop(ui);
	cover_cache_destroy(ui);
	catalog_destroy(ui);
	platform_routes_destroy(ui);
	stream_destroy(ui);
	bluetooth_backend_stop(&ui->bluetooth.backend);
	metrics_destroy(ui);
	render_destroy(ui);
	/*
	 * SDL 2.28.5 switches its ALSA handle back to blocking mode.  On the
	 * physical H700 the playback thread can remain in snd_pcm_writei/ppoll
	 * forever, and SDL_CloseAudioDevice then waits forever for that thread.
	 * A supervisor handoff is a process boundary, so leave only that audio
	 * device to the kernel's process-exit cleanup.  Normal exits retain the
	 * complete SDL teardown.
	 */
	if (!supervisor_handoff)
		audio_close(ui);
	input_close(ui);
	for (int i = 0; i < FONT_COUNT; ++i)
		ui->fonts[i] = NULL;
	if (ui->renderer != NULL)
		SDL_DestroyRenderer(ui->renderer);
	if (ui->window != NULL)
		SDL_DestroyWindow(ui->window);
	ui->renderer = NULL;
	ui->window = NULL;
	TTF_Quit();
	IMG_Quit();
	if (supervisor_handoff)
		SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER |
				      SDL_INIT_JOYSTICK);
	else
		SDL_Quit();
}

static void cleanup(struct ui *ui)
{
	cleanup_mode(ui, false);
}

static bool launch_graphics_released(const struct ui *ui)
{
	if (ui->window != NULL || ui->renderer != NULL || ui->icon_atlas != NULL ||
	    ui->navigation_cache != NULL || ui->controls_cache != NULL ||
	    ui->backdrop_cache != NULL ||
	    ui->settings_background_cache[0] != NULL ||
	    ui->settings_background_cache[1] != NULL ||
	    ui->settings_page_cache != NULL ||
	    ui->cache_count != 0U || ui->retired_texture_count != 0U ||
	    ui->cover_worker.thread != NULL || ui->cover_worker.mutex != NULL ||
	    ui->catalog_refresh_pid != 0)
		return false;
	for (int i = 0; i < FONT_COUNT; ++i) {
		if (ui->fonts[i] != NULL)
			return false;
	}
	for (size_t i = 0; i < COVER_CACHE_MAX; ++i) {
		if (ui->covers[i].texture != NULL ||
		    ui->cover_worker.jobs[i].surface != NULL)
			return false;
	}
	return true;
}

static int parse_args(int argc, char **argv, struct ui *ui,
			      const char **font, const char **rom_root,
			      const char **optional_rom_root,
			      const char **initial_search,
			      const char **favorites_path, const char **stock_path,
			      const char **history_path,
			      const char **settings_path, const char **language,
			      const char **filter_state_path,
			      const char **platform_routes_path,
			      const char **launcher_path,
			      const char **stream_launcher_path,
			      const char **launch_log_path,
			      const char **handoff_path,
			      const char **state_dir,
			      const char **hardwarectl_path,
			      const char **hardwarectl_log_path,
			      const char **hardware_root,
			      const char **ready_file,
			      const char **catalog_snapshot_path,
			      bool *catalog_refresh_only,
			      uint32_t *catalog_refresh_budget_ms,
			      bool *windowed,
			      int *demo_ms)
{
	for (int i = 1; i < argc; ++i) {
		if (strcmp(argv[i], "--font") == 0 && i + 1 < argc)
			*font = argv[++i];
		else if (strcmp(argv[i], "--icon-atlas") == 0 && i + 1 < argc)
			ui->icon_atlas_argument = argv[++i];
		else if (strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc)
			ui->screenshot_path = argv[++i];
		else if (strcmp(argv[i], "--screenshot-delay-ms") == 0 &&
			 i + 1 < argc)
			ui->screenshot_delay_ms = (uint32_t)strtoul(argv[++i], NULL, 10);
		else if (strcmp(argv[i], "--demo-ms") == 0 && i + 1 < argc)
			*demo_ms = atoi(argv[++i]);
		else if (strcmp(argv[i], "--rom-root") == 0 && i + 1 < argc)
			*rom_root = argv[++i];
		else if (strcmp(argv[i], "--optional-rom-root") == 0 &&
			 i + 1 < argc)
			*optional_rom_root = argv[++i];
		else if (strcmp(argv[i], "--search") == 0 && i + 1 < argc)
			*initial_search = argv[++i];
		else if (strcmp(argv[i], "--favorites-file") == 0 && i + 1 < argc)
			*favorites_path = argv[++i];
		else if (strcmp(argv[i], "--stock-favorites") == 0 && i + 1 < argc)
			*stock_path = argv[++i];
		else if (strcmp(argv[i], "--history-file") == 0 && i + 1 < argc)
			*history_path = argv[++i];
		else if (strcmp(argv[i], "--settings-file") == 0 && i + 1 < argc)
			*settings_path = argv[++i];
		else if (strcmp(argv[i], "--language") == 0 && i + 1 < argc)
			*language = argv[++i];
		else if (strcmp(argv[i], "--filter-state") == 0 && i + 1 < argc)
			*filter_state_path = argv[++i];
		else if (strcmp(argv[i], "--platform-routes") == 0 && i + 1 < argc)
			*platform_routes_path = argv[++i];
		else if (strcmp(argv[i], "--launcher") == 0 && i + 1 < argc)
			*launcher_path = argv[++i];
		else if (strcmp(argv[i], "--stream-launcher") == 0 && i + 1 < argc)
			*stream_launcher_path = argv[++i];
		else if (strcmp(argv[i], "--launch-log") == 0 && i + 1 < argc)
			*launch_log_path = argv[++i];
		else if (strcmp(argv[i], "--handoff-file") == 0 && i + 1 < argc)
			*handoff_path = argv[++i];
		else if (strcmp(argv[i], "--state-dir") == 0 && i + 1 < argc)
			*state_dir = argv[++i];
		else if (strcmp(argv[i], "--hardwarectl") == 0 && i + 1 < argc)
			*hardwarectl_path = argv[++i];
		else if (strcmp(argv[i], "--hardwarectl-log") == 0 && i + 1 < argc)
			*hardwarectl_log_path = argv[++i];
		else if ((strcmp(argv[i], "--hardware-root") == 0 ||
			  strcmp(argv[i], "--fixture-root") == 0) && i + 1 < argc)
			*hardware_root = argv[++i];
		else if (strcmp(argv[i], "--ready-file") == 0 && i + 1 < argc)
			*ready_file = argv[++i];
		else if (strcmp(argv[i], "--catalog-snapshot") == 0 &&
			 i + 1 < argc)
			*catalog_snapshot_path = argv[++i];
		else if (strcmp(argv[i], "--catalog-refresh-only") == 0)
			*catalog_refresh_only = true;
		else if (strcmp(argv[i], "--catalog-refresh-budget-ms") == 0 &&
			 i + 1 < argc) {
			char *end = NULL;
			unsigned long value;

			errno = 0;
			value = strtoul(argv[++i], &end, 10);
			if (errno != 0 || end == argv[i] || *end != '\0' ||
			    value == 0U || value > 30000U)
				return -1;
			*catalog_refresh_budget_ms = (uint32_t)value;
		}
		else if (strcmp(argv[i], "--cover-cache-dir") == 0 &&
			 i + 1 < argc) {
			if (cover_cache_configure(ui, argv[++i]) != 0) {
				(void)fprintf(stderr,
					"invalid cover cache directory: %s\n", argv[i]);
				return -1;
			}
		}
		else if (strcmp(argv[i], "--resident") == 0)
			ui->resident = true;
		else if (strcmp(argv[i], "--benchmark") == 0)
			ui->benchmark = true;
		else if (strcmp(argv[i], "--navigation-stress") == 0) {
			ui->navigation_stress = true;
			ui->benchmark = true;
		}
		else if (strcmp(argv[i], "--filter-stress") == 0) {
			ui->filter_stress = true;
			ui->benchmark = true;
		}
		else if (strcmp(argv[i], "--launch-once") == 0)
			ui->launch_once = true;
		else if (strcmp(argv[i], "--settings-preview") == 0)
			ui->settings_preview = true;
		else if (strcmp(argv[i], "--stream-preview") == 0)
			ui->stream_preview = true;
		else if (strcmp(argv[i], "--apps-preview") == 0)
			ui->apps_preview = true;
		else if (strcmp(argv[i], "--rpg-preview") == 0)
			ui->rpg_preview = true;
		else if (strcmp(argv[i], "--network-preview") == 0)
			ui->network_preview = true;
		else if (strcmp(argv[i], "--launch-preview") == 0)
			ui->launch_preview = true;
		else if (strcmp(argv[i], "--lock-preview") == 0)
			ui->lock_preview = true;
		else if (strcmp(argv[i], "--content-preview") == 0)
			ui->content_preview = true;
		else if (strcmp(argv[i], "--catalog-report") == 0)
			ui->catalog_report = true;
		else if (strcmp(argv[i], "--windowed") == 0)
			*windowed = true;
		else {
			fprintf(stderr, "unknown argument: %s\n", argv[i]);
			return -1;
		}
	}
	return 0;
}
int main(int argc, char **argv)
{
	struct ui ui = { 0 };
	struct youtube_capability youtube = { 0 };
	struct frame_scheduler scheduler;
	const char *font_argument = NULL;
	const char *font_path;
	const char *icon_atlas_path;
	const char *rom_root = "/mnt/mmc/Roms";
	const char *optional_rom_root = "/mnt/sdcard/Roms";
	const char *initial_search = NULL;
	const char *favorites_path = "/mnt/data/misc/rg40xxv-shell-favorites.tsv";
	const char *stock_path = "/mnt/data/misc/.favorite";
	const char *history_path = "/mnt/data/misc/rg40xxv-shell-history.tsv";
	const char *settings_path = "/mnt/data/misc/rg40xxv-shell-locale.conf";
	const char *language = NULL;
	const char *filter_state_path = "/mnt/data/misc/rg40xxv-shell-filters.conf";
	const char *platform_routes_path = "/run/rg40xxv-ui/platform-routes.json";
	const char *launcher_path = "/run/rg40xxv-ui/unified-launch.sh";
	const char *stream_launcher_path =
		"/opt/rg40xxv/bin/rg40xxv-stream";
	const char *launch_log_path =
		"/mnt/data/misc/rg40xxv-shell-launch.log";
	const char *handoff_path = NULL;
	const char *state_dir = "/var/lib/rg40xxv/netstream";
	const char *hardwarectl_path = "/usr/sbin/ui-hardwarectl";
	const char *hardwarectl_log_path =
		"/mnt/data/misc/rg40xxv-hardwarectl.log";
	const char *hardware_root = "/";
	const char *ready_file = NULL;
	const char *catalog_snapshot_path = NULL;
	char optional_catalog_snapshot_path[PATH_MAX] = { 0 };
	const char *bluetooth_helper_path = getenv(
		"RG40XXV_UI_BLUETOOTH_HELPER");
	const char *youtube_capability_path = getenv(
		"RG40XXV_UI_YOUTUBE_CAPABILITY");
	const char *rpg_translation_state_path = getenv(
		"RG_RMUT_TRANSLATION_STATE");
	const char *catalog_source = "bounded-fallback";
	bool windowed = false;
	bool catalog_refresh_only = false;
	bool catalog_snapshot_complete = false;
	bool optional_rom_root_available = false;
	bool first_frame_presented = false;
	uint32_t catalog_refresh_budget_ms = 30000U;
	bool ready_published = false;
	bool screenshot_delay_seen = false;
	bool screenshot_force_seen = false;
	int demo_ms = 0;
	uint32_t started;
	uint64_t game_exit_ns;
	uint64_t frames = 0U;
	if (parse_args(argc, argv, &ui, &font_argument, &rom_root,
		       &optional_rom_root,
		       &initial_search, &favorites_path, &stock_path, &history_path,
		       &settings_path, &language,
		       &filter_state_path,
		       &platform_routes_path,
		       &launcher_path, &stream_launcher_path, &launch_log_path,
		       &handoff_path,
		       &state_dir,
		       &hardwarectl_path, &hardwarectl_log_path,
		       &hardware_root, &ready_file, &catalog_snapshot_path,
		       &catalog_refresh_only,
		       &catalog_refresh_budget_ms,
		       &windowed, &demo_ms) != 0)
		return 2;
	if (youtube_capability_path == NULL || youtube_capability_path[0] == '\0')
		youtube_capability_path =
			"/opt/rg40xxv/youtube/runtime/admission.env";
	if (bluetooth_helper_path == NULL || bluetooth_helper_path[0] == '\0')
		bluetooth_helper_path = "/usr/sbin/rg40xxv-bluetooth-control";
	if (rpg_translation_state_path == NULL ||
	    rpg_translation_state_path[0] == '\0')
		rpg_translation_state_path =
			"/mnt/data/rg40xxv/state/rpgmaker/translation-mode";
	if (catalog_refresh_only) {
		uint64_t scan_started_ns;
		uint64_t scan_elapsed_ms;
		int scan_result;
		int output_status;

		if (catalog_snapshot_path == NULL ||
		    catalog_snapshot_path[0] != '/' || rom_root[0] != '/' ||
		    platform_routes_path[0] != '/') {
			(void)fprintf(stderr,
				"catalog refresh: absolute paths are required\n");
			return 2;
		}
		if (SDL_Init(SDL_INIT_TIMER) != 0)
			return 1;
		if (platform_routes_load(&ui, platform_routes_path) != 0) {
			(void)fprintf(stderr, "catalog refresh: routes unavailable\n");
			SDL_Quit();
			return 1;
		}
		scan_started_ns = monotonic_ns();
		scan_result = catalog_snapshot_refresh(&ui, catalog_snapshot_path,
			rom_root, catalog_refresh_budget_ms);
		scan_elapsed_ms = (monotonic_ns() - scan_started_ns) / 1000000ULL;
		output_status = scan_result >= 0 ? 0 : 1;
		(void)fprintf(stderr,
			"CATALOG_REFRESH_RESULT result=%s games=%zu scan_ms=%llu\n",
			output_status != 0 ? "error" : scan_result == 0 ?
			"complete" : "limited", ui.catalog.game_count,
			(unsigned long long)scan_elapsed_ms);
		catalog_destroy(&ui);
		platform_routes_destroy(&ui);
		SDL_Quit();
		if (output_status != 0)
			return 1;
		return scan_result == 0 ? 0 : 75;
	}
	if (install_signal_handlers() != 0) {
		perror("sigaction");
		return 1;
	}
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_TIMER) != 0) {
		fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
		return 1;
	}
	if (TTF_Init() != 0) {
		fprintf(stderr, "TTF_Init: %s\n", TTF_GetError());
		SDL_Quit();
		return 1;
	}
	font_path = render_find_font(font_argument);
	if (font_path == NULL) {
		fprintf(stderr, "no Traditional Chinese font found\n");
		cleanup(&ui);
		return 1;
	}
	icon_atlas_path = material_icons_find(ui.icon_atlas_argument);
	if ((IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG) & IMG_INIT_PNG) == 0) {
		fprintf(stderr, "SDL_image PNG support unavailable: %s\n", SDL_GetError());
		cleanup(&ui);
		return 1;
	}
	/* Small covers avoid the software renderer's costly per-frame bilinear path. */
	(void)SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
	/*
	 * Keep the GLES2 renderer accelerated, but do not retain SDL's private
	 * render-command linked list across a whole frame.  The H700 crash core
	 * captured a corrupted render_commands_pool in SDL 2.0.20.  UI geometry is
	 * batched explicitly below, so immediate command submission remains small
	 * without falling back to the software renderer.
	 */
	(void)SDL_SetHint(SDL_HINT_RENDER_BATCHING, "0");
	if (lifecycle_graphics_init(&ui, font_path, windowed) != 0) {
		fprintf(stderr, "graphics: %s\n", SDL_GetError());
		cleanup(&ui);
		return 1;
	}
	if (icon_atlas_path == NULL ||
	    material_icons_load(&ui, icon_atlas_path) != 0)
		fprintf(stderr, "material icon atlas unavailable: %s\n",
			icon_atlas_path == NULL ? "not found" : icon_atlas_path);
	launch_configure(&ui, launcher_path, launch_log_path, handoff_path,
		font_path, windowed);
	(void)setenv("TZ", "Asia/Taipei", 1);
	tzset();
	hardware_backend_init(&ui.hardware_backend);
	if (hardware_backend_set_fixture_root(&ui.hardware_backend,
					      hardware_root) != 0) {
		fprintf(stderr, "hardware root must be an existing absolute directory: %s\n",
			hardware_root);
		cleanup(&ui);
		return 1;
	}
	(void)hardware_refresh(&ui.hardware_backend, &ui.hardware, 1);
	if (monitor_start(&ui) != 0) {
		fprintf(stderr, "hardware monitor: %s\n", SDL_GetError());
		cleanup(&ui);
		return 1;
	}
	settings_init(&ui.settings);
	ui.settings.preferences.backlight_percent =
		ui.hardware.backlight.percent >= 0 ? ui.hardware.backlight.percent : 50;
	ui.settings.preferences.joystick_rgb_brightness =
		ui.hardware.joystick_rgb.configured_percent >= 0 ?
		ui.hardware.joystick_rgb.configured_percent :
		ui.hardware.joystick_rgb.actual_percent >= 0 ?
		ui.hardware.joystick_rgb.actual_percent : 0;
	ui.settings.preferences.joystick_rgb_enabled =
		ui.settings.preferences.joystick_rgb_brightness > 0;
	if (settings_backend_start(&ui.settings, hardwarectl_path,
				   hardwarectl_log_path) != 0)
		fprintf(stderr, "hardware control unavailable: %s\n",
			hardwarectl_path);
	stream_init(&ui, state_dir, stream_launcher_path);
	network_ui_state_init(&ui.network);
	if (settings_backend_available(&ui.settings))
		network_ui_state_set_backend(&ui.network, network_backend_action,
			&ui.settings);
	(void)network_ui_state_load_snapshot(&ui.network);
	if (bluetooth_backend_start(&ui.bluetooth.backend,
		bluetooth_helper_path) != 0)
		fprintf(stderr, "bluetooth backend: initialization failed\n");
	(void)bluetooth_ui_update(&ui, SDL_GetTicks());
	if (ui.streaming.phase == STREAM_BACKEND_ERROR)
		fprintf(stderr, "stream hosts: %s\n", ui.streaming.load_error);
	locale_init(&ui, settings_path, language);
	if (rpg_translation_init(&ui.rpg_translation,
		    rpg_translation_state_path) != 0)
		(void)fprintf(stderr,
			"RPG_TRANSLATION state=INVALID mode=off source=%s\n",
			rpg_translation_state_path);
	power_state_init(&ui.power,
			 ui.settings.preferences.screen_lock_enabled);
	input_method_init(&ui.input_method, INPUT_FIELD_TEXT);
	if (platform_routes_load(&ui, platform_routes_path) != 0)
		fprintf(stderr, "routes: cannot load %s; using scanner defaults\n",
			platform_routes_path);
	optional_rom_root_available = catalog_optional_source_available(
		"/mnt/sdcard", optional_rom_root);
	if (catalog_snapshot_path != NULL &&
	    snprintf(optional_catalog_snapshot_path,
		     sizeof(optional_catalog_snapshot_path), "%s.tf2",
		     catalog_snapshot_path) >= (int)sizeof(optional_catalog_snapshot_path))
		optional_catalog_snapshot_path[0] = '\0';
	{
		uint32_t scan_started = SDL_GetTicks();
		int scan_result;
		bool primary_catalog_available;

		if (catalog_snapshot_path != NULL &&
		    catalog_snapshot_load(&ui, catalog_snapshot_path, rom_root,
			&catalog_snapshot_complete) == 0) {
			catalog_source = catalog_snapshot_complete ?
				"snapshot-complete" : "snapshot-limited";
			primary_catalog_available = true;
		} else {
			scan_result = catalog_scan_bounded(&ui, rom_root, 2048U, 300U);
			primary_catalog_available = scan_result >= 0;
			if (scan_result < 0)
				fprintf(stderr, "catalog: cannot scan %s\n", rom_root);
			else if (scan_result > 0)
				fprintf(stderr,
					"catalog: foreground scan bounded games=%zu\n",
					ui.catalog.game_count);
		}
		if (primary_catalog_available && optional_rom_root_available) {
			size_t before = ui.catalog.game_count;
			int optional_result;
			const char *optional_source = "direct";

			if (optional_catalog_snapshot_path[0] != '\0') {
				struct ui optional = { 0 };

				optional.routes = ui.routes;
				optional_result = catalog_snapshot_refresh(&optional,
					optional_catalog_snapshot_path,
					optional_rom_root, 300U);
				if (optional_result >= 0 &&
				    catalog_append_cached_source(&ui, &optional,
					optional_rom_root, rom_root) != 0)
					optional_result = -1;
				optional_source = optional_result < 0 ? "cache-error" :
					optional_result > 0 ? "cache-partial" : "cache";
				catalog_destroy(&optional);
			} else {
				optional_result = catalog_append_source_bounded(&ui,
					optional_rom_root, rom_root, 2048U, 300U);
			}

			(void)fprintf(stderr,
				"CATALOG_OPTIONAL source=TF2 state=mounted cache=%s result=%s added=%zu root=%s\n",
				optional_source,
				optional_result < 0 ? "unavailable" :
				optional_result > 0 ? "limited" : "complete",
				ui.catalog.game_count - before, optional_rom_root);
		} else {
			(void)fprintf(stderr,
				"CATALOG_OPTIONAL source=TF2 state=%s added=0 root=%s\n",
				primary_catalog_available ? "not-mounted" :
				"primary-unavailable", optional_rom_root);
		}
		ui.catalog_scan_ms = SDL_GetTicks() - scan_started;
	}
	(void)youtube_capability_load(&youtube, youtube_capability_path);
	if (youtube_catalog_add(&ui, &youtube) != 0) {
		fprintf(stderr, "youtube tile: allocation failed\n");
		cleanup(&ui);
		return 1;
	}
	(void)fprintf(stderr,
		"YOUTUBE_CAPABILITY native_route=%s native_device=%s source=%s\n",
		youtube.native_available ? "READY" : "UNAVAILABLE",
		youtube.native_device_verified ? "PASS" :
			youtube.native_launchable ? "UNVERIFIED" :
				youtube.native_available ? "FAIL" : "UNAVAILABLE",
		youtube_capability_path);
	favorites_load(&ui, favorites_path, stock_path);
	history_load(&ui, history_path);
	filter_state_load(&ui, filter_state_path);
	if (initial_search != NULL) {
		(void)snprintf(ui.catalog.query, sizeof(ui.catalog.query), "%s",
			       initial_search);
		catalog_apply_filters(&ui);
	}
	if (persistence_start(&ui) != 0) {
		fprintf(stderr, "persistence worker: %s\n", SDL_GetError());
		cleanup(&ui);
		return 1;
	}
	audio_init(&ui);
	input_init(&ui);
	if (cover_cache_init(&ui) != 0) {
		fprintf(stderr, "cover worker: %s\n", SDL_GetError());
		cleanup(&ui);
		return 1;
	}
	if (metrics_init(&ui) != 0) {
		fprintf(stderr, "frame metrics: allocation failed\n");
		cleanup(&ui);
		return 1;
	}
	if (initial_search != NULL)
		search_open(&ui);
	ui.running = true;
	ui.nav_index = ui.settings_preview ? NAV_PAGE_SETTINGS :
		ui.network_preview ? NAV_PAGE_NETWORK :
		ui.stream_preview ? NAV_PAGE_STREAMING :
		ui.rpg_preview ? NAV_PAGE_RPG :
		ui.apps_preview ? NAV_PAGE_APPS : NAV_PAGE_LIBRARY;
	ui.focus_region = ui.benchmark || ui.content_preview ?
		UI_FOCUS_CONTENT : UI_FOCUS_TOP_NAV;
	catalog_set_apps_view(&ui, ui.nav_index == NAV_PAGE_APPS);
	catalog_set_rpg_view(&ui, ui.nav_index == NAV_PAGE_RPG);
	if (ui.catalog.apps_only && ui.search_active)
		search_close(&ui);
	if (ui.lock_preview) {
		ui.power.locked = true;
		ui.power.view = POWER_VIEW_LOCKED;
	}
	/* Keep adjacent title rasterization out of input-to-present frames. */
	text_prewarm_visible(&ui);
	ui.action_text = tr(&ui, "preparing");
	render_prepare_static(&ui, SDL_GetTicks());
	if (ui.launch_preview) {
		const struct game_entry *preview_game =
			catalog_visible_game(&ui, ui.game_index);

		ui.launch.transition = LAUNCH_TRANSITION_STARTING;
		ui.launch.kind = LAUNCH_KIND_GAME;
		ui.launch.pending = false;
		(void)snprintf(ui.launch.game_title,
			sizeof(ui.launch.game_title), "%s",
			preview_game != NULL ? preview_game->title :
			tr(&ui, "launch_preview_title"));
		(void)snprintf(ui.launch.game_platform,
			sizeof(ui.launch.game_platform), "%s",
			preview_game != NULL ? preview_game->system_label :
			tr(&ui, "launch_preview_platform"));
		(void)snprintf(ui.launch.game_core,
			sizeof(ui.launch.game_core), "%s",
			preview_game != NULL ? preview_game->runtime : "RetroArch");
	}
	started = SDL_GetTicks();
	ui.last_user_activity_at = started;
	game_exit_ns = game_exit_monotonic_ns();
	if (demo_ms > 0)
		ui.demo_deadline = started + (uint32_t)demo_ms;
	ui.benchmark_next_input = started +
		(ui.navigation_stress || ui.filter_stress ? 8U : 250U);
	ui.launch_once_at = started + 250U;
	frame_scheduler_init(&scheduler, started);
	while (ui.running) {
		struct frame_scheduler_query query;
		enum launch_transition_phase transition_before;
		uint64_t frame_counter;
		uint32_t now = SDL_GetTicks();
		int wake_ms;

		catalog_refresh_poll(&ui);
		if (stream_update(&ui, now))
			frame_scheduler_invalidate(&scheduler);
		if (bluetooth_ui_update(&ui, now))
			frame_scheduler_invalidate(&scheduler);
		if (network_ui_tick(&ui, now))
			frame_scheduler_invalidate(&scheduler);

		if (termination_requested) {
			launch_shutdown(&ui);
			ui.running = false;
			break;
		}
		if (ui.launch.process.active || ui.launch.session_suspended) {
			ui.last_user_activity_at = now;
			(void)input_handle_power_events(&ui, now);
			transition_before = ui.launch.transition;
			launch_update(&ui, now);
			if (ui.launch.transition != transition_before)
				frame_scheduler_invalidate(&scheduler);
			SDL_Delay(10U);
			continue;
		}
		if (ui.launch_once && !ui.launch_once_queued &&
		    SDL_TICKS_PASSED(now, ui.launch_once_at)) {
			ui.launch_once_queued = true;
			if (ui.nav_index == NAV_PAGE_STREAMING)
				stream_activate_selected(&ui, now);
			else
				launch_queue_selected(&ui, now);
			frame_scheduler_invalidate(&scheduler);
		}
		if (benchmark_input_pending(&ui) &&
		    SDL_TICKS_PASSED(now, ui.benchmark_next_input)) {
			SDL_Event event = { 0 };
			unsigned int input = ui.benchmark_input_count++;

			event.type = SDL_KEYDOWN;
			if (ui.filter_stress)
				event.key.keysym.sym = input == 0U ? SDLK_F2 :
					input <= 220U ? SDLK_RIGHT :
					(input % 2U == 0U ? SDLK_DOWN : SDLK_RIGHT);
			else if (ui.navigation_stress)
				event.key.keysym.sym = input % 8U == 7U ?
					SDLK_LEFT : SDLK_RIGHT;
			else if (ui.settings_preview)
				event.key.keysym.sym = settings_benchmark_key(input);
			else
				event.key.keysym.sym = input % 2U == 0U ?
					SDLK_RIGHT : SDLK_LEFT;
			event.key.repeat = 0;
			(void)SDL_PushEvent(&event);
			ui.benchmark_next_input +=
				ui.navigation_stress || ui.filter_stress ? 8U :
				ui.settings_preview && input >= 7U ? 40U : 250U;
			frame_scheduler_invalidate(&scheduler);
		}
		if (ui.screenshot_path != NULL && !ui.screenshot_done) {
			if (!screenshot_delay_seen && SDL_TICKS_PASSED(now,
			    started + ui.screenshot_delay_ms)) {
				screenshot_delay_seen = true;
				frame_scheduler_invalidate(&scheduler);
			}
			if (screenshot_delay_seen && !screenshot_force_seen &&
			    SDL_TICKS_PASSED(now, started + 3000U)) {
				screenshot_force_seen = true;
				frame_scheduler_invalidate(&scheduler);
			}
		}
		if (ui.demo_deadline != 0U &&
		    SDL_TICKS_PASSED(now, ui.demo_deadline)) {
			ui.running = false;
			break;
		}

		wake_ms = required_wake_ms(&ui, now, started,
			screenshot_delay_seen, screenshot_force_seen);
		query = scheduler_query(&ui, now, wake_ms);
		wake_ms = frame_scheduler_next_wait_ms(&scheduler, &query);
		if (wake_ms > 0) {
			if (!wait_for_activity(&ui, wake_ms))
				continue;
		}
		/* SDL_WaitEventTimeout may have consumed most of the static interval. */
		now = SDL_GetTicks();

		monitor_copy_latest(&ui);
		if (input_handle_events(&ui, now))
			frame_scheduler_invalidate(&scheduler);
		if (settings_ui_update(&ui, now))
			frame_scheduler_invalidate(&scheduler);
		if (power_idle_timeout_ms(&ui.power, ui.last_user_activity_at,
		    (uint32_t)ui.settings.preferences.auto_screen_off_minutes *
			UINT32_C(60000), now) == 0) {
			unsigned int actions = power_auto_screen_off(&ui.power);

			ui.last_user_activity_at = now;
			if (actions != POWER_ACTION_NONE) {
				power_ui_apply(&ui, actions, now);
				frame_scheduler_invalidate(&scheduler);
			}
		}
		if (!power_should_render(&ui.power)) {
			ui.metrics.last_present = 0U;
			ui.metrics.input_counter = 0U;
			frame_scheduler_clear(&scheduler);
			continue;
		}
		query = scheduler_query(&ui, now, -1);
		if (!frame_scheduler_render_due(&scheduler, &query))
			continue;
		/* Idle gaps are not missed frame intervals; time the render itself. */
		ui.metrics.last_present = 0U;
		frame_counter = metrics_frame_begin();
		render_scene(&ui, now);
		frame_scheduler_presented(&scheduler, now);
		if (!first_frame_presented) {
			uint64_t presented_ns = monotonic_ns();

			if (game_exit_ns != 0U && presented_ns >= game_exit_ns)
				(void)fprintf(stderr,
					"UI_FIRST_FRAME_PRESENTED monotonic_ns=%llu game_exit_to_present_ms=%llu catalog_source=%s scan_ms=%u\n",
					(unsigned long long)presented_ns,
					(unsigned long long)((presented_ns - game_exit_ns) /
						1000000ULL), catalog_source,
					ui.catalog_scan_ms);
			else
				(void)fprintf(stderr,
					"UI_FIRST_FRAME_PRESENTED monotonic_ns=%llu game_exit_to_present_ms=unavailable catalog_source=%s scan_ms=%u\n",
					(unsigned long long)presented_ns, catalog_source,
					ui.catalog_scan_ms);
			first_frame_presented = true;
			if (catalog_snapshot_path != NULL) {
				bool refresh_needed = !catalog_snapshot_complete ||
					catalog_snapshot_needs_refresh(&ui, rom_root);

				if (refresh_needed && catalog_refresh_start(&ui, rom_root,
					    platform_routes_path,
					    catalog_snapshot_path) != 0)
					(void)fprintf(stderr,
						"CATALOG_REFRESH start=failed\n");
				else if (!refresh_needed)
					(void)fprintf(stderr,
						"CATALOG_REFRESH skipped=unchanged\n");
			}
		}
		if (!ready_published && ready_file != NULL) {
			int output_width = 0;
			int output_height = 0;

			if (SDL_GetRendererOutputSize(ui.renderer, &output_width,
						      &output_height) != 0 ||
			    output_width != UI_WIDTH || output_height != UI_HEIGHT ||
			    publish_ready_marker(ready_file) != 0) {
				fprintf(stderr, "ready marker: first-frame gate failed\n");
				cleanup(&ui);
				return 1;
			}
			ready_published = true;
		}
		metrics_frame_end(&ui, frame_counter);
		++frames;
		transition_before = ui.launch.transition;
		launch_update(&ui, now);
		if (ui.launch.transition != transition_before)
			frame_scheduler_invalidate(&scheduler);
		if (ui.launch.process.active || ui.launch.session_suspended)
			continue;
		if (ui.screenshot_path != NULL && !ui.screenshot_done &&
		    SDL_TICKS_PASSED(now, started + ui.screenshot_delay_ms) &&
		    (ui.nav_index == NAV_PAGE_SETTINGS ||
		     ui.nav_index == NAV_PAGE_NETWORK ||
		     ui.nav_index == NAV_PAGE_APPS ||
		     ui.nav_index == NAV_PAGE_STREAMING ||
		     ui.power.view == POWER_VIEW_LOCKED ||
		     cover_cache_visible_settled(&ui) || now - started >= 3000U)) {
			if (render_save_screenshot(&ui, ui.screenshot_path) != 0) {
				fprintf(stderr, "screenshot: %s\n", SDL_GetError());
				cleanup(&ui);
				return 1;
			}
			ui.screenshot_done = true;
			if (demo_ms == 0)
				ui.running = false;
		}
		if (ui.demo_deadline != 0U &&
		    SDL_TICKS_PASSED(SDL_GetTicks(), ui.demo_deadline))
			ui.running = false;
	}
	{
		uint32_t elapsed = SDL_GetTicks() - started;
		SDL_RendererInfo renderer_info;

		memset(&renderer_info, 0, sizeof(renderer_info));
		(void)SDL_GetRendererInfo(ui.renderer, &renderer_info);
		printf("UI_RESULT PASS renderer=%s frames=%llu elapsed_ms=%u fps=%.2f input=%s audio=%s font=%s roms=%zu visible=%zu covers=%zu cover_rejected=%zu cover_decode_peak=%llu cover_stale_dropped=%zu cover_queue_cancelled=%zu cover_visible_evictions=%zu cover_disk_hits=%zu cover_disk_misses=%zu cover_disk_writes=%zu cover_texture_creates=%zu cover_texture_destroys=%zu cover_texture_bytes=%llu cover_texture_peak=%llu peak_rss_kib=%llu routes=%zu scan_ms=%u catalog_source=%s resident=%s rom_root=%s apps_view=%s rpg_view=%s stream_hosts=%zu stream_selected=%zu stream_store=%s stream_phase=%d stream_discovered=%zu stream_discovery_ms=%u moonlight=%s\n",
		       renderer_info.name != NULL ? renderer_info.name : "unknown",
		       (unsigned long long)frames, elapsed,
		       elapsed > 0U ? (double)frames * 1000.0 / elapsed : 0.0,
		       ui.input_name[0] != '\0' ? ui.input_name : "unknown",
		       ui.audio_device != 0 && SDL_GetCurrentAudioDriver() != NULL ?
			SDL_GetCurrentAudioDriver() : "none", font_path,
		       ui.catalog.game_count, ui.catalog.visible_count,
		       cover_cache_texture_count(&ui),
		       cover_cache_rejected_count(&ui),
		       (unsigned long long)cover_cache_peak_decode_bytes(&ui),
		       cover_cache_stale_dropped_count(&ui),
		       cover_cache_queued_cancelled_count(&ui),
		       cover_cache_visible_eviction_count(&ui),
		       cover_cache_disk_hit_count(&ui),
		       cover_cache_disk_miss_count(&ui),
		       cover_cache_disk_write_count(&ui),
		       cover_cache_texture_create_count(&ui),
		       cover_cache_texture_destroy_count(&ui),
		       (unsigned long long)cover_cache_texture_bytes(&ui),
		       (unsigned long long)cover_cache_texture_peak_bytes(&ui),
		       (unsigned long long)cover_cache_peak_rss_kib(),
		       ui.routes.count,
		       ui.catalog_scan_ms,
		       catalog_source,
		       ui.resident ? "yes" : "no",
		       ui.catalog.rom_root,
		       ui.catalog.apps_only ? "yes" : "no",
		       ui.catalog.rpg_only ? "yes" : "no",
		       ui.streaming.hosts.count,
		       ui.streaming.hosts.count > 0 ?
			ui.streaming.selected_index + 1 : 0,
		       ui.streaming.loaded ? "ready" : "unavailable",
		       (int)ui.streaming.phase,
		       ui.streaming.discovered_count,
		       ui.streaming.discovery_ms,
		       ui.streaming.moonlight_deployed ? "deployed" :
			"not-deployed");
		if (ui.stream_preview) {
			const NsHost *host = stream_selected_host(&ui);

			if (host != NULL)
				printf("STREAM_RESULT PASS hosts=%zu selected=%zu paired=%s width=%u height=%u fps=%u bitrate=%u codec=%s aspect=%s runner=%s\n",
				       ui.streaming.hosts.count,
				       ui.streaming.selected_index + 1,
				       host->paired ? "yes" : "no",
				       host->resolution.width,
				       host->resolution.height, host->fps,
				       host->bitrate_kbps,
				       ns_codec_name(host->codec),
				       ns_aspect_name(host->aspect),
				       ui.streaming.moonlight_deployed ?
					"deployed" : "not-deployed");
		}
		printf("BLUETOOTH_RESULT capability=%s phase=%d devices=%zu powered=%s adapter=%s\n",
		       ui.bluetooth.snapshot.gate == BLUETOOTH_GATE_ADMITTED ?
			"PASS" : ui.bluetooth.snapshot.gate ==
			BLUETOOTH_GATE_REJECTED ? "REJECTED" : "PENDING",
		       (int)ui.bluetooth.snapshot.phase,
		       ui.bluetooth.snapshot.device_count,
		       ui.bluetooth.snapshot.powered ? "yes" : "no",
		       ui.bluetooth.snapshot.adapter_present ? "yes" : "no");
		if (ui.settings_preview)
			system_info_print_result(&ui);
		metrics_print(&ui);
		if (ui.catalog_report) {
			for (size_t i = 0; i < ui.catalog.system_count; ++i)
				printf("CATALOG_SYSTEM system=%s games=%zu\n",
				       ui.catalog.systems[i],
				       catalog_system_game_count(&ui,
					       ui.catalog.systems[i]));
		}
	}
	{
		bool supervisor_handoff = ui.launch.supervisor_handoff;

		if (supervisor_handoff) {
			/*
			 * The request is already durable (fsync, atomic rename, then
			 * directory fsync).  Do not put SDL destructors between that
			 * commit point and the supervisor: SDL 2.28.5's blocking ALSA
			 * path can leave SDLAudioP2 in ppoll() while
			 * SDL_CloseAudioDevice() waits forever in pthread_join().
			 *
			 * Release every UI-owned worker and graphics/input resource, but
			 * leave the proven-stuck ALSA device to process exit.  _exit() is
			 * deliberate: it terminates the audio thread and closes its ALSA fd
			 * before the supervisor's wait(2) returns.
			 */
			cleanup_mode(&ui, true);
			if (!launch_graphics_released(&ui)) {
				(void)fprintf(stderr,
					"UI_LAUNCH_HANDOFF TEARDOWN_FAILED\n");
				return 1;
			}
			(void)fprintf(stderr,
				"UI_LAUNCH_HANDOFF TEARDOWN_COMPLETE pid=%ld covers=0 fonts=0 renderer=none audio=kernel-release status=%d\n",
				(long)getpid(), LAUNCH_HANDOFF_EXIT_STATUS);
			(void)fflush(NULL);
			_exit(LAUNCH_HANDOFF_EXIT_STATUS);
		}
		cleanup(&ui);
	}
	return 0;
}
