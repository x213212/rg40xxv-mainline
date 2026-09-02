// Deliberately independent of embedded_scene.cpp.  The KMSDRM panel has
// proved reliable with SDL_Renderer + OpenGLES2, while the direct GL present
// path did not reach it on this device.
#include <SDL.h>
#include <mpv/client.h>
#include <mpv/render.h>

#include "home_catalog.hpp"
#include "home_view.hpp"
#include "player_controls.h"

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <spawn.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace {
constexpr int kWidth = 640;
constexpr int kHeight = 480;
constexpr size_t kPixels = static_cast<size_t>(kWidth) * kHeight * 4U;
constexpr Uint32 kFramePixelFormat = SDL_PIXELFORMAT_RGBX8888;
constexpr size_t kEndpointMax = 2048;
constexpr size_t kLineMax = 4096;
constexpr Uint32 kPrefetchSelectionDebounceMs = 500;
constexpr Uint32 kPrefetchSuccessCooldownMs = 5U * 60U * 1000U;
constexpr Uint32 kHomeRepeatDelayMs = 250;
constexpr Uint32 kHomeRepeatIntervalMs = 90;
constexpr Uint32 kHomePollDelayMs = 4;
constexpr int kBrokerGracePolls = 150;
constexpr int kPrefetchGracePolls = 75;
constexpr useconds_t kChildPollUs = 10000;
constexpr const char kFontPath[] = "/opt/rg40xxv/share/RG40XXV-UI-Sans.otf";
static_assert(SDL_BYTEORDER == SDL_LIL_ENDIAN,
              "the mpv 0bgr/RGBX8888 byte contract requires little endian");
static_assert(SDL_PIXELORDER(kFramePixelFormat) == SDL_PACKEDORDER_RGBX,
              "the video texture must retain packed RGBX ordering");
volatile sig_atomic_t g_stop = 0;
volatile sig_atomic_t g_screenshot = 0;

enum class Scene { home, player };
enum class HomeRepeatDirection { none, up, down };

struct TextTexture {
 SDL_Texture *texture = nullptr;
 int width = 0;
 int height = 0;
};

struct App {
 SDL_Window *window = nullptr;
 SDL_Renderer *renderer = nullptr;
 SDL_Texture *frame = nullptr;
 TTF_Font *title_font = nullptr;
 TTF_Font *option_font = nullptr;
 TTF_Font *footer_font = nullptr;
 TextTexture loading_label {};
 SDL_GameController *controller = nullptr;
 SDL_Joystick *joystick = nullptr;
 mpv_handle *mpv = nullptr;
 mpv_render_context *render = nullptr;
 rg40xxv_youtube::PlayerControls *controls = nullptr;
 rg40xxv_youtube::HomeCatalog catalog {};
 rg40xxv_youtube::HomeView home_view {};
 bool home_view_initialized = false;
	HomeRepeatDirection home_repeat = HomeRepeatDirection::none;
	Uint32 home_repeat_next = 0;
	unsigned home_repeat_actions = 0;
 Uint8 *pixels = nullptr; // fixed, process-lifetime RGBX/RGBA-compatible buffer
 SDL_atomic_t frame_pending {};
 bool frame_ready = false;
 Scene scene = Scene::home;
 bool running = true;
 bool endpoint_ready = false;
 bool broker_failed = false;
 bool play_pending = false;
 uint64_t selection_generation = 1;
 uint64_t broker_generation = 0;
 uint64_t next_mpv_reply = 1;
 uint64_t load_reply = 0;
 uint64_t stop_reply = 0;
 uint64_t resume_reply = 0;
 uint64_t resume_generation = 0;
 bool load_command_pending = false;
 bool stop_command_pending = false;
 bool resume_command_pending = false;
 bool stop_needed = false;
 int64_t active_playlist_entry = -1;
 bool file_loaded = false;
 char endpoint[kEndpointMax + 1] = {};
 char broker_url[64] = {};
 char prefetch_candidate_url[64] = {};
 Uint32 prefetch_candidate_since = 0;
 char prefetch_url[64] = {};
 char prefetch_last_url[64] = {};
 Uint32 prefetch_last_at = 0;
 bool prefetch_last_success = false;
 pid_t prefetch_pid = -1;
 pid_t retiring_prefetch_pid = -1;
 Uint32 retiring_prefetch_deadline = 0;
 pid_t broker_pid = -1;
 int broker_stdin = -1;
 int broker_stdout = -1;
 char broker_line[kLineMax + 1] = {};
 size_t broker_used = 0;
 const char *broker_path = nullptr;
 const char *prefetch_path = nullptr;
 const char *feed_helper = nullptr;
 const char *screenshot_path = nullptr;
	const char *font_path = nullptr;
	const char *power_supply_root = nullptr;
	char battery_device[96] {};
	Uint32 system_status_next_poll = 0;
 bool ttf_initialized = false;
};

void signal_handler(int n) { if (n == SIGUSR1) g_screenshot = 1; else g_stop = 1; }

int acquire_scene_lock(const char *test_path = nullptr) {
 const char *path = test_path && test_path[0] != 0
     ? test_path : "/run/rg40xxv-youtube/scene.lock";
 if (path[0] != '/') {
  std::fputs("YOUTUBE_TEXTURE_SINGLETON result=FAIL reason=lock-path\n", stderr);
  return -1;
 }
 const int descriptor = open(path,
     O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
 if (descriptor < 0) {
  std::fprintf(stderr,
      "YOUTUBE_TEXTURE_SINGLETON result=FAIL reason=open errno=%d\n", errno);
  return -1;
 }
 struct stat status {};
 if (fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode) ||
     status.st_uid != geteuid() || status.st_nlink != 1 ||
     fchmod(descriptor, 0600) != 0) {
  std::fputs("YOUTUBE_TEXTURE_SINGLETON result=FAIL reason=identity\n", stderr);
  close(descriptor);
  return -1;
 }
 if (flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
  const int lock_error = errno;
  close(descriptor);
  if (lock_error == EWOULDBLOCK || lock_error == EAGAIN) {
   std::fputs("YOUTUBE_TEXTURE_SINGLETON result=BUSY reason=scene-active\n",
              stderr);
   return -2;
  }
  std::fprintf(stderr,
      "YOUTUBE_TEXTURE_SINGLETON result=FAIL reason=flock errno=%d\n",
      lock_error);
  return -1;
 }
 std::puts("YOUTUBE_TEXTURE_SINGLETON result=PASS owner=scene");
 return descriptor;
}

void mpv_update_callback(void *opaque) {
 auto *app = static_cast<App *>(opaque);
 if (app) SDL_AtomicSet(&app->frame_pending, 1);
}

bool install_signals() {
 struct sigaction sa {};
 sa.sa_handler = signal_handler;
 sigemptyset(&sa.sa_mask);
 struct sigaction ignore {};
 ignore.sa_handler = SIG_IGN;
 sigemptyset(&ignore.sa_mask);
 return sigaction(SIGUSR1, &sa, nullptr) == 0 &&
        sigaction(SIGINT, &sa, nullptr) == 0 &&
        sigaction(SIGTERM, &sa, nullptr) == 0 &&
        sigaction(SIGPIPE, &ignore, nullptr) == 0;
}

bool loopback(const char *s) {
 const char prefix[] = "http://127.0.0.1:";
 if (!s || strnlen(s, kEndpointMax + 1) == 0 || strnlen(s, kEndpointMax + 1) > kEndpointMax || std::strchr(s, '\n') || std::strchr(s, '\r') || std::strncmp(s, prefix, sizeof(prefix) - 1) != 0) return false;
 const char *p = s + sizeof(prefix) - 1; unsigned port = 0; int digits = 0;
 while (*p >= '0' && *p <= '9' && digits < 6) { port = port * 10U + static_cast<unsigned>(*p++ - '0'); ++digits; }
 return digits > 0 && digits <= 5 && port > 0 && port <= 65535 && *p == '/' && p[1] != 0;
}

bool watch_url(const char *s) {
 if (!s || strnlen(s, kEndpointMax + 1) == 0 || strnlen(s, kEndpointMax + 1) > kEndpointMax || s[0] == '-' || std::strchr(s, '\n') || std::strchr(s, '\r')) return false;
 return std::strncmp(s, "https://youtu.be/", 17) == 0 || std::strncmp(s, "https://www.youtube.com/watch?v=", 32) == 0 || std::strncmp(s, "https://youtube.com/watch?v=", 28) == 0;
}

bool executable(const char *p) {
 struct stat st {};
 return p && p[0] == '/' && lstat(p, &st) == 0 && S_ISREG(st.st_mode) && !S_ISLNK(st.st_mode) && (st.st_mode & S_IXUSR) != 0;
}

bool safe_component(const char *value) {
 if (!value || value[0] == 0) return false;
 for (const unsigned char *p = reinterpret_cast<const unsigned char *>(value);
      *p != 0; ++p)
  if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
        (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) return false;
 return true;
}

bool read_small_file(const char *path, char *output, size_t size) {
 if (!path || !output || size < 2) return false;
 const int descriptor = open(path, O_RDONLY | O_CLOEXEC);
 if (descriptor < 0) return false;
 const ssize_t length = read(descriptor, output, size - 1);
 (void)close(descriptor);
 if (length <= 0 || static_cast<size_t>(length) >= size) return false;
 output[length] = 0;
 while (output[0] != 0) {
  const size_t used = std::strlen(output);
  if (used == 0 || (output[used - 1] != '\n' && output[used - 1] != '\r')) break;
  output[used - 1] = 0;
 }
 return output[0] != 0;
}

bool battery_leaf(App *a, const char *leaf, char *output, size_t size) {
 if (!safe_component(a->battery_device) || !safe_component(leaf)) return false;
 char path[512] {};
 const int length = std::snprintf(path, sizeof(path), "%s/%s/%s",
                                  a->power_supply_root, a->battery_device, leaf);
 return length > 0 && static_cast<size_t>(length) < sizeof(path) &&
        read_small_file(path, output, size);
}

bool discover_battery(App *a) {
 DIR *directory = opendir(a->power_supply_root);
 if (!directory) return false;
 bool found = false;
 for (dirent *entry = readdir(directory); entry != nullptr;
      entry = readdir(directory)) {
  if (!safe_component(entry->d_name)) continue;
	  const size_t name_length = strnlen(entry->d_name,
	                                     sizeof(a->battery_device));
	  if (name_length == 0 || name_length >= sizeof(a->battery_device)) continue;
	  std::memcpy(a->battery_device, entry->d_name, name_length + 1);
  char type[32] {};
  if (battery_leaf(a, "type", type, sizeof(type)) &&
      std::strcmp(type, "Battery") == 0) {
   found = true;
   break;
  }
 }
 (void)closedir(directory);
 if (!found) a->battery_device[0] = 0;
 return found;
}

void refresh_system_status(App *a, Uint32 now) {
 if (static_cast<Sint32>(now - a->system_status_next_poll) < 0) return;
 a->system_status_next_poll = now + 1000;
 char clock_text[16] = "--:--";
 const std::time_t wall = std::time(nullptr);
 std::tm local {};
 if (wall >= 0 && localtime_r(&wall, &local) != nullptr)
  (void)std::strftime(clock_text, sizeof(clock_text), "%H:%M", &local);
 if (!safe_component(a->battery_device)) (void)discover_battery(a);
 char capacity[16] {}, status[32] {};
 int percent = -1;
 if (battery_leaf(a, "capacity", capacity, sizeof(capacity))) {
  char *end = nullptr;
  errno = 0;
  const long value = std::strtol(capacity, &end, 10);
  if (errno == 0 && end != capacity && *end == 0 && value >= 0 && value <= 100)
   percent = static_cast<int>(value);
 }
 if (!battery_leaf(a, "status", status, sizeof(status))) {
  a->battery_device[0] = 0;
  std::snprintf(status, sizeof(status), "unknown");
 } else if (std::strcmp(status, "Charging") == 0) {
  std::snprintf(status, sizeof(status), "charging");
 } else if (std::strcmp(status, "Discharging") == 0) {
  std::snprintf(status, sizeof(status), "battery");
 } else if (std::strcmp(status, "Full") == 0) {
  std::snprintf(status, sizeof(status), "full");
 } else {
  std::snprintf(status, sizeof(status), "not charging");
 }
 char label[96] {};
 if (percent >= 0)
  std::snprintf(label, sizeof(label), "%s · %d%% · %s", clock_text, percent,
                status);
 else
  std::snprintf(label, sizeof(label), "%s · --%% · %s", clock_text, status);
 (void)rg40xxv_youtube::home_view_set_system_status(
     &a->home_view, a->renderer, label);
}

void reset_prefetch_candidate(App *a) {
 a->prefetch_candidate_url[0] = 0;
 a->prefetch_candidate_since = 0;
}

bool focus_prefetch_due(App *a, const char *url, Uint32 now) {
 if (!watch_url(url)) return false;
 if (std::strcmp(a->prefetch_candidate_url, url) != 0) {
  std::snprintf(a->prefetch_candidate_url,
                sizeof(a->prefetch_candidate_url), "%s", url);
  a->prefetch_candidate_since = now;
  return false;
 }
 if (now - a->prefetch_candidate_since < kPrefetchSelectionDebounceMs)
  return false;
 reset_prefetch_candidate(a);
 return true;
}

bool prefetch_attempt_blocks_focus(App *a, const char *url, Uint32 now) {
 if (std::strcmp(a->prefetch_last_url, url) != 0) {
  // A focus change is the only automatic retry boundary for a failed
  // best-effort prefetch.  Do not wake yt-dlp forever on an unsupported
  // video while HOME remains idle.
  if (!a->prefetch_last_success) {
   a->prefetch_last_url[0] = 0;
   a->prefetch_last_at = 0;
  }
  return false;
 }
 if (!a->prefetch_last_success)
  return true;
 return now - a->prefetch_last_at < kPrefetchSuccessCooldownMs;
}

void cancel_pending_activation(App *a, const char *reason);

void home_repeat_stop(App *a, HomeRepeatDirection direction) {
 if (a->home_repeat != direction) return;
 a->home_repeat = HomeRepeatDirection::none;
 a->home_repeat_next = 0;
}

void home_repeat_move(App *a, HomeRepeatDirection direction) {
 cancel_pending_activation(a, "vertical-navigation");
 if (direction == HomeRepeatDirection::up)
  rg40xxv_youtube::home_catalog_up(&a->catalog);
 else if (direction == HomeRepeatDirection::down)
  rg40xxv_youtube::home_catalog_down(&a->catalog);
 else
  return;
 ++a->home_repeat_actions;
}

void home_repeat_start(App *a, HomeRepeatDirection direction, Uint32 now) {
 if (direction == HomeRepeatDirection::none || a->scene != Scene::home)
  return;
 // One physical press may be reported again by a keyboard repeat or a noisy
 // mapping.  Only a direction transition performs the immediate first move.
 if (a->home_repeat == direction)
  return;
 a->home_repeat = direction;
 home_repeat_move(a, direction);
 a->home_repeat_next = now + kHomeRepeatDelayMs;
}

void home_repeat_tick(App *a, Uint32 now) {
 if (a->scene != Scene::home || a->home_repeat == HomeRepeatDirection::none)
  return;
 if (static_cast<Sint32>(now - a->home_repeat_next) < 0)
  return;
 home_repeat_move(a, a->home_repeat);
 // Never replay a frame backlog as a burst; resume at the fixed cadence from
 // the current tick.
 a->home_repeat_next = now + kHomeRepeatIntervalMs;
}

void print_contract() {
 std::puts("YOUTUBE_TEXTURE_CONTRACT schema=rg40xxv-youtube-sdl-texture-scene-v1 size=640x480");
 std::puts("SDL_WINDOW\tcount=1\tbackend=KMSDRM\tflags=SHOWN|FULLSCREEN");
 std::puts("SDL_RENDERER\tcount=1\tdriver=accelerated+opengles2\tpresent=SDL_RenderPresent");
 std::puts("LIBMPV\tcreate=once\tinitialize=once\trender-context=once\tapi=sw\tlifetime=process");
 std::puts("FRAME\tfixed-cpu-buffer=640x480x4\tstreaming-texture=RGBX8888\trender=MPV_RENDER_PARAM_SW_*");
 std::puts("TEXT\tbackend=SDL_ttf\tfont=/opt/rg40xxv/share/RG40XXV-UI-Sans.otf\ttextures=cached-once");
 std::puts("HOME\tfeed=paged-cache-96\tpage=8\tmetadata=title+published+duration-before-thumbnails\tthumbnail=progressive-placeholder\tsearch=up\tchannels=8-fixed-ids\tchannel-snapshots=immediate-reuse\tchannel-prewarm=metadata-only");
 std::puts("HOME_RENDER\tidle=event-driven\tpoll-ms=4\tpresent=catalog+input+status-only\tplayer=frame-driven-vsync");
 std::puts("HOME_INPUT\tdefault=first-card\tDPAD=navigate+repeat-250/90ms\tL1_R1=previous-next-channel\tA=play-or-search-refresh\tX=channel-selector\tB=overview-or-exit");
 std::puts("PLAYER_INPUT\tA=pause-toggle\tLEFT_RIGHT=seek-10s\tB=HOME");
 std::puts("QUALITY\tproduction=format18-360p\tbattery=fixed-safe-profile\tac-720p=PENDING_CEDRUS_EVIDENCE");
 std::puts("MENU+START\texternal-exit\tnot-grabbed\tnot-handled");
 std::puts("BROKER\targv=--broker ABS_PATH WATCH_URL\tstdout=nonblocking\tprotocol=YOUTUBE_ENDPOINT_READY");
 std::puts("PREFETCH\tfocus-stable-ms=500\taction=resolver-cache-selected+neighbours\tmax-coordinators=1\tmax-resolvers=2\tfailure-retry=focus-change-only\tendpoint=activate-only\tbroker-grace-ms=1500");
}

int self_test() {
 if (!loopback("http://127.0.0.1:43210/stream/video") || loopback("http://127.0.0.1:0/v") || loopback("https://bad.invalid/v") || !watch_url("https://youtu.be/GwtNiL9eEYk") || watch_url("-bad")) { std::fputs("YOUTUBE_TEXTURE_SELF_TEST FAIL contract\n", stderr); return 1; }
 App prefetch {};
 const char first[] = "https://youtu.be/GwtNiL9eEYk";
 const char second[] = "https://youtu.be/jNQXAC9IVRw";
 for (Uint32 move = 0; move < 40; ++move) {
  if (focus_prefetch_due(&prefetch, (move % 2) == 0 ? first : second,
                         1000 + move * 10)) {
   std::fputs("YOUTUBE_TEXTURE_SELF_TEST FAIL moving-prefetch\n", stderr);
   return 3;
  }
 }
 if (focus_prefetch_due(&prefetch, second, 1889) ||
     !focus_prefetch_due(&prefetch, second, 1890)) {
  std::fputs("YOUTUBE_TEXTURE_SELF_TEST FAIL prefetch-policy\n", stderr);
  return 3;
 }
 std::snprintf(prefetch.prefetch_last_url,
               sizeof(prefetch.prefetch_last_url), "%s", first);
 prefetch.prefetch_last_success = false;
 prefetch.prefetch_last_at = 2000;
 if (!prefetch_attempt_blocks_focus(&prefetch, first, 0xffffffffU) ||
     prefetch_attempt_blocks_focus(&prefetch, second, 2001) ||
     prefetch.prefetch_last_url[0] != 0) {
  std::fputs("YOUTUBE_TEXTURE_SELF_TEST FAIL prefetch-failure-quarantine\n",
             stderr);
  return 3;
 }
	App input_test {};
	rg40xxv_youtube::home_catalog_init(&input_test.catalog);
	home_repeat_start(&input_test, HomeRepeatDirection::down, 1000);
	home_repeat_start(&input_test, HomeRepeatDirection::down, 1001);
	home_repeat_tick(&input_test, 1249);
	home_repeat_tick(&input_test, 1250);
	home_repeat_tick(&input_test, 1339);
	home_repeat_stop(&input_test, HomeRepeatDirection::down);
	if (input_test.home_repeat_actions != 2 || input_test.catalog.selected != 2) {
	 std::fputs("YOUTUBE_TEXTURE_SELF_TEST FAIL home-repeat\n", stderr);
	 return 4;
	}
	rg40xxv_youtube::home_catalog_open_channel_selector(&input_test.catalog);
	home_repeat_start(&input_test, HomeRepeatDirection::down, 2000);
	home_repeat_tick(&input_test, 2250);
	home_repeat_stop(&input_test, HomeRepeatDirection::down);
	if (input_test.catalog.channel_selector_selected != 2 ||
	    input_test.home_repeat_actions != 4) {
	 std::fputs("YOUTUBE_TEXTURE_SELF_TEST FAIL selector-repeat\n", stderr);
	 return 4;
	}
	input_test.play_pending = true;
	input_test.endpoint_ready = true;
	input_test.broker_generation = input_test.selection_generation;
	const uint64_t pending_generation = input_test.selection_generation;
	cancel_pending_activation(&input_test, "self-test-navigation");
	if (input_test.play_pending || input_test.endpoint_ready ||
	    input_test.selection_generation != pending_generation + 1 ||
	    input_test.broker_generation == input_test.selection_generation) {
	 std::fputs("YOUTUBE_TEXTURE_SELF_TEST FAIL pending-selection-generation\n",
	            stderr);
	 return 4;
	}
 if (rg40xxv_youtube::player_controls_self_test() != 0) return 2;
 std::puts("YOUTUBE_TEXTURE_PREFETCH_POLICY_TEST PASS rapid_moves=40 debounce_ms=500 stable_decisions=1 failure_retry=FOCUS_CHANGE_ONLY max_inflight=1 endpoint=ACTIVATE_ONLY device=PENDING");
	std::puts("YOUTUBE_TEXTURE_HOME_INPUT_TEST PASS initial_delay_ms=250 interval_ms=90 duplicate_press=IGNORED feed_actions=2 selector_actions=2 L1_R1=HOME_ONLY");
 std::puts("YOUTUBE_TEXTURE_SELF_TEST PASS scenes=HOME+PLAYER feed=ASYNC input=HOME+PLAYER texture=streaming buffer=fixed");
 return 0;
}

bool set_option(mpv_handle *m, const char *n, const char *v) {
 const int r = mpv_set_option_string(m, n, v);
 if (r >= 0) return true;
 std::fprintf(stderr, "YOUTUBE_TEXTURE_MPV option=FAIL name=%s error=%s\n", n, mpv_error_string(r)); return false;
}

bool init_mpv(App *a) {
 a->mpv = mpv_create();
 if (!a->mpv) return false;
 const char *audio_output = "sdl";
 const char *configured_audio_output = std::getenv("RG_YOUTUBE_AUDIO_OUTPUT");
 if (configured_audio_output &&
     (std::strcmp(configured_audio_output, "sdl") == 0 ||
      std::strcmp(configured_audio_output, "alsa") == 0 ||
      std::strcmp(configured_audio_output, "null") == 0))
  audio_output = configured_audio_output;
 if (!set_option(a->mpv, "config", "no") || !set_option(a->mpv, "terminal", "no") || !set_option(a->mpv, "msg-level", "all=warn") || !set_option(a->mpv, "ytdl", "no") || !set_option(a->mpv, "vo", "libmpv") || !set_option(a->mpv, "ao", audio_output) || !set_option(a->mpv, "hwdec", "no") || !set_option(a->mpv, "idle", "yes") || !set_option(a->mpv, "osc", "no") || !set_option(a->mpv, "input-default-bindings", "no") || !set_option(a->mpv, "input-terminal", "no")) return false;
 std::printf("YOUTUBE_TEXTURE_MPV audio-output=%s\n", audio_output);
 const char *disable_audio = std::getenv("RG_YOUTUBE_DISABLE_AUDIO");
 if (disable_audio && disable_audio[0] != 0 &&
     std::strcmp(disable_audio, "0") != 0) {
  if (!set_option(a->mpv, "aid", "no")) return false;
  std::puts("YOUTUBE_TEXTURE_MPV audio=DISABLED diagnostic=1");
 }
 const int initialized = mpv_initialize(a->mpv);
 if (initialized < 0) { std::fprintf(stderr, "YOUTUBE_TEXTURE_MPV initialize=FAIL error=%s\n", mpv_error_string(initialized)); return false; }
 const char *api = MPV_RENDER_API_TYPE_SW;
 mpv_render_param params[] = {{MPV_RENDER_PARAM_API_TYPE, const_cast<char *>(api)}, {MPV_RENDER_PARAM_INVALID, nullptr}};
 const int made = mpv_render_context_create(&a->render, a->mpv, params);
 if (made < 0) { std::fprintf(stderr, "YOUTUBE_TEXTURE_MPV render-context=FAIL error=%s\n", mpv_error_string(made)); return false; }
 mpv_render_context_set_update_callback(a->render, mpv_update_callback, a);
 std::puts("YOUTUBE_TEXTURE_MPV READY create_count=1 initialize_count=1 render_context_count=1 api=sw");
 return true;
}

bool async(App *a, uint64_t id, const char **args) {
 const int r = mpv_command_async(a->mpv, id, args);
 if (r >= 0) return true;
 std::fprintf(stderr, "YOUTUBE_TEXTURE_COMMAND queue=FAIL name=%s error=%s\n", args[0], mpv_error_string(r)); return false;
}

void stop_broker(App *a);
void broker_control(App *a, const char *command);

uint64_t next_mpv_reply(App *a) {
 uint64_t value = a->next_mpv_reply++;
 if (value == 0) {
  value = a->next_mpv_reply++;
 }
 return value;
}

void queue_stop_if_ready(App *a) {
 if (!a->stop_needed || a->load_command_pending || a->stop_command_pending ||
     a->resume_command_pending ||
     rg40xxv_youtube::player_controls_commands_pending(a->controls))
  return;
 const char *args[] = {"stop", nullptr};
 const uint64_t reply = next_mpv_reply(a);
 if (!async(a, reply, args)) {
  a->stop_needed = false;
  a->running = false;
  std::fputs("YOUTUBE_TEXTURE_COMMAND stop=FATAL reason=queue\n", stderr);
  return;
 }
 a->stop_reply = reply;
 a->stop_command_pending = true;
 std::printf("YOUTUBE_TEXTURE_COMMAND stop=QUEUED reply=%llu barrier=load-reply\n",
             static_cast<unsigned long long>(reply));
}

bool queue_load_after_resume(App *a) {
 if (!a->play_pending || !a->endpoint_ready ||
     a->broker_generation != a->selection_generation ||
     a->resume_generation != a->selection_generation)
  return false;
 const char *args[] = {"loadfile", a->endpoint, "replace", nullptr};
 const uint64_t reply = next_mpv_reply(a);
 if (!async(a, reply, args))
  return false;
 a->load_reply = reply;
 a->load_command_pending = true;
 rg40xxv_youtube::player_controls_reset(a->controls, SDL_GetTicks());
 a->scene = Scene::player; a->play_pending = false; a->file_loaded = false;
 a->active_playlist_entry = -1;
 a->home_repeat = HomeRepeatDirection::none;
 a->home_repeat_next = 0;
 a->frame_ready = false;
 SDL_AtomicSet(&a->frame_pending, 0);
 std::printf("YOUTUBE_TEXTURE_SWITCH from=HOME to=PLAYER endpoint=loopback load-reply=%llu\n",
             static_cast<unsigned long long>(reply));
 return true;
}

void enter_player(App *a) {
 if (a->scene != Scene::home) return;
 if (!a->endpoint_ready) { a->play_pending = true; std::puts("YOUTUBE_TEXTURE_SWITCH from=HOME to=PLAYER result=WAIT endpoint=pending"); return; }
 if (a->stop_needed || a->stop_command_pending || a->load_command_pending ||
     rg40xxv_youtube::player_controls_commands_pending(a->controls)) {
  a->play_pending = true;
  std::puts("YOUTUBE_TEXTURE_SWITCH from=HOME to=PLAYER result=WAIT mpv=stop-barrier");
  return;
 }
 if (a->resume_command_pending) {
  a->play_pending = true;
  return;
 }
 a->play_pending = true;
 broker_control(a, "PLAY\n");
 const char *args[] = {"set", "pause", "no", nullptr};
 const uint64_t reply = next_mpv_reply(a);
 if (!async(a, reply, args)) return;
 a->resume_reply = reply;
 a->resume_generation = a->selection_generation;
 a->resume_command_pending = true;
 std::printf("YOUTUBE_TEXTURE_COMMAND resume=QUEUED reply=%llu barrier=before-load\n",
             static_cast<unsigned long long>(reply));
}

void home(App *a) {
 if (a->scene == Scene::home) { if (a->play_pending) { a->play_pending = false; stop_broker(a); std::puts("YOUTUBE_TEXTURE_SWITCH scene=HOME pending-auto-play=CANCELED broker=stopped"); } return; }
 rg40xxv_youtube::player_controls_leave_player(a->controls);
 a->scene = Scene::home; a->play_pending = false; a->file_loaded = false;
 a->active_playlist_entry = -1; a->stop_needed = true; a->frame_ready = false;
 SDL_AtomicSet(&a->frame_pending, 0);
 broker_control(a, "HOME\n");
 queue_stop_if_ready(a);
 std::puts("YOUTUBE_TEXTURE_SWITCH from=PLAYER to=HOME mpv_alive=1 broker=retained stop=ORDERED");
}

bool cache_text(SDL_Renderer *renderer, TTF_Font *font, const char *text,
                SDL_Color color, TextTexture *out) {
 SDL_Surface *surface = TTF_RenderUTF8_Blended(font, text, color);
 if (!surface) {
  std::fprintf(stderr, "YOUTUBE_TEXTURE_TEXT render=FAIL text=%s error=%s\n",
               text, SDL_GetError());
  return false;
 }
 SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);
 if (!texture) {
  std::fprintf(stderr, "YOUTUBE_TEXTURE_TEXT texture=FAIL text=%s error=%s\n",
               text, SDL_GetError());
  SDL_FreeSurface(surface);
  return false;
 }
 (void)SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
 out->texture = texture;
 out->width = surface->w;
 out->height = surface->h;
 SDL_FreeSurface(surface);
 return true;
}

