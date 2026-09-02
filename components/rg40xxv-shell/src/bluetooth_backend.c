#define _GNU_SOURCE

#include "bluetooth_backend.h"

#include <SDL.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum {
	BLUETOOTH_REQUEST_QUEUE = 32,
	BLUETOOTH_CAPABILITY_MAX_BYTES = 4096,
	BLUETOOTH_HELPER_OUTPUT_MAX = 16 * 1024,
	BLUETOOTH_CHILD_TERM_MS = 250,
};

#ifdef RG40XXV_BLUETOOTH_BACKEND_TESTING
enum {
	BLUETOOTH_STATUS_TIMEOUT_MS = 1200,
	BLUETOOTH_SCAN_TIMEOUT_MS = 1200,
	BLUETOOTH_ACTION_TIMEOUT_MS = 1200,
	BLUETOOTH_PAIR_TIMEOUT_MS = 1200,
};
#else
enum {
	BLUETOOTH_STATUS_TIMEOUT_MS = 5000,
	BLUETOOTH_SCAN_TIMEOUT_MS = 15000,
	BLUETOOTH_ACTION_TIMEOUT_MS = 15000,
	BLUETOOTH_PAIR_TIMEOUT_MS = 120000,
};
#endif

#define BLUETOOTH_CAPABILITY_DEFAULT \
	"/opt/rg40xxv/bluetooth/runtime/admission.env"
#define BLUETOOTH_CAPABILITY_SCHEMA \
	"schema=rg40xxv-bluetooth-runtime-admission-v1"
#define BLUETOOTH_SNAPSHOT_HEADER "RG40XXV_BLUETOOTH_SNAPSHOT\t2"

struct bluetooth_request {
	enum bluetooth_backend_action action;
	uint64_t request_id;
	char address[BLUETOOTH_BACKEND_ADDRESS_SIZE];
	bool powered;
};

struct bluetooth_helper_result {
	char output[BLUETOOTH_HELPER_OUTPUT_MAX + 1U];
	size_t output_size;
	int error;
	int exit_code;
	int term_signal;
};

struct bluetooth_backend {
	SDL_Thread *thread;
	SDL_mutex *mutex;
	SDL_cond *condition;
	struct bluetooth_backend_snapshot published;
	struct bluetooth_backend_snapshot confirmed;
	struct bluetooth_request requests[BLUETOOTH_REQUEST_QUEUE];
	size_t request_head;
	size_t request_count;
	uint64_t next_request_id;
	char helper_path[PATH_MAX];
	pid_t child_pgid;
	bool running;
};

static char *const bluetooth_helper_environment[] = {
	(char *)"PATH=/usr/sbin:/usr/bin:/sbin:/bin",
	(char *)"LC_ALL=C",
	(char *)"TZ=Asia/Taipei",
	NULL,
};

static uint64_t monotonic_ms(void)
{
	struct timespec value;

	if (clock_gettime(CLOCK_MONOTONIC, &value) != 0)
		return 0U;
	return (uint64_t)value.tv_sec * 1000U +
		(uint64_t)value.tv_nsec / 1000000U;
}

static void wake_ui(void)
{
	SDL_Event event;

	memset(&event, 0, sizeof(event));
	event.type = SDL_USEREVENT;
	(void)SDL_PushEvent(&event);
}

static void snapshot_init(struct bluetooth_backend_snapshot *snapshot,
			  enum bluetooth_backend_gate gate,
			  enum bluetooth_backend_phase phase)
{
	memset(snapshot, 0, sizeof(*snapshot));
	snapshot->gate = gate;
	snapshot->phase = phase;
	snapshot->last_action = BLUETOOTH_ACTION_NONE;
	snapshot->helper_exit_code = -1;
}

static void publish_locked(struct bluetooth_backend *backend,
			   struct bluetooth_backend_snapshot *snapshot)
{
	snapshot->generation = backend->published.generation + 1U;
	if (snapshot->generation == 0U)
		snapshot->generation = 1U;
	backend->published = *snapshot;
}

static void publish_working(struct bluetooth_backend *backend,
			    const struct bluetooth_request *request)
{
	struct bluetooth_backend_snapshot snapshot;

	(void)SDL_LockMutex(backend->mutex);
	snapshot = backend->confirmed;
	snapshot.phase = BLUETOOTH_PHASE_WORKING;
	snapshot.last_action = request->action;
	snapshot.active_request_id = request->request_id;
	publish_locked(backend, &snapshot);
	(void)SDL_UnlockMutex(backend->mutex);
	wake_ui();
}

