#define _POSIX_C_SOURCE 200809L

#include "launcher.h"

#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void wait_for_exit(struct launcher_process *process)
{
	struct timespec pause = { .tv_sec = 0, .tv_nsec = 10000000L };
	bool finished = false;

	for (int attempt = 0; attempt < 300 && !finished; ++attempt) {
		assert(launcher_process_poll(process, &finished) == 0);
		if (!finished)
			(void)nanosleep(&pause, NULL);
	}
	assert(finished);
}

int main(int argc, char **argv)
{
	struct launcher_process process = { 0 };
	struct launcher_request request;
	struct stream_launcher_request stream_request;

	assert(argc == 5);
	request = (struct launcher_request) {
		.executable = argv[1],
		.route = "aarch64:mgba",
		.platform = "GBA",
		.content = argv[2],
		.log_path = argv[3],
	};
	assert(launcher_request_validate(&request) == 0);
	assert(setenv("FAKE_EXIT", "7", 1) == 0);
	assert(launcher_process_start(&process, &request) == 0);
	wait_for_exit(&process);
	assert(process.exit_code == 7);
	assert(process.term_signal == 0);
	launcher_process_terminate(&process, 300U);

	request.route = "aarch64:mgba;touch";
	assert(launcher_request_validate(&request) == EINVAL);
	request.route = "aarch64:mgba";
	request.platform = "--list-platforms";
	assert(launcher_request_validate(&request) == EINVAL);
	request.platform = "GBA";
	stream_request = (struct stream_launcher_request) {
		.executable = argv[1],
		.host = "sunshine.local",
		.width = 640U,
		.height = 480U,
		.fps = 60U,
		.bitrate_kbps = 5000U,
		.codec = "h264",
		.aspect = "fit",
		.log_path = argv[3],
	};
	assert(stream_launcher_request_validate(&stream_request) == 0);
	stream_request.width = 1280U;
	stream_request.height = 720U;
	assert(stream_launcher_request_validate(&stream_request) == EINVAL);
	stream_request.width = 640U;
	stream_request.height = 480U;

	assert(unsetenv("FAKE_EXIT") == 0);
	assert(setenv("FAKE_SLEEP", "1", 1) == 0);
	assert(launcher_process_start(&process, &request) == 0);
	launcher_process_terminate(&process, 300U);
	assert(!process.active);
	assert(process.term_signal == SIGTERM || process.term_signal == SIGKILL);

	assert(unsetenv("FAKE_SLEEP") == 0);
	assert(setenv("FAKE_BACKGROUND", "1", 1) == 0);
	assert(setenv("LAUNCH_GRANDCHILD_PID", argv[4], 1) == 0);
	assert(launcher_process_start(&process, &request) == 0);
	wait_for_exit(&process);
	assert(process.pgid > 1);
	launcher_process_terminate(&process, 300U);
	assert(process.pgid == 0);

	printf("LAUNCHER_TEST PASS exit=%d signal=%d\n",
	       process.exit_code, process.term_signal);
	return 0;
}
