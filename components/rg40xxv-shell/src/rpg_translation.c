#define _POSIX_C_SOURCE 200809L

#include "ui.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *mode_name(enum rpg_translation_mode mode)
{
	return mode == RPG_TRANSLATION_STATIC ? "static" : "off";
}

static bool valid_absolute_path(const char *path)
{
	size_t length;

	if (path == NULL)
		return false;
	length = strlen(path);
	return length >= 2U && path[0] == '/' &&
		strchr(path, '\n') == NULL && strchr(path, '\r') == NULL &&
		strchr(path, '\t') == NULL && strstr(path, "//") == NULL &&
		strstr(path, "/../") == NULL && strstr(path, "/./") == NULL &&
		(length < 3U || strcmp(path + length - 3U, "/..") != 0) &&
		(length < 2U || strcmp(path + length - 2U, "/.") != 0);
}

static int parent_path(const char *path, char *parent, size_t size)
{
	const char *slash = strrchr(path, '/');
	size_t length;

	if (slash == NULL || slash == path)
		return EINVAL;
	length = (size_t)(slash - path);
	if (length >= size)
		return ENAMETOOLONG;
	memcpy(parent, path, length);
	parent[length] = '\0';
	return 0;
}

static int read_mode(const char *path, enum rpg_translation_mode *mode)
{
	struct stat status;
	char payload[16];
	size_t offset = 0U;
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);

	if (fd < 0)
		return errno;
	if (fstat(fd, &status) != 0 || !S_ISREG(status.st_mode) ||
	    (status.st_uid != geteuid() && status.st_uid != 0) ||
	    status.st_nlink != 1 ||
	    (status.st_mode & 0777) != 0600 ||
	    status.st_size <= 0 || status.st_size >= (off_t)sizeof(payload)) {
		(void)close(fd);
		return EPERM;
	}
	while (offset < (size_t)status.st_size) {
		ssize_t count = read(fd, payload + offset,
			(size_t)status.st_size - offset);

		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0) {
			(void)close(fd);
			return EIO;
		}
		offset += (size_t)count;
	}
	if (close(fd) != 0)
		return EIO;
	payload[offset] = '\0';
	if (strcmp(payload, "off") == 0 || strcmp(payload, "off\n") == 0)
		*mode = RPG_TRANSLATION_OFF;
	else if (strcmp(payload, "static") == 0 ||
		 strcmp(payload, "static\n") == 0)
		*mode = RPG_TRANSLATION_STATIC;
	else
		return EINVAL;
	return 0;
}

static int write_all(int fd, const char *payload, size_t size)
{
	size_t offset = 0U;

	while (offset < size) {
		ssize_t count = write(fd, payload + offset, size - offset);

		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			return EIO;
		offset += (size_t)count;
	}
	return 0;
}

static int write_mode(const struct rpg_translation_state *state,
		      enum rpg_translation_mode mode)
{
	char parent[PATH_MAX];
	char temporary[PATH_MAX];
	struct stat status;
	const char *payload = mode == RPG_TRANSLATION_STATIC ? "static\n" : "off\n";
	int parent_fd = -1;
	int fd = -1;
	int error;

	error = parent_path(state->path, parent, sizeof(parent));
	if (error != 0)
		return error;
	if (lstat(parent, &status) != 0 || !S_ISDIR(status.st_mode) ||
	    S_ISLNK(status.st_mode) || status.st_uid != geteuid() ||
	    (status.st_mode & (S_IWGRP | S_IWOTH)) != 0)
		return ENOENT;
	error = snprintf(temporary, sizeof(temporary), "%s.tmp.%ld", state->path,
		(long)getpid());
	if (error < 0 || error >= (int)sizeof(temporary))
		return ENAMETOOLONG;
	fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC |
		  O_NOFOLLOW, 0600);
	if (fd < 0)
		return errno;
	error = write_all(fd, payload, strlen(payload));
	if (error == 0 && fsync(fd) != 0)
		error = errno;
	if (close(fd) != 0 && error == 0)
		error = errno;
	fd = -1;
	if (error == 0 && rename(temporary, state->path) != 0)
		error = errno;
	if (error == 0) {
		parent_fd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC |
			O_NOFOLLOW);
		if (parent_fd < 0 || fsync(parent_fd) != 0)
			error = errno;
	}
	if (parent_fd >= 0)
		(void)close(parent_fd);
	if (fd >= 0)
		(void)close(fd);
	if (error != 0)
		(void)unlink(temporary);
	return error;
}

int rpg_translation_init(struct rpg_translation_state *state, const char *path)
{
	enum rpg_translation_mode loaded = RPG_TRANSLATION_OFF;
	int error;

	if (state == NULL || !valid_absolute_path(path))
		return EINVAL;
	memset(state, 0, sizeof(*state));
	state->mode = RPG_TRANSLATION_OFF;
	if (setenv("RG_RMUT_TRANSLATION_MODE", "off", 1) != 0)
		return errno;
	error = snprintf(state->path, sizeof(state->path), "%s", path);
	if (error < 0 || error >= (int)sizeof(state->path))
		return ENAMETOOLONG;
	error = read_mode(path, &loaded);
	if (error != 0 && error != ENOENT)
		return error;
	state->mode = loaded;
	if (loaded != RPG_TRANSLATION_OFF &&
	    setenv("RG_RMUT_TRANSLATION_MODE", mode_name(loaded), 1) != 0)
		return errno;
	return 0;
}

void rpg_translation_toggle_selected(struct ui *ui, uint32_t now)
{
	const struct game_entry *game = catalog_visible_game(ui, ui->game_index);
	enum rpg_translation_mode next;
	int error;

	if (game == NULL || !catalog_system_is_rpg(game->system)) {
		render_activate(ui, tr(ui, "rpg_translation_unavailable"), now);
		return;
	}
	if (strcasecmp(game->system, "EASYRPG") == 0) {
		render_activate(ui, tr(ui, "rpg_translation_rm2k_unsupported"), now);
		return;
	}
	next = ui->rpg_translation.mode == RPG_TRANSLATION_STATIC ?
		RPG_TRANSLATION_OFF : RPG_TRANSLATION_STATIC;
	error = write_mode(&ui->rpg_translation, next);
	if (error != 0) {
		render_activate(ui, tr(ui, "rpg_translation_save_failed"), now);
		return;
	}
	ui->rpg_translation.mode = next;
	/* The persistent file is the supervisor/launcher contract.  This process
	 * also updates its own environment for diagnostics, but a rare allocation
	 * failure must not contradict a successfully committed state file. */
	(void)setenv("RG_RMUT_TRANSLATION_MODE", mode_name(next), 1);
	render_activate(ui, tr(ui, next == RPG_TRANSLATION_STATIC ?
		"rpg_translation_static_enabled" : "rpg_translation_disabled"), now);
}
