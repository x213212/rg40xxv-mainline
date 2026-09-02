#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifdef BLUETOOTH_FIXTURE_HELPER

static int sidecar_path(char output[PATH_MAX], const char *program,
			const char *suffix)
{
	int length = snprintf(output, PATH_MAX, "%s%s", program, suffix);

	return length > 0 && length < PATH_MAX ? 0 : -1;
}

static bool file_matches(const char *path, const char *command)
{
	char value[64];
	FILE *file = fopen(path, "r");

	if (file == NULL)
		return false;
	if (fgets(value, sizeof(value), file) == NULL)
		value[0] = '\0';
	(void)fclose(file);
	value[strcspn(value, "\r\n")] = '\0';
	return strcmp(value, command) == 0;
}

static unsigned int command_delay(const char *path, const char *command)
{
	char value[96];
	char selected[32];
	char trailing[2];
	unsigned int milliseconds;
	FILE *file = fopen(path, "r");

	if (file == NULL)
		return 0U;
	if (fgets(value, sizeof(value), file) == NULL)
		value[0] = '\0';
	(void)fclose(file);
	if (sscanf(value, "%31s %u %1s", selected, &milliseconds, trailing) != 2 ||
	    strcmp(selected, command) != 0)
		return 0U;
	return milliseconds;
}

static void sleep_ms(unsigned int milliseconds)
{
	struct timespec delay = {
		.tv_sec = (time_t)(milliseconds / 1000U),
		.tv_nsec = (long)(milliseconds % 1000U) * 1000000L,
	};

	while (nanosleep(&delay, &delay) != 0 && errno == EINTR)
		;
}

static bool valid_mac(const char *address)
{
	if (address == NULL || strlen(address) != 17U)
		return false;
	for (size_t i = 0U; i < 17U; ++i) {
		if (i % 3U == 2U) {
			if (address[i] != ':')
				return false;
		} else if (!((address[i] >= '0' && address[i] <= '9') ||
			     (address[i] >= 'A' && address[i] <= 'F'))) {
			return false;
		}
	}
	return true;
}

static bool valid_arguments(int argc, char **argv)
{
	if (argc == 2 &&
	    (strcmp(argv[1], "status") == 0 || strcmp(argv[1], "scan") == 0))
		return true;
	if (argc != 3)
		return false;
	if (strcmp(argv[1], "power") == 0)
		return strcmp(argv[2], "on") == 0 || strcmp(argv[2], "off") == 0;
	if (strcmp(argv[1], "pair") != 0 && strcmp(argv[1], "connect") != 0 &&
	    strcmp(argv[1], "disconnect") != 0 &&
	    strcmp(argv[1], "forget") != 0)
		return false;
	return valid_mac(argv[2]);
}

static int copy_snapshot(const char *path)
{
	unsigned char buffer[1024];
	FILE *file = fopen(path, "rb");

	if (file == NULL)
		return 66;
	for (;;) {
		size_t count = fread(buffer, 1U, sizeof(buffer), file);

		if (count > 0U && fwrite(buffer, 1U, count, stdout) != count) {
			(void)fclose(file);
			return 74;
		}
		if (count < sizeof(buffer)) {
			int result = ferror(file) ? 74 : 0;

			(void)fclose(file);
			return result;
		}
	}
}

int main(int argc, char **argv)
{
	static const char *const forbidden[] = {
		"LD_PRELOAD", "LD_LIBRARY_PATH", "DEVICE_CONTROL_TESTING",
		"RG40XXV_BLUETOOTH_CAPABILITY", "IFS",
	};
	char active[PATH_MAX];
	char capture[PATH_MAX];
	char delay[PATH_MAX];
	char failure[PATH_MAX];
	char leak[PATH_MAX];
	char snapshot[PATH_MAX];
	FILE *file;
	int result;

	if (!valid_arguments(argc, argv) ||
	    sidecar_path(active, argv[0], ".active") != 0 ||
	    sidecar_path(capture, argv[0], ".capture") != 0 ||
	    sidecar_path(delay, argv[0], ".delay") != 0 ||
	    sidecar_path(failure, argv[0], ".fail") != 0 ||
	    sidecar_path(leak, argv[0], ".env-leak") != 0 ||
	    sidecar_path(snapshot, argv[0], ".snapshot") != 0)
		return 64;
	for (size_t i = 0U; i < sizeof(forbidden) / sizeof(forbidden[0]); ++i) {
		if (getenv(forbidden[i]) != NULL) {
			file = fopen(leak, "w");
			if (file != NULL) {
				(void)fprintf(file, "%s\n", forbidden[i]);
				(void)fclose(file);
			}
			return 65;
		}
	}
	file = fopen(capture, "a");
	if (file == NULL)
		return 73;
	for (int i = 1; i < argc; ++i)
		(void)fprintf(file, "%s%s", i == 1 ? "" : "\t", argv[i]);
	(void)fputc('\n', file);
	if (fclose(file) != 0)
		return 73;
	file = fopen(active, "w");
	if (file == NULL)
		return 73;
	(void)fprintf(file, "%s\n", argv[1]);
	if (fclose(file) != 0)
		return 73;
	sleep_ms(command_delay(delay, argv[1]));
	if (file_matches(failure, argv[1])) {
		(void)unlink(active);
		return 7;
	}
	result = copy_snapshot(snapshot);
	(void)unlink(active);
	return result;
}

