#ifndef RG40XXV_NETWORK_UI_H
#define RG40XXV_NETWORK_UI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	NETWORK_UI_MAX_ACCESS_POINTS = 32,
	NETWORK_UI_BSSID_SIZE = 18,
	NETWORK_UI_UUID_SIZE = 37,
	NETWORK_UI_SSID_SIZE = 97,
	NETWORK_UI_SECURITY_SIZE = 65,
	NETWORK_UI_PASSWORD_SIZE = 64,
	NETWORK_UI_ADDRESS_SIZE = 65,
	NETWORK_UI_PATH_SIZE = 4096,
};

enum network_ui_mode {
	NETWORK_UI_WIFI,
	NETWORK_UI_HOTSPOT,
	NETWORK_UI_USB_DEBUG,
	NETWORK_UI_BLUETOOTH,
	NETWORK_UI_MODE_COUNT,
};

enum network_ui_action {
	NETWORK_UI_ACTION_STATUS,
	NETWORK_UI_ACTION_SCAN,
	NETWORK_UI_ACTION_CONNECT,
	NETWORK_UI_ACTION_DISCONNECT,
	NETWORK_UI_ACTION_FORGET,
	NETWORK_UI_ACTION_HOTSPOT_SET,
	NETWORK_UI_ACTION_RECOVER,
};

struct network_ui_request {
	enum network_ui_action action;
	enum network_ui_mode mode;
	char bssid[NETWORK_UI_BSSID_SIZE];
	char uuid[NETWORK_UI_UUID_SIZE];
	char password[NETWORK_UI_PASSWORD_SIZE];
	bool enabled;
};

typedef int (*network_ui_action_callback)(
	void *context, const struct network_ui_request *request);

struct network_access_point {
	char bssid[NETWORK_UI_BSSID_SIZE];
	char uuid[NETWORK_UI_UUID_SIZE];
	char ssid[NETWORK_UI_SSID_SIZE];
	char security[NETWORK_UI_SECURITY_SIZE];
	int signal;
	bool known;
	bool active;
};

/*
 * The helper owns NetworkManager and atomically publishes snapshot.v1. The UI
 * accepts a bounded, root-owned snapshot and sends typed requests only.
 */
struct network_ui_state {
	enum network_ui_mode selected;
	network_ui_action_callback action;
	void *action_context;
	struct network_access_point access_points[NETWORK_UI_MAX_ACCESS_POINTS];
	size_t access_point_count;
	size_t selected_access_point;
	uint64_t generation;
	char snapshot_path[NETWORK_UI_PATH_SIZE];
	char active_uuid[NETWORK_UI_UUID_SIZE];
	char connected_bssid[NETWORK_UI_BSSID_SIZE];
	char connected_ssid[NETWORK_UI_SSID_SIZE];
	char ip_address[NETWORK_UI_ADDRESS_SIZE];
	char hotspot_ssid[NETWORK_UI_SSID_SIZE];
	char password[NETWORK_UI_PASSWORD_SIZE];
	char password_bssid[NETWORK_UI_BSSID_SIZE];
	uint64_t scan_navigation_epoch;
	uint64_t status_request_id;
	uint32_t status_refresh_at;
	int last_snapshot_error;
	bool backend_available;
	bool wifi_present;
	bool wifi_enabled;
	bool wifi_connected;
	bool hotspot_enabled;
	bool detail_active;
	bool password_active;
	bool scan_pending;
	bool status_pending;
};

void network_ui_state_init(struct network_ui_state *state);
int network_ui_state_set_snapshot_path(struct network_ui_state *state,
				       const char *path);
int network_ui_state_load_snapshot(struct network_ui_state *state);
/*
 * A completed scan publishes the same authenticated snapshot format as the
 * other network actions.  Scan completion must only replace the AP collection:
 * connection/hotspot state is committed by status-changing actions, while UI
 * navigation, password entry and the selected BSSID remain resident.
 */
int network_ui_state_load_access_points(struct network_ui_state *state);
void network_ui_state_set_backend(struct network_ui_state *state,
				  network_ui_action_callback action,
				  void *context);
void network_ui_state_select(struct network_ui_state *state, int direction);
const struct network_access_point *network_ui_state_selected_ap(
	const struct network_ui_state *state);
int network_ui_state_request(struct network_ui_state *state,
			     const struct network_ui_request *request);

#endif
