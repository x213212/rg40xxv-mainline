#define _POSIX_C_SOURCE 200809L

#include "settings.h"

#include "launcher.h"

#include <SDL.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum {
	SETTINGS_WORK_QUEUE = 32,
	SETTINGS_RESULT_QUEUE = 32,
	SETTINGS_COALESCED_STATES = 4,
	HARDWARECTL_TIMEOUT_MS = 5000,
	HARDWARECTL_TERM_GRACE_MS = 250,
};

static char *const hardwarectl_environment[] = {
	(char *)"PATH=/usr/sbin:/usr/bin:/sbin:/bin",
	(char *)"LC_ALL=C",
	(char *)"TZ=Asia/Taipei",
	NULL,
};

struct settings_request {
	enum settings_hardware_command command;
	uint64_t request_id;
	int value;
	int previous_value;
	char identifier[37];
	char secret[64];
};

struct settings_worker {
	SDL_Thread *thread;
	SDL_mutex *mutex;
	SDL_cond *condition;
	struct settings_request requests[SETTINGS_WORK_QUEUE];
	struct settings_hardware_result results[SETTINGS_RESULT_QUEUE];
	size_t request_head;
	size_t request_count;
	size_t result_head;
	size_t result_count;
	struct settings_request active_request;
	bool request_active;
	int last_applied_value[SETTINGS_COALESCED_STATES];
	bool last_applied_valid[SETTINGS_COALESCED_STATES];
	char executable[4096];
	char log_path[4096];
	bool stopping;
};

static bool valid_log_path(const char *path)
{
	if (path == NULL || path[0] != '/')
		return false;
	for (const unsigned char *cursor = (const unsigned char *)path;
	     *cursor != '\0'; ++cursor) {
		if (*cursor < 0x20 || *cursor == 0x7f)
			return false;
	}
	return true;
}

static int configure_child_signals(posix_spawnattr_t *attributes)
{
	sigset_t empty;
	sigset_t defaults;
	short flags = POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK |
		POSIX_SPAWN_SETSIGDEF;
	int error;

	(void)sigemptyset(&empty);
	(void)sigemptyset(&defaults);
	(void)sigaddset(&defaults, SIGINT);
	(void)sigaddset(&defaults, SIGTERM);
	(void)sigaddset(&defaults, SIGHUP);
	(void)sigaddset(&defaults, SIGPIPE);
	error = posix_spawnattr_setflags(attributes, flags);
	if (error == 0)
		error = posix_spawnattr_setpgroup(attributes, 0);
	if (error == 0)
		error = posix_spawnattr_setsigmask(attributes, &empty);
	if (error == 0)
		error = posix_spawnattr_setsigdefault(attributes, &defaults);
	return error;
}

static uint64_t monotonic_ms(void)
{
	struct timespec value;

	if (clock_gettime(CLOCK_MONOTONIC, &value) != 0)
		return 0U;
	return (uint64_t)value.tv_sec * 1000U +
		(uint64_t)value.tv_nsec / 1000000U;
}

static bool worker_stopping(struct settings_worker *worker)
{
	bool stopping;

	(void)SDL_LockMutex(worker->mutex);
	stopping = worker->stopping;
	(void)SDL_UnlockMutex(worker->mutex);
	return stopping;
}

