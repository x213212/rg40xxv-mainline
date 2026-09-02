#define _POSIX_C_SOURCE 200809L

#include "ui.h"

#include <errno.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

static uint64_t monotonic_ms(void)
{
	struct timespec value;

	if (clock_gettime(CLOCK_MONOTONIC, &value) != 0)
		return 0;
	return (uint64_t)value.tv_sec * 1000U + (uint64_t)value.tv_nsec / 1000000U;
}

static void sleep_10ms(void)
{
	struct timespec delay = { .tv_sec = 0, .tv_nsec = 10000000L };

	while (nanosleep(&delay, &delay) != 0 && errno == EINTR)
		;
}

static int refresh_has_exited(pid_t pid)
{
	siginfo_t information = { 0 };
	int result;

	do {
		result = waitid(P_PID, pid, &information,
				WEXITED | WNOHANG | WNOWAIT);
	} while (result != 0 && errno == EINTR);
	return result == 0 && information.si_pid == pid;
}

static int refresh_group_valid(pid_t pid)
{
	return pid > 1 && getpgid(pid) == pid && pid != getpgrp();
}

int catalog_refresh_start(struct ui *ui, const char *rom_root,
			  const char *routes_path, const char *snapshot_path)
{
	char executable[PATH_MAX];
	char *arguments[9];
	ssize_t length;
	posix_spawnattr_t attributes;
	sigset_t empty;
	sigset_t defaults;
	short flags = POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK |
		POSIX_SPAWN_SETSIGDEF;
	pid_t child = 0;
	int error;

	if (ui->catalog_refresh_pid > 0 || snapshot_path == NULL ||
	    snapshot_path[0] != '/')
		return -1;
	length = readlink("/proc/self/exe", executable, sizeof(executable) - 1U);
	if (length <= 0 || (size_t)length >= sizeof(executable) - 1U)
		return -1;
	executable[length] = '\0';
	arguments[0] = executable;
	arguments[1] = "--catalog-refresh-only";
	arguments[2] = "--rom-root";
	arguments[3] = (char *)rom_root;
	arguments[4] = "--platform-routes";
	arguments[5] = (char *)routes_path;
	arguments[6] = "--catalog-snapshot";
	arguments[7] = (char *)snapshot_path;
	arguments[8] = NULL;
	if (posix_spawnattr_init(&attributes) != 0)
		return -1;
	(void)sigemptyset(&empty);
	(void)sigemptyset(&defaults);
	(void)sigaddset(&defaults, SIGHUP);
	(void)sigaddset(&defaults, SIGINT);
	(void)sigaddset(&defaults, SIGTERM);
	error = posix_spawnattr_setflags(&attributes, flags);
	if (error == 0)
		error = posix_spawnattr_setpgroup(&attributes, 0);
	if (error == 0)
		error = posix_spawnattr_setsigmask(&attributes, &empty);
	if (error == 0)
		error = posix_spawnattr_setsigdefault(&attributes, &defaults);
	if (error == 0)
		error = posix_spawn(&child, executable, NULL, &attributes, arguments,
				    environ);
	(void)posix_spawnattr_destroy(&attributes);
	if (error != 0 || child <= 1 || getpgid(child) != child) {
		if (child > 1) {
			(void)kill(child, SIGKILL);
			(void)waitpid(child, NULL, 0);
		}
		return -1;
	}
	ui->catalog_refresh_pid = child;
	return 0;
}

void catalog_refresh_poll(struct ui *ui)
{
	int status;
	pid_t result;

	if (ui->catalog_refresh_pid <= 1 ||
	    !refresh_has_exited(ui->catalog_refresh_pid))
		return;
	do {
		result = waitpid(ui->catalog_refresh_pid, &status, 0);
	} while (result < 0 && errno == EINTR);
	if (result == ui->catalog_refresh_pid) {
		if (WIFEXITED(status))
			(void)fprintf(stderr, "CATALOG_REFRESH exit_status=%d\n",
				      WEXITSTATUS(status));
		else if (WIFSIGNALED(status))
			(void)fprintf(stderr, "CATALOG_REFRESH signal=%d\n",
				      WTERMSIG(status));
	}
	ui->catalog_refresh_pid = 0;
}

int catalog_refresh_stop(struct ui *ui)
{
	uint64_t deadline;
	int status;
	pid_t pid = ui->catalog_refresh_pid;

	if (pid <= 1)
		return 0;
	if (refresh_has_exited(pid)) {
		(void)waitpid(pid, &status, 0);
		ui->catalog_refresh_pid = 0;
		return 0;
	}
	if (!refresh_group_valid(pid))
		return -1;
	(void)kill(-pid, SIGTERM);
	deadline = monotonic_ms() + 500U;
	while (!refresh_has_exited(pid) && monotonic_ms() < deadline)
		sleep_10ms();
	if (!refresh_has_exited(pid)) {
		if (!refresh_group_valid(pid))
			return -1;
		(void)kill(-pid, SIGKILL);
		deadline = monotonic_ms() + 500U;
		while (!refresh_has_exited(pid) && monotonic_ms() < deadline)
			sleep_10ms();
	}
	if (!refresh_has_exited(pid))
		return -1;
	(void)waitpid(pid, &status, 0);
	ui->catalog_refresh_pid = 0;
	return 0;
}