static void publish_success(struct bluetooth_backend *backend,
			    struct bluetooth_backend_snapshot *snapshot,
			    const struct bluetooth_request *request)
{
	snapshot->gate = BLUETOOTH_GATE_ADMITTED;
	snapshot->phase = BLUETOOTH_PHASE_READY;
	snapshot->last_action = request->action;
	snapshot->active_request_id = 0U;
	snapshot->completed_request_id = request->request_id;
	snapshot->last_error = 0;
	snapshot->helper_exit_code = 0;
	snapshot->helper_term_signal = 0;
	(void)SDL_LockMutex(backend->mutex);
	backend->confirmed = *snapshot;
	publish_locked(backend, snapshot);
	backend->confirmed.generation = backend->published.generation;
	(void)SDL_UnlockMutex(backend->mutex);
	wake_ui();
}

/*
 * The helper's "status" output only lists devices BlueZ still remembers
 * (paired/trusted/connected).  A discovered-but-unpaired device from the last
 * "scan" would therefore vanish from the menu on the next silent 5 s status
 * refresh, which read as "the scan finds nothing".  Carry unpaired discoveries
 * over from the confirmed snapshot until an explicit scan replaces the list or
 * the adapter is powered off.
 */
static void retain_discovered(struct bluetooth_backend *backend,
			      struct bluetooth_backend_snapshot *snapshot,
			      const struct bluetooth_request *request)
{
	struct bluetooth_backend_snapshot previous;

	if (request->action == BLUETOOTH_ACTION_SCAN ||
	    !snapshot->adapter_present || !snapshot->powered)
		return;
	(void)SDL_LockMutex(backend->mutex);
	previous = backend->confirmed;
	(void)SDL_UnlockMutex(backend->mutex);
	if (previous.gate != BLUETOOTH_GATE_ADMITTED || !previous.powered)
		return;
	for (size_t i = 0U; i < previous.device_count; ++i) {
		const struct bluetooth_device_snapshot *device =
			&previous.devices[i];
		bool present = false;

		if (device->paired || device->trusted || device->connected)
			continue;
		for (size_t j = 0U; j < snapshot->device_count; ++j) {
			if (strcmp(snapshot->devices[j].address,
				   device->address) == 0) {
				present = true;
				break;
			}
		}
		if (present ||
		    snapshot->device_count >= BLUETOOTH_BACKEND_MAX_DEVICES)
			continue;
		snapshot->devices[snapshot->device_count++] = *device;
	}
}

static void publish_failure(struct bluetooth_backend *backend,
			    const struct bluetooth_request *request,
			    const struct bluetooth_helper_result *result,
			    int error)
{
	struct bluetooth_backend_snapshot snapshot;

	(void)SDL_LockMutex(backend->mutex);
	snapshot = backend->confirmed;
	snapshot.phase = BLUETOOTH_PHASE_ERROR;
	snapshot.last_action = request->action;
	snapshot.active_request_id = 0U;
	snapshot.completed_request_id = request->request_id;
	snapshot.last_error = error != 0 ? error : EIO;
	snapshot.helper_exit_code = result->exit_code;
	snapshot.helper_term_signal = result->term_signal;
	backend->confirmed = snapshot;
	publish_locked(backend, &snapshot);
	backend->confirmed.generation = backend->published.generation;
	(void)SDL_UnlockMutex(backend->mutex);
	wake_ui();
}

static bool backend_stopping(struct bluetooth_backend *backend)
{
	bool stopping;

	(void)SDL_LockMutex(backend->mutex);
	stopping = !backend->running;
	(void)SDL_UnlockMutex(backend->mutex);
	return stopping;
}

static void set_child_pgid(struct bluetooth_backend *backend, pid_t pgid)
{
	(void)SDL_LockMutex(backend->mutex);
	backend->child_pgid = pgid;
	(void)SDL_UnlockMutex(backend->mutex);
}

static bool normalize_mac(const char *input,
			  char output[BLUETOOTH_BACKEND_ADDRESS_SIZE])
{
	if (input == NULL || strlen(input) != BLUETOOTH_BACKEND_ADDRESS_SIZE - 1U)
		return false;
	for (size_t i = 0U; i < BLUETOOTH_BACKEND_ADDRESS_SIZE - 1U; ++i) {
		if (i % 3U == 2U) {
			if (input[i] != ':')
				return false;
			output[i] = ':';
		} else {
			unsigned char value = (unsigned char)input[i];

			if (!isxdigit(value))
				return false;
			output[i] = (char)toupper(value);
		}
	}
	output[BLUETOOTH_BACKEND_ADDRESS_SIZE - 1U] = '\0';
	return true;
}

