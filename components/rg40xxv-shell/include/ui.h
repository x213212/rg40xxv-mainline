#ifndef RG40XXV_UI_H
#define RG40XXV_UI_H

#define _POSIX_C_SOURCE 200809L

#include <SDL.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

#include "cover_limits.h"
#include "bluetooth_backend.h"
#include "hardware.h"
#include "input_method.h"
#include "input_focus.h"
#include "input_latch.h"
#include "input_navigation.h"
#include "launcher.h"
#include "netstream.h"
#include "network_ui.h"
#include "power.h"
#include "power_input_filter.h"
#include "rpg_translation.h"
#include "sdl_image_compat.h"
#include "sdl_ttf_compat.h"
#include "settings.h"
#include "stream_backend.h"
#include "ui_layout.h"
#include "youtube_capability.h"

enum {
	UI_WIDTH = 640,
	UI_HEIGHT = 480,
	TEXT_CACHE_MAX = 160,
	TEXT_VALUE_MAX = 256,
	FONT_COUNT = 4,
	QUICK_MENU_COUNT = 6,
	VISIBLE_COVER_COUNT = UI_LAYOUT_VISIBLE_COVER_COUNT,
	COVER_CACHE_MAX = COVER_NEARBY_CACHE_COUNT,
	SEARCH_TEXT_MAX = 128,
	KEYBOARD_ROWS = 4,
	KEYBOARD_COLUMNS = 10,
	KEYBOARD_PAGES = 4,
	PERSISTENCE_ITEM_COUNT = 4,
	HISTORY_MAX = 64,
	FRAME_SAMPLE_MAX = 65536,
};

enum nav_page {
	NAV_PAGE_RECENT,
	NAV_PAGE_LIBRARY,
	NAV_PAGE_FAVORITES,
	NAV_PAGE_RPG,
	NAV_PAGE_STREAMING,
	NAV_PAGE_APPS,
	NAV_PAGE_NETWORK,
	NAV_PAGE_SETTINGS,
	NAV_COUNT,
};

enum material_icon_id {
	MATERIAL_ICON_HISTORY,
	MATERIAL_ICON_LIBRARY,
	MATERIAL_ICON_FAVORITE,
	MATERIAL_ICON_CAST,
	MATERIAL_ICON_APPS,
	MATERIAL_ICON_SETTINGS,
	MATERIAL_ICON_WIFI,
	MATERIAL_ICON_BATTERY,
	MATERIAL_ICON_VOLUME,
	MATERIAL_ICON_SEARCH,
	MATERIAL_ICON_PLAY,
	MATERIAL_ICON_TUNE,
	MATERIAL_ICON_POWER,
	MATERIAL_ICON_COUNT,
};

enum input_profile {
	INPUT_PROFILE_NONE,
	INPUT_PROFILE_H700_MAINLINE,
	INPUT_PROFILE_ANBERNIC_STOCK,
};

enum ui_language {
	UI_LANGUAGE_ZH_TW,
	UI_LANGUAGE_ENGLISH,
};

enum launch_transition_phase {
	LAUNCH_TRANSITION_NONE,
	LAUNCH_TRANSITION_STARTING,
	LAUNCH_TRANSITION_RETURNED,
	LAUNCH_TRANSITION_ERROR,
};

enum launch_kind {
	LAUNCH_KIND_NONE,
	LAUNCH_KIND_GAME,
	LAUNCH_KIND_STREAM,
};

struct locale_state {
	enum ui_language language;
	char settings_path[PATH_MAX];
};

struct stream_state {
	NsHostDb hosts;
	struct stream_backend *backend;
	uint64_t backend_generation;
	size_t selected_index;
	size_t discovered_count;
	uint32_t discovery_ms;
	char state_dir[PATH_MAX];
	char launcher_path[PATH_MAX];
	char load_error[NS_ERROR_MAX];
	char pair_pin[5];
	int launcher_error;
	int setting_index;
	uint64_t settings_save_navigation_epoch;
	uint32_t discovery_retry_at;
	unsigned int discovery_retry_count;
	enum stream_backend_phase phase;
	bool loaded;
	bool moonlight_deployed;
	bool settings_save_pending;
};