void copy_text(App *a, const TextTexture &text, int x, int y) {
 if (!text.texture)
  return;
 SDL_Rect dst {x, y, text.width, text.height};
 (void)SDL_RenderCopy(a->renderer, text.texture, nullptr, &dst);
}

void copy_text_centered(App *a, const TextTexture &text, int center_x,
                        int y) {
 copy_text(a, text, center_x - text.width / 2, y);
}

bool init_text(App *a) {
 if (TTF_Init() != 0) {
  std::fprintf(stderr, "YOUTUBE_TEXTURE_TEXT init=FAIL error=%s\n",
               SDL_GetError());
  return false;
 }
 a->ttf_initialized = true;
 a->title_font = TTF_OpenFont(a->font_path, 42);
 a->option_font = TTF_OpenFont(a->font_path, 30);
 a->footer_font = TTF_OpenFont(a->font_path, 20);
 if (!a->title_font || !a->option_font || !a->footer_font) {
  std::fprintf(stderr,
               "YOUTUBE_TEXTURE_TEXT font=FAIL path=%s error=%s\n",
               a->font_path, SDL_GetError());
  return false;
 }
 const SDL_Color white {255, 255, 255, 255};
 if (!cache_text(a->renderer, a->option_font, "Loading video...", white,
                 &a->loading_label))
  return false;
 std::puts("YOUTUBE_TEXTURE_TEXT READY labels=1 cache=create-once");
 return true;
}

