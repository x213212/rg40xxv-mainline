#define _POSIX_C_SOURCE 200809L

#include "stream_backend.h"

#include <SDL.h>
#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t monotonic_ns(void)
{
	struct timespec value;

	assert(clock_gettime(CLOCK_MONOTONIC, &value) == 0);
	return (uint64_t)value.tv_sec * 1000000000ULL +
		(uint64_t)value.tv_nsec;
}

static struct stream_backend_snapshot wait_detail(
	struct stream_backend *backend, uint64_t *generation, const char *detail)
{
	struct stream_backend_snapshot snapshot;

	memset(&snapshot, 0, sizeof(snapshot));
	for (unsigned int attempt = 0U; attempt < 600U; ++attempt) {
		if (stream_backend_poll(backend, &snapshot, generation) > 0) {
			(void)fprintf(stderr,
				"STREAM_BACKEND_TEST phase=%d detail=%s hosts=%zu\n",
				(int)snapshot.phase, snapshot.detail,
				snapshot.hosts.count);
			if (strcmp(snapshot.detail, detail) == 0)
				return snapshot;
		}
		SDL_Delay(10U);
	}
	assert(!"stream backend result timeout");
	return snapshot;
}

static NsHostDb load_saved(const char *state_dir)
{
	NsStore store = { .dir_fd = -1, .lock_fd = -1 };
	NsHostDb hosts;
	char error[NS_ERROR_MAX] = { 0 };

	assert(ns_store_open(&store, state_dir, error, sizeof(error)) == 0);
	assert(ns_hosts_load(&store, &hosts, error, sizeof(error)) == 0);
	ns_store_close(&store);
	return hosts;
}

static void append_u16(unsigned char *packet, size_t *length, unsigned int value)
{
	packet[(*length)++] = (unsigned char)(value >> 8);
	packet[(*length)++] = (unsigned char)value;
}

static void append_u32(unsigned char *packet, size_t *length, uint32_t value)
{
	packet[(*length)++] = (unsigned char)(value >> 24);
	packet[(*length)++] = (unsigned char)(value >> 16);
	packet[(*length)++] = (unsigned char)(value >> 8);
	packet[(*length)++] = (unsigned char)value;
}

static void append_name(unsigned char *packet, size_t *length,
			const char *const *labels, size_t count)
{
	for (size_t i = 0U; i < count; ++i) {
		size_t label_length = strlen(labels[i]);

		assert(label_length > 0U && label_length <= 63U);
		packet[(*length)++] = (unsigned char)label_length;
		memcpy(packet + *length, labels[i], label_length);
		*length += label_length;
	}
	packet[(*length)++] = 0U;
}

static void test_mdns_parser(void)
{
	static const char *const service_type[] = {
		"_nvstream", "_tcp", "local",
	};
	static const char *const instance[] = {
		"LabRig", "_nvstream", "_tcp", "local",
	};
	static const char *const target[] = { "lab-rig", "local" };
	unsigned char packet[1024] = { 0 };
	unsigned char malformed[18] = { 0 };
	NsHostDb hosts;
	size_t length = 12U;
	size_t data_length_offset;
	size_t data_started;

	packet[2] = 0x84U;
	packet[7] = 3U;
	append_name(packet, &length, service_type, 3U);
	append_u16(packet, &length, 12U);
	append_u16(packet, &length, 1U);
	append_u32(packet, &length, 120U);
	data_length_offset = length;
	append_u16(packet, &length, 0U);
	data_started = length;
	append_name(packet, &length, instance, 4U);
	packet[data_length_offset] = (unsigned char)((length - data_started) >> 8);
	packet[data_length_offset + 1U] = (unsigned char)(length - data_started);

	append_name(packet, &length, instance, 4U);
	append_u16(packet, &length, 33U);
	append_u16(packet, &length, 1U);
	append_u32(packet, &length, 120U);
	data_length_offset = length;
	append_u16(packet, &length, 0U);
	data_started = length;
	append_u16(packet, &length, 0U);
	append_u16(packet, &length, 0U);
	append_u16(packet, &length, 47989U);
	append_name(packet, &length, target, 2U);
	packet[data_length_offset] = (unsigned char)((length - data_started) >> 8);
	packet[data_length_offset + 1U] = (unsigned char)(length - data_started);

	append_name(packet, &length, target, 2U);
	append_u16(packet, &length, 1U);
	append_u16(packet, &length, 1U);
	append_u32(packet, &length, 120U);
	append_u16(packet, &length, 4U);
	packet[length++] = 10U;
	packet[length++] = 23U;
	packet[length++] = 45U;
	packet[length++] = 67U;
	assert(stream_backend_test_parse_mdns(packet, length, &hosts) == 0);
	assert(hosts.count == 1U);
	assert(strcmp(hosts.hosts[0].name, "LabRig") == 0);
	assert(strcmp(hosts.hosts[0].address, "10.23.45.67") == 0);

	malformed[7] = 1U;
	malformed[12] = 0xc0U;
	malformed[13] = 12U;
	assert(stream_backend_test_parse_mdns(malformed, sizeof(malformed),
					      &hosts) != 0);
}