struct bluetooth_ui_state {
	struct bluetooth_backend *backend;
	struct bluetooth_backend_snapshot snapshot;
	uint64_t generation;
	uint64_t pending_request_id;
	uint64_t pending_navigation_epoch;
	uint32_t status_refresh_at;
	size_t selected_row;
	bool detail_active;
	bool pending_silent;
};

struct text_item {
	char text[TEXT_VALUE_MAX];
	int font_index;
	SDL_Color color;
	SDL_Texture *texture;
	int width;
	int height;
	uint64_t last_used;
};

struct game_entry {
	char *title;
	char *path;
	char *cover_path;
	char *system;
	char *system_label;
	char *core;
	char *frontend;
	char *runtime;
	char *fallback;
	char content_hash[65];
	time_t modified;
	time_t last_played;
	bool favorite;
	bool recent;
	bool route_lock;
	bool save_present;
	bool save_warning;
	bool playable;
};

struct platform_route {
	char *system;
	char *label_zh_tw;
	char *primary;
	char *fallback;
	bool playable;
};

struct platform_routes {
	struct platform_route *items;
	size_t count;
	size_t capacity;
	char source_path[PATH_MAX];
	/* Normalized admission state used to invalidate catalog snapshots. */
	unsigned int rpg_truth_level;
	bool rpg_launchable;
};

struct catalog_platform_record {
	char *name;
	uint64_t device;
	uint64_t inode;
	uint64_t mtime_ns;
	uint64_t ctime_ns;
	uint64_t entry_count;
};

struct catalog_state {
	struct game_entry *games;
	size_t game_count;
	size_t game_capacity;
	size_t *visible;
	size_t visible_count;
	size_t visible_capacity;
	char **systems;
	size_t system_count;
	size_t system_capacity;
	char **cores;
	size_t core_count;
	size_t core_capacity;
	struct catalog_platform_record *platforms;
	size_t platform_count;
	size_t platform_capacity;
	char refresh_cursor[NAME_MAX + 1U];
	size_t system_filter;
	size_t core_filter;
	bool favorites_only;
	bool recent_only;
	bool search_all_systems;
	bool apps_only;
	bool rpg_only;
	char query[SEARCH_TEXT_MAX];
	char rom_root[PATH_MAX];
	char filter_state_path[PATH_MAX];
	char favorites_path[PATH_MAX];
	char stock_favorites_path[PATH_MAX];
	char history_path[PATH_MAX];
};

struct cover_item {
	size_t game_id;
	SDL_Texture *texture;
	int width;
	int height;
	uint64_t last_used;
	uint64_t texture_bytes;
	bool occupied;
};

struct cover_job {
	size_t game_id;
	size_t priority;
	uint64_t generation;
	/* Immutable enqueue-time snapshot.  The worker must never dereference the
	 * main-thread-owned catalog, which may be reallocated by delayed refresh. */
	char path[PATH_MAX];
	SDL_Surface *surface;
	bool occupied;
	bool loading;
	bool ready;
	bool cancelled;
};

struct cover_worker {
	SDL_Thread *thread;
	SDL_mutex *mutex;
	SDL_cond *condition;
	struct cover_job jobs[COVER_CACHE_MAX];
	size_t visible_game_ids[COVER_CACHE_MAX];
	size_t visible_game_count;
	uint64_t visible_generation;
	size_t decoded_count;
	size_t rejected_count;
	size_t stale_dropped_count;
	size_t queued_cancelled_count;
	size_t visible_eviction_count;
	size_t disk_hit_count;
	size_t disk_miss_count;
	size_t disk_write_count;
	size_t disk_writes_since_prune;
	uint64_t peak_decode_bytes;
	bool running;
};

struct monitor_worker {
	SDL_Thread *thread;
	SDL_mutex *mutex;
	struct hardware_snapshot latest;
	bool running;
	bool ready;
};

enum persistence_item_id {
	PERSISTENCE_FAVORITES,
	PERSISTENCE_FILTERS,
	PERSISTENCE_LOCALE,
	PERSISTENCE_HISTORY,
};