static void request_arguments(const struct settings_worker *worker,
			      const struct settings_request *request,
			      char value[4], char *arguments[4])
{
	arguments[0] = (char *)worker->executable;
	arguments[2] = NULL;
	arguments[3] = NULL;
	switch (request->command) {
	case SETTINGS_HW_SCREEN_OFF:
		arguments[1] = (char *)"screen-off";
		break;
	case SETTINGS_HW_SCREEN_ON:
		arguments[1] = (char *)"screen-on";
		break;
	case SETTINGS_HW_BRIGHTNESS:
		arguments[1] = (char *)"brightness";
		(void)snprintf(value, 4, "%d", request->value);
		arguments[2] = value;
		break;
	case SETTINGS_HW_JOYSTICK_RGB:
		arguments[1] = (char *)"joystick-rgb";
		(void)snprintf(value, 4, "%d", request->value);
		arguments[2] = value;
		break;
	case SETTINGS_HW_USB_DEBUG:
		arguments[1] = (char *)"usb-debug";
		arguments[2] = (char *)(request->value != 0 ? "on" : "off");
		break;
	case SETTINGS_HW_VOLUME:
		arguments[1] = (char *)"volume";
		(void)snprintf(value, 4, "%d", request->value);
		arguments[2] = value;
		break;
	case SETTINGS_HW_MUTE_TOGGLE:
		arguments[1] = (char *)"mute-toggle";
		break;
	case SETTINGS_HW_ORDERLY_SHUTDOWN:
		arguments[1] = (char *)"orderly-shutdown";
		break;
	case SETTINGS_HW_REBOOT_CUSTOM:
		arguments[1] = (char *)"reboot-custom";
		break;
	case SETTINGS_HW_NETWORK_STATUS:
		arguments[1] = (char *)"network-status";
		break;
	case SETTINGS_HW_WIFI_RECOVER:
		arguments[1] = (char *)"network-wifi-recover";
		break;
	case SETTINGS_HW_WIFI_SCAN:
		arguments[1] = (char *)"network-wifi-scan";
		break;
	case SETTINGS_HW_WIFI_CONNECT:
		arguments[1] = (char *)"network-wifi-connect";
		arguments[2] = (char *)request->identifier;
		break;
	case SETTINGS_HW_WIFI_DISCONNECT:
		arguments[1] = (char *)"network-wifi-disconnect";
		break;
	case SETTINGS_HW_WIFI_FORGET:
		arguments[1] = (char *)"network-wifi-forget";
		arguments[2] = (char *)request->identifier;
		break;
	case SETTINGS_HW_HOTSPOT_SET:
		arguments[1] = (char *)"network-hotspot";
		arguments[2] = (char *)(request->value != 0 ? "on" : "off");
		break;
	}
}

static uint64_t request_timeout_ms(enum settings_hardware_command command)
{
	switch (command) {
	case SETTINGS_HW_NETWORK_STATUS:
		return 5000U;
	case SETTINGS_HW_WIFI_SCAN:
		return 20000U;
	case SETTINGS_HW_WIFI_RECOVER:
	case SETTINGS_HW_WIFI_CONNECT:
	case SETTINGS_HW_HOTSPOT_SET:
		return 30000U;
	case SETTINGS_HW_WIFI_DISCONNECT:
	case SETTINGS_HW_WIFI_FORGET:
		return 15000U;
	default:
		return HARDWARECTL_TIMEOUT_MS;
	}
}

static void secure_clear(void *memory, size_t size)
{
	volatile unsigned char *bytes = memory;

	while (size-- > 0U)
		*bytes++ = 0U;
}

static int preload_secret_pipe(const struct settings_request *request,
			       int pipe_fds[2])
{
	size_t length = strlen(request->secret);
	char payload[65];
	size_t offset = 0U;

	pipe_fds[0] = -1;
	pipe_fds[1] = -1;
	if (request->command != SETTINGS_HW_WIFI_CONNECT)
		return 0;
	if (pipe(pipe_fds) != 0)
		return errno;
	if (fcntl(pipe_fds[0], F_SETFD, FD_CLOEXEC) != 0 ||
	    fcntl(pipe_fds[1], F_SETFD, FD_CLOEXEC) != 0) {
		int error = errno;

		(void)close(pipe_fds[0]);
		(void)close(pipe_fds[1]);
		pipe_fds[0] = -1;
		pipe_fds[1] = -1;
		return error;
	}
	memcpy(payload, request->secret, length);
	payload[length++] = '\n';
	while (offset < length) {
		ssize_t count = write(pipe_fds[1], payload + offset, length - offset);

		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0) {
			int error = count < 0 ? errno : EIO;

			secure_clear(payload, sizeof(payload));
			(void)close(pipe_fds[0]);
			(void)close(pipe_fds[1]);
			pipe_fds[0] = -1;
			pipe_fds[1] = -1;
			return error;
		}
		offset += (size_t)count;
	}
	secure_clear(payload, sizeof(payload));
	(void)close(pipe_fds[1]);
	pipe_fds[1] = -1;
	return 0;
}

