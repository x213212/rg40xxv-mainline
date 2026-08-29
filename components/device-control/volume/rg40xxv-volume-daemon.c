#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <sound/asound.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define MAX_CONTROL_ELEMENTS 256U
#define MAX_INPUT_ENTRIES 64U
#define MAX_CLIENTS 8U
#define REQUEST_SIZE 64U

#define PRODUCTION_SOCKET "/run/rg40xxv-volume/control.sock"
#define PRODUCTION_RUN_DIR "/run/rg40xxv-volume"
#define PRODUCTION_UI_DIR "/run/rg40xxv-ui"
#define PRODUCTION_INPUT_DIR "/dev/input"
#define PRODUCTION_MIXER "/dev/snd/controlC0"
#define PRODUCTION_PERSISTENT_DIR "/mnt/data/rg40xxv/state/audio"
#define PERSISTENT_STATE_NAME "volume.v1"
#define PERSISTENT_STATE_HEADER "RG40XXV_VOLUME_V1\n"
#define DEFAULT_VOLUME_PERCENT 60
#define INPUT_DEVICE_NAME "gpio-keys-volume"

enum control_index {
	CONTROL_DAC_VOLUME,
	CONTROL_LINE_VOLUME,
	CONTROL_LINE_SWITCH,
	CONTROL_DAC_SWITCH,
	CONTROL_DAC_REVERSED_SWITCH,
	CONTROL_LINE_SOURCE_ROUTE,
	CONTROL_SPEAKER_SWITCH,
	CONTROL_COUNT,
};

struct control_definition {
	const char *alsa_name;
	const char *fixture_name;
	snd_ctl_elem_type_t type;
	unsigned int minimum_count;
	long minimum;
	long maximum;
};

static const struct control_definition control_definitions[CONTROL_COUNT] = {
	[CONTROL_DAC_VOLUME] = {
		"DAC Playback Volume", "dac-volume", SNDRV_CTL_ELEM_TYPE_INTEGER,
		1U, 0L, 63L,
	},
	[CONTROL_LINE_VOLUME] = {
		"Line Out Playback Volume", "lineout-volume",
		SNDRV_CTL_ELEM_TYPE_INTEGER, 1U, 0L, 31L,
	},
	[CONTROL_LINE_SWITCH] = {
		"Line Out Playback Switch", "lineout-switch",
		SNDRV_CTL_ELEM_TYPE_BOOLEAN, 1U, 0L, 1L,
	},
	[CONTROL_DAC_SWITCH] = {
		"DAC Playback Switch", "dac-switch", SNDRV_CTL_ELEM_TYPE_BOOLEAN,
		1U, 0L, 1L,
	},
	[CONTROL_DAC_REVERSED_SWITCH] = {
		"DAC Reversed Playback Switch", "dac-reversed-switch",
		SNDRV_CTL_ELEM_TYPE_BOOLEAN, 1U, 0L, 1L,
	},
	[CONTROL_LINE_SOURCE_ROUTE] = {
		"Line Out Source Playback Route", "lineout-source-route",
		SNDRV_CTL_ELEM_TYPE_ENUMERATED, 1U, 0L, 0L,
	},
	[CONTROL_SPEAKER_SWITCH] = {
		"Speaker Switch", "speaker-switch", SNDRV_CTL_ELEM_TYPE_BOOLEAN,
		1U, 0L, 1L,
	},
};

struct control_handle {
	struct snd_ctl_elem_id id;
	unsigned int count;
	long minimum;
	long maximum;
#ifdef RG40XXV_VOLUME_TESTING
	int fixture_fd;
#endif
};

struct control_snapshot {
	long values[CONTROL_COUNT][128];
};

struct daemon_state {
	int mixer_fd;
	int input_fd;
	int listen_fd;
	int ui_dir_fd;
	int persistent_dir_fd;
	int volume_percent;
	int muted;
	int last_nonzero;
	bool initialized;
	unsigned int marker_sequence;
	unsigned int persistent_sequence;
	struct control_handle controls[CONTROL_COUNT];
	char socket_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
#ifdef RG40XXV_VOLUME_TESTING
	char test_root[PATH_MAX];
	int fixture_mixer_dir_fd;
#endif
};

static void log_message(const char *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	(void)fprintf(stderr, "rg40xxv-volume-daemon: ");
	(void)vfprintf(stderr, format, arguments);
	(void)fputc('\n', stderr);
	va_end(arguments);
}

static int write_all(int fd, const char *buffer, size_t length)
{
	while (length > 0U) {
		ssize_t written = write(fd, buffer, length);

		if (written < 0 && errno == EINTR)
			continue;
		if (written <= 0)
			return -1;
		buffer += (size_t)written;
		length -= (size_t)written;
	}
	return 0;
}