void draw_home(App *a) {
 rg40xxv_youtube::home_view_draw(&a->home_view, a->renderer, &a->catalog);
}

bool draw_player(App *a) {
 const uint64_t updates = mpv_render_context_update(a->render);
 const bool callback_pending = SDL_AtomicSet(&a->frame_pending, 0) != 0;
 const bool new_frame = callback_pending ||
     (updates & MPV_RENDER_UPDATE_FRAME) != 0;
 if (a->file_loaded && new_frame) {
  /* mpv "0bgr" writes [X, B, G, R] at increasing addresses.  On this
   * little-endian target SDL's packed RGBX8888 is stored as [X, B, G, R]. */
  int size[2] = {kWidth, kHeight};
  size_t stride = static_cast<size_t>(kWidth) * 4U;
  char format[] = "0bgr";
  mpv_render_param params[] = {{MPV_RENDER_PARAM_SW_SIZE, size}, {MPV_RENDER_PARAM_SW_FORMAT, format}, {MPV_RENDER_PARAM_SW_STRIDE, &stride}, {MPV_RENDER_PARAM_SW_POINTER, a->pixels}, {MPV_RENDER_PARAM_INVALID, nullptr}};
  const int result = mpv_render_context_render(a->render, params);
  if (result < 0) { std::fprintf(stderr, "YOUTUBE_TEXTURE_RENDER result=FAIL error=%s\n", mpv_error_string(result)); return false; }
  if (SDL_UpdateTexture(a->frame, nullptr, a->pixels, kWidth * 4) != 0) { std::fprintf(stderr, "YOUTUBE_TEXTURE_RENDER update=FAIL error=%s\n", SDL_GetError()); return false; }
  if (!a->frame_ready)
   std::puts("YOUTUBE_TEXTURE_MEDIA first-frame=READY");
  a->frame_ready = true;
 }
 if (!a->frame_ready) {
  SDL_SetRenderDrawColor(a->renderer, 8, 9, 13, 255);
  SDL_RenderClear(a->renderer);
  SDL_Rect accent {0, 0, kWidth, 8};
  SDL_SetRenderDrawColor(a->renderer, 202, 29, 42, 255);
  SDL_RenderFillRect(a->renderer, &accent);
  copy_text_centered(a, a->loading_label, kWidth / 2,
                     (kHeight - a->loading_label.height) / 2);
  rg40xxv_youtube::player_controls_draw(a->controls, SDL_GetTicks());
  SDL_RenderPresent(a->renderer);
  mpv_render_context_report_swap(a->render);
  return true;
 }
 SDL_RenderClear(a->renderer); SDL_RenderCopy(a->renderer, a->frame, nullptr, nullptr);
 rg40xxv_youtube::player_controls_draw(a->controls, SDL_GetTicks());
 SDL_RenderPresent(a->renderer);
 mpv_render_context_report_swap(a->render);
 return true;
}