static struct settings_hardware_result execute_request(
	struct settings_worker *worker,
	const struct settings_request *request)
{
	struct settings_hardware_result result = {
		.command = request->command,
		.request_id = request->request_id,
		.value = request->value,
		.previous_value = request->previous_value,
		.exit_code = -1,
	};
	posix_spawn_file_actions_t actions;
	posix_spawnattr_t attributes;
	char value[4] = { 0 };
	char *arguments[4];
	pid_t child = 0;
	int log_fd = -1;
	int secret_pipe[2] = { -1, -1 };
	int status = 0;
	int error;
	pid_t waited;
	bool timed_out = false;
	struct stat log_status;

	request_arguments(worker, request, value, arguments);
	error = preload_secret_pipe(request, secret_pipe);
	if (error != 0)
		goto done;
	error = posix_spawn_file_actions_init(&actions);
	if (error != 0)
		goto done;
	error = posix_spawnattr_init(&attributes);
	if (error != 0) {
		(void)posix_spawn_file_actions_destroy(&actions);
		goto done;
	}
	log_fd = open(worker->log_path,
		      O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW |
		      O_NONBLOCK,
		      0600);
	if (log_fd < 0)
		error = errno;
	if (error == 0 && fstat(log_fd, &log_status) != 0)
		error = errno;
	if (error == 0 && !S_ISREG(log_status.st_mode))
		error = EINVAL;
	if (error == 0)
		error = posix_spawn_file_actions_adddup2(&actions, log_fd,
							 STDOUT_FILENO);
	if (error == 0)
		error = posix_spawn_file_actions_adddup2(&actions, log_fd,
							 STDERR_FILENO);
	if (error == 0)
		error = posix_spawn_file_actions_addclose(&actions, log_fd);
	if (error == 0 && secret_pipe[0] >= 0)
		error = posix_spawn_file_actions_adddup2(&actions, secret_pipe[0],
			STDIN_FILENO);
	if (error == 0 && secret_pipe[0] >= 0)
		error = posix_spawn_file_actions_addclose(&actions, secret_pipe[0]);
	if (error == 0)
		error = configure_child_signals(&attributes);
	if (error == 0)
		error = posix_spawn(&child, worker->executable, &actions,
				    &attributes, arguments,
				    hardwarectl_environment);
	if (log_fd >= 0)
		(void)close(log_fd);
	if (secret_pipe[0] >= 0) {
		(void)close(secret_pipe[0]);
		secret_pipe[0] = -1;
	}
	(void)posix_spawnattr_destroy(&attributes);
	(void)posix_spawn_file_actions_destroy(&actions);
	if (error != 0)
		goto done;
	{
		uint64_t deadline = monotonic_ms() +
			request_timeout_ms(request->command);
		struct timespec pause = { .tv_sec = 0, .tv_nsec = 10000000L };

		for (;;) {
			do {
				waited = waitpid(child, &status, WNOHANG);
			} while (waited < 0 && errno == EINTR);
			if (waited == child)
				break;
			if (waited < 0) {
				error = errno;
				goto done;
			}
			if (worker_stopping(worker) || monotonic_ms() >= deadline) {
				timed_out = true;
				break;
			}
			(void)nanosleep(&pause, NULL);
		}
		if (timed_out) {
			uint64_t grace = monotonic_ms() + HARDWARECTL_TERM_GRACE_MS;

			(void)kill(-child, SIGTERM);
			while (monotonic_ms() < grace) {
				do {
					waited = waitpid(child, &status, WNOHANG);
				} while (waited < 0 && errno == EINTR);
				if (waited == child)
					break;
				if (waited < 0) {
					error = errno;
					goto done;
				}
				(void)nanosleep(&pause, NULL);
			}
			if (waited == 0) {
				(void)kill(-child, SIGKILL);
				do {
					waited = waitpid(child, &status, 0);
				} while (waited < 0 && errno == EINTR);
				if (waited < 0) {
					error = errno;
					goto done;
				}
			}
			(void)kill(-child, SIGKILL);
		}
	}
	if (!timed_out)
		(void)kill(-child, SIGTERM);
	result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
	result.term_signal = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
	if (timed_out)
		result.spawn_error = ETIMEDOUT;
	return result;

done:
	if (secret_pipe[0] >= 0)
		(void)close(secret_pipe[0]);
	if (secret_pipe[1] >= 0)
		(void)close(secret_pipe[1]);
	result.spawn_error = error;
	return result;
}