#ifdef RG40XXV_VOLUME_TESTING
static int read_small_file_at(int directory_fd, const char *name,
			      char *buffer, size_t capacity)
{
	struct stat status;
	ssize_t length;
	int fd;

	if (capacity < 2U) {
		errno = EINVAL;
		return -1;
	}
	fd = openat(directory_fd, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return -1;
	if (fstat(fd, &status) != 0 || !S_ISREG(status.st_mode) ||
	    (status.st_mode & 0022) != 0 || status.st_size < 0 ||
	    (uintmax_t)status.st_size >= capacity) {
		close(fd);
		errno = EINVAL;
		return -1;
	}
	length = read(fd, buffer, capacity - 1U);
	if (length < 0) {
		int saved_errno = errno;

		close(fd);
		errno = saved_errno;
		return -1;
	}
	buffer[length] = '\0';
	close(fd);
	return 0;
}
#endif

static bool event_entry_name(const char *name)
{
	const unsigned char *cursor;

	if (strncmp(name, "event", 5U) != 0 || name[5] == '\0')
		return false;
	for (cursor = (const unsigned char *)name + 5U; *cursor != '\0'; ++cursor)
		if (*cursor < (unsigned char)'0' || *cursor > (unsigned char)'9')
			return false;
	return true;
}

#ifndef RG40XXV_VOLUME_TESTING
static bool test_bit_set(const unsigned long *bits, unsigned int bit)
{
	const unsigned int bits_per_word = (unsigned int)(sizeof(unsigned long) * 8U);

	return (bits[bit / bits_per_word] &
		(1UL << (bit % bits_per_word))) != 0UL;
}

static int validate_production_input(int fd)
{
	unsigned long event_bits[(EV_MAX / (sizeof(unsigned long) * 8U)) + 1U];
	unsigned long key_bits[(KEY_MAX / (sizeof(unsigned long) * 8U)) + 1U];
	struct stat status;
	char name[256];

	memset(event_bits, 0, sizeof(event_bits));
	memset(key_bits, 0, sizeof(key_bits));
	memset(name, 0, sizeof(name));
	if (fstat(fd, &status) != 0 || !S_ISCHR(status.st_mode) ||
	    ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0 ||
	    memchr(name, '\0', sizeof(name)) == NULL ||
	    strcmp(name, INPUT_DEVICE_NAME) != 0 ||
	    ioctl(fd, EVIOCGBIT(0, sizeof(event_bits)), event_bits) < 0 ||
	    !test_bit_set(event_bits, EV_KEY) ||
	    ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) < 0 ||
	    !test_bit_set(key_bits, KEY_VOLUMEUP) ||
	    !test_bit_set(key_bits, KEY_VOLUMEDOWN)) {
		errno = ENODEV;
		return -1;
	}
	return 0;
}
#endif

#ifdef RG40XXV_VOLUME_TESTING
static int validate_test_root(struct daemon_state *state, const char *argument)
{
	static const char prefix[] = "/tmp/rg40xxv-device-control-test.volume.";
	struct stat status;
	char resolved[PATH_MAX];

	if (argument == NULL || strncmp(argument, prefix, sizeof(prefix) - 1U) != 0 ||
	    strchr(argument + sizeof(prefix) - 1U, '/') != NULL ||
	    realpath(argument, resolved) == NULL || strcmp(argument, resolved) != 0 ||
	    lstat(resolved, &status) != 0 || !S_ISDIR(status.st_mode) ||
	    S_ISLNK(status.st_mode) || (status.st_mode & 0022) != 0 ||
	    strlen(resolved) >= sizeof(state->test_root)) {
		errno = EINVAL;
		return -1;
	}
	(void)strcpy(state->test_root, resolved);
	return 0;
}

static int validate_fixture_input(int input_directory_fd, const char *entry,
				  int fd)
{
	struct stat status;
	char metadata_name[NAME_MAX + 16U];
	char contents[128];

	if (fstat(fd, &status) != 0 || !S_ISFIFO(status.st_mode)) {
		errno = EINVAL;
		return -1;
	}
	if (snprintf(metadata_name, sizeof(metadata_name), "%s.name", entry) < 0 ||
	    read_small_file_at(input_directory_fd, metadata_name, contents,
			       sizeof(contents)) != 0 ||
	    strcmp(contents, INPUT_DEVICE_NAME "\n") != 0)
		return -1;
	if (snprintf(metadata_name, sizeof(metadata_name), "%s.keys", entry) < 0 ||
	    read_small_file_at(input_directory_fd, metadata_name, contents,
			       sizeof(contents)) != 0 ||
	    strcmp(contents, "KEY_VOLUMEUP=1\nKEY_VOLUMEDOWN=1\n") != 0)
		return -1;
	return 0;
}
#endif

