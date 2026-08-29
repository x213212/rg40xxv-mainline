#define _POSIX_C_SOURCE 200809L

#include "launcher.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

static bool valid_token(const char *value, bool allow_empty)
{
	if (value == NULL || (!allow_empty && value[0] == '\0'))
		return false;
	if (value[0] == '-')
		return false;
	for (const unsigned char *cursor = (const unsigned char *)value;
	     *cursor != '\0'; ++cursor) {
		if (!isalnum(*cursor) && *cursor != '_' && *cursor != '-' &&
		    *cursor != '.' && *cursor != ':' && *cursor != '+')
			return false;
	}
	return true;
}

static bool valid_path_text(const char *path)
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

static bool valid_argument_text(const char *value)
{
	if (value == NULL || value[0] == '\0')
		return false;
	for (const unsigned char *cursor = (const unsigned char *)value;
	     *cursor != '\0'; ++cursor) {
		if (*cursor < 0x20 || *cursor == 0x7f)
			return false;
	}
	return true;
}

int launcher_executable_validate(const char *executable)
{
	struct stat status;

	if (!valid_path_text(executable))
		return EINVAL;
	if (lstat(executable, &status) != 0)
		return errno;
	if (!S_ISREG(status.st_mode) || S_ISLNK(status.st_mode) ||
	    access(executable, X_OK) != 0)
		return EACCES;
	return 0;
}

int launcher_request_validate(const struct launcher_request *request)
{
	struct stat status;
	int error;

	if (request == NULL || !valid_path_text(request->executable) ||
	    !valid_token(request->route, true) ||
	    !valid_token(request->platform, false) ||
	    !valid_path_text(request->content) ||
	    (request->log_path != NULL && request->log_path[0] != '\0' &&
	     !valid_path_text(request->log_path)))
		return EINVAL;
	error = launcher_executable_validate(request->executable);
	if (error != 0)
		return error;
	if (stat(request->content, &status) != 0)
		return errno;
	if (!S_ISREG(status.st_mode) && !S_ISDIR(status.st_mode))
		return EINVAL;
	return 0;
}