static void store_result_locked(struct settings_worker *worker,
				const struct settings_hardware_result *result)
{
	size_t position;

	if (worker->result_count == SETTINGS_RESULT_QUEUE) {
		worker->result_head =
			(worker->result_head + 1U) % SETTINGS_RESULT_QUEUE;
		--worker->result_count;
	}
	position = (worker->result_head + worker->result_count) %
		SETTINGS_RESULT_QUEUE;
	worker->results[position] = *result;
	++worker->result_count;
}

static void notify_result(void)
{
	{
		SDL_Event event = { .type = SDL_USEREVENT };

		event.user.code = SETTINGS_WORKER_EVENT_CODE;
		(void)SDL_PushEvent(&event);
	}
}

static void store_result(struct settings_worker *worker,
			 const struct settings_hardware_result *result)
{
	(void)SDL_LockMutex(worker->mutex);
	store_result_locked(worker, result);
	(void)SDL_UnlockMutex(worker->mutex);
	notify_result();
}

static int coalesced_state_index(enum settings_hardware_command command)
{
	switch (command) {
	case SETTINGS_HW_BRIGHTNESS: return 0;
	case SETTINGS_HW_JOYSTICK_RGB: return 1;
	case SETTINGS_HW_USB_DEBUG: return 2;
	case SETTINGS_HW_VOLUME: return 3;
	default: return -1;
	}
}

static int settings_worker_main(void *context)
{
	struct settings_worker *worker = context;

	for (;;) {
		struct settings_request request;
		struct settings_hardware_result result;

		(void)SDL_LockMutex(worker->mutex);
		while (!worker->stopping && worker->request_count == 0U)
			(void)SDL_CondWait(worker->condition, worker->mutex);
		if (worker->stopping) {
			(void)SDL_UnlockMutex(worker->mutex);
			break;
		}
		request = worker->requests[worker->request_head];
		secure_clear(&worker->requests[worker->request_head],
			sizeof(worker->requests[worker->request_head]));
		worker->request_head =
			(worker->request_head + 1U) % SETTINGS_WORK_QUEUE;
		--worker->request_count;
		worker->active_request = request;
		worker->request_active = true;
		(void)SDL_UnlockMutex(worker->mutex);
		result = execute_request(worker, &request);
		secure_clear(request.secret, sizeof(request.secret));
		(void)SDL_LockMutex(worker->mutex);
		worker->request_active = false;
		secure_clear(&worker->active_request,
			sizeof(worker->active_request));
		{
			int state_index = coalesced_state_index(request.command);

			if (state_index >= 0 && result.spawn_error == 0 &&
			    result.exit_code == 0 && result.term_signal == 0) {
				worker->last_applied_value[state_index] = request.value;
				worker->last_applied_valid[state_index] = true;
			}
		}
		(void)SDL_UnlockMutex(worker->mutex);
		store_result(worker, &result);
	}
	return 0;
}

static void remove_pending_requests(struct settings_worker *worker,
				    enum settings_hardware_command command)
{
	size_t write_offset = 0U;