static int open_volume_input(struct daemon_state *state)
{
	DIR *directory;
	struct dirent *entry;
	int directory_fd;
	int selected = -1;
	unsigned int entries_seen = 0U;
	char input_path[PATH_MAX];

#ifdef RG40XXV_VOLUME_TESTING
	if (snprintf(input_path, sizeof(input_path), "%s/dev/input", state->test_root) < 0)
		return -1;
#else
	(void)state;
	(void)snprintf(input_path, sizeof(input_path), "%s", PRODUCTION_INPUT_DIR);
#endif
	directory_fd = open(input_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	if (directory_fd < 0)
		return -1;
	directory = fdopendir(dup(directory_fd));
	if (directory == NULL) {
		close(directory_fd);
		return -1;
	}
	for (;;) {
		int candidate;

		errno = 0;
		entry = readdir(directory);
		if (entry == NULL) {
			if (errno != 0)
				goto fail;
			break;
		}

		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
			continue;
		if (++entries_seen > MAX_INPUT_ENTRIES) {
			errno = E2BIG;
			goto fail;
		}
		if (!event_entry_name(entry->d_name))
			continue;
#ifdef RG40XXV_VOLUME_TESTING
		candidate = openat(directory_fd, entry->d_name,
				   O_RDWR | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
		if (candidate >= 0 &&
		    validate_fixture_input(directory_fd, entry->d_name, candidate) != 0) {
			close(candidate);
			candidate = -1;
		}
#else
		candidate = openat(directory_fd, entry->d_name,
				   O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
		if (candidate >= 0 && validate_production_input(candidate) != 0) {
			close(candidate);
			candidate = -1;
		}
#endif
		if (candidate < 0)
			continue;
		if (selected >= 0) {
			close(candidate);
			errno = EEXIST;
			goto fail;
		}
		selected = candidate;
	}
	closedir(directory);
	close(directory_fd);
	if (selected < 0)
		errno = ENODEV;
	return selected;

fail:
	{
		int saved_errno = errno;

		if (selected >= 0)
			close(selected);
		closedir(directory);
		close(directory_fd);
		errno = saved_errno;
		return -1;
	}
}

#ifndef RG40XXV_VOLUME_TESTING
static int validate_control_info(const struct control_definition *definition,
				 const struct snd_ctl_elem_info *info)
{
	if (info->type != definition->type ||
	    info->count < definition->minimum_count || info->count > 128U ||
	    (info->access & SNDRV_CTL_ELEM_ACCESS_READWRITE) !=
		SNDRV_CTL_ELEM_ACCESS_READWRITE)
		return -1;
	if (info->type == SNDRV_CTL_ELEM_TYPE_INTEGER &&
	    (info->value.integer.min != definition->minimum ||
	     info->value.integer.max != definition->maximum))
		return -1;
	if (info->type == SNDRV_CTL_ELEM_TYPE_ENUMERATED &&
	    info->value.enumerated.items < 1U)
		return -1;
	return 0;
}

static int open_production_mixer(struct daemon_state *state)
{
	struct snd_ctl_card_info card_info;
	struct snd_ctl_elem_list list;
	struct snd_ctl_elem_id *ids = NULL;
	bool found[CONTROL_COUNT];
	int fd = -1;

	memset(found, 0, sizeof(found));
	fd = open(PRODUCTION_MIXER, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return -1;
	{
		struct stat status;

		if (fstat(fd, &status) != 0 || !S_ISCHR(status.st_mode)) {
			errno = ENODEV;
			goto fail;
		}
	}
	memset(&card_info, 0, sizeof(card_info));
	if (ioctl(fd, SNDRV_CTL_IOCTL_CARD_INFO, &card_info) != 0 ||
	    memchr(card_info.id, '\0', sizeof(card_info.id)) == NULL ||
	    memchr(card_info.driver, '\0', sizeof(card_info.driver)) == NULL ||
	    memchr(card_info.name, '\0', sizeof(card_info.name)) == NULL ||
	    (strcmp((const char *)card_info.id, "Codec") != 0 &&
	     strcmp((const char *)card_info.driver, "sun4i-codec") != 0 &&
	     strstr((const char *)card_info.name, "H616 Audio Codec") == NULL)) {
		errno = ENODEV;
		goto fail;
	}
	memset(&list, 0, sizeof(list));
	if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_LIST, &list) != 0 || list.count == 0U ||
	    list.count > MAX_CONTROL_ELEMENTS) {
		errno = EINVAL;
		goto fail;
	}
	ids = calloc(list.count, sizeof(*ids));
	if (ids == NULL)
		goto fail;
	list.space = list.count;
	list.pids = ids;
	if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_LIST, &list) != 0 ||
	    list.used > list.count)
		goto fail;
	for (unsigned int item = 0U; item < list.used; ++item) {
		if (memchr(ids[item].name, '\0', sizeof(ids[item].name)) == NULL)
			continue;
		for (unsigned int index = 0U; index < CONTROL_COUNT; ++index) {
			struct snd_ctl_elem_info info;

			if (strcmp((const char *)ids[item].name,
				   control_definitions[index].alsa_name) != 0)
				continue;
			if (found[index]) {
				errno = EEXIST;
				goto fail;
			}
			memset(&info, 0, sizeof(info));
			info.id = ids[item];
			if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &info) != 0 ||
			    validate_control_info(&control_definitions[index], &info) != 0) {
				errno = EINVAL;
				goto fail;
			}
			state->controls[index].id = ids[item];
			state->controls[index].count = info.count;
			if (info.type == SNDRV_CTL_ELEM_TYPE_INTEGER) {
				state->controls[index].minimum = info.value.integer.min;
				state->controls[index].maximum = info.value.integer.max;
			} else {
				state->controls[index].minimum = 0L;
				state->controls[index].maximum =
					info.type == SNDRV_CTL_ELEM_TYPE_ENUMERATED ?
					(long)info.value.enumerated.items - 1L : 1L;
			}
			found[index] = true;
		}
	}
	for (unsigned int index = 0U; index < CONTROL_COUNT; ++index) {
		if (!found[index]) {
			errno = ENOENT;
			goto fail;
		}
	}
	free(ids);
	return fd;

fail:
	{
		int saved_errno = errno;

		free(ids);
		if (fd >= 0)
			close(fd);
		errno = saved_errno;
		return -1;
	}
}
#else
static int parse_fixture_values(int fd, long *values, unsigned int count)
{
	char buffer[512];
	char *cursor = buffer;
	ssize_t length;

	if (lseek(fd, 0, SEEK_SET) < 0)
		return -1;
	length = read(fd, buffer, sizeof(buffer) - 1U);
	if (length <= 0 || (size_t)length >= sizeof(buffer) - 1U)
		return -1;
	buffer[length] = '\0';
	for (unsigned int index = 0U; index < count; ++index) {
		char *end;
		long value;

		errno = 0;
		value = strtol(cursor, &end, 10);
		if (errno != 0 || end == cursor)
			return -1;
		values[index] = value;
		cursor = end;
		while (*cursor == ' ' || *cursor == '\t')
			++cursor;
		if (index + 1U < count && (*cursor == '\n' || *cursor == '\0'))
			return -1;
	}
	if (*cursor == '\n')
		++cursor;
	return *cursor == '\0' ? 0 : -1;
}

static int open_fixture_mixer(struct daemon_state *state)
{
	struct stat status;
	char path[PATH_MAX];

	if (snprintf(path, sizeof(path), "%s/mixer", state->test_root) < 0)
		return -1;
	state->fixture_mixer_dir_fd =
		open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	if (state->fixture_mixer_dir_fd < 0)
		return -1;
	for (unsigned int index = 0U; index < CONTROL_COUNT; ++index) {
		long values[128];
		int fd = openat(state->fixture_mixer_dir_fd,
				control_definitions[index].fixture_name,
				O_RDWR | O_CLOEXEC | O_NOFOLLOW);

		if (fd < 0 || fstat(fd, &status) != 0 || !S_ISREG(status.st_mode) ||
		    (status.st_mode & 0022) != 0) {
			if (fd >= 0)
				close(fd);
			errno = EINVAL;
			return -1;
		}
		state->controls[index].count =
			index == CONTROL_LINE_SWITCH || index == CONTROL_DAC_SWITCH ||
			index == CONTROL_DAC_REVERSED_SWITCH ? 2U : 1U;
		state->controls[index].minimum = control_definitions[index].minimum;
		state->controls[index].maximum = control_definitions[index].maximum;
		state->controls[index].fixture_fd = fd;
		if (parse_fixture_values(fd, values, state->controls[index].count) != 0) {
			errno = EINVAL;
			return -1;
		}
		for (unsigned int item = 0U; item < state->controls[index].count; ++item)
			if (values[item] < state->controls[index].minimum ||
			    values[item] > state->controls[index].maximum) {
				errno = ERANGE;
				return -1;
			}
	}
	return open("/dev/null", O_RDWR | O_CLOEXEC);
}
#endif

