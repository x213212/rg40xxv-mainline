#include <mpv/client.h>

#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace {

constexpr int kTimeoutSeconds = 45;

bool set_option(mpv_handle *player, const char *name, const char *value)
{
	const int result = mpv_set_option_string(player, name, value);
	if (result < 0) {
		std::fprintf(stderr, "YOUTUBE_MPV_OPTION_FAILED name=%s error=%s\n",
			     name, mpv_error_string(result));
		return false;
	}
	return true;
}

bool set_single_string_list_option(mpv_handle *player, const char *name,
				   const char *value)
{
	mpv_node item {};
	item.format = MPV_FORMAT_STRING;
	item.u.string = const_cast<char *>(value);
	mpv_node_list list {};
	list.num = 1;
	list.values = &item;
	mpv_node node {};
	node.format = MPV_FORMAT_NODE_ARRAY;
	node.u.list = &list;
	const int result = mpv_set_option(player, name, MPV_FORMAT_NODE, &node);
	if (result < 0) {
		std::fprintf(stderr, "YOUTUBE_MPV_OPTION_FAILED name=%s error=%s\n",
			     name, mpv_error_string(result));
		return false;
	}
	return true;
}

void print_media_properties(mpv_handle *player)
{
	char *codec = nullptr;
	char *audio_codec = nullptr;
	int64_t width = 0;
	int64_t height = 0;
	int64_t estimated_frames = 0;
	double duration = 0.0;
	double container_fps = 0.0;

	if (mpv_get_property(player, "video-codec", MPV_FORMAT_STRING, &codec) < 0)
		codec = nullptr;
	if (mpv_get_property(player, "audio-codec", MPV_FORMAT_STRING,
			     &audio_codec) < 0)
		audio_codec = nullptr;
	(void)mpv_get_property(player, "width", MPV_FORMAT_INT64, &width);
	(void)mpv_get_property(player, "height", MPV_FORMAT_INT64, &height);
	(void)mpv_get_property(player, "duration", MPV_FORMAT_DOUBLE, &duration);
	(void)mpv_get_property(player, "container-fps", MPV_FORMAT_DOUBLE,
			       &container_fps);
	(void)mpv_get_property(player, "estimated-frame-count", MPV_FORMAT_INT64,
			       &estimated_frames);
	std::printf("YOUTUBE_MEDIA codec=%s audio_codec=%s width=%lld height=%lld fps=%.3f "
		    "frames=%lld duration=%.3f\n",
		    codec != nullptr ? codec : "unknown",
		    audio_codec != nullptr ? audio_codec : "none",
		    static_cast<long long>(width), static_cast<long long>(height),
		    container_fps, static_cast<long long>(estimated_frames), duration);
	if (codec != nullptr)
		mpv_free(codec);
	if (audio_codec != nullptr)
		mpv_free(audio_codec);
}

struct HttpTransport {
	const char *user_agent = nullptr;
	int64_t initial_range_end = -1;
};

enum class InputProfile {
	none,
	h700,
	stock,
};

struct ControllerState {
	int hat_x = 0;
};

int open_controller(InputProfile *profile)
{
	for (int index = 0; index < 32; ++index) {
		char path[64] = {0};
		char name[128] = {0};
		(void)std::snprintf(path, sizeof(path), "/dev/input/event%d", index);
		const int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
		if (fd < 0)
			continue;
		if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) >= 0) {
			if (std::strstr(name, "H700 Gamepad") != nullptr)
				*profile = InputProfile::h700;
			else if (std::strstr(name, "ANBERNIC-keys") != nullptr)
				*profile = InputProfile::stock;
		}
		if (*profile != InputProfile::none)
			return fd; // Deliberately no EVIOCGRAB: MENU+START stays external.
		(void)close(fd);
	}
	return -1;
}