int main(int argc, char **argv)
{
	struct stream_backend *backend = NULL;
	struct stream_backend_snapshot snapshot;
	NsHostDb saved;
	NsHost changed;
	uint64_t generation = 0U;
	uint64_t enqueue_started;
	uint64_t enqueue_us;
	uint64_t stop_started;
	uint64_t stop_ms;
	uint64_t max_poll_us = 0U;

	assert(argc == 4);
	assert(SDL_Init(SDL_INIT_EVENTS | SDL_INIT_TIMER) == 0);
	test_mdns_parser();
	assert(setenv("RG40XXV_STREAM_DISCOVERY_FIXTURE", argv[2], 1) == 0);
	assert(unsetenv("RG40XXV_STREAM_FIXTURE_DELAY_MS") == 0);
	assert(stream_backend_start(&backend, argv[1], argv[3]) == 0);
	assert(stream_backend_request_discovery(backend) == 0);
	snapshot = wait_detail(backend, &generation, "hosts-found");
	assert(snapshot.phase == STREAM_BACKEND_READY);
	assert(snapshot.store_loaded);
	assert(snapshot.hosts.count == 2U);
	assert(snapshot.discovered_count == 2U);

	changed = snapshot.hosts.hosts[0];
	changed.resolution.width = 1280U;
	changed.resolution.height = 720U;
	changed.resolution.custom = 0;
	changed.bitrate_kbps = 12000U;
	changed.fps = 30U;
	changed.codec = NS_CODEC_H264;
	changed.aspect = NS_ASPECT_STRETCH;
	enqueue_started = monotonic_ns();
	assert(stream_backend_request_settings_save(backend, &changed) == 0);
	enqueue_us = (monotonic_ns() - enqueue_started) / 1000U;
	assert(enqueue_us < 10000U);
	snapshot = wait_detail(backend, &generation, "settings-saved");
	assert(snapshot.hosts.hosts[0].resolution.width == 1280U);
	assert(snapshot.hosts.hosts[0].bitrate_kbps == 12000U);
	assert(snapshot.hosts.hosts[0].fps == 30U);
	assert(snapshot.hosts.hosts[0].aspect == NS_ASPECT_STRETCH);
	saved = load_saved(argv[1]);
	assert(saved.hosts[0].resolution.width == 1280U);
	assert(saved.hosts[0].bitrate_kbps == 12000U);
	assert(saved.hosts[0].fps == 30U);
	assert(saved.hosts[0].aspect == NS_ASPECT_STRETCH);

	changed = snapshot.hosts.hosts[0];
	changed.paired = 0;
	enqueue_started = monotonic_ns();
	assert(stream_backend_request_pair(backend, &changed) == 0);
	assert((monotonic_ns() - enqueue_started) / 1000U < 10000U);
	snapshot = wait_detail(backend, &generation, "pair-complete");
	assert(snapshot.phase == STREAM_BACKEND_PAIRED);
	assert(strlen(snapshot.pin) == 4U);
	assert(strcmp(snapshot.pin, "0000") != 0);
	for (size_t i = 0U; i < 4U; ++i)
		assert(isdigit((unsigned char)snapshot.pin[i]));
	saved = load_saved(argv[1]);
	assert(saved.hosts[0].paired == 1);
	stream_backend_stop(&backend);

	/* A slow discovery source must never hold the snapshot mutex or shutdown. */
	assert(setenv("RG40XXV_STREAM_FIXTURE_DELAY_MS", "2000", 1) == 0);
	generation = 0U;
	assert(stream_backend_start(&backend, argv[1], argv[3]) == 0);
	assert(stream_backend_request_discovery(backend) == 0);
	for (unsigned int attempt = 0U; attempt < 50U; ++attempt) {
		uint64_t started = monotonic_ns();

		(void)stream_backend_poll(backend, &snapshot, &generation);
		uint64_t elapsed = (monotonic_ns() - started) / 1000U;

		if (elapsed > max_poll_us)
			max_poll_us = elapsed;
		SDL_Delay(4U);
	}
	assert(max_poll_us < 10000U);
	stop_started = monotonic_ns();
	stream_backend_stop(&backend);
	stop_ms = (monotonic_ns() - stop_started) / 1000000U;
	assert(stop_ms < 800U);
	SDL_Quit();
	printf("STREAM_BACKEND_TEST PASS enqueue_us=%llu max_poll_us=%llu stop_ms=%llu pin=fixed-argv\n",
	       (unsigned long long)enqueue_us,
	       (unsigned long long)max_poll_us,
	       (unsigned long long)stop_ms);
	return 0;
}
