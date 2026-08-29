#define _POSIX_C_SOURCE 200809L
#include "ui.h"
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static volatile sig_atomic_t termination_requested;
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
static void cleanup(struct ui *ui)
{
	launch_shutdown(ui);
	settings_backend_stop(&ui->settings);
	search_close(ui);
	persistence_stop(ui);
	monitor_stop(ui);
	cover_cache_destroy(ui);
	catalog_destroy(ui);
	platform_routes_destroy(ui);
	stream_destroy(ui);
	metrics_destroy(ui);
	render_destroy(ui);
	audio_close(ui);
	input_close(ui);
	for (int i = 0; i < FONT_COUNT; ++i)
		ui->fonts[i] = NULL;
	if (ui->renderer != NULL)
		SDL_DestroyRenderer(ui->renderer);
	if (ui->window != NULL)
		SDL_DestroyWindow(ui->window);
	TTF_Quit();
	IMG_Quit();
	SDL_Quit();
}
static int parse_args(int argc, char **argv, struct ui *ui,
			      const char **font, const char **rom_root,
			      const char **initial_search,
			      const char **favorites_path, const char **stock_path,
			      const char **history_path,
			      const char **settings_path, const char **language,
			      const char **filter_state_path,
			      const char **platform_routes_path,
			      const char **launcher_path,
			      const char **stream_launcher_path,
			      const char **launch_log_path,
			      const char **state_dir,
			      const char **hardwarectl_path,
			      const char **hardwarectl_log_path,
			      const char **hardware_root,
			      const char **ready_file,
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
	const char *font_argument = NULL;
	const char *font_path;
	const char *icon_atlas_path;
	const char *rom_root = "/mnt/mmc/Roms";
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
	const char *state_dir = "/var/lib/rg40xxv/netstream";
	const char *hardwarectl_path = "/usr/sbin/ui-hardwarectl";
	const char *hardwarectl_log_path =
		"/mnt/data/misc/rg40xxv-hardwarectl.log";
	const char *hardware_root = "/";
	const char *ready_file = NULL;
	bool windowed = false;
	bool ready_published = false;
	int demo_ms = 0;
	uint32_t started;
	uint64_t frames = 0U;
	if (parse_args(argc, argv, &ui, &font_argument, &rom_root,
		       &initial_search, &favorites_path, &stock_path, &history_path,
		       &settings_path, &language,
		       &filter_state_path,
		       &platform_routes_path,
		       &launcher_path, &stream_launcher_path, &launch_log_path,
		       &state_dir,
		       &hardwarectl_path, &hardwarectl_log_path,
		       &hardware_root, &ready_file,
		       &windowed, &demo_ms) != 0)
		return 2;
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
	if (lifecycle_graphics_init(&ui, font_path, windowed) != 0) {
		fprintf(stderr, "graphics: %s\n", SDL_GetError());
		cleanup(&ui);
		return 1;
	}
	if (icon_atlas_path == NULL ||
	    material_icons_load(&ui, icon_atlas_path) != 0)
		fprintf(stderr, "material icon atlas unavailable: %s\n",
			icon_atlas_path == NULL ? "not found" : icon_atlas_path);
	launch_configure(&ui, launcher_path, launch_log_path, font_path, windowed);
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
	if (!ui.streaming.loaded)
		fprintf(stderr, "stream hosts: %s\n", ui.streaming.load_error);
	locale_init(&ui, settings_path, language);
	power_state_init(&ui.power,
			 ui.settings.preferences.screen_lock_enabled);
	input_method_init(&ui.input_method, INPUT_FIELD_TEXT);
	if (platform_routes_load(&ui, platform_routes_path) != 0)
		fprintf(stderr, "routes: cannot load %s; using scanner defaults\n",
			platform_routes_path);
	{
		uint32_t scan_started = SDL_GetTicks();

		if (catalog_scan(&ui, rom_root) != 0)
			fprintf(stderr, "catalog: cannot scan %s\n", rom_root);
		ui.catalog_scan_ms = SDL_GetTicks() - scan_started;
	}
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
		ui.apps_preview ? NAV_PAGE_APPS : NAV_PAGE_LIBRARY;
	ui.focus_region = ui.benchmark || ui.content_preview ?
		UI_FOCUS_CONTENT : UI_FOCUS_TOP_NAV;
	catalog_set_apps_view(&ui, ui.nav_index == NAV_PAGE_APPS);
	if (ui.catalog.apps_only && ui.search_active)
		search_close(&ui);
	if (ui.lock_preview) {
		ui.power.locked = true;
		ui.power.view = POWER_VIEW_LOCKED;
	}
	/* Keep adjacent title rasterization out of input-to-present frames. */
	text_prewarm_visible(&ui);
	ui.action_text = tr(&ui, "preparing");
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
	if (demo_ms > 0)
		ui.demo_deadline = started + (uint32_t)demo_ms;
	ui.benchmark_next_input = started +
		(ui.navigation_stress || ui.filter_stress ? 8U : 250U);
	ui.launch_once_at = started + 250U;
	while (ui.running) {
		if (termination_requested) {
			launch_shutdown(&ui);
			ui.running = false;
			break;
		}
		if (ui.launch.process.active || ui.launch.session_suspended) {
			launch_update(&ui, SDL_GetTicks());
			if (ui.launch.process.active)
				SDL_Delay(10U);
			continue;
		}
		uint64_t frame_counter = metrics_frame_begin();
		uint32_t now = SDL_GetTicks();
		if (ui.launch_once && !ui.launch_once_queued &&
		    SDL_TICKS_PASSED(now, ui.launch_once_at)) {
			ui.launch_once_queued = true;
			if (ui.nav_index == NAV_PAGE_STREAMING)
				stream_activate_selected(&ui, now);
			else
				launch_queue_selected(&ui, now);
		}
		if (ui.benchmark && SDL_TICKS_PASSED(now, ui.benchmark_next_input)) {
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
			else
				event.key.keysym.sym = input % 2U == 0U ?
					SDLK_RIGHT : SDLK_LEFT;
			event.key.repeat = 0;
			(void)SDL_PushEvent(&event);
			ui.benchmark_next_input +=
				ui.navigation_stress || ui.filter_stress ? 8U : 250U;
		}

		monitor_copy_latest(&ui);
		input_handle_events(&ui, now);
		settings_ui_update(&ui, now);
		if (!power_should_render(&ui.power)) {
			SDL_Event event;
			int timeout = power_next_timeout_ms(&ui.power, now);
			int received;

			ui.metrics.last_present = 0U;
			ui.metrics.input_counter = 0U;
			received = timeout < 0 ? SDL_WaitEvent(&event) :
				SDL_WaitEventTimeout(&event, timeout);
			if (received != 0)
				(void)SDL_PushEvent(&event);
			continue;
		}
		render_scene(&ui, now);
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
		launch_update(&ui, now);
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
		if (ui.demo_deadline != 0U && now >= ui.demo_deadline)
			ui.running = false;
		if ((SDL_GetWindowFlags(ui.window) & SDL_WINDOW_INPUT_FOCUS) == 0U &&
		    demo_ms == 0)
			SDL_Delay(8U);
	}
	{
		uint32_t elapsed = SDL_GetTicks() - started;
		SDL_RendererInfo renderer_info;

		memset(&renderer_info, 0, sizeof(renderer_info));
		(void)SDL_GetRendererInfo(ui.renderer, &renderer_info);
		printf("UI_RESULT PASS renderer=%s frames=%llu elapsed_ms=%u fps=%.2f input=%s audio=%s font=%s roms=%zu visible=%zu covers=%zu cover_rejected=%zu cover_decode_peak=%llu cover_stale_dropped=%zu cover_queue_cancelled=%zu cover_visible_evictions=%zu routes=%zu scan_ms=%u resident=%s rom_root=%s apps_view=%s stream_hosts=%zu stream_selected=%zu stream_store=%s moonlight=%s\n",
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
		       ui.routes.count,
		       ui.catalog_scan_ms,
		       ui.resident ? "yes" : "no",
		       ui.catalog.rom_root,
		       ui.catalog.apps_only ? "yes" : "no",
		       ui.streaming.hosts.count,
		       ui.streaming.hosts.count > 0 ?
			ui.streaming.selected_index + 1 : 0,
		       ui.streaming.loaded ? "ready" : "unavailable",
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
	cleanup(&ui);
	return 0;
}
