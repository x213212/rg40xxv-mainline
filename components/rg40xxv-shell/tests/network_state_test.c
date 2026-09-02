#include "network_ui.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

struct capture {
	struct network_ui_request request;
	int calls;
};

static int capture_action(void *context,
			  const struct network_ui_request *request)
{
	struct capture *capture = context;

	capture->request = *request;
	++capture->calls;
	return 0;
}

int main(int argc, char **argv)
{
	struct network_ui_state state;
	struct capture capture = { 0 };
	struct network_ui_request request = {
		.action = NETWORK_UI_ACTION_CONNECT,
		.mode = NETWORK_UI_WIFI,
	};
	const struct network_access_point *point;
	uint64_t generation;

	assert(argc == 5);
	network_ui_state_init(&state);
	assert(state.selected == NETWORK_UI_WIFI);
	assert(!state.backend_available);
	assert(network_ui_state_request(&state, &request) == ENODEV);
	assert(network_ui_state_set_snapshot_path(&state, argv[1]) == 0);
	assert(network_ui_state_load_snapshot(&state) == 0);
	assert(state.wifi_present);
	assert(state.wifi_enabled);
	assert(state.wifi_connected);
	assert(!state.hotspot_enabled);
	assert(strcmp(state.connected_ssid, "Home:WiFi") == 0);
	assert(strcmp(state.ip_address, "192.168.0.125/24") == 0);
	assert(state.access_point_count == 2U);
	point = network_ui_state_selected_ap(&state);
	assert(point != NULL);
	assert(strcmp(point->bssid, "AA:BB:CC:DD:EE:FF") == 0);
	assert(strcmp(point->ssid, "Home:WiFi") == 0);
	assert(point->signal == 78);
	assert(point->known);
	assert(point->active);
	network_ui_state_select(&state, 1);
	assert(state.selected == NETWORK_UI_HOTSPOT);
	network_ui_state_select(&state, -1);
	assert(state.selected == NETWORK_UI_WIFI);
	network_ui_state_set_backend(&state, capture_action, &capture);
	assert(state.backend_available);
	(void)snprintf(request.bssid, sizeof(request.bssid), "%s",
		"11:22:33:44:55:66");
	(void)snprintf(request.password, sizeof(request.password), "%s",
		"fixture-secret");
	assert(network_ui_state_request(&state, &request) == 0);
	assert(capture.calls == 1);
	assert(capture.request.action == NETWORK_UI_ACTION_CONNECT);
	assert(capture.request.mode == NETWORK_UI_WIFI);
	assert(strcmp(capture.request.bssid, "11:22:33:44:55:66") == 0);
	assert(strcmp(capture.request.password, "fixture-secret") == 0);

	/*
	 * A scan is an AP-list delta, not a page/model replacement.  Reordering
	 * the rows must keep the selected BSSID and all resident UI/confirmed
	 * connection state.  A later status-changing action may commit the full
	 * snapshot from the same trusted file.
	 */
	state.selected_access_point = 1U;
	state.detail_active = true;
	state.password_active = true;
	(void)snprintf(state.password, sizeof(state.password), "%s",
		"password-in-progress");
	(void)snprintf(state.password_bssid, sizeof(state.password_bssid), "%s",
		"11:22:33:44:55:66");
	generation = state.generation;
	assert(network_ui_state_set_snapshot_path(&state, argv[2]) == 0);
	assert(network_ui_state_load_access_points(&state) == 0);
	assert(state.generation == generation + 1U);
	assert(state.selected == NETWORK_UI_WIFI);
	assert(state.detail_active);
	assert(state.password_active);
	assert(strcmp(state.password, "password-in-progress") == 0);
	assert(strcmp(state.password_bssid, "11:22:33:44:55:66") == 0);
	assert(state.backend_available && state.action == capture_action &&
		state.action_context == &capture);
	assert(state.wifi_connected);
	assert(!state.hotspot_enabled);
	assert(strcmp(state.connected_ssid, "Home:WiFi") == 0);
	assert(strcmp(state.ip_address, "192.168.0.125/24") == 0);
	assert(state.access_point_count == 2U);
	assert(state.selected_access_point == 0U);
	point = network_ui_state_selected_ap(&state);
	assert(point != NULL);
	assert(strcmp(point->bssid, "11:22:33:44:55:66") == 0);
	assert(strcmp(point->ssid, "Cafe Net") == 0);
	assert(point->signal == 88);

	assert(network_ui_state_load_snapshot(&state) == 0);
	assert(!state.wifi_connected);
	assert(!state.hotspot_enabled);
	assert(state.connected_ssid[0] == '\0');
	assert(state.ip_address[0] == '\0');
	assert(state.selected_access_point == 0U);
	assert(state.detail_active && state.password_active);
	assert(strcmp(state.password, "password-in-progress") == 0);

	assert(network_ui_state_set_snapshot_path(&state, argv[3]) == 0);
	assert(network_ui_state_load_snapshot(&state) == EACCES);
	assert(state.access_point_count == 2U);
	assert(network_ui_state_set_snapshot_path(&state, argv[4]) == 0);
	assert(network_ui_state_load_snapshot(&state) == ELOOP);
	assert(state.access_point_count == 2U);
	puts("NETWORK_STATE_TEST PASS bounded snapshot, AP-only incremental apply, BSSID focus, typed action API");
	return 0;
}