static bool utf8_text_valid(const unsigned char *text, size_t length)
{
	size_t offset = 0U;

	while (offset < length) {
		uint32_t codepoint;
		size_t extra;
		unsigned char first = text[offset++];

		if (first < 0x80U) {
			if (first < 0x20U || first == 0x7fU)
				return false;
			continue;
		}
		if (first >= 0xc2U && first <= 0xdfU) {
			codepoint = first & 0x1fU;
			extra = 1U;
		} else if (first >= 0xe0U && first <= 0xefU) {
			codepoint = first & 0x0fU;
			extra = 2U;
		} else if (first >= 0xf0U && first <= 0xf4U) {
			codepoint = first & 0x07U;
			extra = 3U;
		} else {
			return false;
		}
		if (offset + extra > length)
			return false;
		for (size_t i = 0U; i < extra; ++i) {
			unsigned char continuation = text[offset++];

			if ((continuation & 0xc0U) != 0x80U)
				return false;
			codepoint = (codepoint << 6) | (continuation & 0x3fU);
		}
		if ((extra == 2U && codepoint < 0x800U) ||
		    (extra == 3U && codepoint < 0x10000U) ||
		    (codepoint >= 0xd800U && codepoint <= 0xdfffU) ||
		    codepoint > 0x10ffffU)
			return false;
	}
	return true;
}

static int hex_value(unsigned char value)
{
	if (value >= '0' && value <= '9')
		return value - '0';
	if (value >= 'a' && value <= 'f')
		return value - 'a' + 10;
	if (value >= 'A' && value <= 'F')
		return value - 'A' + 10;
	return -1;
}

static bool decode_name(const char *encoded,
			char output[BLUETOOTH_BACKEND_NAME_SIZE])
{
	size_t input_length = strlen(encoded);
	size_t output_length = 0U;

	if (input_length == 0U || input_length >
	    (BLUETOOTH_BACKEND_NAME_SIZE - 1U) * 3U)
		return false;
	for (size_t i = 0U; i < input_length; ++i) {
		unsigned char value = (unsigned char)encoded[i];

		if (value == '%') {
			int high;
			int low;

			if (i + 2U >= input_length ||
			    (high = hex_value((unsigned char)encoded[i + 1U])) < 0 ||
			    (low = hex_value((unsigned char)encoded[i + 2U])) < 0)
				return false;
			value = (unsigned char)((high << 4) | low);
			i += 2U;
		}
		if (output_length + 1U >= BLUETOOTH_BACKEND_NAME_SIZE)
			return false;
		output[output_length++] = (char)value;
	}
	if (output_length == 0U ||
	    !utf8_text_valid((const unsigned char *)output, output_length))
		return false;
	output[output_length] = '\0';
	return true;
}

static int split_fields(char *line, char **fields, size_t capacity)
{
	size_t count = 0U;
	char *cursor = line;

	for (;;) {
		char *tab;

		if (count >= capacity)
			return -1;
		fields[count++] = cursor;
		tab = strchr(cursor, '\t');
		if (tab == NULL)
			break;
		*tab = '\0';
		cursor = tab + 1;
	}
	return (int)count;
}

static bool parse_bool(const char *text, bool *value)
{
	if (strcmp(text, "0") == 0) {
		*value = false;
		return true;
	}
	if (strcmp(text, "1") == 0) {
		*value = true;
		return true;
	}
	return false;
}

static bool parse_rssi(const char *text, int *value)
{
	char *end = NULL;
	long parsed;

	errno = 0;
	parsed = strtol(text, &end, 10);
	if (errno != 0 || end == text || *end != '\0' ||
	    !((parsed >= -127 && parsed <= 20) || parsed == 127))
		return false;
	*value = (int)parsed;
	return true;
}

static bool parse_kind(const char *text, enum bluetooth_device_kind *kind)
{
	static const struct {
		const char *name;
		enum bluetooth_device_kind kind;
	} kinds[] = {
		{ "unknown", BLUETOOTH_DEVICE_UNKNOWN },
		{ "controller", BLUETOOTH_DEVICE_CONTROLLER },
		{ "audio", BLUETOOTH_DEVICE_AUDIO },
		{ "keyboard", BLUETOOTH_DEVICE_KEYBOARD },
		{ "mouse", BLUETOOTH_DEVICE_MOUSE },
	};

	for (size_t i = 0U; i < sizeof(kinds) / sizeof(kinds[0]); ++i) {
		if (strcmp(text, kinds[i].name) == 0) {
			*kind = kinds[i].kind;
			return true;
		}
	}
	return false;
}

static int parse_snapshot(char *output, size_t output_size,
			  struct bluetooth_backend_snapshot *snapshot)
{
	char *cursor = output;
	size_t line_number = 0U;
	bool adapter_seen = false;