int stream_launcher_request_validate(
	const struct stream_launcher_request *request)
{
	int error;

	if (request == NULL || !valid_path_text(request->executable) ||
	    !valid_argument_text(request->host) || request->width == 0U ||
	    request->height == 0U || request->fps == 0U ||
	    request->bitrate_kbps == 0U ||
	    !valid_token(request->codec, false) ||
	    !valid_token(request->aspect, false) ||
	    strcmp(request->codec, "h264") != 0 ||
	    (strcmp(request->aspect, "fit") != 0 &&
	     strcmp(request->aspect, "fill") != 0 &&
	     strcmp(request->aspect, "stretch") != 0) ||
	    (request->log_path != NULL && request->log_path[0] != '\0' &&
	     !valid_path_text(request->log_path)))
		return EINVAL;
	error = launcher_executable_validate(request->executable);
	return error;
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

static void decode_status(struct launcher_process *process, int status)
{
	process->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
	process->term_signal = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
	process->pid = 0;
	process->active = false;
}

static int launcher_process_start_argv(struct launcher_process *process,
			       const char *executable, const char *log_path,
			       char *const arguments[])
{
	posix_spawn_file_actions_t actions;
	posix_spawnattr_t attributes;
	int log_fd = -1;
	int error;

	if (process == NULL)
		return EINVAL;
	if (process->active || process->pgid > 0)
		return EBUSY;
	memset(process, 0, sizeof(*process));
	process->exit_code = -1;
	error = posix_spawn_file_actions_init(&actions);
	if (error != 0)
		goto fail_early;
	error = posix_spawnattr_init(&attributes);
	if (error != 0) {
		(void)posix_spawn_file_actions_destroy(&actions);
		goto fail_early;
	}
	if (log_path != NULL && log_path[0] != '\0') {
		log_fd = open(log_path,
			      O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
		if (log_fd < 0)
			error = errno;
		if (error == 0)
			error = posix_spawn_file_actions_adddup2(&actions, log_fd,
							 STDOUT_FILENO);
		if (error == 0)
			error = posix_spawn_file_actions_adddup2(&actions, log_fd,
							 STDERR_FILENO);
		if (error == 0)
			error = posix_spawn_file_actions_addclose(&actions, log_fd);
	}
	if (error == 0)
		error = configure_signals(&attributes);
	if (error == 0)
		error = posix_spawn(&process->pid, executable, &actions,
				    &attributes, arguments, environ);
	if (log_fd >= 0)
		(void)close(log_fd);
	(void)posix_spawnattr_destroy(&attributes);
	(void)posix_spawn_file_actions_destroy(&actions);
	if (error != 0)
		goto fail_early;
	process->pgid = process->pid;
	process->active = true;
	return 0;

fail_early:
	process->spawn_error = error;
	return error;
}

static int launcher_process_reject(struct launcher_process *process, int error)
{
	if (process == NULL)
		return EINVAL;
	if (process->active || process->pgid > 0)
		return EBUSY;
	memset(process, 0, sizeof(*process));
	process->exit_code = -1;
	process->spawn_error = error;
	return error;
}

int launcher_process_start(struct launcher_process *process,
			   const struct launcher_request *request)
{
	char *arguments[8];
	size_t count = 0;
	int error;

	if (process == NULL)
		return EINVAL;
	if (process->active || process->pgid > 0)
		return EBUSY;
	error = launcher_request_validate(request);
	if (error != 0)
		return launcher_process_reject(process, error);
	arguments[count++] = (char *)request->executable;
	if (request->route[0] != '\0') {
		arguments[count++] = (char *)"--route";
		arguments[count++] = (char *)request->route;
	}
	arguments[count++] = (char *)"--";
	arguments[count++] = (char *)request->platform;
	arguments[count++] = (char *)request->content;
	arguments[count] = NULL;
	return launcher_process_start_argv(process, request->executable,
					   request->log_path, arguments);
}

int stream_launcher_process_start(
	struct launcher_process *process,
	const struct stream_launcher_request *request)
{
	char width[16];
	char height[16];
	char fps[16];
	char bitrate[16];
	char *arguments[10];
	int error;

	if (process == NULL)
		return EINVAL;
	if (process->active || process->pgid > 0)
		return EBUSY;
	error = stream_launcher_request_validate(request);
	if (error != 0)
		return launcher_process_reject(process, error);
	(void)snprintf(width, sizeof(width), "%u", request->width);
	(void)snprintf(height, sizeof(height), "%u", request->height);
	(void)snprintf(fps, sizeof(fps), "%u", request->fps);
	(void)snprintf(bitrate, sizeof(bitrate), "%u", request->bitrate_kbps);
	arguments[0] = (char *)request->executable;
	arguments[1] = (char *)"stream";
	arguments[2] = (char *)request->host;
	arguments[3] = width;
	arguments[4] = height;
	arguments[5] = fps;
	arguments[6] = bitrate;
	arguments[7] = (char *)request->codec;
	arguments[8] = (char *)request->aspect;
	arguments[9] = NULL;
	return launcher_process_start_argv(process, request->executable,
					   request->log_path, arguments);
}

int launcher_process_poll(struct launcher_process *process, bool *finished)
{
	int status;
	pid_t result;

	if (process == NULL || finished == NULL)
		return EINVAL;
	*finished = !process->active;
	if (!process->active)
		return 0;
	do {
		result = waitpid(process->pid, &status, WNOHANG);
	} while (result < 0 && errno == EINTR);
	if (result == 0)
		return 0;
	if (result < 0) {
		if (errno == ECHILD) {
			process->pid = 0;
			process->active = false;
			*finished = true;
			return 0;
		}
		return errno;
	}
	decode_status(process, status);
	*finished = true;
	return 0;
}

static uint64_t monotonic_ms(void)
{
	struct timespec value;

	if (clock_gettime(CLOCK_MONOTONIC, &value) != 0)
		return 0;
	return (uint64_t)value.tv_sec * 1000U + (uint64_t)value.tv_nsec / 1000000U;
}

static bool process_group_alive(pid_t pgid)
{
	if (pgid <= 1)
		return false;
	if (kill(-pgid, 0) == 0)
		return true;
	return errno == EPERM;
}

static void terminate_remaining_group(pid_t pgid, unsigned int timeout_ms)
{
	uint64_t deadline;
	struct timespec pause = { .tv_sec = 0, .tv_nsec = 10000000L };

	if (!process_group_alive(pgid))
		return;
	(void)kill(-pgid, SIGTERM);
	deadline = monotonic_ms() + timeout_ms;
	while (process_group_alive(pgid) && monotonic_ms() < deadline)
		(void)nanosleep(&pause, NULL);
	if (process_group_alive(pgid))
		(void)kill(-pgid, SIGKILL);
}

void launcher_process_terminate(struct launcher_process *process,
				unsigned int timeout_ms)
{
	uint64_t deadline;
	bool finished = false;
	struct timespec pause = { .tv_sec = 0, .tv_nsec = 10000000L };

	if (process == NULL)
		return;
	if (process->active) {
		(void)kill(-process->pgid, SIGTERM);
		deadline = monotonic_ms() + timeout_ms;
		while (!finished && monotonic_ms() < deadline) {
			(void)launcher_process_poll(process, &finished);
			if (!finished)
				(void)nanosleep(&pause, NULL);
		}
	}
	if (process->active) {
		int status;
		pid_t waited;

		(void)kill(-process->pgid, SIGKILL);
		do {
			waited = waitpid(process->pid, &status, 0);
		} while (waited < 0 && errno == EINTR);
		if (waited == process->pid)
			decode_status(process, status);
		else {
			process->pid = 0;
			process->active = false;
		}
	}
	terminate_remaining_group(process->pgid, timeout_ms);
	process->pgid = 0;
}
