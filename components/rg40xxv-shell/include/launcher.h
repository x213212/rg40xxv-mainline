#ifndef RG40XXV_LAUNCHER_H
#define RG40XXV_LAUNCHER_H

#define _POSIX_C_SOURCE 200809L

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

struct launcher_request {
	const char *executable;
	const char *route;
	const char *platform;
	const char *content;
	const char *log_path;
};

struct stream_launcher_request {
	const char *executable;
	const char *host;
	uint32_t width;
	uint32_t height;
	uint32_t fps;
	uint32_t bitrate_kbps;
	const char *codec;
	const char *aspect;
	const char *log_path;
};

struct launcher_process {
	pid_t pid;
	pid_t pgid;
	int exit_code;
	int term_signal;
	int spawn_error;
	bool active;
};

int launcher_request_validate(const struct launcher_request *request);
int launcher_executable_validate(const char *executable);
int launcher_process_start(struct launcher_process *process,
			   const struct launcher_request *request);
int stream_launcher_request_validate(
	const struct stream_launcher_request *request);
int stream_launcher_process_start(
	struct launcher_process *process,
	const struct stream_launcher_request *request);
int launcher_process_poll(struct launcher_process *process, bool *finished);
void launcher_process_terminate(struct launcher_process *process,
				unsigned int timeout_ms);

#endif