#else

#include <SDL.h>

#include "bluetooth_backend.h"

static void fail(const char *expression, int line)
{
	(void)fprintf(stderr, "bluetooth backend assertion failed at %d: %s\n",
		line, expression);
	exit(1);
}

#define CHECK(expression) do { \
	if (!(expression)) \
		fail(#expression, __LINE__); \
} while (0)

static uint64_t monotonic_ms(void)
{
	struct timespec value;

	CHECK(clock_gettime(CLOCK_MONOTONIC, &value) == 0);
	return (uint64_t)value.tv_sec * 1000U +
		(uint64_t)value.tv_nsec / 1000000U;
}

static void sleep_ms(unsigned int milliseconds)
{
	struct timespec delay = {
		.tv_sec = (time_t)(milliseconds / 1000U),
		.tv_nsec = (long)(milliseconds % 1000U) * 1000000L,
	};

	while (nanosleep(&delay, &delay) != 0 && errno == EINTR)
		;
}

static void make_sidecar(char output[PATH_MAX], const char *helper,
			 const char *suffix)
{
	int length = snprintf(output, PATH_MAX, "%s%s", helper, suffix);

	CHECK(length > 0 && length < PATH_MAX);
}

static void write_text(const char *path, const char *text)
{
	FILE *file = fopen(path, "wb");
	size_t length = strlen(text);

	CHECK(file != NULL);
	CHECK(fwrite(text, 1U, length, file) == length);
	CHECK(fclose(file) == 0);
}

static void write_oversized_snapshot(const char *path)
{
	FILE *file = fopen(path, "wb");

	CHECK(file != NULL);
	CHECK(fprintf(file, "RG40XXV_BLUETOOTH_SNAPSHOT\t2\n"
		"A\t1\t1\t0\t10:22:33:44:55:66\n") > 0);
	for (unsigned int i = 0U;
	     i < BLUETOOTH_BACKEND_MAX_DEVICES + 1U; ++i) {
		CHECK(fprintf(file,
			"D\t02:00:00:00:00:%02X\tDevice%02u\tunknown\t0\t0\t0\t-50\n",
			i, i) > 0);
	}
	CHECK(fclose(file) == 0);
}

static void wait_for_request(struct bluetooth_backend *backend,
			     uint64_t request_id,
			     struct bluetooth_backend_snapshot *snapshot,
			     uint64_t *generation)
{
	uint64_t deadline = monotonic_ms() + 2500U;

	while (monotonic_ms() < deadline) {
		int changed = bluetooth_backend_poll(backend, snapshot, generation);

		CHECK(changed >= 0);
		if (snapshot->completed_request_id == request_id &&
		    snapshot->active_request_id == 0U)
			return;
		sleep_ms(5U);
	}
	fail("request completed before timeout", __LINE__);
}

static void wait_for_working(struct bluetooth_backend *backend,
			     uint64_t request_id,
			     struct bluetooth_backend_snapshot *snapshot,
			     uint64_t *generation)
{
	uint64_t deadline = monotonic_ms() + 1000U;

	while (monotonic_ms() < deadline) {
		int changed = bluetooth_backend_poll(backend, snapshot, generation);

		CHECK(changed >= 0);
		if (snapshot->phase == BLUETOOTH_PHASE_WORKING &&
		    snapshot->active_request_id == request_id)
			return;
		sleep_ms(2U);
	}
	fail("request entered WORKING before timeout", __LINE__);
}