	for (size_t read_offset = 0U; read_offset < worker->request_count;
	     ++read_offset) {
		size_t read_position =
			(worker->request_head + read_offset) % SETTINGS_WORK_QUEUE;
		size_t write_position;

		if (worker->requests[read_position].command == command)
			continue;
		write_position = (worker->request_head + write_offset) %
			SETTINGS_WORK_QUEUE;
		if (write_position != read_position)
			worker->requests[write_position] =
				worker->requests[read_position];
		++write_offset;
	}
	worker->request_count = write_offset;
}

static int enqueue_hardware(struct settings_state *settings,
			    enum settings_hardware_command command, int value)
{
	struct settings_worker *worker = settings->worker;
	size_t position;
	int state_index = coalesced_state_index(command);
	int previous_value = -1;
	int error = 0;
	uint64_t request_id;
	bool cached_result = false;

	settings->last_request_id = 0U;

	switch (command) {
	case SETTINGS_HW_BRIGHTNESS:
		previous_value = settings->preferences.backlight_percent;
		break;
	case SETTINGS_HW_JOYSTICK_RGB:
		previous_value = settings->preferences.joystick_rgb_brightness;
		break;
	case SETTINGS_HW_USB_DEBUG:
		previous_value = settings->preferences.usb_debug_enabled ? 1 : 0;
		break;
	case SETTINGS_HW_VOLUME:
		previous_value = settings->volume_target;
		break;
	default:
		break;
	}

	if (worker == NULL)
		return ENODEV;
	(void)SDL_LockMutex(worker->mutex);
	if (worker->stopping)
		error = ESHUTDOWN;
	else {
		/*
		 * Brightness, RGB, USB debug and volume are last-value state.  Keep at
		 * most the request that is executing plus the newest pending value.
		 * Edge-triggered commands (mute and shutdown) remain strict FIFO.
		 */
		if (state_index >= 0) {
			remove_pending_requests(worker, command);
			/*
			 * Do not duplicate an identical command that is still running; its
			 * result carries the same request id.  For a value already confirmed
			 * by this worker, publish a synthetic completion so the UI remains
			 * result-driven without spawning another helper.
			 */
			if (worker->request_count == 0U && worker->request_active &&
			    worker->active_request.command == command &&
			    worker->active_request.value == value) {
				settings->last_request_id =
					worker->active_request.request_id;
				(void)SDL_UnlockMutex(worker->mutex);
				return 0;
			}
			if (worker->request_count == 0U && !worker->request_active &&
			    worker->last_applied_valid[state_index] &&
			    worker->last_applied_value[state_index] == value) {
				struct settings_hardware_result cached = {
					.command = command,
					.value = value,
					.previous_value = previous_value,
					.exit_code = 0,
				};

				request_id = ++settings->next_request_id;
				if (request_id == 0U)
					request_id = ++settings->next_request_id;
				cached.request_id = request_id;
				settings->last_request_id = request_id;
				store_result_locked(worker, &cached);
				cached_result = true;
				goto unlock;
			}
		}
		if (worker->request_count == SETTINGS_WORK_QUEUE)
			error = EAGAIN;
		else {
			request_id = ++settings->next_request_id;
			if (request_id == 0U)
				request_id = ++settings->next_request_id;
			position = (worker->request_head + worker->request_count) %
				SETTINGS_WORK_QUEUE;
			worker->requests[position] = (struct settings_request) {
				.command = command,
				.request_id = request_id,
				.value = value,
				.previous_value = previous_value,
			};
			++worker->request_count;
			settings->last_request_id = request_id;
			(void)SDL_CondSignal(worker->condition);
		}
	}
unlock:
	(void)SDL_UnlockMutex(worker->mutex);
	if (cached_result)
		notify_result();
	return error;
}