	if (output_size == 0U || output[output_size - 1U] != '\n' ||
	    memchr(output, '\0', output_size) != NULL ||
	    memchr(output, '\r', output_size) != NULL)
		return EPROTO;
	snapshot_init(snapshot, BLUETOOTH_GATE_ADMITTED,
		      BLUETOOTH_PHASE_READY);
	while (*cursor != '\0') {
		char *newline = strchr(cursor, '\n');
		char *fields[8];
		int count;

		if (newline == NULL || newline == cursor)
			return EPROTO;
		*newline = '\0';
		if (line_number == 0U) {
			if (strcmp(cursor, BLUETOOTH_SNAPSHOT_HEADER) != 0)
				return EPROTO;
		} else if (line_number == 1U) {
			count = split_fields(cursor, fields,
				sizeof(fields) / sizeof(fields[0]));
			if (count != 5 || strcmp(fields[0], "A") != 0 ||
			    !parse_bool(fields[1], &snapshot->adapter_present) ||
			    !parse_bool(fields[2], &snapshot->powered) ||
			    !parse_bool(fields[3], &snapshot->discovering) ||
			    (snapshot->adapter_present ?
			     !normalize_mac(fields[4], snapshot->adapter_address) :
			     strcmp(fields[4], "-") != 0))
				return EPROTO;
			adapter_seen = true;
		} else {
			struct bluetooth_device_snapshot *device;
			char normalized[BLUETOOTH_BACKEND_ADDRESS_SIZE];

			if (snapshot->device_count >= BLUETOOTH_BACKEND_MAX_DEVICES)
				return E2BIG;
			count = split_fields(cursor, fields,
				sizeof(fields) / sizeof(fields[0]));
			if (count != 8 || strcmp(fields[0], "D") != 0 ||
			    !normalize_mac(fields[1], normalized))
				return EPROTO;
			for (size_t i = 0U; i < snapshot->device_count; ++i) {
				if (strcmp(snapshot->devices[i].address, normalized) == 0)
					return EEXIST;
			}
			device = &snapshot->devices[snapshot->device_count];
			(void)snprintf(device->address, sizeof(device->address), "%s",
				       normalized);
			if (!decode_name(fields[2], device->name) ||
			    !parse_kind(fields[3], &device->kind) ||
			    !parse_bool(fields[4], &device->paired) ||
			    !parse_bool(fields[5], &device->trusted) ||
			    !parse_bool(fields[6], &device->connected) ||
			    !parse_rssi(fields[7], &device->rssi))
				return EPROTO;
			++snapshot->device_count;
		}
		++line_number;
		cursor = newline + 1;
	}
	if (!adapter_seen || line_number < 2U ||
	    (!snapshot->adapter_present &&
	     (snapshot->powered || snapshot->discovering ||
	      snapshot->device_count != 0U)) ||
	    (!snapshot->powered && snapshot->discovering))
		return EPROTO;
	return 0;
}

static enum bluetooth_backend_gate read_capability(int *error)
{
	const char *path = getenv("RG40XXV_BLUETOOTH_CAPABILITY");
	char content[BLUETOOTH_CAPABILITY_MAX_BYTES + 1U];
	struct stat status;
	ssize_t total = 0;
	bool schema_seen = false;
	bool status_seen = false;
	bool admitted = false;
	int fd;