void controller_command(mpv_handle *player, const char *first,
			const char *second = nullptr,
			const char *third = nullptr)
{
	const char *arguments[] = {first, second, third, nullptr};
	const int result = mpv_command(player, arguments);
	if (result < 0)
		std::fprintf(stderr, "YOUTUBE_CONTROLLER command=%s error=%s\n",
			     first, mpv_error_string(result));
}

void process_controller(mpv_handle *player, int fd, InputProfile profile,
			ControllerState *state)
{
	struct input_event events[16] {};
	for (;;) {
		const ssize_t count = read(fd, events, sizeof(events));
		if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return;
		if (count <= 0)
			return;
		const size_t event_count = static_cast<size_t>(count) / sizeof(events[0]);
		for (size_t index = 0; index < event_count; ++index) {
			const input_event &event = events[index];
			if (event.type == EV_ABS && event.code == ABS_HAT0X) {
				if (event.value != state->hat_x && event.value < 0)
					controller_command(player, "seek", "-10", "relative");
				else if (event.value != state->hat_x && event.value > 0)
					controller_command(player, "seek", "10", "relative");
				state->hat_x = event.value;
				continue;
			}
			if (event.type != EV_KEY || event.value != 1)
				continue;
			const unsigned int a = profile == InputProfile::h700 ? BTN_EAST : 0x130U;
			const unsigned int b = profile == InputProfile::h700 ? BTN_SOUTH : 0x131U;
			const unsigned int start = profile == InputProfile::h700 ? BTN_START : 0x137U;
			if (event.code == a || event.code == start)
				controller_command(player, "cycle", "pause");
			else if (event.code == b)
				controller_command(player, "quit");
			else if (profile == InputProfile::h700 && event.code == BTN_DPAD_LEFT)
				controller_command(player, "seek", "-10", "relative");
			else if (profile == InputProfile::h700 && event.code == BTN_DPAD_RIGHT)
				controller_command(player, "seek", "10", "relative");
			else if (profile == InputProfile::h700 && event.code == BTN_TL)
				controller_command(player, "seek", "-30", "relative");
			else if (profile == InputProfile::h700 && event.code == BTN_TR)
				controller_command(player, "seek", "30", "relative");
		}
	}
}