static int enqueue_network(struct settings_state *settings,
			   enum settings_hardware_command command, int value,
			   const char *identifier, const char *secret)
{
	struct settings_worker *worker = settings->worker;
	size_t position;
	uint64_t request_id;
	int error = 0;

	settings->last_request_id = 0U;
	if (worker == NULL)
		return ENODEV;
	if (identifier != NULL && strlen(identifier) >=
	    sizeof(worker->requests[0].identifier))
		return EINVAL;
	if (secret != NULL && strlen(secret) >= sizeof(worker->requests[0].secret))
		return EINVAL;
	(void)SDL_LockMutex(worker->mutex);
	if (worker->stopping)
		error = ESHUTDOWN;
	else if (worker->request_count == SETTINGS_WORK_QUEUE)
		error = EAGAIN;
	else {
		request_id = ++settings->next_request_id;
		if (request_id == 0U)
			request_id = ++settings->next_request_id;
		position = (worker->request_head + worker->request_count) %
			SETTINGS_WORK_QUEUE;
		worker->requests[position] = (struct settings_request) {
			.command = command,
			.request_id = request_id,
			.value = value,
			.previous_value = -1,
		};
		if (identifier != NULL)
			(void)snprintf(worker->requests[position].identifier,
				sizeof(worker->requests[position].identifier), "%s",
				identifier);
		if (secret != NULL)
			(void)snprintf(worker->requests[position].secret,
				sizeof(worker->requests[position].secret), "%s", secret);
		++worker->request_count;
		settings->last_request_id = request_id;
		(void)SDL_CondSignal(worker->condition);
	}
	(void)SDL_UnlockMutex(worker->mutex);
	return error;
}

static bool valid_bssid(const char *value)
{
	if (value == NULL || strlen(value) != 17U)
		return false;
	for (size_t i = 0U; i < 17U; ++i) {
		if (i % 3U == 2U) {
			if (value[i] != ':')
				return false;
		} else if (!((value[i] >= '0' && value[i] <= '9') ||
			     (value[i] >= 'A' && value[i] <= 'F'))) {
			return false;
		}
	}
	return true;
}

static bool valid_uuid(const char *value)
{
	static const size_t hyphens[] = { 8U, 13U, 18U, 23U };

	if (value == NULL || strlen(value) != 36U)
		return false;
	for (size_t i = 0U; i < 36U; ++i) {
		bool hyphen = false;

		for (size_t j = 0U; j < sizeof(hyphens) / sizeof(hyphens[0]); ++j)
			hyphen = hyphen || i == hyphens[j];
		if ((hyphen && value[i] != '-') || (!hyphen &&
		    !((value[i] >= '0' && value[i] <= '9') ||
		      (value[i] >= 'a' && value[i] <= 'f'))))
			return false;
	}
	return value[14] >= '1' && value[14] <= '5' &&
		strchr("89ab", value[19]) != NULL;
}

static bool valid_password(const char *password)
{
	if (password == NULL || strlen(password) > 63U)
		return false;
	for (const unsigned char *cursor = (const unsigned char *)password;
	     *cursor != '\0'; ++cursor) {
		if (*cursor < 0x20U || *cursor > 0x7eU)
			return false;
	}
	return true;
}

static int apply_backlight(void *context, int percent, bool screen_off)
{
	struct settings_state *settings = context;

	if (percent < 0 || percent > 100)
		return EINVAL;
	return enqueue_hardware(settings, screen_off ? SETTINGS_HW_SCREEN_OFF :
				SETTINGS_HW_BRIGHTNESS, percent);
}

static int set_screen_off(void *context, bool screen_off)
{
	return enqueue_hardware(context, screen_off ? SETTINGS_HW_SCREEN_OFF :
				SETTINGS_HW_SCREEN_ON, 0);
}

static int apply_joystick_rgb(void *context,
			      const struct device_preferences *preferences)
{
	if (preferences == NULL || preferences->joystick_rgb_brightness < 0 ||
	    preferences->joystick_rgb_brightness > 100)
		return EINVAL;
	return enqueue_hardware(context, SETTINGS_HW_JOYSTICK_RGB,
				preferences->joystick_rgb_brightness);
}