	*error = 0;
	if (path == NULL || path[0] == '\0')
		path = BLUETOOTH_CAPABILITY_DEFAULT;
	if (path[0] != '/') {
		*error = EINVAL;
		return BLUETOOTH_GATE_REJECTED;
	}
	fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
	if (fd < 0) {
		if (errno == ENOENT)
			return BLUETOOTH_GATE_PENDING;
		*error = errno;
		return BLUETOOTH_GATE_REJECTED;
	}
	if (fstat(fd, &status) != 0) {
		*error = errno;
		(void)close(fd);
		return BLUETOOTH_GATE_REJECTED;
	}
	if (!S_ISREG(status.st_mode) || status.st_uid != geteuid() ||
	    status.st_nlink != 1 ||
	    (status.st_mode & 022) != 0 || status.st_size <= 0 ||
	    status.st_size > BLUETOOTH_CAPABILITY_MAX_BYTES) {
		*error = EACCES;
		(void)close(fd);
		return BLUETOOTH_GATE_REJECTED;
	}
	while (total < status.st_size) {
		ssize_t count = read(fd, content + total,
			(size_t)(status.st_size - total));

		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0) {
			*error = count < 0 ? errno : EIO;
			(void)close(fd);
			return BLUETOOTH_GATE_REJECTED;
		}
		total += count;
	}
	(void)close(fd);
	if (total <= 0 || content[total - 1] != '\n' ||
	    memchr(content, '\0', (size_t)total) != NULL ||
	    memchr(content, '\r', (size_t)total) != NULL) {
		*error = EPROTO;
		return BLUETOOTH_GATE_REJECTED;
	}
	content[total] = '\0';
	for (char *cursor = content; *cursor != '\0';) {
		char *newline = strchr(cursor, '\n');

		if (newline == NULL || newline == cursor) {
			*error = EPROTO;
			return BLUETOOTH_GATE_REJECTED;
		}
		*newline = '\0';
		if (strcmp(cursor, BLUETOOTH_CAPABILITY_SCHEMA) == 0) {
			if (schema_seen) {
				*error = EPROTO;
				return BLUETOOTH_GATE_REJECTED;
			}
			schema_seen = true;
		} else if (strcmp(cursor, "status=PASS") == 0 ||
			   strcmp(cursor, "status=PENDING") == 0) {
			if (status_seen) {
				*error = EPROTO;
				return BLUETOOTH_GATE_REJECTED;
			}
			status_seen = true;
			admitted = strcmp(cursor, "status=PASS") == 0;
		} else {
			*error = EPROTO;
			return BLUETOOTH_GATE_REJECTED;
		}
		cursor = newline + 1;
	}
	if (!schema_seen || !status_seen) {
		*error = EPROTO;
		return BLUETOOTH_GATE_REJECTED;
	}
	return admitted ? BLUETOOTH_GATE_ADMITTED : BLUETOOTH_GATE_PENDING;
}

static int validate_helper(const char *path)
{
	struct stat status;

	if (path == NULL || path[0] != '/' || strlen(path) >= PATH_MAX)
		return EINVAL;
	if (lstat(path, &status) != 0)
		return errno;
	if (!S_ISREG(status.st_mode) || S_ISLNK(status.st_mode) ||
	    status.st_uid != geteuid() || status.st_nlink != 1 ||
	    (status.st_mode & 022) != 0 ||
	    (status.st_mode & S_IXUSR) == 0 || access(path, X_OK) != 0)
		return EACCES;
	return 0;
}

static int configure_signals(posix_spawnattr_t *attributes)
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
	(void)sigaddset(&defaults, SIGQUIT);
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

static unsigned int request_timeout_ms(enum bluetooth_backend_action action)
{
	if (action == BLUETOOTH_ACTION_SCAN)
		return BLUETOOTH_SCAN_TIMEOUT_MS;
	if (action == BLUETOOTH_ACTION_PAIR)
		return BLUETOOTH_PAIR_TIMEOUT_MS;
	if (action == BLUETOOTH_ACTION_STATUS)
		return BLUETOOTH_STATUS_TIMEOUT_MS;
	return BLUETOOTH_ACTION_TIMEOUT_MS;
}

static void request_arguments(struct bluetooth_backend *backend,
			      const struct bluetooth_request *request,
			      char *arguments[4])
{
	arguments[0] = backend->helper_path;
	arguments[2] = NULL;
	arguments[3] = NULL;
	switch (request->action) {
	case BLUETOOTH_ACTION_STATUS:
		arguments[1] = (char *)"status";
		break;
	case BLUETOOTH_ACTION_SCAN:
		arguments[1] = (char *)"scan";
		break;
	case BLUETOOTH_ACTION_POWER:
		arguments[1] = (char *)"power";
		arguments[2] = (char *)(request->powered ? "on" : "off");
		break;
	case BLUETOOTH_ACTION_PAIR:
		arguments[1] = (char *)"pair";
		arguments[2] = (char *)request->address;
		break;
	case BLUETOOTH_ACTION_CONNECT:
		arguments[1] = (char *)"connect";
		arguments[2] = (char *)request->address;
		break;
	case BLUETOOTH_ACTION_DISCONNECT:
		arguments[1] = (char *)"disconnect";
		arguments[2] = (char *)request->address;
		break;
	case BLUETOOTH_ACTION_FORGET:
		arguments[1] = (char *)"forget";
		arguments[2] = (char *)request->address;
		break;
	case BLUETOOTH_ACTION_NONE:
		arguments[1] = NULL;
		break;
	}
}

static void decode_child_status(struct bluetooth_helper_result *result,
				int status)
{
	result->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
	result->term_signal = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
}

static void terminate_child(pid_t child, int *status)
{
	uint64_t deadline = monotonic_ms() + BLUETOOTH_CHILD_TERM_MS;
	struct timespec pause = { .tv_sec = 0, .tv_nsec = 10000000L };
	pid_t waited;

	(void)kill(-child, SIGTERM);
	for (;;) {
		do {
			waited = waitpid(child, status, WNOHANG);
		} while (waited < 0 && errno == EINTR);
		if (waited == child || (waited < 0 && errno == ECHILD))
			return;
		if (monotonic_ms() >= deadline)
			break;
		(void)nanosleep(&pause, NULL);
	}
	(void)kill(-child, SIGKILL);
	do {
		waited = waitpid(child, status, 0);
	} while (waited < 0 && errno == EINTR);
}