struct persistence_item {
	char path[PATH_MAX];
	char *payload;
};

struct persistence_worker {
	SDL_Thread *thread;
	SDL_mutex *mutex;
	SDL_cond *condition;
	struct persistence_item items[PERSISTENCE_ITEM_COUNT];
	uint32_t deadline;
	unsigned int write_failures;
	bool running;
	bool dirty;
};

struct frame_metrics {
	uint32_t *samples_us;
	size_t count;
	size_t capacity;
	size_t cursor;
	uint64_t total_frames;
	uint64_t frequency;
	uint64_t last_present;
	uint64_t input_counter;
	uint32_t input_max_us;
	uint64_t input_events;
	uint64_t over_budget;
};

struct settings_render_key {
	struct hardware_snapshot hardware;
	struct device_preferences preferences;
	int volume_target;
	int usb_debug_available;
	int settings_index;
	enum ui_focus_region focus_region;
	enum ui_language language;
	bool detail_active;
	bool backend_available;
};

struct launch_state {
	struct launcher_process process;
	char executable[PATH_MAX];
	char log_path[PATH_MAX];
	char handoff_path[PATH_MAX];
	char font_path[PATH_MAX];
	char game_title[TEXT_VALUE_MAX];
	char game_platform[TEXT_VALUE_MAX];
	char game_core[TEXT_VALUE_MAX];
	char transition_detail[TEXT_VALUE_MAX];
	char diagnostics[TEXT_VALUE_MAX];
	NsHost pending_stream_host;
	size_t pending_game_id;
	uint32_t transition_until;
	int last_error;
	enum launch_transition_phase transition;
	enum launch_kind kind;
	bool pending;
	bool transition_presented;
	bool session_suspended;
	bool supervisor_handoff;
	bool history_needs_write;
	bool diagnostics_expanded;
	bool windowed;
};

struct ui {
	SDL_Window *window;
	SDL_Renderer *renderer;
	SDL_GameController *controller;
	SDL_Joystick *joystick;
	SDL_AudioDeviceID audio_device;
	SDL_AudioSpec audio_spec;
	SDL_Texture *navigation_cache;
	SDL_Texture *controls_cache;
	SDL_Texture *backdrop_cache;
	SDL_Texture *settings_background_cache[2];
	SDL_Texture *settings_page_cache;
	SDL_Texture *icon_atlas;
	char icon_atlas_path[PATH_MAX];
	const char *icon_atlas_argument;
	int navigation_cache_index;
	enum ui_focus_region navigation_cache_focus_region;
	int controls_cache_mode;
	enum ui_language navigation_cache_language;
	enum ui_language controls_cache_language;
	TTF_Font *fonts[FONT_COUNT];
	struct text_item cache[TEXT_CACHE_MAX];
	size_t cache_count;
	SDL_Texture *retired_textures[TEXT_CACHE_MAX];
	size_t retired_texture_count;
	uint64_t text_tick;
	struct settings_render_key settings_page_cache_key;
	bool settings_page_cache_valid;
	struct catalog_state catalog;
	struct platform_routes routes;
	struct stream_state streaming;
	struct bluetooth_ui_state bluetooth;
	struct network_ui_state network;
	struct locale_state locale;
	struct hardware_backend hardware_backend;
	struct hardware_snapshot hardware;
	struct monitor_worker monitor;
	struct persistence_worker persistence;
	struct frame_metrics metrics;
	struct settings_state settings;
	struct power_state power;
	struct input_method input_method;
	struct launch_state launch;
	struct rpg_translation_state rpg_translation;
	struct cover_item covers[COVER_CACHE_MAX];
	struct cover_worker cover_worker;
	uint64_t cover_tick;
	uint64_t cover_texture_bytes;
	uint64_t cover_texture_peak_bytes;
	size_t cover_texture_create_count;
	size_t cover_texture_destroy_count;
	char cover_cache_dir[PATH_MAX];
	int nav_index;
	uint64_t navigation_epoch;
	enum ui_focus_region focus_region;
	size_t game_index;
	uint32_t action_until;
	uint32_t last_user_activity_at;
	const char *action_text;
	bool running;
	bool resident;
	bool benchmark;
	bool navigation_stress;
	bool filter_stress;
	bool settings_preview;
	bool stream_preview;
	bool apps_preview;
	bool rpg_preview;
	bool network_preview;
	bool launch_preview;
	bool lock_preview;
	int settings_index;
	bool settings_detail_active;
	bool settings_marquee_active;
	bool catalog_report;
	bool screenshot_done;
	const char *screenshot_path;
	uint32_t screenshot_delay_ms;
	bool content_preview;
	uint32_t demo_deadline;
	uint32_t catalog_scan_ms;
	pid_t catalog_refresh_pid;
	uint32_t clock_sync_deadline;
	bool clock_sync_waiting;
	uint32_t benchmark_next_input;
	unsigned int benchmark_input_count;
	uint32_t launch_once_at;
	bool launch_once;
	bool launch_once_queued;
	struct input_navigation input_navigation;
	struct input_latch input_latch;
	struct input_activation_guard activation_guard;
	int input_fd;
	int power_input_fd;
	enum input_profile input_profile;
	bool input_evdev_sync_lost;
	uint32_t input_retry_at;
	struct power_input_filter power_input_filter;
	uint32_t power_input_retry_at;
	char input_name[64];
	double carousel_position;
	double carousel_from;
	double carousel_target;
	uint32_t carousel_started;
	bool search_active;
	bool quick_menu_open;
	int quick_menu_index;
	int keyboard_row;
	int keyboard_column;
	int keyboard_page;
};