static int mixer_read_control(struct daemon_state *state, unsigned int index,
			      long values[128])
{
	struct control_handle *control = &state->controls[index];

#ifdef RG40XXV_VOLUME_TESTING
	return parse_fixture_values(control->fixture_fd, values, control->count);
#else
	struct snd_ctl_elem_value value;

	memset(&value, 0, sizeof(value));
	value.id = control->id;
	if (ioctl(state->mixer_fd, SNDRV_CTL_IOCTL_ELEM_READ, &value) != 0)
		return -1;
	for (unsigned int item = 0U; item < control->count; ++item) {
		if (control_definitions[index].type == SNDRV_CTL_ELEM_TYPE_ENUMERATED)
			values[item] = (long)value.value.enumerated.item[item];
		else
			values[item] = value.value.integer.value[item];
	}
	return 0;
#endif
}

#ifdef RG40XXV_VOLUME_TESTING
static bool fixture_should_fail(struct daemon_state *state, unsigned int index)
{
	char contents[128];
	int fd = openat(state->fixture_mixer_dir_fd, "fail-control",
			O_RDONLY | O_CLOEXEC | O_NOFOLLOW);

	if (fd < 0)
		return false;
	close(fd);
	if (read_small_file_at(state->fixture_mixer_dir_fd, "fail-control", contents,
			       sizeof(contents)) != 0)
		return true;
	if (strcmp(contents, control_definitions[index].fixture_name) != 0) {
		char expected[96];

		(void)snprintf(expected, sizeof(expected), "%s\n",
			       control_definitions[index].fixture_name);
		if (strcmp(contents, expected) != 0)
			return false;
	}
	(void)unlinkat(state->fixture_mixer_dir_fd, "fail-control", 0);
	errno = EIO;
	return true;
}
#endif

static int mixer_write_control(struct daemon_state *state, unsigned int index,
			       const long values[128])
{
	struct control_handle *control = &state->controls[index];

	for (unsigned int item = 0U; item < control->count; ++item)
		if (values[item] < control->minimum || values[item] > control->maximum) {
			errno = ERANGE;
			return -1;
		}
#ifdef RG40XXV_VOLUME_TESTING
	{
		char buffer[512];
		size_t used = 0U;

		if (fixture_should_fail(state, index))
			return -1;
		for (unsigned int item = 0U; item < control->count; ++item) {
			int length = snprintf(buffer + used, sizeof(buffer) - used,
					      item == 0U ? "%ld" : " %ld", values[item]);

			if (length < 0 || (size_t)length >= sizeof(buffer) - used)
				return -1;
			used += (size_t)length;
		}
		if (used + 1U >= sizeof(buffer))
			return -1;
		buffer[used++] = '\n';
		if (lseek(control->fixture_fd, 0, SEEK_SET) < 0 ||
		    ftruncate(control->fixture_fd, 0) != 0 ||
		    write_all(control->fixture_fd, buffer, used) != 0 ||
		    fsync(control->fixture_fd) != 0)
			return -1;
		return 0;
	}
#else
	{
		struct snd_ctl_elem_value value;

		memset(&value, 0, sizeof(value));
		value.id = control->id;
		for (unsigned int item = 0U; item < control->count; ++item) {
			if (control_definitions[index].type == SNDRV_CTL_ELEM_TYPE_ENUMERATED)
				value.value.enumerated.item[item] = (unsigned int)values[item];
			else
				value.value.integer.value[item] = values[item];
		}
		return ioctl(state->mixer_fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &value);
	}
#endif
}

static int snapshot_controls(struct daemon_state *state,
			     struct control_snapshot *snapshot)
{
	for (unsigned int index = 0U; index < CONTROL_COUNT; ++index)
		if (mixer_read_control(state, index, snapshot->values[index]) != 0)
			return -1;
	return 0;
}

static int restore_controls(struct daemon_state *state,
			    const struct control_snapshot *snapshot)
{
	int result = 0;

	for (unsigned int reverse = CONTROL_COUNT; reverse > 0U; --reverse)
		if (mixer_write_control(state, reverse - 1U,
					snapshot->values[reverse - 1U]) != 0)
			result = -1;
	return result;
}

static int percent_to_line_raw(int percent)
{
	int raw;

	if (percent <= 0)
		return 0;
	raw = (percent * 31 + 50) / 100;
	return raw < 2 ? 2 : raw;
}

static void fill_values(const struct control_handle *control,
			long values[128], long desired)
{
	for (unsigned int item = 0U; item < control->count; ++item)
		values[item] = desired;
}

static int write_and_verify(struct daemon_state *state, unsigned int index,
			    long desired)
{
	long values[128];
	long readback[128];

	fill_values(&state->controls[index], values, desired);
	if (mixer_write_control(state, index, values) != 0 ||
	    mixer_read_control(state, index, readback) != 0)
		return -1;
	for (unsigned int item = 0U; item < state->controls[index].count; ++item)
		if (readback[item] != desired) {
			errno = EIO;
			return -1;
		}
	return 0;
}