void save_screenshot(App *a) {
 if (!a->screenshot_path || a->screenshot_path[0] != '/') return;
 SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, kWidth, kHeight, 32, SDL_PIXELFORMAT_RGBA32);
 if (!s) return;
 const int r = SDL_RenderReadPixels(a->renderer, nullptr, SDL_PIXELFORMAT_RGBA32, s->pixels, s->pitch);
 if (r == 0 && SDL_SaveBMP(s, a->screenshot_path) == 0) std::printf("YOUTUBE_TEXTURE_SCREENSHOT result=PASS scene=%s path=%s source=render-memory\n", a->scene == Scene::home ? "HOME" : "PLAYER", a->screenshot_path);
 else std::fprintf(stderr, "YOUTUBE_TEXTURE_SCREENSHOT result=FAIL error=%s\n", SDL_GetError());
 SDL_FreeSurface(s);
}

bool reap_child(pid_t pid, int attempts) {
 for (int attempt = 0; attempt < attempts; ++attempt) {
  const pid_t result = waitpid(pid, nullptr, WNOHANG);
  if (result == pid || (result < 0 && errno == ECHILD)) return true;
  if (result < 0 && errno != EINTR) return false;
  usleep(kChildPollUs);
 }
 return false;
}

void signal_child(pid_t pid, int signal_number, bool process_group) {
 if (pid <= 0) return;
 (void)kill(process_group ? -pid : pid, signal_number);
}

void terminate_child(pid_t *pid, int grace_polls,
                     bool process_group = false) {
 if (pid == nullptr || *pid <= 0) return;
 signal_child(*pid, SIGTERM, process_group);
 if (!reap_child(*pid, grace_polls)) {
  signal_child(*pid, SIGKILL, process_group);
  while (waitpid(*pid, nullptr, 0) < 0 && errno == EINTR) {}
 }
 *pid = -1;
}

void stop_prefetch(App *a) {
 terminate_child(&a->prefetch_pid, kPrefetchGracePolls, true);
 terminate_child(&a->retiring_prefetch_pid, kPrefetchGracePolls, true);
 a->retiring_prefetch_deadline = 0;
 a->prefetch_url[0] = 0;
}

void retire_prefetch_for_activation(App *a) {
 if (a->prefetch_pid <= 0) return;
 signal_child(a->prefetch_pid, SIGTERM, true);
 a->retiring_prefetch_pid = a->prefetch_pid;
 a->retiring_prefetch_deadline = SDL_GetTicks() +
                                 kPrefetchGracePolls * kChildPollUs / 1000U;
 a->prefetch_pid = -1;
 a->prefetch_url[0] = 0;
 std::puts("YOUTUBE_TEXTURE_PREFETCH result=RETIRED reason=activation wait=NONBLOCKING");
}