int play_media(const char *path, const char *audio_path, double max_seconds,
	       const HttpTransport &transport, bool device_mode)
{
	mpv_handle *player = mpv_create();
	if (player == nullptr) {
		std::fputs("YOUTUBE_QEMU_PLAYBACK FAIL create\n", stderr);
		return 2;
	}

	char length_value[32] = { 0 };
	char range_header[96] = { 0 };
	if (max_seconds > 0.0)
		(void)std::snprintf(length_value, sizeof(length_value), "%.3f",
				    max_seconds);
	if (transport.initial_range_end >= 0)
		(void)std::snprintf(range_header, sizeof(range_header),
				    "Range: bytes=0-%lld",
				    static_cast<long long>(transport.initial_range_end));
	bool configured =
		set_option(player, "config", "no") &&
		set_option(player, "terminal", "no") &&
		set_option(player, "msg-level", "all=warn") &&
		/* The input is our already-resolved loopback range bridge.  Leaving
		 * mpv's hook enabled starts the old system youtube-dl against
		 * 127.0.0.1 and adds a second, useless extraction delay. */
		set_option(player, "ytdl", "no") &&
		(audio_path == nullptr ||
		 set_single_string_list_option(player, "audio-files", audio_path)) &&
		set_option(player, "hwdec", "no") &&
		(transport.user_agent == nullptr ||
		 set_option(player, "user-agent", transport.user_agent)) &&
		(transport.initial_range_end < 0 ||
		 set_option(player, "http-header-fields", range_header)) &&
		(max_seconds <= 0.0 || set_option(player, "length", length_value));
	if (device_mode) {
		const char *drm_device = std::getenv("RG_YOUTUBE_DRM_DEVICE");
		const char *drm_connector = std::getenv("RG_YOUTUBE_DRM_CONNECTOR");
		if (drm_device == nullptr || drm_device[0] == '\0')
			drm_device = "/dev/dri/card1";
		if (drm_connector == nullptr || drm_connector[0] == '\0')
			drm_connector = "DSI-1";
		configured = configured &&
			set_option(player, "vo", "gpu") &&
			set_option(player, "gpu-context", "drm") &&
			set_option(player, "drm-device", drm_device) &&
			set_option(player, "drm-connector", drm_connector) &&
			set_option(player, "ao", "alsa") &&
			set_option(player, "fullscreen", "yes") &&
			set_option(player, "osc", "no") &&
			set_option(player, "input-default-bindings", "no");
	} else {
		configured = configured &&
			set_option(player, "vo", "null") &&
			set_option(player, "ao", "null") &&
			set_option(player, "untimed", "yes");
	}
	if (!configured) {
		mpv_terminate_destroy(player);
		return 3;
	}

	const int initialize_result = mpv_initialize(player);
	if (initialize_result < 0) {
		std::fprintf(stderr, "YOUTUBE_QEMU_PLAYBACK FAIL initialize=%s\n",
			     mpv_error_string(initialize_result));
		mpv_terminate_destroy(player);
		return 4;
	}

	const char *command[] = { "loadfile", path, nullptr };
	const int command_result = mpv_command(player, command);
	if (command_result < 0) {
		std::fprintf(stderr, "YOUTUBE_QEMU_PLAYBACK FAIL load=%s\n",
			     mpv_error_string(command_result));
		mpv_terminate_destroy(player);
		return 5;
	}

	bool loaded = false;
	bool ended = false;
	bool failed = false;
	InputProfile input_profile = InputProfile::none;
	ControllerState controller_state {};
	const int input_fd = device_mode ? open_controller(&input_profile) : -1;
	if (device_mode && input_fd < 0) {
		std::fputs("YOUTUBE_DEVICE_PLAYBACK FAIL controller-unavailable\n", stderr);
		mpv_terminate_destroy(player);
		return 7;
	}
	if (device_mode)
		std::puts("YOUTUBE_CONTROLLER READY mapping=A/START:pause B:return LEFT/RIGHT:seek MENU+START:external-exit not_grabbed=1");
	double maximum_playback_time = 0.0;
	int64_t maximum_frame_number = 0;
	const auto started = std::chrono::steady_clock::now();
	auto deadline = std::chrono::steady_clock::now() +
		std::chrono::seconds(kTimeoutSeconds);
	while (!ended && (device_mode || std::chrono::steady_clock::now() < deadline)) {
		mpv_event *event = mpv_wait_event(player, device_mode ? 0.05 : 0.25);
		if (device_mode)
			process_controller(player, input_fd, input_profile,
					   &controller_state);
		double playback_time = 0.0;
		int64_t frame_number = 0;
		if (mpv_get_property(player, "playback-time", MPV_FORMAT_DOUBLE,
				     &playback_time) >= 0 &&
		    playback_time > maximum_playback_time)
			maximum_playback_time = playback_time;
		if (mpv_get_property(player, "estimated-frame-number",
				     MPV_FORMAT_INT64, &frame_number) >= 0 &&
		    frame_number > maximum_frame_number)
			maximum_frame_number = frame_number;
		if (event == nullptr)
			continue;
		switch (event->event_id) {
		case MPV_EVENT_FILE_LOADED:
			loaded = true;
			print_media_properties(player);
			break;
		case MPV_EVENT_END_FILE: {
			const auto *end = static_cast<mpv_event_end_file *>(event->data);
			failed = end != nullptr && end->error < 0;
			if (failed)
				std::fprintf(stderr,
					     "YOUTUBE_QEMU_PLAYBACK FAIL end=%s reason=%d\n",
					     mpv_error_string(end->error), end->reason);
			ended = true;
			break;
		}
		case MPV_EVENT_SHUTDOWN:
			ended = true;
			break;
		default:
			break;
		}
	}
	const double wall_seconds = std::chrono::duration<double>(
		std::chrono::steady_clock::now() - started).count();
	std::printf("YOUTUBE_PLAYBACK_METRICS media_seconds=%.3f frame=%lld "
		    "wall_seconds=%.3f\n",
		    maximum_playback_time,
		    static_cast<long long>(maximum_frame_number), wall_seconds);

	if (input_fd >= 0)
		(void)close(input_fd);
	mpv_terminate_destroy(player);
	if (!loaded || !ended || failed) {
		std::fprintf(stderr,
			     "YOUTUBE_QEMU_PLAYBACK FAIL loaded=%d ended=%d failed=%d\n",
			     loaded ? 1 : 0, ended ? 1 : 0, failed ? 1 : 0);
		return 6;
	}
	std::puts(device_mode ? "YOUTUBE_DEVICE_PLAYBACK RETURNED" :
		  "YOUTUBE_QEMU_PLAYBACK PASS");
	return 0;
}

} // namespace