static int apply_controls(struct daemon_state *state, int volume_percent,
			  int muted)
{
	static const unsigned int mute_order[] = {
		CONTROL_SPEAKER_SWITCH, CONTROL_LINE_SWITCH, CONTROL_LINE_VOLUME,
		CONTROL_DAC_VOLUME, CONTROL_DAC_SWITCH,
		CONTROL_DAC_REVERSED_SWITCH, CONTROL_LINE_SOURCE_ROUTE,
	};
	static const unsigned int unmute_order[] = {
		CONTROL_DAC_VOLUME, CONTROL_LINE_VOLUME, CONTROL_DAC_SWITCH,
		CONTROL_DAC_REVERSED_SWITCH, CONTROL_LINE_SOURCE_ROUTE,
		CONTROL_LINE_SWITCH, CONTROL_SPEAKER_SWITCH,
	};
	const unsigned int *order = muted ? mute_order : unmute_order;
	const size_t order_count = muted ? ARRAY_SIZE(mute_order) : ARRAY_SIZE(unmute_order);
	long desired[CONTROL_COUNT] = {
		[CONTROL_DAC_VOLUME] = 63L,
		[CONTROL_LINE_VOLUME] = percent_to_line_raw(volume_percent),
		[CONTROL_LINE_SWITCH] = muted ? 0L : 1L,
		[CONTROL_DAC_SWITCH] = 1L,
		[CONTROL_DAC_REVERSED_SWITCH] = 0L,
		[CONTROL_LINE_SOURCE_ROUTE] = 0L,
		[CONTROL_SPEAKER_SWITCH] = muted ? 0L : 1L,
	};

	for (size_t item = 0U; item < order_count; ++item)
		if (write_and_verify(state, order[item], desired[order[item]]) != 0)
			return -1;
	return 0;
}

static int ensure_directory(const char *path, mode_t mode)
{
	struct stat status;

	if (mkdir(path, mode) != 0 && errno != EEXIST)
		return -1;
	if (lstat(path, &status) != 0 || !S_ISDIR(status.st_mode) ||
	    S_ISLNK(status.st_mode) || status.st_uid != geteuid() ||
	    (status.st_mode & 0022) != 0) {
		errno = EACCES;
		return -1;
	}
	return 0;
}

enum persistent_load_result {
	PERSISTENT_LOAD_VALID,
	PERSISTENT_LOAD_MISSING,
	PERSISTENT_LOAD_INVALID,
};

static int parse_persistent_state(const char *contents, size_t length,
				  int *volume_percent, int *muted,
				  int *last_nonzero)
{
	char canonical[128];
	int consumed = -1;
	int parsed_length;
	int volume;
	int mute;
	int last;

	if (length == 0U || length >= sizeof(canonical) ||
	    sscanf(contents,
		   PERSISTENT_STATE_HEADER
		   "volume_percent=%d\nmuted=%d\nlast_nonzero=%d\n%n",
		   &volume, &mute, &last, &consumed) != 3 ||
	    consumed < 0 || (size_t)consumed != length || volume < 0 ||
	    volume > 100 || (mute != 0 && mute != 1) || last < 1 ||
	    last > 100 || (volume == 0 && mute != 1) ||
	    (volume > 0 && last != volume)) {
		errno = EINVAL;
		return -1;
	}
	parsed_length = snprintf(canonical, sizeof(canonical),
				 PERSISTENT_STATE_HEADER
				 "volume_percent=%d\nmuted=%d\nlast_nonzero=%d\n",
				 volume, mute, last);
	if (parsed_length < 0 || (size_t)parsed_length != length ||
	    memcmp(canonical, contents, length) != 0) {
		errno = EINVAL;
		return -1;
	}
	*volume_percent = volume;
	*muted = mute;
	*last_nonzero = last;
	return 0;
}

static enum persistent_load_result
load_persistent_state(struct daemon_state *state, int *volume_percent,
			      int *muted, int *last_nonzero)
{
	char contents[128];
	struct stat status;
	ssize_t length;
	ssize_t offset = 0;
	int fd;

	fd = openat(state->persistent_dir_fd, PERSISTENT_STATE_NAME,
		    O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0) {
		if (errno == ENOENT)
			return PERSISTENT_LOAD_MISSING;
		return PERSISTENT_LOAD_INVALID;
	}
	if (fstat(fd, &status) != 0 || !S_ISREG(status.st_mode) ||
	    status.st_uid != geteuid() || (status.st_mode & 07777) != 0600 ||
	    status.st_size <= 0 || (uintmax_t)status.st_size >= sizeof(contents)) {
		close(fd);
		errno = EINVAL;
		return PERSISTENT_LOAD_INVALID;
	}
	while (offset < status.st_size) {
		length = read(fd, contents + offset, (size_t)(status.st_size - offset));
		if (length < 0 && errno == EINTR)
			continue;
		if (length <= 0) {
			close(fd);
			errno = EIO;
			return PERSISTENT_LOAD_INVALID;
		}
		offset += length;
	}
	contents[offset] = '\0';
	if (close(fd) != 0 ||
	    parse_persistent_state(contents, (size_t)offset, volume_percent,
				   muted, last_nonzero) != 0)
		return PERSISTENT_LOAD_INVALID;
	return PERSISTENT_LOAD_VALID;
}

static int persist_state(struct daemon_state *state, int volume_percent,
			 int muted, int last_nonzero)
{
	char contents[128];
	char temporary[64];
	struct stat status;
	int length;
	int fd = -1;
	bool renamed = false;

#ifdef RG40XXV_VOLUME_TESTING
	if (faccessat(state->persistent_dir_fd, ".inject-write-failure", F_OK,
		      AT_SYMLINK_NOFOLLOW) == 0) {
		errno = EIO;
		return -1;
	}
#endif
	length = snprintf(contents, sizeof(contents),
			  PERSISTENT_STATE_HEADER
			  "volume_percent=%d\nmuted=%d\nlast_nonzero=%d\n",
			  volume_percent, muted, last_nonzero);
	if (length < 0 || (size_t)length >= sizeof(contents)) {
		errno = EOVERFLOW;
		return -1;
	}
	for (unsigned int attempt = 0U; attempt < 16U; ++attempt) {
		(void)snprintf(temporary, sizeof(temporary), ".volume.v1.%ld.%u.%u",
			       (long)getpid(), state->persistent_sequence++, attempt);
		fd = openat(state->persistent_dir_fd, temporary,
			    O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
			    0600);
		if (fd >= 0)
			break;
		if (errno != EEXIST)
			return -1;
	}
	if (fd < 0)
		return -1;
	if (fstat(fd, &status) != 0 || !S_ISREG(status.st_mode) ||
	    status.st_uid != geteuid() || fchmod(fd, 0600) != 0 ||
	    write_all(fd, contents, (size_t)length) != 0 || fsync(fd) != 0) {
		int saved_errno = errno;

		(void)close(fd);
		(void)unlinkat(state->persistent_dir_fd, temporary, 0);
		errno = saved_errno;
		return -1;
	}
	if (close(fd) != 0) {
		int saved_errno = errno;

		(void)unlinkat(state->persistent_dir_fd, temporary, 0);
		errno = saved_errno;
		return -1;
	}
	fd = -1;
	if (renameat(state->persistent_dir_fd, temporary,
		     state->persistent_dir_fd, PERSISTENT_STATE_NAME) != 0)
		goto fail;
	renamed = true;
	if (fsync(state->persistent_dir_fd) != 0)
		goto fail;
	return 0;

fail:
	{
		int saved_errno = errno;

		if (!renamed)
			(void)unlinkat(state->persistent_dir_fd, temporary, 0);
		errno = saved_errno;
		return -1;
	}
}