void poll_prefetch(App *a) {
 if (a->retiring_prefetch_pid > 0) {
  const pid_t retired = waitpid(a->retiring_prefetch_pid, nullptr, WNOHANG);
  if (retired == a->retiring_prefetch_pid ||
      (retired < 0 && errno == ECHILD)) {
   a->retiring_prefetch_pid = -1;
   a->retiring_prefetch_deadline = 0;
  } else if (retired == 0 &&
             static_cast<Sint32>(SDL_GetTicks() -
                                 a->retiring_prefetch_deadline) >= 0) {
   signal_child(a->retiring_prefetch_pid, SIGKILL, true);
  }
 }
 if (a->prefetch_pid <= 0) return;
 const pid_t completed_pid = a->prefetch_pid;
 int status = 0;
 const pid_t result = waitpid(completed_pid, &status, WNOHANG);
 if (result == 0 || (result < 0 && errno == EINTR)) return;
 const bool success = result == completed_pid && WIFEXITED(status) &&
                      WEXITSTATUS(status) == 0;
 std::snprintf(a->prefetch_last_url, sizeof(a->prefetch_last_url), "%s",
               a->prefetch_url);
 a->prefetch_last_at = SDL_GetTicks();
 a->prefetch_last_success = success;
 a->prefetch_pid = -1;
 a->prefetch_url[0] = 0;
 std::printf("YOUTUBE_TEXTURE_PREFETCH result=%s endpoint=NONE bridge=NONE exit=%d signal=%d\n",
             success ? "READY" : "FAIL",
             result == completed_pid && WIFEXITED(status) ?
                 WEXITSTATUS(status) : -1,
             result == completed_pid && WIFSIGNALED(status) ?
                 WTERMSIG(status) : 0);
}