int main(int argc, char **argv)
{
	if (argc == 3 && std::strcmp(argv[1], "--headless-play") == 0)
		return play_media(argv[2], nullptr, 0.0, HttpTransport {}, false);
	if (argc == 4 &&
	    std::strcmp(argv[1], "--headless-play-seconds") == 0) {
		char *end = nullptr;
		const double seconds = std::strtod(argv[3], &end);
		if (end == argv[3] || *end != '\0' || seconds < 0.1 ||
		    seconds > 30.0) {
			std::fputs("invalid playback duration\n", stderr);
			return 64;
		}
		return play_media(argv[2], nullptr, seconds, HttpTransport {}, false);
	}
	if (argc == 5 &&
	    std::strcmp(argv[1], "--headless-av-seconds") == 0) {
		char *end = nullptr;
		const double seconds = std::strtod(argv[4], &end);
		if (end == argv[4] || *end != '\0' || seconds < 0.1 ||
		    seconds > 30.0) {
			std::fputs("invalid playback duration\n", stderr);
			return 64;
		}
		return play_media(argv[2], argv[3], seconds,
				  HttpTransport {}, false);
	}
	if (argc == 6 &&
	    std::strcmp(argv[1], "--headless-http-seconds") == 0) {
		char *seconds_end = nullptr;
		char *range_end = nullptr;
		const double seconds = std::strtod(argv[3], &seconds_end);
		const long long range = std::strtoll(argv[5], &range_end, 10);
		if (seconds_end == argv[3] || *seconds_end != '\0' ||
		    seconds < 0.1 || seconds > 30.0 ||
		    argv[4][0] == '\0' || range_end == argv[5] ||
		    *range_end != '\0' || range < 65535 ||
		    range > 16777215) {
			std::fputs("invalid HTTP playback arguments\n", stderr);
			return 64;
		}
		return play_media(argv[2], nullptr, seconds,
				  HttpTransport { argv[4], range }, false);
	}
	if (argc == 3 && std::strcmp(argv[1], "--device-play") == 0)
		return play_media(argv[2], nullptr, 0.0, HttpTransport {}, true);
	if (argc == 4 && std::strcmp(argv[1], "--device-av") == 0)
		return play_media(argv[2], argv[3], 0.0, HttpTransport {}, true);
	if (argc == 2 && std::strcmp(argv[1], "--controller-contract") == 0) {
		std::puts("A/START\tpause-toggle");
		std::puts("B\treturn");
		std::puts("DPAD_LEFT/RIGHT\tseek-10/+10");
		std::puts("L1/R1\tseek-30/+30");
		std::puts("MENU+START\texternal-exit\tnot-grabbed");
		return 0;
	}
	std::fprintf(stderr,
		     "usage: %s --headless-play VIDEO | "
		     "--headless-play-seconds URL SECONDS | "
		     "--headless-av-seconds VIDEO AUDIO SECONDS | "
		     "--headless-http-seconds URL SECONDS USER_AGENT "
		     "RANGE_END | --device-play VIDEO | --device-av VIDEO AUDIO | "
		     "--controller-contract\n",
		     argv[0]);
	return 64;
}