static int prepare_persistent_directory(struct daemon_state *state)
{
	char path[PATH_MAX];
	struct stat status;

#ifdef RG40XXV_VOLUME_TESTING
	if (snprintf(path, sizeof(path), "%s/mnt/data/rg40xxv/state/audio",
		     state->test_root) < 0)
		return -1;
#else
	(void)snprintf(path, sizeof(path), "%s", PRODUCTION_PERSISTENT_DIR);
#endif
	if (ensure_directory(path, 0700) != 0)
		return -1;
	state->persistent_dir_fd =
		open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	if (state->persistent_dir_fd < 0 ||
	    fstat(state->persistent_dir_fd, &status) != 0 ||
	    !S_ISDIR(status.st_mode) || status.st_uid != geteuid() ||
	    (status.st_mode & 07777) != 0700) {
		if (state->persistent_dir_fd >= 0) {
			close(state->persistent_dir_fd);
			state->persistent_dir_fd = -1;
		}
		errno = EACCES;
		return -1;
	}
	return 0;
}

static int publish_state(struct daemon_state *state, int volume_percent, int muted)
{
	char contents[64];
	char temporary[64];
	int length;
	int fd = -1;

	length = snprintf(contents, sizeof(contents),
			  "volume_percent=%d\nmuted=%d\n", volume_percent, muted);
	if (length < 0 || (size_t)length >= sizeof(contents)) {
		errno = EOVERFLOW;
		return -1;
	}
	for (unsigned int attempt = 0U; attempt < 16U; ++attempt) {
		(void)snprintf(temporary, sizeof(temporary), ".alsa-volume.%ld.%u.%u",
			       (long)getpid(), state->marker_sequence++, attempt);
		fd = openat(state->ui_dir_fd, temporary,
			    O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0644);
		if (fd >= 0)
			break;
		if (errno != EEXIST)
			return -1;
	}
	if (fd < 0)
		return -1;
	if (fchmod(fd, 0644) != 0 || write_all(fd, contents, (size_t)length) != 0 ||
	    fsync(fd) != 0 || close(fd) != 0) {
		int saved_errno = errno;

		if (fd >= 0)
			(void)close(fd);
		(void)unlinkat(state->ui_dir_fd, temporary, 0);
		errno = saved_errno;
		return -1;
	}
	fd = -1;
	if (renameat(state->ui_dir_fd, temporary, state->ui_dir_fd,
		     "alsa-volume") != 0 || fsync(state->ui_dir_fd) != 0) {
		int saved_errno = errno;

		(void)unlinkat(state->ui_dir_fd, temporary, 0);
		errno = saved_errno;
		return -1;
	}
	return 0;
}

static int commit_state(struct daemon_state *state, int volume_percent, int muted)
{
	struct control_snapshot snapshot;
	const int previous_volume = state->volume_percent;
	const int previous_muted = state->muted;
	const int previous_last_nonzero = state->last_nonzero;
	const bool was_initialized = state->initialized;
	int next_last_nonzero;
	bool marker_published = false;
	bool persistence_attempted = false;

	if (volume_percent < 0 || volume_percent > 100 || (muted != 0 && muted != 1)) {
		errno = EINVAL;
		return -1;
	}
	if (volume_percent == 0)
		muted = 1;
	next_last_nonzero = volume_percent > 0 ? volume_percent : state->last_nonzero;
	if (next_last_nonzero < 1 || next_last_nonzero > 100)
		next_last_nonzero = DEFAULT_VOLUME_PERCENT;
	if (snapshot_controls(state, &snapshot) != 0)
		return -1;
	if (apply_controls(state, volume_percent, muted) != 0)
		goto rollback;
	if (publish_state(state, volume_percent, muted) != 0)
		goto rollback;
	marker_published = true;
	persistence_attempted = true;
	if (persist_state(state, volume_percent, muted, next_last_nonzero) != 0)
		goto rollback;
	state->volume_percent = volume_percent;
	state->muted = muted;
	state->last_nonzero = next_last_nonzero;
	state->initialized = true;
	return 0;

rollback:
	{
		int saved_errno = errno;

		if (restore_controls(state, &snapshot) != 0)
			log_message("ALSA rollback failed after transaction error");
		if (marker_published) {
			if (was_initialized) {
				if (publish_state(state, previous_volume,
						  previous_muted) != 0)
					log_message("runtime marker rollback failed");
			} else {
				(void)unlinkat(state->ui_dir_fd, "alsa-volume", 0);
				(void)fsync(state->ui_dir_fd);
			}
		}
		if (persistence_attempted && was_initialized &&
		    persist_state(state, previous_volume, previous_muted,
				  previous_last_nonzero) != 0)
			log_message("persistent state rollback failed");
		errno = saved_errno;
		return -1;
	}
}

static int initialize_audio_state(struct daemon_state *state)
{
	int percent = DEFAULT_VOLUME_PERCENT;
	int muted = 0;
	int last_nonzero = DEFAULT_VOLUME_PERCENT;
	enum persistent_load_result load_result =
		load_persistent_state(state, &percent, &muted, &last_nonzero);

	if (load_result == PERSISTENT_LOAD_MISSING)
		log_message("persistent volume missing; using default=%d",
			    DEFAULT_VOLUME_PERCENT);
	else if (load_result == PERSISTENT_LOAD_INVALID)
		log_message("persistent volume invalid; using safe default=%d",
			    DEFAULT_VOLUME_PERCENT);
	state->last_nonzero = last_nonzero;
	return commit_state(state, percent, muted);
}