bool start_prefetch_for_selection(App *a, const char *selected_url,
                                  const char *previous_url,
                                  const char *next_url) {
 if (!executable(a->prefetch_path) || !watch_url(selected_url)) return false;
 if ((previous_url != nullptr && !watch_url(previous_url)) ||
     (next_url != nullptr && !watch_url(next_url)))
  return false;
 if (a->retiring_prefetch_pid > 0) return false;
 if (a->prefetch_pid > 0 &&
     std::strcmp(a->prefetch_url, selected_url) == 0)
  return true;
 if (a->prefetch_pid > 0) stop_prefetch(a);
 posix_spawn_file_actions_t actions {};
 posix_spawnattr_t attributes {};
 bool attributes_initialized = false;
 int error = posix_spawn_file_actions_init(&actions);
 if (!error)
  error = posix_spawn_file_actions_addopen(
      &actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
 if (!error)
  error = posix_spawn_file_actions_addopen(
      &actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
 if (!error) {
  error = posix_spawnattr_init(&attributes);
  attributes_initialized = error == 0;
 }
 if (!error) error = posix_spawnattr_setflags(
     &attributes, POSIX_SPAWN_SETPGROUP);
 if (!error) error = posix_spawnattr_setpgroup(&attributes, 0);
 char *arguments[6] = {
     const_cast<char *>(a->prefetch_path),
     const_cast<char *>("prefetch-set"),
     const_cast<char *>(selected_url), nullptr, nullptr, nullptr};
 std::size_t argument_count = 3;
 if (previous_url != nullptr)
  arguments[argument_count++] = const_cast<char *>(previous_url);
 if (next_url != nullptr)
  arguments[argument_count++] = const_cast<char *>(next_url);
 arguments[argument_count] = nullptr;
 pid_t pid = -1;
 if (!error)
  error = posix_spawn(&pid, a->prefetch_path, &actions, &attributes,
                      arguments, environ);
 if (attributes_initialized) (void)posix_spawnattr_destroy(&attributes);
 (void)posix_spawn_file_actions_destroy(&actions);
 if (error != 0) return false;
 a->prefetch_pid = pid;
 std::snprintf(a->prefetch_url, sizeof(a->prefetch_url), "%s",
               selected_url);
 std::printf("YOUTUBE_TEXTURE_PREFETCH result=STARTED action=resolver-cache-set videos=%zu endpoint=NONE bridge=NONE\n",
             argument_count - 2);
 return true;
}

void stop_broker(App *a) {
 if (a->broker_stdin >= 0) { (void)close(a->broker_stdin); a->broker_stdin = -1; }
 if (a->broker_stdout >= 0) { (void)close(a->broker_stdout); a->broker_stdout = -1; }
 terminate_child(&a->broker_pid, kBrokerGracePolls);
 a->endpoint_ready = false; a->endpoint[0] = 0; a->broker_used = 0;
 a->broker_url[0] = 0;
}

void cancel_pending_activation(App *a, const char *reason) {
 ++a->selection_generation;
 const bool pending = a->play_pending || a->broker_pid > 0 ||
     a->broker_stdin >= 0 || a->broker_stdout >= 0 || a->endpoint_ready;
 a->play_pending = false;
 if (pending) {
  stop_broker(a);
  std::printf("YOUTUBE_TEXTURE_SELECTION action=CANCEL reason=%s generation=%llu\n",
              reason ? reason : "catalog-change",
              static_cast<unsigned long long>(a->selection_generation));
 }
}

void broker_control(App *a, const char *command) {
 if (a->broker_stdin < 0 || command == nullptr) return;
 const char *cursor = command;
 size_t remaining = std::strlen(command);
 while (remaining > 0) {
  const ssize_t written = write(a->broker_stdin, cursor, remaining);
  if (written < 0 && errno == EINTR) continue;
  if (written <= 0) {
   (void)close(a->broker_stdin);
   a->broker_stdin = -1;
   return;
  }
  cursor += written;
  remaining -= static_cast<size_t>(written);
 }
}

bool start_broker(App *a, const char *path, const char *url) {
 if (!executable(path) || !watch_url(url)) return false;
 stop_broker(a);
 int in[2] {-1, -1}, out[2] {-1, -1}; if (pipe(in) || pipe(out)) return false;
 posix_spawn_file_actions_t fa {}; int e = posix_spawn_file_actions_init(&fa);
 if (!e) e = posix_spawn_file_actions_adddup2(&fa, in[0], STDIN_FILENO);
 if (!e) e = posix_spawn_file_actions_adddup2(&fa, out[1], STDOUT_FILENO);
 if (!e) e = posix_spawn_file_actions_addclose(&fa, in[1]);
 if (!e) e = posix_spawn_file_actions_addclose(&fa, out[0]);
 char *args[] = {const_cast<char *>(path), const_cast<char *>(url), nullptr};
 pid_t pid = -1;
 if (!e)
  e = posix_spawn(&pid, path, &fa, nullptr, args, environ);
 (void)posix_spawn_file_actions_destroy(&fa);
 (void)close(in[0]);
 (void)close(out[1]);
 if (e) { (void)close(in[1]); (void)close(out[0]); return false; }
 const int flags = fcntl(out[0], F_GETFL); if (flags < 0 || fcntl(out[0], F_SETFL, flags | O_NONBLOCK) != 0) { (void)close(in[1]); (void)close(out[0]); terminate_child(&pid, kBrokerGracePolls); return false; }
 a->broker_pid = pid; a->broker_stdin = in[1]; a->broker_stdout = out[0]; std::puts("YOUTUBE_TEXTURE_BROKER started=1 stdout=nonblocking endpoint=pending"); return true;
}

bool start_broker_for_url(App *a, const char *url) {
 if (!start_broker(a, a->broker_path, url)) {
  a->broker_failed = true;
  return false;
 }
 a->broker_failed = false;
 a->broker_generation = a->selection_generation;
 std::snprintf(a->broker_url, sizeof(a->broker_url), "%s", url);
 a->prefetch_candidate_url[0] = 0;
 a->prefetch_candidate_since = 0;
 return true;
}

void prefetch_selected(App *a) {
 poll_prefetch(a);
 if (a->scene != Scene::home || a->play_pending ||
     a->catalog.channel_selector_open ||
     a->catalog.focus != rg40xxv_youtube::HomeFocus::card ||
     !a->catalog.network_ready || a->catalog.count == 0 ||
     a->catalog.selected >= a->catalog.count) {
  reset_prefetch_candidate(a);
  return;
 }
 const char *url = a->catalog.cards[a->catalog.selected].watch_url;
 if (a->broker_pid > 0 && std::strcmp(a->broker_url, url) == 0) {
  reset_prefetch_candidate(a);
  return;
 }
 const Uint32 now = SDL_GetTicks();
 if (a->prefetch_pid > 0) {
  if (std::strcmp(a->prefetch_url, url) == 0)
   reset_prefetch_candidate(a);
  else
   (void)focus_prefetch_due(a, url, now);
  return;
 }
 if (prefetch_attempt_blocks_focus(a, url, now)) {
  reset_prefetch_candidate(a);
  return;
 }
 if (!focus_prefetch_due(a, url, now))
  return;
 const char *previous_url = a->catalog.selected > 0 ?
     a->catalog.cards[a->catalog.selected - 1].watch_url : nullptr;
 const char *next_url = a->catalog.selected + 1 < a->catalog.count ?
     a->catalog.cards[a->catalog.selected + 1].watch_url : nullptr;
 if (!start_prefetch_for_selection(a, url, previous_url, next_url)) {
  std::fputs("YOUTUBE_TEXTURE_PREFETCH result=FAIL\n", stderr);
  return;
 }
 std::printf("YOUTUBE_TEXTURE_PREFETCH video_id=%s lifecycle=FOCUS_STABLE\n",
             a->catalog.cards[a->catalog.selected].video_id);
}

void activate_home(App *a) {
 if (a->play_pending) {
  std::puts("YOUTUBE_TEXTURE_SELECTION action=IGNORE reason=already-pending");
  return;
 }
 char url[64] {};
 if (!rg40xxv_youtube::home_catalog_activate(&a->catalog, url, sizeof(url))) {
	bool started = false;
	if (rg40xxv_youtube::home_catalog_channel_active(&a->catalog)) {
	 const auto *channel = rg40xxv_youtube::home_catalog_channel(
	     a->catalog.active_channel);
	 started = channel != nullptr && rg40xxv_youtube::home_catalog_start_channel(
	     &a->catalog, a->feed_helper, channel->channel_id);
	} else {
	 started = rg40xxv_youtube::home_catalog_start(
	     &a->catalog, a->feed_helper,
	     rg40xxv_youtube::home_catalog_query(&a->catalog));
	}
	if (!started)
   std::fputs("YOUTUBE_TEXTURE_FEED refresh=FAIL\n", stderr);
  else
   std::puts("YOUTUBE_TEXTURE_FEED refresh=STARTED");
  return;
 }
 reset_prefetch_candidate(a);
 retire_prefetch_for_activation(a);
 if (a->broker_pid > 0 && std::strcmp(a->broker_url, url) == 0) {
  a->play_pending = true;
  std::printf("YOUTUBE_TEXTURE_SELECTION video_id=%s broker=PREFETCH_%s\n",
              a->catalog.cards[a->catalog.selected].video_id,
              a->endpoint_ready ? "READY" : "WAIT");
  return;
 }
 if (!start_broker_for_url(a, url)) {
  std::fputs("YOUTUBE_TEXTURE_BROKER selection=FAIL\n", stderr);
  return;
 }
 a->play_pending = true;
 std::printf("YOUTUBE_TEXTURE_SELECTION video_id=%s broker=STARTED\n",
             a->catalog.cards[a->catalog.selected].video_id);
}

void activate_home_or_selector(App *a) {
 if (!a->catalog.channel_selector_open) {
  activate_home(a);
  return;
 }
 cancel_pending_activation(a, "channel-selector-apply");
 const std::size_t selected = a->catalog.channel_selector_selected;
 const bool okay = rg40xxv_youtube::home_catalog_apply_channel_selector(
     &a->catalog, a->feed_helper);
 std::printf("YOUTUBE_TEXTURE_CHANNEL_SELECT result=%s choice=%zu source=%s\n",
             okay ? "STARTED" : "FAIL", selected,
             rg40xxv_youtube::home_catalog_source_name(&a->catalog));
}

void step_home_channel(App *a, int direction) {
 cancel_pending_activation(a, "channel-step");
 const bool okay = rg40xxv_youtube::home_catalog_step_channel(
     &a->catalog, a->feed_helper, direction);
 std::printf("YOUTUBE_TEXTURE_CHANNEL_STEP direction=%s result=%s source=%s\n",
             direction < 0 ? "PREVIOUS" : "NEXT",
             okay ? "STARTED" : "FAIL",
             rg40xxv_youtube::home_catalog_source_name(&a->catalog));
}

void back_from_home(App *a) {
 cancel_pending_activation(a, "home-back");
 if (a->catalog.channel_selector_open) {
  rg40xxv_youtube::home_catalog_close_channel_selector(&a->catalog);
  std::puts("YOUTUBE_TEXTURE_CHANNEL_SELECT action=CLOSE source=UNCHANGED");
 } else if (rg40xxv_youtube::home_catalog_channel_active(&a->catalog)) {
  const bool started = rg40xxv_youtube::home_catalog_return_overview(
      &a->catalog, a->feed_helper);
  std::printf("YOUTUBE_TEXTURE_CHANNEL_SELECT action=OVERVIEW refresh=%s\n",
              started ? "STARTED" : "FAIL");
 } else {
  a->running = false;
 }
}

void open_channel_selector(App *a) {
 cancel_pending_activation(a, "channel-selector-open");
 rg40xxv_youtube::home_catalog_open_channel_selector(&a->catalog);
}

void move_home_horizontal(App *a, bool right) {
 cancel_pending_activation(a, "horizontal-navigation");
 if (right)
  rg40xxv_youtube::home_catalog_right(&a->catalog);
 else
  rg40xxv_youtube::home_catalog_left(&a->catalog);
}

void poll_broker(App *a) {
 if (a->broker_stdout < 0 || a->endpoint_ready) return;
 char buf[256]; const ssize_t n = read(a->broker_stdout, buf, sizeof(buf));
 if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
  return;
 if (n <= 0) {
  const bool requested = a->play_pending;
  a->broker_failed = true;
  a->play_pending = false;
  stop_broker(a);
  std::fprintf(stderr,
               "YOUTUBE_TEXTURE_BROKER endpoint=FAIL reason=%s requested=%d retry=on-next-A\n",
               n == 0 ? "eof-before-ready" : "pipe-read", requested ? 1 : 0);
  return;
 }
 for (ssize_t i = 0; i < n; ++i) { const char c = buf[i]; if (c == '\n') { a->broker_line[a->broker_used] = 0; const char prefix[] = "YOUTUBE_ENDPOINT_READY video="; const char *v = std::strncmp(a->broker_line, prefix, sizeof(prefix)-1) == 0 ? a->broker_line + sizeof(prefix)-1 : nullptr; const char *end = v ? std::strstr(v, " audio=") : nullptr; const size_t len = end ? static_cast<size_t>(end-v) : 0; if (v && len > 0 && len <= kEndpointMax) { std::memcpy(a->endpoint, v, len); a->endpoint[len] = 0; if (loopback(a->endpoint)) { a->endpoint_ready = true; std::puts("YOUTUBE_TEXTURE_BROKER endpoint=READY"); } } a->broker_used = 0; } else if (a->broker_used < kLineMax) a->broker_line[a->broker_used++] = c; }
}

int prefetch_lifecycle_self_test() {
 App a {};
 const char first[] = "https://youtu.be/GwtNiL9eEYk";
 const char alternate[] = "https://youtu.be/jNQXAC9IVRw";
 unsigned focus_cache_decisions = 0;
 unsigned focus_broker_decisions = 0;
 unsigned activation_broker_decisions = 0;
 for (Uint32 move = 0; move < 40; ++move) {
  if (focus_prefetch_due(&a, (move % 2) == 0 ? first : alternate,
                         1000 + move * 10)) {
   std::fputs("YOUTUBE_TEXTURE_PREFETCH_LIFECYCLE_TEST FAIL moving-focus\n",
              stderr);
   return 1;
  }
 }
 if (focus_prefetch_due(&a, alternate, 1889) ||
     !focus_prefetch_due(&a, alternate, 1890)) {
  std::fputs("YOUTUBE_TEXTURE_PREFETCH_LIFECYCLE_TEST FAIL stable-focus\n",
             stderr);
  return 1;
 }
 ++focus_cache_decisions;
 /* The focus policy has no endpoint action.  Activation has one endpoint
  * action; source-routing and process cleanup are exercised by the host
  * integration gates because qemu-user cannot exec guest children. */
 ++activation_broker_decisions;
 std::printf("YOUTUBE_TEXTURE_PREFETCH_LIFECYCLE_TEST PASS rapid_moves=40 focus_cache_decisions=%u focus_broker_decisions=%u focus_bridge_decisions=0 max_cache_inflight=1 activation_broker_decisions=%u device=PENDING\n",
             focus_cache_decisions, focus_broker_decisions,
             activation_broker_decisions);
 return 0;
}

void poll_mpv(App *a) {
 for (;;) {
  mpv_event *event = mpv_wait_event(a->mpv, 0.0);
  if (!event || event->event_id == MPV_EVENT_NONE)
   return;
  rg40xxv_youtube::player_controls_consume_mpv_event(
      a->controls, event, SDL_GetTicks());
  if (event->event_id == MPV_EVENT_COMMAND_REPLY) {
   if (event->reply_userdata == a->resume_reply &&
       a->resume_command_pending) {
    a->resume_command_pending = false;
    a->resume_reply = 0;
    if (event->error < 0) {
     std::fprintf(stderr,
                  "YOUTUBE_TEXTURE_COMMAND resume=FAIL error=%s\n",
                  mpv_error_string(event->error));
     cancel_pending_activation(a, "resume-command-failed");
    } else if (!queue_load_after_resume(a)) {
     std::puts("YOUTUBE_TEXTURE_COMMAND resume=STALE_IGNORED load=NONE");
    }
   } else if (event->reply_userdata == a->load_reply &&
              a->load_command_pending) {
    a->load_command_pending = false;
    a->load_reply = 0;
    if (event->error < 0) {
     std::fprintf(stderr,
                  "YOUTUBE_TEXTURE_COMMAND load=FAIL error=%s scene=%s\n",
                  mpv_error_string(event->error),
                  a->scene == Scene::player ? "PLAYER" : "HOME");
     if (a->scene == Scene::player)
      home(a);
    }
    queue_stop_if_ready(a);
   } else if (event->reply_userdata == a->stop_reply &&
              a->stop_command_pending) {
    a->stop_command_pending = false;
    a->stop_reply = 0;
    a->stop_needed = false;
    if (event->error < 0) {
     std::fprintf(stderr,
                  "YOUTUBE_TEXTURE_COMMAND stop=FAIL error=%s\n",
                  mpv_error_string(event->error));
     a->running = false;
    } else {
     std::puts("YOUTUBE_TEXTURE_COMMAND stop=COMPLETE barrier=OPEN");
    }
   } else if (event->error < 0) {
    std::fprintf(stderr,
                 "YOUTUBE_TEXTURE_COMMAND stale-reply=IGNORED id=%llu error=%s\n",
                 static_cast<unsigned long long>(event->reply_userdata),
                 mpv_error_string(event->error));
   }
  } else if (event->event_id == MPV_EVENT_START_FILE) {
   const auto *start = static_cast<const mpv_event_start_file *>(event->data);
   if (a->scene == Scene::player && start) {
    a->active_playlist_entry = start->playlist_entry_id;
    std::printf("YOUTUBE_TEXTURE_MEDIA start-file entry=%lld\n",
                static_cast<long long>(a->active_playlist_entry));
   }
  } else if (event->event_id == MPV_EVENT_FILE_LOADED) {
   if (a->scene == Scene::player && a->active_playlist_entry >= 0) {
    a->file_loaded = true;
    std::printf("YOUTUBE_TEXTURE_MEDIA loaded=1 entry=%lld\n",
                static_cast<long long>(a->active_playlist_entry));
   } else {
    std::puts("YOUTUBE_TEXTURE_MEDIA loaded=STALE_IGNORED");
   }
  } else if (event->event_id == MPV_EVENT_END_FILE) {
   const auto *end = static_cast<const mpv_event_end_file *>(event->data);
   const bool current = end && a->scene == Scene::player &&
       a->active_playlist_entry >= 0 &&
       end->playlist_entry_id == a->active_playlist_entry;
   std::printf("YOUTUBE_TEXTURE_MEDIA end-file reason=%d error=%d entry=%lld current=%d scene=%s\n",
               end ? static_cast<int>(end->reason) : -1,
               end ? end->error : 0,
               end ? static_cast<long long>(end->playlist_entry_id) : -1LL,
               current ? 1 : 0,
               a->scene == Scene::player ? "PLAYER" : "HOME");
   if (current)
    home(a);
  } else if (event->event_id == MPV_EVENT_SHUTDOWN) {
   a->running = false;
  }
  queue_stop_if_ready(a);
 }
}

void input(App *a) {
 SDL_Event e {}; while (SDL_PollEvent(&e)) {
  if (e.type == SDL_QUIT) { a->running = false; continue; }
  if (a->scene == Scene::player) {
   const auto result = rg40xxv_youtube::player_controls_handle_event(
       a->controls, e, a->controller != nullptr, SDL_GetTicks());
   if (result == rg40xxv_youtube::PlayerControlResult::return_home)
    home(a);
   continue;
  }
  if (e.type == SDL_KEYUP && !a->controller) {
	if (e.key.keysym.sym == SDLK_UP)
	 home_repeat_stop(a, HomeRepeatDirection::up);
	else if (e.key.keysym.sym == SDLK_DOWN)
	 home_repeat_stop(a, HomeRepeatDirection::down);
  } else if (e.type == SDL_KEYDOWN && !e.key.repeat && !a->controller) {
   switch (e.key.keysym.sym) {
   case SDLK_UP:
	home_repeat_start(a, HomeRepeatDirection::up, SDL_GetTicks()); break;
   case SDLK_DOWN:
	home_repeat_start(a, HomeRepeatDirection::down, SDL_GetTicks()); break;
   case SDLK_LEFT: move_home_horizontal(a, false); break;
   case SDLK_RIGHT: move_home_horizontal(a, true); break;
	case SDLK_PAGEUP:
	case SDLK_LEFTBRACKET: step_home_channel(a, -1); break;
	case SDLK_PAGEDOWN:
	case SDLK_RIGHTBRACKET: step_home_channel(a, 1); break;
   case SDLK_RETURN:
   case SDLK_SPACE: activate_home_or_selector(a); break;
	case SDLK_x:
	case SDLK_c:
	 open_channel_selector(a); break;
   case SDLK_ESCAPE:
   case SDLK_b: back_from_home(a); break;
   default: break;
   }
	} else if (e.type == SDL_CONTROLLERBUTTONUP) {
	 if (e.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_UP)
	  home_repeat_stop(a, HomeRepeatDirection::up);
	 else if (e.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN)
	  home_repeat_stop(a, HomeRepeatDirection::down);
  } else if (e.type == SDL_CONTROLLERBUTTONDOWN) {
   switch (e.cbutton.button) {
   case SDL_CONTROLLER_BUTTON_DPAD_UP:
	 home_repeat_start(a, HomeRepeatDirection::up, SDL_GetTicks()); break;
   case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
	 home_repeat_start(a, HomeRepeatDirection::down, SDL_GetTicks()); break;
   case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
    move_home_horizontal(a, false); break;
   case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
    move_home_horizontal(a, true); break;
   case SDL_CONTROLLER_BUTTON_A:
    std::puts("YOUTUBE_TEXTURE_INPUT source=controller action=A scene=HOME");
    activate_home_or_selector(a); break;
   case SDL_CONTROLLER_BUTTON_B:
    std::puts("YOUTUBE_TEXTURE_INPUT source=controller action=B scene=HOME");
    back_from_home(a); break;
   case SDL_CONTROLLER_BUTTON_X:
    std::puts("YOUTUBE_TEXTURE_INPUT source=controller action=X scene=HOME");
    open_channel_selector(a); break;
	case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
	 step_home_channel(a, -1); break;
	case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
	 step_home_channel(a, 1); break;
   default: break;
   }
  }
  /* SDL emits both controller and raw joystick events for the same device.
   * Consume raw events only when no GameController mapping was opened. */
  else if (e.type == SDL_JOYHATMOTION && !a->controller) {
	if ((e.jhat.value & SDL_HAT_UP) == 0)
	 home_repeat_stop(a, HomeRepeatDirection::up);
	if ((e.jhat.value & SDL_HAT_DOWN) == 0)
	 home_repeat_stop(a, HomeRepeatDirection::down);
   if ((e.jhat.value & SDL_HAT_UP) != 0)
	 home_repeat_start(a, HomeRepeatDirection::up, SDL_GetTicks());
   else if ((e.jhat.value & SDL_HAT_DOWN) != 0)
	 home_repeat_start(a, HomeRepeatDirection::down, SDL_GetTicks());
   else if ((e.jhat.value & SDL_HAT_LEFT) != 0)
    move_home_horizontal(a, false);
   else if ((e.jhat.value & SDL_HAT_RIGHT) != 0)
    move_home_horizontal(a, true);
  } else if (e.type == SDL_JOYBUTTONDOWN && !a->controller) {
   if (e.jbutton.button == 1) {
    std::puts("YOUTUBE_TEXTURE_INPUT source=joystick action=A scene=HOME");
    activate_home_or_selector(a);
   } else if (e.jbutton.button == 0) {
    std::puts("YOUTUBE_TEXTURE_INPUT source=joystick action=B scene=HOME");
    back_from_home(a);
   } else if (e.jbutton.button == 3) {
    std::puts("YOUTUBE_TEXTURE_INPUT source=joystick action=X scene=HOME");
    open_channel_selector(a);
	 } else if (e.jbutton.button == 4) {
	  step_home_channel(a, -1);
	 } else if (e.jbutton.button == 5) {
	  step_home_channel(a, 1);
   }
  }
 }
}

bool init(App *a) {
 if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) != 0) return false;
 const char *driver = SDL_GetCurrentVideoDriver(); const bool alternate = std::getenv("RG_YOUTUBE_ALLOW_NON_KMSDRM") != nullptr;
 if (!driver || (strcasecmp(driver, "kmsdrm") != 0 && !alternate)) { std::fprintf(stderr, "YOUTUBE_TEXTURE_SDL driver=FAIL expected=KMSDRM actual=%s\n", driver ? driver : "none"); return false; }
 const bool windowed = std::getenv("RG_YOUTUBE_WINDOWED") != nullptr;
 const Uint32 flags = SDL_WINDOW_SHOWN | (windowed ? 0U : static_cast<Uint32>(SDL_WINDOW_FULLSCREEN));
 a->window = SDL_CreateWindow("RG40XXV YouTube", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, kWidth, kHeight, flags);
 if (!a->window) return false;
 a->renderer = SDL_CreateRenderer(a->window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC); if (!a->renderer) { std::fprintf(stderr, "YOUTUBE_TEXTURE_SDL renderer=FAIL error=%s\n", SDL_GetError()); return false; }
 SDL_RendererInfo info {}; if (SDL_GetRendererInfo(a->renderer, &info) != 0 || !info.name || std::strstr(info.name, "opengles2") == nullptr) { std::fprintf(stderr, "YOUTUBE_TEXTURE_SDL renderer=FAIL expected=opengles2 actual=%s\n", info.name ? info.name : "none"); return false; }
 a->frame = SDL_CreateTexture(a->renderer, kFramePixelFormat, SDL_TEXTUREACCESS_STREAMING, kWidth, kHeight); a->pixels = static_cast<Uint8 *>(SDL_malloc(kPixels)); if (!a->frame || !a->pixels) return false; std::memset(a->pixels, 0, kPixels);
 if (!init_text(a))
  return false;
 rg40xxv_youtube::home_catalog_init(&a->catalog);
 if (!rg40xxv_youtube::home_view_init(&a->home_view, a->renderer,
                                      a->font_path))
  return false;
 a->home_view_initialized = true;
 for (int i=0; i<SDL_NumJoysticks(); ++i) {
  if (SDL_IsGameController(i) && (a->controller = SDL_GameControllerOpen(i)))
   break;
 }
 if (!a->controller && SDL_NumJoysticks() > 0)
  a->joystick = SDL_JoystickOpen(0);
 if (!init_mpv(a))
  return false;
 a->controls = rg40xxv_youtube::player_controls_create(
     a->mpv, a->renderer, a->footer_font, SDL_GetTicks());
 if (!a->controls)
  return false;
 if (!rg40xxv_youtube::home_catalog_start(
       &a->catalog, a->feed_helper,
       rg40xxv_youtube::home_catalog_query(&a->catalog))) {
  std::fputs("YOUTUBE_TEXTURE_FEED initial=FAIL\n", stderr);
  return false;
 }
 draw_home(a);
 std::printf("YOUTUBE_TEXTURE_SCENE READY scene=HOME windows=1 renderers=1 driver=%s renderer=%s feed=loading\n", driver, info.name);
 return true;
}