static int set_usb_debug(void *context, bool enabled)
{
	return enqueue_hardware(context, SETTINGS_HW_USB_DEBUG,
				enabled ? 1 : 0);
}

static int set_volume(void *context, int percent)
{
	if (percent < 0 || percent > 100)
		return EINVAL;
	return enqueue_hardware(context, SETTINGS_HW_VOLUME, percent);
}

static int toggle_mute(void *context)
{
	return enqueue_hardware(context, SETTINGS_HW_MUTE_TOGGLE, 0);
}

static int request_shutdown(void *context)
{
	return enqueue_hardware(context, SETTINGS_HW_ORDERLY_SHUTDOWN, 0);
}

static int request_reboot_custom(void *context)
{
	return enqueue_hardware(context, SETTINGS_HW_REBOOT_CUSTOM, 0);
}

void settings_init(struct settings_state *settings)
{
	memset(settings, 0, sizeof(*settings));
	settings->preferences.backlight_percent = -1;
	settings->preferences.auto_screen_off_minutes = 0;
	settings->preferences.joystick_rgb_brightness = -1;
	settings->preferences.usb_debug_enabled = true;
	settings->preferences.screen_lock_enabled = true;
	settings->preferences.boot_target_custom = true;
	settings->usb_debug_available = -1;
	settings->volume_target = -1;
	for (int i = 0; i < SETTINGS_PENDING_COUNT; ++i) {
		settings->pending[i].value = -1;
		settings->pending[i].nav_index = -1;
	}
	settings->mute_pending.value = -1;
	settings->mute_pending.nav_index = -1;
	settings->reboot_pending.value = -1;
	settings->reboot_pending.nav_index = -1;
	for (int i = 0; i < DEBUG_LOG_CATEGORY_COUNT; ++i)
		settings->log_sizes[i] = -1;
}

int settings_backend_start(struct settings_state *settings,
			   const char *executable, const char *log_path)
{
	struct settings_worker *worker;
	int error;

	settings->backend.context = settings;
	settings->backend.apply_backlight = apply_backlight;
	settings->backend.set_screen_off = set_screen_off;
	settings->backend.apply_joystick_rgb = apply_joystick_rgb;
	settings->backend.set_usb_debug = set_usb_debug;
	settings->backend.set_volume = set_volume;
	settings->backend.toggle_mute = toggle_mute;
	settings->backend.request_shutdown = request_shutdown;
	settings->backend.request_reboot_custom = request_reboot_custom;
	settings->usb_debug_available = 0;
	error = launcher_executable_validate(executable);
	if (error != 0)
		return error;
	if (!valid_log_path(log_path))
		return EINVAL;
	worker = calloc(1, sizeof(*worker));
	if (worker == NULL)
		return ENOMEM;
	if (snprintf(worker->executable, sizeof(worker->executable), "%s",
		     executable) < 0 ||
	    strlen(executable) >= sizeof(worker->executable) ||
	    snprintf(worker->log_path, sizeof(worker->log_path), "%s",
		     log_path) < 0 || strlen(log_path) >= sizeof(worker->log_path)) {
		free(worker);
		return ENAMETOOLONG;
	}
	worker->mutex = SDL_CreateMutex();
	worker->condition = SDL_CreateCond();
	if (worker->mutex == NULL || worker->condition == NULL) {
		if (worker->condition != NULL)
			SDL_DestroyCond(worker->condition);
		if (worker->mutex != NULL)
			SDL_DestroyMutex(worker->mutex);
		free(worker);
		return ENOMEM;
	}
	worker->thread = SDL_CreateThread(settings_worker_main,
					  "ui-hardwarectl", worker);
	if (worker->thread == NULL) {
		SDL_DestroyCond(worker->condition);
		SDL_DestroyMutex(worker->mutex);
		free(worker);
		return EAGAIN;
	}
	settings->worker = worker;
	settings->usb_debug_available = 1;
	return 0;
}