static int prepare_runtime(struct daemon_state *state)
{
	char run_dir[PATH_MAX];
	char ui_dir[PATH_MAX];
	struct stat socket_status;
	struct sockaddr_un address;

#ifdef RG40XXV_VOLUME_TESTING
	if (snprintf(run_dir, sizeof(run_dir), "%s/run/rg40xxv-volume",
		     state->test_root) < 0 ||
	    snprintf(ui_dir, sizeof(ui_dir), "%s/run/rg40xxv-ui",
		     state->test_root) < 0 ||
	    snprintf(state->socket_path, sizeof(state->socket_path), "%s/control.sock",
		     run_dir) < 0)
		return -1;
#else
	(void)snprintf(run_dir, sizeof(run_dir), "%s", PRODUCTION_RUN_DIR);
	(void)snprintf(ui_dir, sizeof(ui_dir), "%s", PRODUCTION_UI_DIR);
	(void)snprintf(state->socket_path, sizeof(state->socket_path), "%s",
		       PRODUCTION_SOCKET);
#endif
	if (ensure_directory(run_dir, 0750) != 0 || ensure_directory(ui_dir, 0755) != 0)
		return -1;
	if (prepare_persistent_directory(state) != 0)
		return -1;
	state->ui_dir_fd = open(ui_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	if (state->ui_dir_fd < 0)
		return -1;
	if (lstat(state->socket_path, &socket_status) == 0) {
		if (!S_ISSOCK(socket_status.st_mode) || socket_status.st_uid != geteuid()) {
			errno = EEXIST;
			return -1;
		}
		if (unlink(state->socket_path) != 0)
			return -1;
	} else if (errno != ENOENT) {
		return -1;
	}
	state->listen_fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC,
				 0);
	if (state->listen_fd < 0)
		return -1;
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	(void)strcpy(address.sun_path, state->socket_path);
	if (bind(state->listen_fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
	    chmod(state->socket_path, 0600) != 0 || listen(state->listen_fd, 8) != 0)
		return -1;
	return 0;
}

static int parse_set_request(const char *request, int *percent)
{
	const char *cursor;
	unsigned int value = 0U;

	if (strncmp(request, "set ", 4U) != 0)
		return -1;
	cursor = request + 4U;
	if (*cursor == '\0')
		return -1;
	for (; *cursor != '\0'; ++cursor) {
		if (*cursor < '0' || *cursor > '9')
			return -1;
		value = value * 10U + (unsigned int)(*cursor - '0');
		if (value > 100U)
			return -1;
	}
	*percent = (int)value;
	return 0;
}

static int process_command(struct daemon_state *state, const char *request)
{
	int volume = state->volume_percent;
	int muted = state->muted;

	if (strcmp(request, "get") == 0)
		return 0;
	if (strcmp(request, "up") == 0) {
		volume = volume > 95 ? 100 : volume + 5;
		muted = volume == 0;
	} else if (strcmp(request, "down") == 0) {
		volume = volume < 5 ? 0 : volume - 5;
		muted = volume == 0;
	} else if (strcmp(request, "mute-toggle") == 0) {
		if (muted) {
			if (volume == 0)
				volume = state->last_nonzero;
			muted = 0;
		} else {
			muted = 1;
		}
	} else if (parse_set_request(request, &volume) == 0) {
		muted = volume == 0;
	} else {
		errno = EINVAL;
		return -1;
	}
	return commit_state(state, volume, muted);
}

static void handle_client(struct daemon_state *state, int client_fd)
{
	struct ucred credentials;
	socklen_t credentials_length = sizeof(credentials);
	char request[REQUEST_SIZE];
	char response[128];
	ssize_t length;
	int response_length;

	memset(&credentials, 0, sizeof(credentials));
	if (getsockopt(client_fd, SOL_SOCKET, SO_PEERCRED, &credentials,
		       &credentials_length) != 0 ||
	    credentials_length != sizeof(credentials) || credentials.uid != 0) {
		(void)send(client_fd, "ERR permission-denied\n", 22U, MSG_NOSIGNAL);
		return;
	}
	length = recv(client_fd, request, sizeof(request) - 1U, MSG_TRUNC);
	if (length <= 0 || (size_t)length >= sizeof(request)) {
		(void)send(client_fd, "ERR invalid-command\n", 20U, MSG_NOSIGNAL);
		return;
	}
	request[length] = '\0';
	for (ssize_t index = 0; index < length; ++index) {
		unsigned char byte = (unsigned char)request[index];

		if (byte < 0x20U || byte > 0x7eU) {
			(void)send(client_fd, "ERR invalid-command\n", 20U, MSG_NOSIGNAL);
			return;
		}
	}
	if (process_command(state, request) != 0) {
		(void)send(client_fd,
			   errno == EINVAL ? "ERR invalid-command\n" :
			   "ERR hardware-transaction\n",
			   errno == EINVAL ? 20U : 25U, MSG_NOSIGNAL);
		return;
	}
	response_length = snprintf(response, sizeof(response),
				   "OK volume_percent=%d muted=%d\n",
				   state->volume_percent, state->muted);
	if (response_length > 0 && (size_t)response_length < sizeof(response))
		(void)send(client_fd, response, (size_t)response_length, MSG_NOSIGNAL);
}

static int handle_input(struct daemon_state *state)
{
	struct input_event events[16];
	ssize_t length;

	for (;;) {
		length = read(state->input_fd, events, sizeof(events));
		if (length < 0 && errno == EINTR)
			continue;
		if (length < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return 0;
		if (length <= 0 || (size_t)length % sizeof(events[0]) != 0U) {
			errno = ENODEV;
			return -1;
		}
		for (size_t item = 0U; item < (size_t)length / sizeof(events[0]); ++item) {
			int result = 0;

			if (events[item].type != EV_KEY ||
			    (events[item].value != 1 && events[item].value != 2))
				continue;
			if (events[item].code == KEY_VOLUMEUP)
				result = process_command(state, "up");
			else if (events[item].code == KEY_VOLUMEDOWN)
				result = process_command(state, "down");
			else
				continue;
			if (result != 0)
				log_message("volume key transaction failed: %s", strerror(errno));
		}
	}
}

static int run_event_loop(struct daemon_state *state)
{
	int clients[MAX_CLIENTS];

	for (unsigned int index = 0U; index < MAX_CLIENTS; ++index)
		clients[index] = -1;
	for (;;) {
		struct pollfd poll_fds[2U + MAX_CLIENTS];
		int result;

		poll_fds[0].fd = state->input_fd;
		poll_fds[0].events = POLLIN | POLLERR | POLLHUP;
		poll_fds[0].revents = 0;
		poll_fds[1].fd = state->listen_fd;
		poll_fds[1].events = POLLIN | POLLERR | POLLHUP;
		poll_fds[1].revents = 0;
		for (unsigned int index = 0U; index < MAX_CLIENTS; ++index) {
			poll_fds[2U + index].fd = clients[index];
			poll_fds[2U + index].events = POLLIN | POLLERR | POLLHUP;
			poll_fds[2U + index].revents = 0;
		}
		result = poll(poll_fds, ARRAY_SIZE(poll_fds), -1);
		if (result < 0 && errno == EINTR)
			continue;
		if (result < 0)
			return -1;
		if ((poll_fds[0].revents & POLLIN) != 0 && handle_input(state) != 0)
			return -1;
		if ((poll_fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
			errno = ENODEV;
			return -1;
		}
		if ((poll_fds[1].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
			errno = EIO;
			return -1;
		}
		if ((poll_fds[1].revents & POLLIN) != 0) {
			for (;;) {
				int client = accept4(state->listen_fd, NULL, NULL,
						     SOCK_NONBLOCK | SOCK_CLOEXEC);
				bool stored = false;

				if (client < 0 && errno == EINTR)
					continue;
				if (client < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
					break;
				if (client < 0)
					return -1;
				for (unsigned int index = 0U; index < MAX_CLIENTS; ++index)
					if (clients[index] < 0) {
						clients[index] = client;
						stored = true;
						break;
					}
				if (!stored) {
					(void)send(client, "ERR busy\n", 9U, MSG_NOSIGNAL);
					close(client);
				}
			}
		}
		for (unsigned int index = 0U; index < MAX_CLIENTS; ++index) {
			short revents = poll_fds[2U + index].revents;

			if (clients[index] < 0 || revents == 0)
				continue;
			if ((revents & POLLIN) != 0)
				handle_client(state, clients[index]);
			close(clients[index]);
			clients[index] = -1;
		}
	}
}

static void close_state(struct daemon_state *state)
{
	if (state->listen_fd >= 0)
		close(state->listen_fd);
	if (state->socket_path[0] != '\0') {
		struct stat status;

		if (lstat(state->socket_path, &status) == 0 && S_ISSOCK(status.st_mode) &&
		    status.st_uid == geteuid())
			(void)unlink(state->socket_path);
	}
	if (state->ui_dir_fd >= 0)
		close(state->ui_dir_fd);
	if (state->persistent_dir_fd >= 0)
		close(state->persistent_dir_fd);
	if (state->input_fd >= 0)
		close(state->input_fd);
	if (state->mixer_fd >= 0)
		close(state->mixer_fd);
#ifdef RG40XXV_VOLUME_TESTING
	for (unsigned int index = 0U; index < CONTROL_COUNT; ++index)
		if (state->controls[index].fixture_fd >= 0)
			close(state->controls[index].fixture_fd);
	if (state->fixture_mixer_dir_fd >= 0)
		close(state->fixture_mixer_dir_fd);
#endif
}

int main(int argc, char **argv)
{
	struct daemon_state state;
	int result = EXIT_FAILURE;

	memset(&state, 0, sizeof(state));
	state.mixer_fd = -1;
	state.input_fd = -1;
	state.listen_fd = -1;
	state.ui_dir_fd = -1;
	state.persistent_dir_fd = -1;
#ifdef RG40XXV_VOLUME_TESTING
	state.fixture_mixer_dir_fd = -1;
	for (unsigned int index = 0U; index < CONTROL_COUNT; ++index)
		state.controls[index].fixture_fd = -1;
	if (argc != 3 || strcmp(argv[1], "--test-root") != 0 ||
	    validate_test_root(&state, argv[2]) != 0) {
		log_message("testing requires a canonical /tmp fixture root");
		return EXIT_FAILURE;
	}
#else
	if (argc != 1) {
		log_message("this daemon accepts no command-line arguments");
		return EXIT_FAILURE;
	}
	(void)argv;
#endif
	(void)signal(SIGPIPE, SIG_IGN);
	state.input_fd = open_volume_input(&state);
	if (state.input_fd < 0) {
		log_message("validated %s input not available: %s", INPUT_DEVICE_NAME,
			    strerror(errno));
		goto out;
	}
#ifdef RG40XXV_VOLUME_TESTING
	state.mixer_fd = open_fixture_mixer(&state);
#else
	state.mixer_fd = open_production_mixer(&state);
#endif
	if (state.mixer_fd < 0) {
		log_message("validated ALSA card0 Codec controls not available: %s",
			    strerror(errno));
		goto out;
	}
	if (prepare_runtime(&state) != 0) {
		log_message("secure runtime setup failed: %s", strerror(errno));
		goto out;
	}
	if (initialize_audio_state(&state) != 0) {
		log_message("initial ALSA transaction failed: %s", strerror(errno));
		goto out;
	}
	log_message("ready input=%s card=0 control=Line-Out step=5 wait=poll",
		    INPUT_DEVICE_NAME);
	if (run_event_loop(&state) != 0) {
		log_message("event loop stopped: %s", strerror(errno));
		goto out;
	}
	result = EXIT_SUCCESS;
out:
	close_state(&state);
	return result;
}