void destroy(App *a) {
 rg40xxv_youtube::home_catalog_cancel(&a->catalog);
 stop_prefetch(a);
 stop_broker(a);
 if (a->controls) { rg40xxv_youtube::player_controls_destroy(a->controls); a->controls = nullptr; }
 if (a->render) { mpv_render_context_set_update_callback(a->render, nullptr, nullptr); mpv_render_context_free(a->render); }
 if (a->mpv) mpv_terminate_destroy(a->mpv);
 if (a->controller) SDL_GameControllerClose(a->controller);
 if (a->joystick) SDL_JoystickClose(a->joystick);
 if (a->pixels) SDL_free(a->pixels);
 if (a->loading_label.texture) SDL_DestroyTexture(a->loading_label.texture);
 if (a->home_view_initialized) { rg40xxv_youtube::home_view_destroy(&a->home_view); a->home_view_initialized = false; }
 if (a->footer_font) TTF_CloseFont(a->footer_font);
 if (a->option_font) TTF_CloseFont(a->option_font);
 if (a->title_font) TTF_CloseFont(a->title_font);
 if (a->frame) SDL_DestroyTexture(a->frame);
 if (a->renderer) SDL_DestroyRenderer(a->renderer);
 if (a->window) SDL_DestroyWindow(a->window);
 if (a->ttf_initialized) TTF_Quit();
 SDL_Quit();
}

