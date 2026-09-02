#include "ui.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

struct request_capture {
	struct network_ui_request request;
	int calls;
};

static unsigned int status_requests;
static unsigned int bluetooth_status_requests;
static uint64_t bluetooth_status_request_id = 700U;
static bool bluetooth_available;
static bool bluetooth_poll_ready;
static struct bluetooth_backend_snapshot bluetooth_poll_snapshot;

int settings_request_network_status(struct settings_state *settings)
{
	settings->last_request_id = 100U + ++status_requests;
	return 0;
}

static int capture_action(void *context,
			  const struct network_ui_request *request)
{
	struct request_capture *capture = context;

	capture->request = *request;
	++capture->calls;
	return 0;
}

const char *tr(const struct ui *ui, const char *key)
{
	(void)ui;
	return key;
}

void render_activate(struct ui *ui, const char *message, uint32_t now)
{
	ui->action_text = message;
	ui->action_until = now + 1U;
}

void settings_ui_track_pending(struct ui *ui,
			       enum settings_pending_kind kind, int value)
{
	(void)ui;
	(void)kind;
	(void)value;
}

bool bluetooth_backend_available(struct bluetooth_backend *backend)
{
	(void)backend;
	return bluetooth_available;
}

int bluetooth_backend_request_status(struct bluetooth_backend *backend,
				     uint64_t *request_id)
{
	(void)backend;
	++bluetooth_status_requests;
	*request_id = ++bluetooth_status_request_id;
	return 0;
}

int bluetooth_backend_poll(struct bluetooth_backend *backend,
			   struct bluetooth_backend_snapshot *snapshot,
			   uint64_t *last_generation)
{
	(void)backend;
	if (!bluetooth_poll_ready)
		return 0;
	*snapshot = bluetooth_poll_snapshot;
	*last_generation = bluetooth_poll_snapshot.generation;
	bluetooth_poll_ready = false;
	return 1;
}

int bluetooth_backend_request_power(struct bluetooth_backend *backend,
				    bool powered, uint64_t *request_id)
{
	(void)backend; (void)powered; (void)request_id; return ENODEV;
}

int bluetooth_backend_request_pair(struct bluetooth_backend *backend,
				   const char *address, uint64_t *request_id)
{
	(void)backend; (void)address; (void)request_id; return ENODEV;
}

int bluetooth_backend_request_connect(struct bluetooth_backend *backend,
				      const char *address, uint64_t *request_id)
{
	(void)backend; (void)address; (void)request_id; return ENODEV;
}

int bluetooth_backend_request_disconnect(struct bluetooth_backend *backend,
					 const char *address, uint64_t *request_id)
{
	(void)backend; (void)address; (void)request_id; return ENODEV;
}

int bluetooth_backend_request_scan(struct bluetooth_backend *backend,
				   uint64_t *request_id)
{
	(void)backend; (void)request_id; return ENODEV;
}

int bluetooth_backend_request_forget(struct bluetooth_backend *backend,
				     const char *address, uint64_t *request_id)
{
	(void)backend; (void)address; (void)request_id; return ENODEV;
}