void stream_init(struct ui *ui, const char *state_dir,
		 const char *launcher_path);
int stream_reload(struct ui *ui);
bool stream_update(struct ui *ui, uint32_t now);
void stream_select_host(struct ui *ui, int direction);
void stream_move_setting(struct ui *ui, int direction);
void stream_adjust_setting(struct ui *ui, int direction, uint32_t now);
const NsHost *stream_selected_host(const struct ui *ui);
void stream_activate_selected(struct ui *ui, uint32_t now);
void render_stream_page(struct ui *ui, uint32_t now);
void stream_destroy(struct ui *ui);

void network_ui_select(struct ui *ui, int direction);
void network_ui_activate(struct ui *ui, uint32_t now);
void network_ui_secondary(struct ui *ui, uint32_t now);
void network_ui_tertiary(struct ui *ui, uint32_t now);
bool network_ui_back(struct ui *ui);
void network_ui_leave_page(struct ui *ui);
bool network_ui_tick(struct ui *ui, uint32_t now);
bool network_ui_password_append(struct ui *ui, const char *text);
void network_ui_password_backspace(struct ui *ui);
void network_ui_password_cancel(struct ui *ui);
void network_ui_password_submit(struct ui *ui, uint32_t now);
bool bluetooth_ui_update(struct ui *ui, uint32_t now);
void render_network_page(struct ui *ui, uint32_t now);

void locale_init(struct ui *ui, const char *settings_path,
		 const char *requested_language);
const char *tr(const struct ui *ui, const char *key);
void locale_toggle(struct ui *ui);

int catalog_scan(struct ui *ui, const char *rom_root);
int catalog_scan_bounded(struct ui *ui, const char *rom_root,
			 size_t max_entries, uint32_t max_ms);
int catalog_append_source_bounded(struct ui *ui, const char *rom_root,
				  const char *preferred_root,
				  size_t max_entries, uint32_t max_ms);
int catalog_append_cached_source(struct ui *ui, struct ui *cached,
				 const char *rom_root,
				 const char *preferred_root);
bool catalog_optional_source_available(const char *mount_point,
				       const char *rom_root);
int catalog_scan_platform(struct ui *ui, const char *platform_path,
			  const char *system, size_t max_entries, uint32_t max_ms,
			  uint64_t *direct_entries);
int catalog_snapshot_load(struct ui *ui, const char *path,
			  const char *rom_root, bool *complete);
int catalog_snapshot_write(const struct ui *ui, const char *path,
			   const char *rom_root, bool complete);