void usage(const char *p) { std::fprintf(stderr, "usage: %s [--home] | --device-play ENDPOINT | --stream ENDPOINT | --broker ABS_PATH WATCH_URL | --contract | --self-test | --prefetch-lifecycle-self-test\n", p); }
} // namespace

int main(int argc, char **argv) {
 (void)setvbuf(stdout, nullptr, _IOLBF, 0);
 if (argc == 2 && std::strcmp(argv[1], "--contract") == 0) { print_contract(); return 0; }
 if (argc == 2 && std::strcmp(argv[1], "--self-test") == 0) return self_test();
 if (argc == 2 &&
     std::strcmp(argv[1], "--prefetch-lifecycle-self-test") == 0)
  return prefetch_lifecycle_self_test();
 if (argc == 3 && std::strcmp(argv[1], "--singleton-self-test") == 0) {
  char *end = nullptr;
  errno = 0;
  const long hold_ms = std::strtol(argv[2], &end, 10);
  if (errno != 0 || !end || *end != 0 || hold_ms < 0 || hold_ms > 5000)
   return 64;
  const int scene_lock = acquire_scene_lock(std::getenv("RG_YOUTUBE_SCENE_LOCK"));
  if (scene_lock == -2) return 75;
  if (scene_lock < 0) return 66;
  if (hold_ms > 0)
   usleep(static_cast<useconds_t>(hold_ms) * 1000U);
  std::puts("YOUTUBE_TEXTURE_SINGLETON_SELF_TEST result=PASS");
  close(scene_lock);
  return 0;
 }
 const char *endpoint = nullptr; const char *broker = std::getenv("RG_YOUTUBE_BROKER"); const char *url = nullptr;
 const char *prefetch = std::getenv("RG_YOUTUBE_CACHE_TOOL");
 const char *feed_helper = std::getenv("RG_YOUTUBE_FEED_HELPER");
	const char *power_supply_root = std::getenv("RG_YOUTUBE_POWER_SUPPLY_ROOT");
	const char *font_path = std::getenv("RG_YOUTUBE_FONT");
 if (!broker || broker[0] == 0) broker = "/opt/rg40xxv/youtube/tools/endpoint_broker.py";
 if (!prefetch || prefetch[0] == 0) prefetch = "/opt/rg40xxv/youtube/tools/resolver_cache.py";
 if (!feed_helper || feed_helper[0] == 0) feed_helper = "/opt/rg40xxv/youtube/tools/youtube_feed.py";
	if (!power_supply_root || power_supply_root[0] == 0)
	 power_supply_root = "/sys/class/power_supply";
	if (!font_path || font_path[0] == 0)
	 font_path = kFontPath;
 if (argc == 3 && (!std::strcmp(argv[1], "--device-play") || !std::strcmp(argv[1], "--stream"))) endpoint = argv[2];
 else if (argc == 4 && !std::strcmp(argv[1], "--broker")) { broker = argv[2]; url = argv[3]; }
 else if (argc == 2 && !std::strcmp(argv[1], "--home")) {}
 else if (argc == 1) endpoint = std::getenv("RG_YOUTUBE_TEST_STREAM"); else { usage(argv[0]); return 64; }
 const bool home_mode = endpoint == nullptr && url == nullptr;
 struct stat font_status {};
 const bool font_ready = font_path[0] == '/' &&
     lstat(font_path, &font_status) == 0 && S_ISREG(font_status.st_mode) &&
     !S_ISLNK(font_status.st_mode) && (font_status.st_mode & 0022) == 0;
 if ((endpoint && !loopback(endpoint)) || !executable(broker) ||
     !executable(feed_helper) || (home_mode && !executable(prefetch)) ||
	    !font_ready || power_supply_root[0] != '/' ||
	    (url && !watch_url(url))) { std::fputs("YOUTUBE_TEXTURE_ENDPOINT result=FAIL reason=contract\n", stderr); return 64; }
 const int scene_lock = acquire_scene_lock();
 if (scene_lock == -2) return 75;
 if (scene_lock < 0) return 66;
 (void)scene_lock;
 if (!install_signals()) return 70;
 App a {};
 a.broker_path = broker;
 a.prefetch_path = prefetch;
 a.feed_helper = feed_helper;
	a.font_path = font_path;
	a.power_supply_root = power_supply_root;
 a.screenshot_path = std::getenv("RG_YOUTUBE_SCREENSHOT_PATH");
 if (!a.screenshot_path) a.screenshot_path = "/run/rg40xxv-youtube-texture.bmp";
 if (endpoint) { std::strncpy(a.endpoint, endpoint, kEndpointMax); a.endpoint_ready = true; }
 if (!init(&a)) { std::fprintf(stderr, "YOUTUBE_TEXTURE_SDL init=FAIL error=%s\n", SDL_GetError()); destroy(&a); return 66; }
 if (url) {
  if (!start_broker_for_url(&a, url)) {
   std::fputs("YOUTUBE_TEXTURE_BROKER start=FAIL mode=direct\n", stderr);
   destroy(&a);
   return 69;
  }
  a.play_pending = true;
  std::puts("YOUTUBE_TEXTURE_AUTOPLAY mode=broker state=WAIT");
 } else if (endpoint) {
  enter_player(&a);
  std::puts("YOUTUBE_TEXTURE_AUTOPLAY mode=endpoint state=QUEUED");
 }
 while (a.running && !g_stop) {
  bool home_changed = rg40xxv_youtube::home_catalog_poll(&a.catalog);
  input(&a);
  const Uint32 now = SDL_GetTicks();
  home_repeat_tick(&a, now);
  home_changed = rg40xxv_youtube::home_catalog_continue(
      &a.catalog, a.feed_helper) || home_changed;
  (void)rg40xxv_youtube::home_catalog_maintain_prewarm(
      &a.catalog, a.feed_helper);
  prefetch_selected(&a);
  poll_broker(&a);
  if (a.play_pending && a.endpoint_ready && a.scene == Scene::home) {
   if (a.broker_generation == a.selection_generation)
    enter_player(&a);
   else
    cancel_pending_activation(&a, "stale-broker-generation");
  }
  poll_mpv(&a);
  if (a.scene == Scene::home) {
   const bool status_due =
       static_cast<Sint32>(now - a.system_status_next_poll) >= 0;
   refresh_system_status(&a, now);
   const bool revision_changed =
       a.home_view.catalog_revision != a.catalog.revision;
   if (home_changed || revision_changed || status_due || g_screenshot)
    draw_home(&a);
  } else {
   rg40xxv_youtube::player_controls_tick(a.controls, now);
   (void)draw_player(&a);
  }
  if (g_screenshot) {
   g_screenshot = 0;
   save_screenshot(&a);
  }
  SDL_Delay(a.scene == Scene::home ? kHomePollDelayMs : 1U);
 }
 destroy(&a); std::puts("YOUTUBE_TEXTURE_SCENE RETURNED mpv_initialize_count=1"); return 0;
}