int main(void)
{
	struct ui ui;
	struct request_capture capture = { 0 };
	struct network_access_point *point;

	memset(&ui, 0, sizeof(ui));
	network_ui_state_init(&ui.network);
	network_ui_state_set_backend(&ui.network, capture_action, &capture);
	input_method_init(&ui.input_method, INPUT_FIELD_TEXT);
	ui.network.access_point_count = 2U;
	ui.nav_index = NAV_PAGE_NETWORK;
	assert(network_ui_tick(&ui, 1U));
	assert(ui.network.status_pending);
	assert(ui.network.status_request_id == 101U);
	assert(!network_ui_tick(&ui, 2U));
	ui.network.status_pending = false;
	/* A missing/disabled radio requests bounded recovery, not a fake scan. */
	ui.network.generation = 1U;
	ui.network.wifi_present = false;
	network_ui_activate(&ui, 1U);
	assert(!ui.network.detail_active);
	assert(capture.calls == 1);
	assert(capture.request.action == NETWORK_UI_ACTION_RECOVER);
	ui.network.wifi_present = true;
	ui.network.wifi_enabled = false;
	network_ui_activate(&ui, 1U);
	assert(!ui.network.detail_active);
	assert(capture.calls == 2);
	assert(capture.request.action == NETWORK_UI_ACTION_RECOVER);
	capture.calls = 0;
	ui.network.wifi_enabled = true;
	point = &ui.network.access_points[0];
	(void)snprintf(point->bssid, sizeof(point->bssid), "%s",
		"11:22:33:44:55:66");
	(void)snprintf(point->ssid, sizeof(point->ssid), "%s", "Cafe Net");
	(void)snprintf(point->security, sizeof(point->security), "%s", "WPA2");
	point->signal = 64;
	point = &ui.network.access_points[1];
	(void)snprintf(point->bssid, sizeof(point->bssid), "%s",
		"AA:BB:CC:DD:EE:FF");
	(void)snprintf(point->uuid, sizeof(point->uuid), "%s",
		"550e8400-e29b-41d4-a716-446655440000");
	(void)snprintf(point->ssid, sizeof(point->ssid), "%s", "Home WiFi");
	(void)snprintf(point->security, sizeof(point->security), "%s", "WPA2");
	point->known = true;

	/* A on Wi-Fi opens the AP list and starts a real scan request. */
	network_ui_activate(&ui, 1U);
	assert(ui.network.detail_active);
	assert(ui.network.scan_pending);
	assert(ui.network.scan_navigation_epoch == ui.navigation_epoch);
	assert(capture.calls == 1);
	assert(capture.request.action == NETWORK_UI_ACTION_SCAN);
	/* Repeated refresh input cannot queue a second long-running scan. */
	network_ui_secondary(&ui, 2U);
	assert(capture.calls == 1);
	assert(strcmp(ui.action_text, "network_scan_pending") == 0);
	ui.network.scan_pending = false;
	ui.network.scan_navigation_epoch = 0U;
	network_ui_secondary(&ui, 2U);
	assert(ui.network.scan_pending);
	assert(capture.calls == 2);
	assert(capture.request.action == NETWORK_UI_ACTION_SCAN);

	/* An unknown WPA AP opens the password keyboard; Start submits it. */
	network_ui_activate(&ui, 3U);
	assert(ui.network.password_active);
	assert(ui.input_method.field == INPUT_FIELD_PASSWORD);
	assert(network_ui_password_append(&ui, "fixture-secret"));
	network_ui_password_submit(&ui, 4U);
	assert(capture.calls == 3);
	assert(capture.request.action == NETWORK_UI_ACTION_CONNECT);
	assert(strcmp(capture.request.bssid, "11:22:33:44:55:66") == 0);
	assert(strcmp(capture.request.password, "fixture-secret") == 0);
	assert(!ui.network.password_active);
	assert(ui.network.password[0] == '\0');
	assert(ui.input_method.field == INPUT_FIELD_TEXT);

	/* Y disconnects an active AP, then forgets the saved inactive AP. */
	ui.network.selected_access_point = 1U;
	ui.network.access_points[1].active = true;
	network_ui_tertiary(&ui, 5U);
	assert(capture.calls == 4);
	assert(capture.request.action == NETWORK_UI_ACTION_DISCONNECT);
	ui.network.access_points[1].active = false;
	network_ui_tertiary(&ui, 6U);
	assert(capture.calls == 5);
	assert(capture.request.action == NETWORK_UI_ACTION_FORGET);
	assert(strcmp(capture.request.uuid,
		"550e8400-e29b-41d4-a716-446655440000") == 0);

	assert(network_ui_back(&ui));
	assert(!ui.network.detail_active);
	ui.network.selected = NETWORK_UI_HOTSPOT;
	network_ui_secondary(&ui, 7U);
	assert(capture.calls == 6);
	assert(capture.request.action == NETWORK_UI_ACTION_STATUS);
	assert(capture.request.mode == NETWORK_UI_HOTSPOT);
	network_ui_activate(&ui, 7U);
	assert(capture.calls == 7);
	assert(capture.request.action == NETWORK_UI_ACTION_HOTSPOT_SET);
	assert(capture.request.enabled);

	/* Bluetooth refresh is visible-page-only, single-flight and silent. */
	ui.network.selected = NETWORK_UI_BLUETOOTH;
	ui.bluetooth.detail_active = true;
	ui.bluetooth.snapshot.gate = BLUETOOTH_GATE_ADMITTED;
	ui.bluetooth.snapshot.phase = BLUETOOTH_PHASE_READY;
	ui.bluetooth.snapshot.device_count = 2U;
	(void)snprintf(ui.bluetooth.snapshot.devices[0].address,
		sizeof(ui.bluetooth.snapshot.devices[0].address), "%s",
		"11:22:33:44:55:66");
	(void)snprintf(ui.bluetooth.snapshot.devices[1].address,
		sizeof(ui.bluetooth.snapshot.devices[1].address), "%s",
		"AA:BB:CC:DD:EE:FF");
	ui.bluetooth.selected_row = 2U;
	ui.bluetooth.status_refresh_at = 0U;
	bluetooth_available = true;
	assert(!bluetooth_ui_update(&ui, 100U));
	assert(bluetooth_status_requests == 1U);
	assert(ui.bluetooth.pending_silent);
	assert(ui.bluetooth.pending_request_id == bluetooth_status_request_id);
	assert(!bluetooth_ui_update(&ui, 101U));
	assert(bluetooth_status_requests == 1U);
	ui.action_text = "sentinel";
	bluetooth_poll_snapshot = ui.bluetooth.snapshot;
	bluetooth_poll_snapshot.generation = 2U;
	bluetooth_poll_snapshot.phase = BLUETOOTH_PHASE_READY;
	bluetooth_poll_snapshot.completed_request_id =
		ui.bluetooth.pending_request_id;
	bluetooth_poll_snapshot.devices[0] = ui.bluetooth.snapshot.devices[1];
	bluetooth_poll_snapshot.devices[1] = ui.bluetooth.snapshot.devices[0];
	bluetooth_poll_ready = true;
	assert(bluetooth_ui_update(&ui, 102U));
	assert(ui.bluetooth.selected_row == 1U);
	assert(ui.bluetooth.pending_request_id == 0U);
	assert(!ui.bluetooth.pending_silent);
	assert(strcmp(ui.action_text, "sentinel") == 0);
	assert(!bluetooth_ui_update(&ui, 103U));
	assert(bluetooth_status_requests == 1U);

	puts("NETWORK_FLOW_TEST PASS truthful recover + nonblocking single-flight Wi-Fi/Bluetooth flow");
	return 0;
}