static void drain_output(int fd, struct bluetooth_helper_result *result)
{
	for (;;) {
		ssize_t count;

		if (result->output_size >= BLUETOOTH_HELPER_OUTPUT_MAX) {
			char overflow;

			count = read(fd, &overflow, 1U);
			if (count > 0)
				result->error = EFBIG;
			return;
		}
		count = read(fd, result->output + result->output_size,
			BLUETOOTH_HELPER_OUTPUT_MAX - result->output_size);
		if (count > 0) {
			result->output_size += (size_t)count;
			continue;
		}
		if (count < 0 && errno == EINTR)
			continue;
		if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
			result->error = errno;
		return;
	}
}

static struct bluetooth_helper_result execute_helper(
	struct bluetooth_backend *backend, const struct bluetooth_request *request)
{
	struct bluetooth_helper_result result = {
		.exit_code = -1,
	};
	posix_spawn_file_actions_t actions;
	posix_spawnattr_t attributes;
	char *arguments[4];
	uint64_t deadline;
	int descriptors[2] = { -1, -1 };
	int status = 0;
	int error;
	pid_t child = 0;
	bool actions_ready = false;
	bool attributes_ready = false;
	bool finished = false;

	request_arguments(backend, request, arguments);
	if (arguments[1] == NULL) {
		result.error = EINVAL;
		return result;
	}
	if (backend_stopping(backend)) {
		result.error = ECANCELED;
		return result;
	}
	if (pipe2(descriptors, O_CLOEXEC | O_NONBLOCK) != 0) {
		result.error = errno;
		return result;
	}
	error = posix_spawn_file_actions_init(&actions);
	if (error == 0)
		actions_ready = true;
	if (error == 0) {
		error = posix_spawnattr_init(&attributes);
		if (error == 0)
			attributes_ready = true;
	}
	if (error == 0)
		error = posix_spawn_file_actions_adddup2(&actions, descriptors[1],
			STDOUT_FILENO);
	if (error == 0)
		error = posix_spawn_file_actions_adddup2(&actions, descriptors[1],
			STDERR_FILENO);
	if (error == 0)
		error = posix_spawn_file_actions_addclose(&actions, descriptors[0]);
	if (error == 0)
		error = posix_spawn_file_actions_addclose(&actions, descriptors[1]);
	if (error == 0)
		error = configure_signals(&attributes);
	if (error == 0)
		error = posix_spawn(&child, backend->helper_path, &actions,
			&attributes, arguments, bluetooth_helper_environment);
	if (attributes_ready)
		(void)posix_spawnattr_destroy(&attributes);
	if (actions_ready)
		(void)posix_spawn_file_actions_destroy(&actions);
	(void)close(descriptors[1]);
	descriptors[1] = -1;
	if (error != 0) {
		result.error = error;
		(void)close(descriptors[0]);
		return result;
	}
	(void)setpgid(child, child);
	set_child_pgid(backend, child);
	deadline = monotonic_ms() + request_timeout_ms(request->action);
	while (!finished) {
		struct pollfd descriptor = {
			.fd = descriptors[0],
			.events = POLLIN | POLLHUP | POLLERR,
		};
		pid_t waited;

		(void)poll(&descriptor, 1, 25);
		drain_output(descriptors[0], &result);
		do {
			waited = waitpid(child, &status, WNOHANG);
		} while (waited < 0 && errno == EINTR);
		if (waited == child || (waited < 0 && errno == ECHILD)) {
			finished = true;
			break;
		}
		if (waited < 0) {
			result.error = errno;
			break;
		}
		if (result.error != 0 || backend_stopping(backend) ||
		    monotonic_ms() >= deadline) {
			if (result.error == 0)
				result.error = backend_stopping(backend) ? ECANCELED :
					ETIMEDOUT;
			break;
		}
	}
	if (!finished) {
		terminate_child(child, &status);
		finished = true;
	}
	drain_output(descriptors[0], &result);
	(void)close(descriptors[0]);
	set_child_pgid(backend, 0);
	if (finished)
		decode_child_status(&result, status);
	if (result.error == 0 && result.term_signal != 0)
		result.error = EINTR;
	if (result.error == 0 && result.exit_code != 0)
		result.error = EIO;
	result.output[result.output_size] = '\0';
	return result;
}