static void wait_for_file(const char *path)
{
	uint64_t deadline = monotonic_ms() + 1000U;

	while (monotonic_ms() < deadline) {
		if (access(path, F_OK) == 0)
			return;
		sleep_ms(2U);
	}
	fail("fixture active marker appeared", __LINE__);
}

static void verify_closed_gate(const char *helper, const char *capability,
			       enum bluetooth_backend_gate expected)
{
	struct bluetooth_backend_snapshot snapshot;
	struct bluetooth_backend *backend = NULL;
	uint64_t generation = 0U;
	uint64_t request_id = 99U;

	CHECK(setenv("RG40XXV_BLUETOOTH_CAPABILITY", capability, 1) == 0);
	CHECK(bluetooth_backend_start(&backend, helper) == 0);
	CHECK(backend != NULL);
	CHECK(!bluetooth_backend_available(backend));
	CHECK(bluetooth_backend_poll(backend, &snapshot, &generation) == 1);
	CHECK(snapshot.gate == expected);
	CHECK(snapshot.phase == (expected == BLUETOOTH_GATE_PENDING ?
		BLUETOOTH_PHASE_PENDING : BLUETOOTH_PHASE_ERROR));
	CHECK(bluetooth_backend_request_status(backend, &request_id) == ENODEV);
	CHECK(request_id == 0U);
	bluetooth_backend_stop(&backend);
	CHECK(backend == NULL);
}

