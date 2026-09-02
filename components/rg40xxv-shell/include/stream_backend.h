#ifndef RG40XXV_STREAM_BACKEND_H
#define RG40XXV_STREAM_BACKEND_H

#include <SDL.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "netstream.h"

enum stream_backend_phase {
	STREAM_BACKEND_STOPPED,
	STREAM_BACKEND_LOADING,
	STREAM_BACKEND_DISCOVERING,
	STREAM_BACKEND_READY,
	STREAM_BACKEND_SAVING,
	STREAM_BACKEND_PAIRING,
	STREAM_BACKEND_PAIRED,
	STREAM_BACKEND_PENDING,
	STREAM_BACKEND_ERROR,
};

struct stream_backend_snapshot {
	NsHostDb hosts;
	uint64_t generation;
	uint32_t discovery_ms;
	size_t discovered_count;
	enum stream_backend_phase phase;
	char pin[5];
	char detail[NS_ERROR_MAX];
	bool store_loaded;
};

struct stream_backend;

int stream_backend_start(struct stream_backend **backend,
			 const char *state_dir, const char *launcher_path);
int stream_backend_request_discovery(struct stream_backend *backend);
int stream_backend_request_pair(struct stream_backend *backend,
				const NsHost *host);
int stream_backend_request_settings_save(struct stream_backend *backend,
					 const NsHost *host);
int stream_backend_poll(struct stream_backend *backend,
			struct stream_backend_snapshot *snapshot,
			uint64_t *last_generation);
void stream_backend_stop(struct stream_backend **backend);

#ifdef RG40XXV_STREAM_BACKEND_TESTING
int stream_backend_test_parse_mdns(const unsigned char *packet,
				   size_t packet_size, NsHostDb *hosts);
#endif

#endif
