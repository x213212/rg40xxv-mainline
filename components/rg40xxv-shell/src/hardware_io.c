#define _POSIX_C_SOURCE 200809L

#include "hardware_internal.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int hw_path(char *out, size_t size, const struct hardware_backend *backend,
	    const char *absolute_tail)
{
	int length;

	if (out == NULL || backend == NULL || absolute_tail == NULL ||
	    absolute_tail[0] != '/')
		return -1;
	if (strcmp(backend->fixture_root, "/") == 0)
		length = snprintf(out, size, "%s", absolute_tail);
	else
		length = snprintf(out, size, "%s%s", backend->fixture_root,
				  absolute_tail);
	return length >= 0 && (size_t)length < size ? 0 : -1;
}

/* At most eight nonblocking reads plus one truncation probe are attempted. */
int hw_read_text(const char *path, char *out, size_t size)
{
	struct stat status;
	size_t length = 0;
	int attempts = 0;
	int fd;
	char extra;

	if (path == NULL || out == NULL || size < 2)
		return -1;
	fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
	if (fd < 0)
		return -1;
	if (fstat(fd, &status) != 0 || !S_ISREG(status.st_mode)) {
		(void)close(fd);
		return -1;
	}
	while (length < size - 1 && attempts++ < 8) {
		ssize_t count = read(fd, out + length, size - 1 - length);

		if (count > 0) {
			length += (size_t)count;
			continue;
		}
		if (count == 0)
			break;
		if (errno == EINTR)
			continue;
		(void)close(fd);
		return -1;
	}
	if (attempts >= 8 && length < size - 1) {
		(void)close(fd);
		return -1;
	}
	if (length == size - 1) {
		ssize_t count;

		do {
			count = read(fd, &extra, 1);
		} while (count < 0 && errno == EINTR);
		if (count != 0) {
			(void)close(fd);
			return -1;
		}
	}
	(void)close(fd);
	out[length] = '\0';
	while (length > 0 && (out[length - 1] == '\0' ||
	       isspace((unsigned char)out[length - 1]) != 0))
		out[--length] = '\0';
	if (memchr(out, '\0', length) != NULL)
		return -1;
	return length > 0 ? 0 : -1;
}

int64_t hw_read_number(const char *path, int64_t low, int64_t high)
{
	char text[64];
	char *end;
	intmax_t value;

	if (hw_read_text(path, text, sizeof(text)) != 0)
		return -1;
	errno = 0;
	value = strtoimax(text, &end, 10);
	while (isspace((unsigned char)*end) != 0)
		++end;
	return errno == 0 && end != text && *end == '\0' && value >= low &&
		value <= high ? (int64_t)value : -1;
}

int hw_regular_exists(const char *path)
{
	struct stat status;
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);

	if (fd < 0)
		return 0;
	if (fstat(fd, &status) != 0 || !S_ISREG(status.st_mode)) {
		(void)close(fd);
		return 0;
	}
	(void)close(fd);
	return 1;
}

DIR *hw_open_directory(const char *path)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_DIRECTORY |
		      O_NOFOLLOW);
	DIR *directory;

	if (fd < 0)
		return NULL;
	directory = fdopendir(fd);
	if (directory == NULL)
		(void)close(fd);
	return directory;
}

DIR *hw_discovery_open_directory(struct hardware_backend *backend,
				 const char *path)
{
	if (backend != NULL && backend->discovery_scan_count != UINT64_MAX)
		++backend->discovery_scan_count;
	return hw_open_directory(path);
}

void hw_discovery_cache_reset(struct hardware_backend *backend)
{
	if (backend == NULL)
		return;
	memset(backend->discovery_cache, 0, sizeof(backend->discovery_cache));
	backend->discovery_generation = 0;
}

void hw_discovery_begin_refresh(struct hardware_backend *backend)
{
	if (backend == NULL)
		return;
	if (backend->discovery_generation == UINT64_MAX) {
		hw_discovery_cache_reset(backend);
		backend->discovery_generation = 1;
	} else {
		++backend->discovery_generation;
	}
}

static struct hardware_discovery_cache_entry *discovery_entry(
	struct hardware_backend *backend, enum hw_discovery_slot slot)
{
	if (backend == NULL || slot < 0 || slot >= HW_DISCOVERY_SLOT_COUNT)
		return NULL;
	return &backend->discovery_cache[slot];
}

void hw_discovery_forget(struct hardware_backend *backend,
			 enum hw_discovery_slot slot)
{
	struct hardware_discovery_cache_entry *entry = discovery_entry(backend, slot);

	if (entry != NULL)
		memset(entry, 0, sizeof(*entry));
}

int hw_discovery_cached_device(struct hardware_backend *backend,
			       enum hw_discovery_slot slot, const char *root,
			       char *name, size_t size, char *leaf,
			       size_t leaf_size)
{
	struct hardware_discovery_cache_entry *entry = discovery_entry(backend, slot);
	struct stat status;
	struct stat leaf_status;
	char path[PATH_MAX];
	char leaf_path[PATH_MAX];
	int length;

	if (entry == NULL || entry->state != 1 || root == NULL || name == NULL ||
	    size == 0 || !hw_valid_component(entry->device))
		return -1;
	length = snprintf(path, sizeof(path), "%s/%s", root, entry->device);
	if (length < 0 || (size_t)length >= sizeof(path) || stat(path, &status) != 0 ||
	    !S_ISDIR(status.st_mode) ||
	    (unsigned int)(status.st_mode & S_IFMT) != entry->mode_type ||
	    (uint64_t)status.st_dev != entry->device_id ||
	    (uint64_t)status.st_ino != entry->inode) {
		hw_discovery_forget(backend, slot);
		return -1;
	}
	if (entry->leaf[0] != '\0' &&
	    (hw_device_path(leaf_path, sizeof(leaf_path), root, entry->device,
			    entry->leaf) != 0 || stat(leaf_path, &leaf_status) != 0 ||
	     !S_ISREG(leaf_status.st_mode) ||
	     (unsigned int)(leaf_status.st_mode & S_IFMT) != entry->leaf_mode_type ||
	     (uint64_t)leaf_status.st_dev != entry->leaf_device_id ||
	     (uint64_t)leaf_status.st_ino != entry->leaf_inode)) {
		hw_discovery_forget(backend, slot);
		return -1;
	}
	if (snprintf(name, size, "%s", entry->device) >= (int)size)
		return -1;
	if (leaf != NULL && leaf_size > 0 &&
	    snprintf(leaf, leaf_size, "%s", entry->leaf) >= (int)leaf_size)
		return -1;
	return 0;
}