int catalog_snapshot_refresh(struct ui *ui, const char *path,
			     const char *rom_root, uint32_t max_ms);
bool catalog_snapshot_needs_refresh(const struct ui *ui, const char *rom_root);
int catalog_refresh_start(struct ui *ui, const char *rom_root,
			  const char *routes_path, const char *snapshot_path);
void catalog_refresh_poll(struct ui *ui);
int catalog_refresh_stop(struct ui *ui);
void catalog_destroy(struct ui *ui);
void catalog_apply_filters(struct ui *ui);
void catalog_set_apps_view(struct ui *ui, bool enabled);
void catalog_set_rpg_view(struct ui *ui, bool enabled);
const struct game_entry *catalog_visible_game(const struct ui *ui,
					       size_t visible_index);
size_t catalog_visible_id(const struct ui *ui, size_t visible_index);
void catalog_cycle_filter(struct ui *ui, int filter, int direction);
void catalog_filter_text(const struct ui *ui, int filter, char *buffer,
			 size_t size);
void catalog_toggle_favorite(struct ui *ui);
size_t catalog_system_game_count(const struct ui *ui, const char *system);
bool catalog_system_is_rpg(const char *system);
bool catalog_system_is_pending_rpg(const char *system);
bool catalog_game_can_launch(const struct game_entry *game);

void filter_state_load(struct ui *ui, const char *path);

int platform_routes_load(struct ui *ui, const char *path);
void platform_routes_destroy(struct ui *ui);
const struct platform_route *platform_route_find(const struct ui *ui,
						  const char *system);
const char *platform_route_frontend(const char *selector);
const char *platform_route_runtime(const char *selector);

void favorites_load(struct ui *ui, const char *favorites_path,
		    const char *stock_path);
void history_load(struct ui *ui, const char *path);
void history_mark_launched(struct ui *ui, size_t game_id);

void search_open(struct ui *ui);
void search_close(struct ui *ui);
void search_append(struct ui *ui, const char *text);
void search_backspace(struct ui *ui);
void search_clear(struct ui *ui);

void keyboard_render(struct ui *ui);
bool keyboard_handle_key(struct ui *ui, SDL_Keycode key, uint32_t now);

void cover_cache_sync_visible(struct ui *ui);
int cover_cache_init(struct ui *ui);
int cover_cache_configure(struct ui *ui, const char *directory);
SDL_Texture *cover_cache_get(struct ui *ui, size_t game_id, int *width,
			     int *height);
void cover_cache_destroy(struct ui *ui);
size_t cover_cache_texture_count(const struct ui *ui);
size_t cover_cache_rejected_count(struct ui *ui);
size_t cover_cache_stale_dropped_count(struct ui *ui);
size_t cover_cache_queued_cancelled_count(struct ui *ui);
size_t cover_cache_visible_eviction_count(struct ui *ui);
uint64_t cover_cache_peak_decode_bytes(struct ui *ui);
size_t cover_cache_disk_hit_count(struct ui *ui);
size_t cover_cache_disk_miss_count(struct ui *ui);
size_t cover_cache_disk_write_count(struct ui *ui);
size_t cover_cache_texture_create_count(const struct ui *ui);
size_t cover_cache_texture_destroy_count(const struct ui *ui);
uint64_t cover_cache_texture_bytes(const struct ui *ui);
uint64_t cover_cache_texture_peak_bytes(const struct ui *ui);
uint64_t cover_cache_peak_rss_kib(void);
bool cover_cache_visible_settled(struct ui *ui);

int monitor_start(struct ui *ui);
void monitor_copy_latest(struct ui *ui);
void monitor_stop(struct ui *ui);

int persistence_start(struct ui *ui);
void persistence_request_favorites(struct ui *ui);
void persistence_request_filters(struct ui *ui);
void persistence_request_locale(struct ui *ui);
void persistence_request_history(struct ui *ui);
void persistence_stop(struct ui *ui);

int metrics_init(struct ui *ui);
uint64_t metrics_frame_begin(void);
void metrics_note_input(struct ui *ui);
void metrics_frame_end(struct ui *ui, uint64_t frame_counter);
void metrics_print(const struct ui *ui);
void metrics_destroy(struct ui *ui);

