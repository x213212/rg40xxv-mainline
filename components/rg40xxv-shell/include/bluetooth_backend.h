#ifndef RG40XXV_BLUETOOTH_BACKEND_H
#define RG40XXV_BLUETOOTH_BACKEND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	BLUETOOTH_BACKEND_MAX_DEVICES = 16,
	BLUETOOTH_BACKEND_ADDRESS_SIZE = 18,
	BLUETOOTH_BACKEND_NAME_SIZE = 97,
};

enum bluetooth_backend_gate {
	BLUETOOTH_GATE_PENDING,
	BLUETOOTH_GATE_ADMITTED,
	BLUETOOTH_GATE_REJECTED,
};

enum bluetooth_backend_phase {
	BLUETOOTH_PHASE_STOPPED,
	BLUETOOTH_PHASE_PENDING,
	BLUETOOTH_PHASE_LOADING,
	BLUETOOTH_PHASE_READY,
	BLUETOOTH_PHASE_WORKING,
	BLUETOOTH_PHASE_ERROR,
};

enum bluetooth_backend_action {
	BLUETOOTH_ACTION_NONE,
	BLUETOOTH_ACTION_STATUS,
	BLUETOOTH_ACTION_SCAN,
	BLUETOOTH_ACTION_POWER,
	BLUETOOTH_ACTION_PAIR,
	BLUETOOTH_ACTION_CONNECT,
	BLUETOOTH_ACTION_DISCONNECT,
	BLUETOOTH_ACTION_FORGET,
};

enum bluetooth_device_kind {
	BLUETOOTH_DEVICE_UNKNOWN,
	BLUETOOTH_DEVICE_CONTROLLER,
	BLUETOOTH_DEVICE_AUDIO,
	BLUETOOTH_DEVICE_KEYBOARD,
	BLUETOOTH_DEVICE_MOUSE,
};

struct bluetooth_device_snapshot {
	char address[BLUETOOTH_BACKEND_ADDRESS_SIZE];
	char name[BLUETOOTH_BACKEND_NAME_SIZE];
	enum bluetooth_device_kind kind;
	int rssi;
	bool paired;
	bool trusted;
	bool connected;
};

struct bluetooth_backend_snapshot {
	struct bluetooth_device_snapshot devices[BLUETOOTH_BACKEND_MAX_DEVICES];
	size_t device_count;
	uint64_t generation;
	uint64_t active_request_id;
	uint64_t completed_request_id;
	enum bluetooth_backend_gate gate;
	enum bluetooth_backend_phase phase;
	enum bluetooth_backend_action last_action;
	int last_error;
	int helper_exit_code;
	int helper_term_signal;
	char adapter_address[BLUETOOTH_BACKEND_ADDRESS_SIZE];
	bool adapter_present;
	bool powered;
	bool discovering;
};

struct bluetooth_backend;

/*
 * RG40XXV_BLUETOOTH_CAPABILITY may override the production capability path.
 * A missing file or status=PENDING produces a PENDING startup snapshot.  Only
 * these two newline-terminated records (in either order) admit the helper:
 *
 * schema=rg40xxv-bluetooth-runtime-admission-v1
 * status=PASS
 *
 * Once admitted, requests are bounded O(1) enqueues.  A single worker invokes
 * the absolute helper path directly, never through a shell, with exactly one
 * of the following argv tails:
 *
 * status | scan | power on|off | pair MAC | connect MAC |
 * disconnect MAC | forget MAC
 *
 * A successful helper prints one complete, newline-terminated snapshot:
 *
 * RG40XXV_BLUETOOTH_SNAPSHOT<TAB>2
 * A<TAB>present<TAB>powered<TAB>discovering<TAB>adapter-MAC-or-dash
 * D<TAB>MAC<TAB>percent-encoded-name<TAB>kind<TAB>paired<TAB>trusted<TAB>connected<TAB>rssi
 *
 * There may be at most BLUETOOTH_BACKEND_MAX_DEVICES D records.  Nonzero
 * helper exit, timeout, oversized output, or an invalid snapshot publishes
 * error metadata while retaining the last confirmed adapter/device state.
 */
int bluetooth_backend_start(struct bluetooth_backend **backend,
			    const char *helper_path);
bool bluetooth_backend_available(struct bluetooth_backend *backend);

int bluetooth_backend_request_status(struct bluetooth_backend *backend,
				     uint64_t *request_id);
int bluetooth_backend_request_scan(struct bluetooth_backend *backend,
				   uint64_t *request_id);
int bluetooth_backend_request_power(struct bluetooth_backend *backend,
				    bool powered, uint64_t *request_id);
int bluetooth_backend_request_pair(struct bluetooth_backend *backend,
				   const char *address, uint64_t *request_id);
int bluetooth_backend_request_connect(struct bluetooth_backend *backend,
				      const char *address,
				      uint64_t *request_id);
int bluetooth_backend_request_disconnect(struct bluetooth_backend *backend,
					 const char *address,
					 uint64_t *request_id);
int bluetooth_backend_request_forget(struct bluetooth_backend *backend,
				     const char *address, uint64_t *request_id);

int bluetooth_backend_poll(struct bluetooth_backend *backend,
			   struct bluetooth_backend_snapshot *snapshot,
			   uint64_t *last_generation);
void bluetooth_backend_stop(struct bluetooth_backend **backend);

#endif