static int worker_main(void *context)
{
	struct bluetooth_backend *backend = context;

	(void)setpriority(PRIO_PROCESS, 0, 19);
	for (;;) {
		struct bluetooth_request request;
		struct bluetooth_helper_result result;
		struct bluetooth_backend_snapshot snapshot;
		int parse_error;

		memset(&request, 0, sizeof(request));
		(void)SDL_LockMutex(backend->mutex);
		while (backend->running && backend->request_count == 0U)
			(void)SDL_CondWait(backend->condition, backend->mutex);
		if (!backend->running) {
			(void)SDL_UnlockMutex(backend->mutex);
			break;
		}
		request = backend->requests[backend->request_head];
		backend->request_head = (backend->request_head + 1U) %
			BLUETOOTH_REQUEST_QUEUE;
		--backend->request_count;
		(void)SDL_UnlockMutex(backend->mutex);

		publish_working(backend, &request);
		result = execute_helper(backend, &request);
		if (backend_stopping(backend))
			break;
		if (result.error != 0) {
			publish_failure(backend, &request, &result, result.error);
			continue;
		}
		parse_error = parse_snapshot(result.output, result.output_size,
			&snapshot);
		if (parse_error != 0)
			publish_failure(backend, &request, &result, parse_error);
		else {
			retain_discovered(backend, &snapshot, &request);
			publish_success(backend, &snapshot, &request);
		}
	}
	return 0;
}

static int enqueue_request(struct bluetooth_backend *backend,
			   enum bluetooth_backend_action action,
			   bool powered, const char *address,
			   uint64_t *request_id)
{
	struct bluetooth_request request;
	size_t tail;
	int error = 0;

	if (request_id == NULL)
		return EINVAL;
	*request_id = 0U;
	if (backend == NULL || action <= BLUETOOTH_ACTION_NONE ||
	    action > BLUETOOTH_ACTION_FORGET)
		return EINVAL;
	memset(&request, 0, sizeof(request));
	request.action = action;
	request.powered = powered;
	if (address != NULL && !normalize_mac(address, request.address))
		return EINVAL;
	if (action >= BLUETOOTH_ACTION_PAIR && request.address[0] == '\0')
		return EINVAL;
	(void)SDL_LockMutex(backend->mutex);
	if (!backend->running || backend->thread == NULL ||
	    backend->published.gate != BLUETOOTH_GATE_ADMITTED) {
		error = ENODEV;
	} else if (backend->request_count >= BLUETOOTH_REQUEST_QUEUE) {
		error = EAGAIN;
	} else {
		++backend->next_request_id;
		if (backend->next_request_id == 0U)
			++backend->next_request_id;
		request.request_id = backend->next_request_id;
		tail = (backend->request_head + backend->request_count) %
			BLUETOOTH_REQUEST_QUEUE;
		backend->requests[tail] = request;
		++backend->request_count;
		*request_id = request.request_id;
		(void)SDL_CondSignal(backend->condition);
	}
	(void)SDL_UnlockMutex(backend->mutex);
	return error;
}

int bluetooth_backend_start(struct bluetooth_backend **output,
			    const char *helper_path)
{
	struct bluetooth_backend *backend;
	enum bluetooth_backend_gate gate;
	int capability_error;
	int helper_error;

	if (output == NULL || helper_path == NULL || helper_path[0] != '/' ||
	    strlen(helper_path) >= PATH_MAX)
		return EINVAL;
	*output = NULL;
	backend = calloc(1U, sizeof(*backend));
	if (backend == NULL)
		return ENOMEM;
	backend->mutex = SDL_CreateMutex();
	backend->condition = SDL_CreateCond();
	if (backend->mutex == NULL || backend->condition == NULL) {
		bluetooth_backend_stop(&backend);
		return ENOMEM;
	}
	(void)snprintf(backend->helper_path, sizeof(backend->helper_path), "%s",
		       helper_path);
	gate = read_capability(&capability_error);
	if (gate == BLUETOOTH_GATE_PENDING) {
		snapshot_init(&backend->published, gate, BLUETOOTH_PHASE_PENDING);
		backend->published.generation = 1U;
		backend->confirmed = backend->published;
		*output = backend;
		return 0;
	}
	if (gate == BLUETOOTH_GATE_REJECTED) {
		snapshot_init(&backend->published, gate, BLUETOOTH_PHASE_ERROR);
		backend->published.generation = 1U;
		backend->published.last_error = capability_error != 0 ?
			capability_error : EACCES;
		backend->confirmed = backend->published;
		*output = backend;
		return 0;
	}
	helper_error = validate_helper(helper_path);
	if (helper_error != 0) {
		snapshot_init(&backend->published, BLUETOOTH_GATE_ADMITTED,
			BLUETOOTH_PHASE_ERROR);
		backend->published.generation = 1U;
		backend->published.last_error = helper_error;
		backend->confirmed = backend->published;
		*output = backend;
		return 0;
	}
	snapshot_init(&backend->published, BLUETOOTH_GATE_ADMITTED,
		BLUETOOTH_PHASE_LOADING);
	backend->published.generation = 1U;
	backend->confirmed = backend->published;
	backend->running = true;
	backend->next_request_id = 1U;
	backend->requests[0] = (struct bluetooth_request) {
		.action = BLUETOOTH_ACTION_STATUS,
		.request_id = 1U,
	};
	backend->request_count = 1U;
	backend->thread = SDL_CreateThread(worker_main, "bluetooth-backend", backend);
	if (backend->thread == NULL) {
		backend->running = false;
		backend->request_count = 0U;
		backend->published.phase = BLUETOOTH_PHASE_ERROR;
		backend->published.last_error = EAGAIN;
		backend->confirmed = backend->published;
	}
	*output = backend;
	return 0;
}