void settings_backend_stop(struct settings_state *settings)
{
	struct settings_worker *worker = settings->worker;

	if (worker == NULL)
		return;
	(void)SDL_LockMutex(worker->mutex);
	worker->stopping = true;
	(void)SDL_CondSignal(worker->condition);
	(void)SDL_UnlockMutex(worker->mutex);
	SDL_WaitThread(worker->thread, NULL);
	SDL_DestroyCond(worker->condition);
	SDL_DestroyMutex(worker->mutex);
	secure_clear(worker, sizeof(*worker));
	free(worker);
	settings->worker = NULL;
	settings->usb_debug_available = 0;
}

int settings_backend_poll(struct settings_state *settings,
			  struct settings_hardware_result *result)
{
	struct settings_worker *worker = settings->worker;

	if (worker == NULL || result == NULL)
		return 0;
	(void)SDL_LockMutex(worker->mutex);
	if (worker->result_count == 0U) {
		(void)SDL_UnlockMutex(worker->mutex);
		return 0;
	}
	*result = worker->results[worker->result_head];
	worker->result_head = (worker->result_head + 1U) % SETTINGS_RESULT_QUEUE;
	--worker->result_count;
	(void)SDL_UnlockMutex(worker->mutex);
	return 1;
}

bool settings_backend_available(const struct settings_state *settings)
{
	return settings->worker != NULL;
}

int settings_request_network_status(struct settings_state *settings)
{
	return enqueue_network(settings, SETTINGS_HW_NETWORK_STATUS, 0, NULL, NULL);
}

int settings_request_wifi_recover(struct settings_state *settings)
{
	return enqueue_network(settings, SETTINGS_HW_WIFI_RECOVER, 0, NULL, NULL);
}

int settings_request_wifi_scan(struct settings_state *settings)
{
	return enqueue_network(settings, SETTINGS_HW_WIFI_SCAN, 0, NULL, NULL);
}

int settings_request_wifi_connect(struct settings_state *settings,
				  const char *bssid, const char *password)
{
	char normalized[18];

	if (bssid == NULL || strlen(bssid) != sizeof(normalized) - 1U ||
	    !valid_password(password))
		return EINVAL;
	for (size_t i = 0U; i < sizeof(normalized) - 1U; ++i) {
		unsigned char value = (unsigned char)bssid[i];

		normalized[i] = value >= 'a' && value <= 'f' ?
			(char)(value - 'a' + 'A') : (char)value;
	}
	normalized[sizeof(normalized) - 1U] = '\0';
	if (!valid_bssid(normalized))
		return EINVAL;
	return enqueue_network(settings, SETTINGS_HW_WIFI_CONNECT, 0,
		normalized, password);
}

int settings_request_wifi_disconnect(struct settings_state *settings)
{
	return enqueue_network(settings, SETTINGS_HW_WIFI_DISCONNECT, 0,
		NULL, NULL);
}

int settings_request_wifi_forget(struct settings_state *settings,
				 const char *uuid)
{
	if (!valid_uuid(uuid))
		return EINVAL;
	return enqueue_network(settings, SETTINGS_HW_WIFI_FORGET, 0, uuid, NULL);
}

int settings_request_hotspot_set(struct settings_state *settings, bool enabled)
{
	return enqueue_network(settings, SETTINGS_HW_HOTSPOT_SET,
		enabled ? 1 : 0, NULL, NULL);
}

static bool valid_category(enum debug_log_category category)
{
	return category >= DEBUG_LOG_UI && category < DEBUG_LOG_CATEGORY_COUNT;
}

int settings_export_log(struct settings_state *settings,
			enum debug_log_category category)
{
	if (!valid_category(category) || settings->backend.export_log == NULL)
		return -1;
	return settings->backend.export_log(settings->backend.context, category);
}

int settings_clear_log(struct settings_state *settings,
		       enum debug_log_category category, bool long_press_confirmed)
{
	if (!long_press_confirmed || !valid_category(category) ||
	    settings->backend.clear_log == NULL)
		return -1;
	return settings->backend.clear_log(settings->backend.context, category, true);
}