int hw_discovery_should_scan(const struct hardware_backend *backend,
			     enum hw_discovery_slot slot)
{
	const struct hardware_discovery_cache_entry *entry;

	if (backend == NULL || slot < 0 || slot >= HW_DISCOVERY_SLOT_COUNT)
		return 0;
	entry = &backend->discovery_cache[slot];
	return entry->state == 0 ||
		(entry->state < 0 &&
		 backend->discovery_generation >= entry->retry_generation);
}

int hw_discovery_remember(struct hardware_backend *backend,
			  enum hw_discovery_slot slot, const char *root,
			  const char *name, const char *leaf)
{
	struct hardware_discovery_cache_entry *entry = discovery_entry(backend, slot);
	struct stat status;
	struct stat leaf_status;
	char path[PATH_MAX];
	char leaf_path[PATH_MAX];
	int length;

	if (entry == NULL || root == NULL || !hw_valid_component(name) ||
	    (leaf != NULL && leaf[0] != '\0' && !hw_valid_component(leaf)))
		return -1;
	length = snprintf(path, sizeof(path), "%s/%s", root, name);
	if (length < 0 || (size_t)length >= sizeof(path) || stat(path, &status) != 0 ||
	    !S_ISDIR(status.st_mode))
		return -1;
	if (leaf != NULL && leaf[0] != '\0' &&
	    (hw_device_path(leaf_path, sizeof(leaf_path), root, name, leaf) != 0 ||
	     stat(leaf_path, &leaf_status) != 0 || !S_ISREG(leaf_status.st_mode)))
		return -1;
	memset(entry, 0, sizeof(*entry));
	(void)snprintf(entry->device, sizeof(entry->device), "%s", name);
	if (leaf != NULL) {
		(void)snprintf(entry->leaf, sizeof(entry->leaf), "%s", leaf);
		if (leaf[0] != '\0') {
			entry->leaf_device_id = (uint64_t)leaf_status.st_dev;
			entry->leaf_inode = (uint64_t)leaf_status.st_ino;
			entry->leaf_mode_type =
				(unsigned int)(leaf_status.st_mode & S_IFMT);
		}
	}
	entry->device_id = (uint64_t)status.st_dev;
	entry->inode = (uint64_t)status.st_ino;
	entry->mode_type = (unsigned int)(status.st_mode & S_IFMT);
	entry->state = 1;
	return 0;
}

void hw_discovery_mark_missing(struct hardware_backend *backend,
			       enum hw_discovery_slot slot)
{
	struct hardware_discovery_cache_entry *entry = discovery_entry(backend, slot);

	if (entry == NULL)
		return;
	memset(entry, 0, sizeof(*entry));
	entry->state = -1;
	entry->retry_generation = backend->discovery_generation > UINT64_MAX - 60 ?
		UINT64_MAX : backend->discovery_generation + 60;
}

int hw_valid_component(const char *name)
{
	size_t index;

	if (name == NULL || name[0] == '\0' || name[0] == '.')
		return 0;
	for (index = 0; name[index] != '\0'; ++index) {
		unsigned char byte = (unsigned char)name[index];

		if (isalnum(byte) == 0 && byte != '-' && byte != '_' && byte != '.' &&
		    byte != ':')
			return 0;
		if (index >= 63)
			return 0;
	}
	return 1;
}

int hw_device_path(char *path, size_t size, const char *root,
		   const char *name, const char *attribute)
{
	int length;

	if (!hw_valid_component(name) || !hw_valid_component(attribute))
		return -1;
	length = snprintf(path, size, "%s/%s/%s", root, name, attribute);
	return length >= 0 && (size_t)length < size ? 0 : -1;
}

int64_t hw_device_number(const char *root, const char *name,
			 const char *attribute, int64_t low, int64_t high)
{
	char path[PATH_MAX];

	return hw_device_path(path, sizeof(path), root, name, attribute) == 0 ?
		hw_read_number(path, low, high) : -1;
}

int hw_contains_casefold(const char *text, const char *needle)
{
	size_t i;
	size_t j;
	size_t count = strlen(needle);

	for (i = 0; text[i] != '\0'; ++i) {
		for (j = 0; j < count && text[i + j] != '\0' &&
		     tolower((unsigned char)text[i + j]) ==
		     tolower((unsigned char)needle[j]); ++j)
			;
		if (j == count)
			return 1;
	}
	return 0;
}

int hw_percent_of(int64_t value, int64_t maximum)
{
	if (value < 0 || maximum <= 0 || value > maximum ||
	    value > INT64_MAX / 100)
		return -1;
	return (int)((value * 100 + maximum / 2) / maximum);
}