bool bluetooth_backend_available(struct bluetooth_backend *backend)
{
	bool available;

	if (backend == NULL || backend->mutex == NULL)
		return false;
	(void)SDL_LockMutex(backend->mutex);
	available = backend->running && backend->thread != NULL &&
		backend->published.gate == BLUETOOTH_GATE_ADMITTED;
	(void)SDL_UnlockMutex(backend->mutex);
	return available;
}

int bluetooth_backend_request_status(struct bluetooth_backend *backend,
				     uint64_t *request_id)
{
	return enqueue_request(backend, BLUETOOTH_ACTION_STATUS, false, NULL,
		request_id);
}

int bluetooth_backend_request_scan(struct bluetooth_backend *backend,
				   uint64_t *request_id)
{
	return enqueue_request(backend, BLUETOOTH_ACTION_SCAN, false, NULL,
		request_id);
}

int bluetooth_backend_request_power(struct bluetooth_backend *backend,
				    bool powered, uint64_t *request_id)
{
	return enqueue_request(backend, BLUETOOTH_ACTION_POWER, powered, NULL,
		request_id);
}

int bluetooth_backend_request_pair(struct bluetooth_backend *backend,
				   const char *address, uint64_t *request_id)
{
	return enqueue_request(backend, BLUETOOTH_ACTION_PAIR, false, address,
		request_id);
}

int bluetooth_backend_request_connect(struct bluetooth_backend *backend,
				      const char *address,
				      uint64_t *request_id)
{
	return enqueue_request(backend, BLUETOOTH_ACTION_CONNECT, false, address,
		request_id);
}

int bluetooth_backend_request_disconnect(struct bluetooth_backend *backend,
					 const char *address,
					 uint64_t *request_id)
{
	return enqueue_request(backend, BLUETOOTH_ACTION_DISCONNECT, false, address,
		request_id);
}

int bluetooth_backend_request_forget(struct bluetooth_backend *backend,
				     const char *address, uint64_t *request_id)
{
	return enqueue_request(backend, BLUETOOTH_ACTION_FORGET, false, address,
		request_id);
}

int bluetooth_backend_poll(struct bluetooth_backend *backend,
			   struct bluetooth_backend_snapshot *snapshot,
			   uint64_t *last_generation)
{
	int result = 0;

	if (backend == NULL || snapshot == NULL || last_generation == NULL)
		return -EINVAL;
	(void)SDL_LockMutex(backend->mutex);
	if (backend->published.generation != *last_generation) {
		*snapshot = backend->published;
		*last_generation = backend->published.generation;
		result = 1;
	}
	(void)SDL_UnlockMutex(backend->mutex);
	return result;
}

void bluetooth_backend_stop(struct bluetooth_backend **pointer)
{
	struct bluetooth_backend *backend;
	pid_t child = 0;

	if (pointer == NULL || *pointer == NULL)
		return;
	backend = *pointer;
	if (backend->mutex != NULL) {
		(void)SDL_LockMutex(backend->mutex);
		backend->running = false;
		backend->request_count = 0U;
		child = backend->child_pgid;
		if (backend->condition != NULL)
			(void)SDL_CondSignal(backend->condition);
		(void)SDL_UnlockMutex(backend->mutex);
	}
	if (child > 1)
		(void)kill(-child, SIGTERM);
	if (backend->thread != NULL)
		SDL_WaitThread(backend->thread, NULL);
	if (backend->condition != NULL)
		SDL_DestroyCond(backend->condition);
	if (backend->mutex != NULL)
		SDL_DestroyMutex(backend->mutex);
	memset(backend, 0, sizeof(*backend));
	free(backend);
	*pointer = NULL;
}