int main(int argc, char **argv)
{
	static const char initial_snapshot[] =
		"RG40XXV_BLUETOOTH_SNAPSHOT\t2\n"
		"A\t1\t1\t0\t10:22:33:44:55:66\n"
		"D\tAA:BB:CC:DD:EE:FF\tGamepad%20One\tcontroller\t0\t0\t0\t-42\n";
	static const char scan_snapshot[] =
		"RG40XXV_BLUETOOTH_SNAPSHOT\t2\n"
		"A\t1\t1\t1\t10:22:33:44:55:66\n"
		"D\tAA:BB:CC:DD:EE:FF\tGamepad%20One\tcontroller\t0\t0\t0\t-40\n";
	static const char powered_off_snapshot[] =
		"RG40XXV_BLUETOOTH_SNAPSHOT\t2\n"
		"A\t1\t0\t0\t10:22:33:44:55:66\n";
	static const char paired_snapshot[] =
		"RG40XXV_BLUETOOTH_SNAPSHOT\t2\n"
		"A\t1\t1\t0\t10:22:33:44:55:66\n"
		"D\tAA:BB:CC:DD:EE:FF\tGamepad%20One\tcontroller\t1\t1\t0\t-39\n";
	static const char connected_snapshot[] =
		"RG40XXV_BLUETOOTH_SNAPSHOT\t2\n"
		"A\t1\t1\t0\t10:22:33:44:55:66\n"
		"D\tAA:BB:CC:DD:EE:FF\tGamepad%20One\tcontroller\t1\t1\t1\t-38\n";
	static const char duplicate_snapshot[] =
		"RG40XXV_BLUETOOTH_SNAPSHOT\t2\n"
		"A\t1\t1\t0\t10:22:33:44:55:66\n"
		"D\tAA:BB:CC:DD:EE:FF\tOne\tcontroller\t1\t1\t1\t-38\n"
		"D\taa:bb:cc:dd:ee:ff\tTwo\taudio\t1\t1\t0\t-50\n";
	static const char empty_snapshot[] =
		"RG40XXV_BLUETOOTH_SNAPSHOT\t2\n"
		"A\t1\t1\t0\t10:22:33:44:55:66\n";
	struct bluetooth_backend_snapshot snapshot;
	struct bluetooth_backend *backend = NULL;
	char active[PATH_MAX];
	char capture[PATH_MAX];
	char delay[PATH_MAX];
	char failure[PATH_MAX];
	char leak[PATH_MAX];
	char snapshot_path[PATH_MAX];
	uint64_t generation = 0U;
	uint64_t request_id;
	uint64_t started;
	uint64_t elapsed;
	uint64_t enqueue_elapsed;
	uint64_t previous_generation;

	CHECK(argc == 8);
	CHECK(SDL_Init(SDL_INIT_EVENTS | SDL_INIT_TIMER) == 0);
	make_sidecar(active, argv[1], ".active");
	make_sidecar(capture, argv[1], ".capture");
	make_sidecar(delay, argv[1], ".delay");
	make_sidecar(failure, argv[1], ".fail");
	make_sidecar(leak, argv[1], ".env-leak");
	make_sidecar(snapshot_path, argv[1], ".snapshot");
	(void)unlink(active);
	(void)unlink(capture);
	(void)unlink(delay);
	(void)unlink(failure);
	(void)unlink(leak);

	verify_closed_gate(argv[1], argv[6], BLUETOOTH_GATE_PENDING);
	verify_closed_gate(argv[1], argv[3], BLUETOOTH_GATE_PENDING);
	verify_closed_gate(argv[1], argv[4], BLUETOOTH_GATE_REJECTED);
	verify_closed_gate(argv[1], argv[5], BLUETOOTH_GATE_REJECTED);
	verify_closed_gate(argv[1], argv[7], BLUETOOTH_GATE_REJECTED);

	write_text(snapshot_path, initial_snapshot);
	CHECK(setenv("LD_PRELOAD", "/fixture/must-not-leak.so", 1) == 0);
	CHECK(setenv("LD_LIBRARY_PATH", "/fixture/must-not-leak", 1) == 0);
	CHECK(setenv("DEVICE_CONTROL_TESTING", "must-not-leak", 1) == 0);
	CHECK(setenv("RG40XXV_BLUETOOTH_CAPABILITY", argv[2], 1) == 0);
	CHECK(bluetooth_backend_start(&backend, argv[1]) == 0);
	CHECK(backend != NULL);
	CHECK(bluetooth_backend_available(backend));
	memset(&snapshot, 0, sizeof(snapshot));
	wait_for_request(backend, 1U, &snapshot, &generation);
	CHECK(snapshot.gate == BLUETOOTH_GATE_ADMITTED);
	CHECK(snapshot.phase == BLUETOOTH_PHASE_READY);
	CHECK(snapshot.last_action == BLUETOOTH_ACTION_STATUS);
	CHECK(snapshot.device_count == 1U);
	CHECK(snapshot.adapter_present && snapshot.powered && !snapshot.discovering);
	CHECK(strcmp(snapshot.adapter_address, "10:22:33:44:55:66") == 0);
	CHECK(strcmp(snapshot.devices[0].address, "AA:BB:CC:DD:EE:FF") == 0);
	CHECK(strcmp(snapshot.devices[0].name, "Gamepad One") == 0);
	CHECK(snapshot.devices[0].kind == BLUETOOTH_DEVICE_CONTROLLER);

	write_text(snapshot_path, scan_snapshot);
	write_text(delay, "scan 300\n");
	started = monotonic_ms();
	CHECK(bluetooth_backend_request_scan(backend, &request_id) == 0);
	elapsed = monotonic_ms() - started;
	enqueue_elapsed = elapsed;
	CHECK(request_id == 2U);
	CHECK(elapsed < 100U);
	wait_for_working(backend, request_id, &snapshot, &generation);
	CHECK(snapshot.device_count == 1U && !snapshot.discovering);
	wait_for_request(backend, request_id, &snapshot, &generation);
	CHECK(snapshot.phase == BLUETOOTH_PHASE_READY && snapshot.discovering);
	(void)unlink(delay);

	write_text(snapshot_path, powered_off_snapshot);
	write_text(delay, "power 300\n");
	CHECK(bluetooth_backend_request_power(backend, false, &request_id) == 0);
	wait_for_working(backend, request_id, &snapshot, &generation);
	CHECK(snapshot.powered && snapshot.device_count == 1U);
	wait_for_request(backend, request_id, &snapshot, &generation);
	CHECK(!snapshot.powered && snapshot.device_count == 0U);
	(void)unlink(delay);

	write_text(snapshot_path, initial_snapshot);
	CHECK(bluetooth_backend_request_power(backend, true, &request_id) == 0);
	wait_for_request(backend, request_id, &snapshot, &generation);
	CHECK(snapshot.powered && snapshot.device_count == 1U);

	write_text(snapshot_path, paired_snapshot);
	write_text(failure, "pair\n");
	CHECK(bluetooth_backend_request_pair(backend, "aa:bb:cc:dd:ee:ff",
		&request_id) == 0);
	wait_for_request(backend, request_id, &snapshot, &generation);
	CHECK(snapshot.phase == BLUETOOTH_PHASE_ERROR);
	CHECK(snapshot.helper_exit_code == 7);
	CHECK(snapshot.last_error == EIO);
	CHECK(snapshot.device_count == 1U && !snapshot.devices[0].paired);
	(void)unlink(failure);

	CHECK(bluetooth_backend_request_pair(backend, "aa:bb:cc:dd:ee:ff",
		&request_id) == 0);
	wait_for_request(backend, request_id, &snapshot, &generation);
	CHECK(snapshot.phase == BLUETOOTH_PHASE_READY);
	CHECK(snapshot.devices[0].paired && snapshot.devices[0].trusted &&
		!snapshot.devices[0].connected);

	write_text(snapshot_path, connected_snapshot);
	CHECK(bluetooth_backend_request_connect(backend, "AA:BB:CC:DD:EE:FF",
		&request_id) == 0);
	wait_for_request(backend, request_id, &snapshot, &generation);
	CHECK(snapshot.devices[0].paired && snapshot.devices[0].connected);

	previous_generation = snapshot.generation;
	write_text(snapshot_path, duplicate_snapshot);
	CHECK(bluetooth_backend_request_status(backend, &request_id) == 0);
	wait_for_request(backend, request_id, &snapshot, &generation);
	CHECK(snapshot.phase == BLUETOOTH_PHASE_ERROR);
	CHECK(snapshot.last_error == EEXIST);
	CHECK(snapshot.devices[0].connected);
	CHECK(snapshot.generation > previous_generation);

	write_oversized_snapshot(snapshot_path);
	CHECK(bluetooth_backend_request_status(backend, &request_id) == 0);
	wait_for_request(backend, request_id, &snapshot, &generation);
	CHECK(snapshot.phase == BLUETOOTH_PHASE_ERROR);
	CHECK(snapshot.last_error == E2BIG);
	CHECK(snapshot.device_count == 1U && snapshot.devices[0].connected);

	write_text(snapshot_path, connected_snapshot);
	CHECK(bluetooth_backend_request_status(backend, &request_id) == 0);
	wait_for_request(backend, request_id, &snapshot, &generation);
	CHECK(snapshot.phase == BLUETOOTH_PHASE_READY && snapshot.last_error == 0);

	write_text(snapshot_path, paired_snapshot);
	CHECK(bluetooth_backend_request_disconnect(backend,
		"AA:BB:CC:DD:EE:FF", &request_id) == 0);
	wait_for_request(backend, request_id, &snapshot, &generation);
	CHECK(snapshot.devices[0].paired && !snapshot.devices[0].connected);

	write_text(snapshot_path, empty_snapshot);
	CHECK(bluetooth_backend_request_forget(backend, "AA:BB:CC:DD:EE:FF",
		&request_id) == 0);
	wait_for_request(backend, request_id, &snapshot, &generation);
	CHECK(snapshot.device_count == 0U);

	request_id = 44U;
	CHECK(bluetooth_backend_request_connect(backend, "AA:BB;CC:DD:EE:FF",
		&request_id) == EINVAL);
	CHECK(request_id == 0U);
	CHECK(bluetooth_backend_request_disconnect(backend, NULL,
		&request_id) == EINVAL);
	CHECK(request_id == 0U);

	write_text(delay, "status 5000\n");
	(void)unlink(active);
	CHECK(bluetooth_backend_request_status(backend, &request_id) == 0);
	wait_for_file(active);
	for (size_t i = 0U; i < 32U; ++i) {
		uint64_t queued_id = 0U;

		CHECK(bluetooth_backend_request_scan(backend, &queued_id) == 0);
		CHECK(queued_id != 0U);
	}
	request_id = 123U;
	CHECK(bluetooth_backend_request_scan(backend, &request_id) == EAGAIN);
	CHECK(request_id == 0U);
	started = monotonic_ms();
	bluetooth_backend_stop(&backend);
	elapsed = monotonic_ms() - started;
	CHECK(backend == NULL);
	CHECK(elapsed < 1000U);
	CHECK(access(leak, F_OK) != 0);
	CHECK(access(capture, R_OK) == 0);

	SDL_Quit();
	(void)printf("BLUETOOTH_BACKEND_TEST PASS enqueue_ms=%llu stop_ms=%llu\n",
		(unsigned long long)enqueue_elapsed, (unsigned long long)elapsed);
	return 0;
}

#endif