void audio_init(struct ui *ui);
void audio_close(struct ui *ui);
void audio_play_chime(struct ui *ui, double pitch);

void input_init(struct ui *ui);
void input_close(struct ui *ui);
bool input_handle_events(struct ui *ui, uint32_t now);
bool input_handle_power_events(struct ui *ui, uint32_t now);

void launch_configure(struct ui *ui, const char *executable,
			      const char *log_path, const char *handoff_path,
			      const char *font_path, bool windowed);
void launch_queue_selected(struct ui *ui, uint32_t now);
int launch_queue_stream(struct ui *ui, const NsHost *host, uint32_t now);
bool launch_cancel_pending(struct ui *ui, uint32_t now);
void launch_update(struct ui *ui, uint32_t now);
void launch_shutdown(struct ui *ui);
void launch_transition_presented(struct ui *ui);

int lifecycle_graphics_init(struct ui *ui, const char *font_path,
			    bool windowed);
void lifecycle_session_suspend(struct ui *ui);
int lifecycle_session_resume(struct ui *ui);

void power_ui_apply(struct ui *ui, unsigned int actions, uint32_t now);
bool power_ui_handle_key(struct ui *ui, SDL_Keycode key, uint32_t now);
void power_ui_toggle_screen_lock(struct ui *ui, uint32_t now);

void settings_ui_move(struct ui *ui, int direction);
void settings_ui_adjust(struct ui *ui, int direction, uint32_t now);
void settings_ui_activate(struct ui *ui, uint32_t now);
void settings_ui_track_pending(struct ui *ui,
			       enum settings_pending_kind kind, int value);
bool settings_ui_back(struct ui *ui);
void settings_ui_leave_page(struct ui *ui);
/* Returns true only when a completed request changes the visible scene. */
bool settings_ui_update(struct ui *ui, uint32_t now);

void render_set_color(SDL_Renderer *renderer, SDL_Color color);
void render_fill_rect(SDL_Renderer *renderer, int x, int y, int w, int h,
		      SDL_Color color);
void render_outline_rect(SDL_Renderer *renderer, int x, int y, int w, int h,
			 SDL_Color color);
void render_fill_round_rect(SDL_Renderer *renderer, int x, int y, int w, int h,
			    int radius, SDL_Color color);
void render_outline_round_rect(SDL_Renderer *renderer, int x, int y, int w,
			       int h, int radius, SDL_Color color);
void render_glass_panel(SDL_Renderer *renderer, int x, int y, int w, int h,
			bool focused);
bool render_set_opaque_cache_blend(SDL_Texture *texture);
void render_backdrop(struct ui *ui);
void render_settings_background(struct ui *ui, bool focused);
void render_status(struct ui *ui);
void render_navigation(struct ui *ui, uint32_t now);
void render_system_info(struct ui *ui, uint32_t now);
void render_settings_page(struct ui *ui, uint32_t now);
void system_info_print_result(const struct ui *ui);
void render_lock_screen(struct ui *ui, uint32_t now);
void render_cover(struct ui *ui, int x, int y, int w, int h,
		  size_t visible_index, bool selected);
void render_scene(struct ui *ui, uint32_t now);
void render_prepare_static(struct ui *ui, uint32_t now);
int render_save_screenshot(struct ui *ui, const char *path);
void render_activate(struct ui *ui, const char *message, uint32_t now);
const char *render_find_font(const char *requested);
void render_destroy(struct ui *ui);

const char *material_icons_find(const char *requested);
int material_icons_load(struct ui *ui, const char *path);
void material_icon_draw(struct ui *ui, enum material_icon_id icon,
			int x, int y, int size, SDL_Color color, bool filled,
			bool glow);

void text_draw(struct ui *ui, int font_index, const char *text, int x, int y,
	       SDL_Color color);
int text_width(struct ui *ui, int font_index, const char *text, SDL_Color color);
void text_prewarm_visible(struct ui *ui);
void text_cache_collect_retired(struct ui *ui);
void text_cache_destroy(struct ui *ui);

#endif
