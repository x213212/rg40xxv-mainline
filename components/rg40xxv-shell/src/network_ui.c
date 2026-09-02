#include "ui.h"

#include <errno.h>
#include <stdio.h>

static const SDL_Color network_primary = { 238, 238, 238, 255 };
static const SDL_Color network_secondary = { 160, 160, 160, 255 };
static const SDL_Color network_accent = { 220, 220, 220, 255 };

static void clear_password(struct network_ui_state *state)
{
	volatile unsigned char *bytes = (volatile unsigned char *)state->password;

	for (size_t i = 0U; i < sizeof(state->password); ++i)
		bytes[i] = 0U;
	memset(state->password_bssid, 0, sizeof(state->password_bssid));
}

static bool access_point_is_open(const struct network_access_point *point)
{
	return point->security[0] == '\0' || strcmp(point->security, "--") == 0 ||
		strcmp(point->security, "NONE") == 0;
}

static bool access_point_supported(const struct network_access_point *point)
{
	return strstr(point->security, "WEP") == NULL &&
		strstr(point->security, "802.1X") == NULL &&
		strstr(point->security, "EAP") == NULL;
}

static void draw_network_card(struct ui *ui, enum network_ui_mode mode,
			      enum material_icon_id icon, int x, int width,
			      const char *title,
			      const char *status, const char *detail)
{
	bool selected = ui->focus_region == UI_FOCUS_CONTENT &&
		ui->network.selected == mode;
	SDL_Rect detail_clip = { x + 10, 260, width - 20, 46 };

	render_fill_round_rect(ui->renderer, x + 3, 153, width, 166, 18,
		(SDL_Color){ 0, 0, 0, 72 });
	render_fill_round_rect(ui->renderer, x, 148, width, 166, 18,
		selected ? (SDL_Color){ 52, 52, 52, 226 } :
		(SDL_Color){ 24, 24, 24, 214 });
	render_outline_round_rect(ui->renderer, x, 148, width, 166, 18,
		selected ? (SDL_Color){ 220, 220, 220, 156 } :
		(SDL_Color){ 180, 180, 180, 52 });
	render_fill_round_rect(ui->renderer, x + 10, 164, 34, 34, 11,
		selected ? (SDL_Color){ 78, 78, 78, 152 } :
		(SDL_Color){ 48, 48, 48, 130 });
	material_icon_draw(ui, icon, x + 17, 171, 20,
		selected ? network_accent : network_secondary, selected, selected);
	text_draw(ui, 0, title, x + 50, 171,
		selected ? network_primary : network_secondary);
	text_draw(ui, 1, status, x + 10, 218,
		selected ? network_accent : network_primary);
	(void)SDL_RenderSetClipRect(ui->renderer, &detail_clip);
	text_draw(ui, 0, detail, x + 10, 260, network_secondary);
	(void)SDL_RenderSetClipRect(ui->renderer, NULL);
}

static const char *bluetooth_device_kind_text(struct ui *ui,
				       enum bluetooth_device_kind kind)
{
	switch (kind) {
	case BLUETOOTH_DEVICE_CONTROLLER: return tr(ui, "bluetooth_controller");
	case BLUETOOTH_DEVICE_AUDIO: return tr(ui, "bluetooth_audio");
	case BLUETOOTH_DEVICE_KEYBOARD: return tr(ui, "bluetooth_keyboard");
	case BLUETOOTH_DEVICE_MOUSE: return tr(ui, "bluetooth_mouse");
	case BLUETOOTH_DEVICE_UNKNOWN: return tr(ui, "unknown");
	}
	return tr(ui, "unknown");
}

static const char *bluetooth_status_text(struct ui *ui)
{
	const struct bluetooth_backend_snapshot *snapshot =
		&ui->bluetooth.snapshot;

	if (snapshot->gate == BLUETOOTH_GATE_PENDING)
		return tr(ui, "bluetooth_runtime_unavailable");
	if (snapshot->gate == BLUETOOTH_GATE_REJECTED ||
	    snapshot->phase == BLUETOOTH_PHASE_ERROR)
		return tr(ui, "unavailable");
	if (snapshot->phase == BLUETOOTH_PHASE_LOADING ||
	    (snapshot->phase == BLUETOOTH_PHASE_WORKING &&
	     !ui->bluetooth.pending_silent))
		return tr(ui, "bluetooth_working");
	if (!snapshot->adapter_present)
		return tr(ui, "bluetooth_adapter_missing");
	return tr(ui, snapshot->powered ? "enabled" : "disabled");
}

static const char *wifi_status_text(struct ui *ui)
{
	const struct network_ui_state *network = &ui->network;

	if (!network->backend_available)
		return tr(ui, "unavailable");
	if (network->generation == 0U)
		return tr(ui, network->status_pending ?
			"network_status_checking" : "unavailable");
	if (!network->wifi_present)
		return tr(ui, "network_adapter_missing");
	if (!network->wifi_enabled)
		return tr(ui, "network_wifi_disabled");
	if (network->hotspot_enabled)
		return tr(ui, "network_hotspot_active");
	return tr(ui, network->wifi_connected ? "connected" : "disconnected");
}

static const char *hotspot_status_text(struct ui *ui)
{
	const struct network_ui_state *network = &ui->network;

	if (!network->backend_available)
		return tr(ui, "unavailable");
	if (network->generation == 0U)
		return tr(ui, network->status_pending ?
			"network_status_checking" : "unavailable");
	if (!network->wifi_present)
		return tr(ui, "network_adapter_missing");
	return tr(ui, network->hotspot_enabled ? "enabled" : "disabled");
}

static void render_bluetooth_detail(struct ui *ui, uint32_t now)
{
	const struct bluetooth_backend_snapshot *snapshot =
		&ui->bluetooth.snapshot;
	char summary[128];
	char adapter[128];
	size_t first = ui->bluetooth.selected_row > 4U ?
		ui->bluetooth.selected_row - 4U : 1U;

	(void)now;
	(void)snprintf(summary, sizeof(summary), "%s · %zu",
		tr(ui, "bluetooth_found"), snapshot->device_count);
	text_draw(ui, 2, tr(ui, "bluetooth"), 28, 94, network_primary);
	text_draw(ui, 0, summary, 28, 122, network_secondary);
	if (snapshot->adapter_present && snapshot->adapter_address[0] != '\0')
		(void)snprintf(adapter, sizeof(adapter), "%s · %s · %s",
			tr(ui, "bluetooth_adapter"), bluetooth_status_text(ui),
			snapshot->adapter_address);
	else
		(void)snprintf(adapter, sizeof(adapter), "%s · %s",
			tr(ui, "bluetooth_adapter"), bluetooth_status_text(ui));
	render_fill_round_rect(ui->renderer, 30, 151, 580, 43, 12,
		ui->bluetooth.selected_row == 0U ?
			(SDL_Color){ 58, 58, 58, 235 } :
			(SDL_Color){ 25, 25, 25, 218 });
	text_draw(ui, 1, adapter, 46, 161,
		ui->bluetooth.selected_row == 0U ? network_primary :
			network_secondary);
	for (size_t row = first; row <= snapshot->device_count && row < first + 5U;
	     ++row) {
		const struct bluetooth_device_snapshot *device =
			&snapshot->devices[row - 1U];
		bool selected = ui->bluetooth.selected_row == row;
		char detail[160];
		int y = 201 + (int)(row - first) * 43;
		const char *state = device->connected ? tr(ui, "connected") :
			device->paired && device->trusted ? tr(ui, "bluetooth_trusted") :
			device->paired ? tr(ui, "stream_paired") :
			tr(ui, "stream_not_paired");

		(void)snprintf(detail, sizeof(detail), "%s · %s · %s",
			device->name, bluetooth_device_kind_text(ui, device->kind), state);
		render_fill_round_rect(ui->renderer, 30, y, 580, 38, 11,
			selected ? (SDL_Color){ 54, 54, 54, 232 } :
				(SDL_Color){ 22, 22, 22, 210 });
		text_draw(ui, 0, detail, 46, y + 9,
			selected ? network_primary : network_secondary);
	}
}

static void render_wifi_detail(struct ui *ui)
{
	char summary[256];
	size_t first = ui->network.selected_access_point > 3U ?
		ui->network.selected_access_point - 3U : 0U;
	const char *status = ui->network.wifi_connected &&
		ui->network.connected_ssid[0] != '\0' ?
		ui->network.connected_ssid : wifi_status_text(ui);

	text_draw(ui, 2, tr(ui, "network_wifi_mode"), 28, 94, network_primary);
	(void)snprintf(summary, sizeof(summary), "%s · %zu · %s",
		status, ui->network.access_point_count,
		ui->network.ip_address[0] != '\0' ? ui->network.ip_address :
			tr(ui, "network_no_address"));
	text_draw(ui, 0, summary, 28, 122, network_secondary);
	if (ui->network.generation == 0U || !ui->network.wifi_present ||
	    !ui->network.wifi_enabled || ui->network.hotspot_enabled) {
		text_draw(ui, 1, wifi_status_text(ui), 46, 174,
			network_secondary);
		return;
	}
	if (ui->network.access_point_count == 0U) {
		text_draw(ui, 1, tr(ui, "network_no_access_points"), 46, 174,
			network_secondary);
		return;
	}
	for (size_t index = first;
	     index < ui->network.access_point_count && index < first + 5U;
	     ++index) {
		const struct network_access_point *point =
			&ui->network.access_points[index];
		bool selected = index == ui->network.selected_access_point;
		const char *state = point->active ? tr(ui, "connected") :
			point->known ? tr(ui, "network_saved") :
			tr(ui, "network_password_needed");
		char detail[320];
		int y = 151 + (int)(index - first) * 43;

		(void)snprintf(detail, sizeof(detail), "%s · %d%% · %s · %s",
			point->ssid, point->signal,
			point->security[0] != '\0' ? point->security :
				tr(ui, "network_open"), state);
		render_fill_round_rect(ui->renderer, 30, y, 580, 38, 11,
			selected ? (SDL_Color){ 54, 54, 54, 232 } :
				(SDL_Color){ 22, 22, 22, 210 });
		text_draw(ui, 0, detail, 46, y + 9,
			selected ? network_primary : network_secondary);
	}
}

void render_network_page(struct ui *ui, uint32_t now)
{
	const char *wifi_status = wifi_status_text(ui);
	const char *usb_status = ui->settings.usb_debug_available > 0 ?
		tr(ui, ui->settings.preferences.usb_debug_enabled ?
			"enabled" : "disabled") : tr(ui, "unavailable");
	const char *message = ui->action_until > now && ui->action_text != NULL ?
		ui->action_text : tr(ui, ui->network.backend_available ?
			"network_backend_ready" : "network_backend_required");
	char bluetooth_detail[96];
	size_t bluetooth_connected = 0U;
	SDL_Rect clip = { 28, 374, 584, 34 };

	render_glass_panel(ui->renderer, UI_LAYOUT_CONTENT_X, UI_LAYOUT_CONTENT_Y,
		UI_LAYOUT_CONTENT_WIDTH, UI_LAYOUT_CONTENT_HEIGHT,
		ui->focus_region == UI_FOCUS_CONTENT);
	if (ui->network.selected == NETWORK_UI_BLUETOOTH &&
	    ui->bluetooth.detail_active) {
		render_bluetooth_detail(ui, now);
		return;
	}
	if (ui->network.selected == NETWORK_UI_WIFI && ui->network.detail_active) {
		render_wifi_detail(ui);
		return;
	}
	text_draw(ui, 2, tr(ui, "network_title"), 28, 94,
		ui->focus_region == UI_FOCUS_CONTENT ? network_primary :
		network_secondary);
	text_draw(ui, 0, tr(ui, "network_subtitle"), 28, 121,
		network_secondary);
	for (size_t index = 0U;
	     index < ui->bluetooth.snapshot.device_count; ++index) {
		if (ui->bluetooth.snapshot.devices[index].connected)
			++bluetooth_connected;
	}
	(void)snprintf(bluetooth_detail, sizeof(bluetooth_detail),
		"%s %zu · %s %zu", tr(ui, "bluetooth_found"),
		ui->bluetooth.snapshot.device_count, tr(ui, "connected"),
		bluetooth_connected);
	draw_network_card(ui, NETWORK_UI_WIFI, MATERIAL_ICON_WIFI, 24, 140,
		tr(ui, "network_wifi_mode"), wifi_status,
		tr(ui, "network_wifi_detail"));
	draw_network_card(ui, NETWORK_UI_HOTSPOT, MATERIAL_ICON_WIFI, 174, 140,
		tr(ui, "network_hotspot_mode"),
		hotspot_status_text(ui),
		ui->network.hotspot_enabled && ui->network.hotspot_ssid[0] != '\0' ?
			ui->network.hotspot_ssid : tr(ui, "network_hotspot_detail"));
	draw_network_card(ui, NETWORK_UI_USB_DEBUG, MATERIAL_ICON_TUNE, 324, 140,
		tr(ui, "usb_debug"), usb_status,
		tr(ui, "network_usb_detail"));
	draw_network_card(ui, NETWORK_UI_BLUETOOTH, MATERIAL_ICON_CAST, 474, 140,
		tr(ui, "bluetooth"), bluetooth_status_text(ui), bluetooth_detail);
	(void)SDL_RenderSetClipRect(ui->renderer, &clip);
	text_draw(ui, 0, message, 28, 374, network_secondary);
	(void)SDL_RenderSetClipRect(ui->renderer, NULL);
}

void network_ui_select(struct ui *ui, int direction)
{
	if (ui->network.detail_active &&
	    ui->network.selected == NETWORK_UI_WIFI) {
		if (direction != 0 && ui->network.access_point_count > 0U) {
			if (direction < 0)
				ui->network.selected_access_point =
					ui->network.selected_access_point == 0U ?
					ui->network.access_point_count - 1U :
					ui->network.selected_access_point - 1U;
			else
				ui->network.selected_access_point =
					(ui->network.selected_access_point + 1U) %
					ui->network.access_point_count;
		}
		return;
	}
	if (ui->bluetooth.detail_active &&
	    ui->network.selected == NETWORK_UI_BLUETOOTH) {
		size_t count = ui->bluetooth.snapshot.device_count + 1U;

		if (direction < 0)
			ui->bluetooth.selected_row =
				ui->bluetooth.selected_row == 0U ? count - 1U :
				ui->bluetooth.selected_row - 1U;
		else if (direction > 0)
			ui->bluetooth.selected_row =
				(ui->bluetooth.selected_row + 1U) % count;
		return;
	}
	network_ui_state_select(&ui->network, direction);
}

static int submit_network_request(struct ui *ui,
				  const struct network_ui_request *request,
				  uint32_t now)
{
	int error;

	if (request->action == NETWORK_UI_ACTION_SCAN &&
	    ui->network.scan_pending) {
		render_activate(ui, tr(ui, "network_scan_pending"), now);
		return EBUSY;
	}
	error = network_ui_state_request(&ui->network, request);

	if (error == ENODEV)
		render_activate(ui, tr(ui, "network_backend_required"), now);
	else if (error != 0)
		render_activate(ui, tr(ui, "network_action_failed"), now);
	else {
		if (request->action == NETWORK_UI_ACTION_SCAN) {
			ui->network.scan_pending = true;
			ui->network.scan_navigation_epoch = ui->navigation_epoch;
		}
		render_activate(ui, tr(ui, "network_action_queued"), now);
	}
	return error;
}

bool network_ui_tick(struct ui *ui, uint32_t now)
{
	int error;

	if (ui->nav_index != NAV_PAGE_NETWORK || ui->network.status_pending ||
	    !SDL_TICKS_PASSED(now, ui->network.status_refresh_at))
		return false;
	/*
	 * Status is queued on the existing worker.  No nmcli/DBus work is ever
	 * performed by the render thread, and one in-flight request bounds refresh
	 * pressure when NetworkManager is slow or absent.
	 */
	error = settings_request_network_status(&ui->settings);
	ui->network.status_refresh_at = now + 5000U;
	if (error != 0 || ui->settings.last_request_id == 0U) {
		ui->network.last_snapshot_error = error != 0 ? error : EIO;
		return false;
	}
	ui->network.status_pending = true;
	ui->network.status_request_id = ui->settings.last_request_id;
	return true;
}

static void begin_wifi_password(struct ui *ui,
				const struct network_access_point *point)
{
	clear_password(&ui->network);
	(void)snprintf(ui->network.password_bssid,
		sizeof(ui->network.password_bssid), "%s", point->bssid);
	ui->network.password_active = true;
	ui->input_method.field = INPUT_FIELD_PASSWORD;
	input_method_reset(&ui->input_method);
	(void)input_method_set_layout(&ui->input_method, INPUT_METHOD_ENGLISH);
	ui->keyboard_page = INPUT_METHOD_ENGLISH;
	ui->keyboard_row = 0;
	ui->keyboard_column = 0;
	SDL_StartTextInput();
}

static void bluetooth_request_result(struct ui *ui, int error,
				     uint64_t request_id, uint32_t now)
{
	if (error == 0) {
		ui->bluetooth.pending_request_id = request_id;
		ui->bluetooth.pending_navigation_epoch = ui->navigation_epoch;
		ui->bluetooth.pending_silent = false;
		render_activate(ui, tr(ui, "bluetooth_action_queued"), now);
	} else if (error == ENODEV) {
		render_activate(ui, tr(ui, "bluetooth_runtime_unavailable"), now);
	} else if (error == EAGAIN || error == EBUSY) {
		render_activate(ui, tr(ui, "bluetooth_busy"), now);
	} else {
		render_activate(ui, tr(ui, "bluetooth_action_failed"), now);
	}
}

static void bluetooth_activate(struct ui *ui, uint32_t now)
{
	const struct bluetooth_backend_snapshot *snapshot =
		&ui->bluetooth.snapshot;
	uint64_t request_id = 0U;
	int error;

	if (!ui->bluetooth.detail_active) {
		if (!bluetooth_backend_available(ui->bluetooth.backend)) {
			render_activate(ui, tr(ui, snapshot->gate ==
				BLUETOOTH_GATE_PENDING ?
				"bluetooth_runtime_unavailable" :
				"bluetooth_action_failed"), now);
			return;
		}
		ui->bluetooth.detail_active = true;
		ui->bluetooth.selected_row = 0U;
		ui->bluetooth.status_refresh_at = now;
		return;
	}
	if (ui->bluetooth.pending_request_id != 0U) {
		render_activate(ui, tr(ui, "bluetooth_busy"), now);
		return;
	}
	if (ui->bluetooth.selected_row == 0U) {
		error = bluetooth_backend_request_power(ui->bluetooth.backend,
			!snapshot->powered, &request_id);
	} else {
		const struct bluetooth_device_snapshot *device =
			&snapshot->devices[ui->bluetooth.selected_row - 1U];

		if (device->connected)
			error = bluetooth_backend_request_disconnect(
				ui->bluetooth.backend, device->address, &request_id);
		else if (device->paired)
			error = bluetooth_backend_request_connect(
				ui->bluetooth.backend, device->address, &request_id);
		else
			error = bluetooth_backend_request_pair(ui->bluetooth.backend,
				device->address, &request_id);
	}
	bluetooth_request_result(ui, error, request_id, now);
}

void network_ui_activate(struct ui *ui, uint32_t now)
{
	int error;

	if (ui->network.selected == NETWORK_UI_BLUETOOTH) {
		bluetooth_activate(ui, now);
		return;
	}
	if (ui->network.selected == NETWORK_UI_USB_DEBUG) {
		struct settings_pending_state *pending =
			&ui->settings.pending[SETTINGS_PENDING_USB_DEBUG];
		bool enabled = !(pending->value >= 0 ? pending->value != 0 :
			ui->settings.preferences.usb_debug_enabled);

		error = ui->settings.backend.set_usb_debug == NULL ? ENODEV :
			ui->settings.backend.set_usb_debug(
				ui->settings.backend.context, enabled);
		if (error == 0)
			settings_ui_track_pending(ui, SETTINGS_PENDING_USB_DEBUG,
				enabled ? 1 : 0);
		if (error == ENODEV)
			render_activate(ui, tr(ui, "network_backend_required"), now);
		else if (error != 0)
			render_activate(ui, tr(ui, "network_action_failed"), now);
		return;
	}
	if (ui->network.selected == NETWORK_UI_HOTSPOT) {
		struct network_ui_request request = {
			.action = NETWORK_UI_ACTION_HOTSPOT_SET,
			.mode = NETWORK_UI_HOTSPOT,
			.enabled = !ui->network.hotspot_enabled,
		};

		(void)submit_network_request(ui, &request, now);
		return;
	}
	if (ui->network.selected == NETWORK_UI_WIFI) {
		const struct network_access_point *point;

		if (!ui->network.detail_active) {
			struct network_ui_request request = {
				.action = NETWORK_UI_ACTION_RECOVER,
				.mode = NETWORK_UI_WIFI,
			};

			if (ui->network.hotspot_enabled) {
				render_activate(ui,
					tr(ui, "network_disable_hotspot_first"), now);
				return;
			}
			if (ui->network.generation == 0U ||
			    !ui->network.wifi_present || !ui->network.wifi_enabled) {
				if (submit_network_request(ui, &request, now) == 0)
					render_activate(ui,
						tr(ui, "network_recovery_queued"), now);
				return;
			}
			request.action = NETWORK_UI_ACTION_SCAN;
			ui->network.detail_active = true;
			(void)submit_network_request(ui, &request, now);
			return;
		}
		point = network_ui_state_selected_ap(&ui->network);
		if (point == NULL) {
			render_activate(ui, tr(ui, "network_no_access_points"), now);
			return;
		}
		if (point->active) {
			render_activate(ui, tr(ui, "network_already_connected"), now);
			return;
		}
		if (!access_point_supported(point)) {
			render_activate(ui, tr(ui, "network_security_unsupported"), now);
			return;
		}
		if (!point->known && !access_point_is_open(point)) {
			begin_wifi_password(ui, point);
			return;
		}
		{
			struct network_ui_request request = {
				.action = NETWORK_UI_ACTION_CONNECT,
				.mode = NETWORK_UI_WIFI,
			};

			(void)snprintf(request.bssid, sizeof(request.bssid), "%s",
				point->bssid);
			(void)submit_network_request(ui, &request, now);
		}
		return;
	}
	render_activate(ui, tr(ui, "network_action_failed"), now);
}

void network_ui_secondary(struct ui *ui, uint32_t now)
{
	const struct bluetooth_backend_snapshot *snapshot =
		&ui->bluetooth.snapshot;
	uint64_t request_id = 0U;
	int error;

	if (ui->network.selected == NETWORK_UI_WIFI &&
	    ui->network.detail_active) {
		struct network_ui_request request = {
			.action = NETWORK_UI_ACTION_SCAN,
			.mode = NETWORK_UI_WIFI,
		};

		if (ui->network.hotspot_enabled) {
			render_activate(ui, tr(ui, "network_disable_hotspot_first"), now);
			return;
		}
		if (ui->network.generation == 0U || !ui->network.wifi_present ||
		    !ui->network.wifi_enabled) {
			request.action = NETWORK_UI_ACTION_RECOVER;
			if (submit_network_request(ui, &request, now) == 0)
				render_activate(ui, tr(ui, "network_recovery_queued"), now);
			return;
		}
		(void)submit_network_request(ui, &request, now);
		return;
	}
	if (ui->network.selected == NETWORK_UI_HOTSPOT) {
		struct network_ui_request request = {
			.action = NETWORK_UI_ACTION_STATUS,
			.mode = NETWORK_UI_HOTSPOT,
		};

		(void)submit_network_request(ui, &request, now);
		return;
	}
	if (ui->network.selected != NETWORK_UI_BLUETOOTH ||
	    !ui->bluetooth.detail_active)
		return;
	if (ui->bluetooth.pending_request_id != 0U) {
		render_activate(ui, tr(ui, "bluetooth_busy"), now);
		return;
	}
	if (ui->bluetooth.selected_row == 0U ||
	    !snapshot->devices[ui->bluetooth.selected_row - 1U].paired)
		error = bluetooth_backend_request_scan(ui->bluetooth.backend,
			&request_id);
	else
		error = bluetooth_backend_request_forget(ui->bluetooth.backend,
			snapshot->devices[ui->bluetooth.selected_row - 1U].address,
			&request_id);
	bluetooth_request_result(ui, error, request_id, now);
}

void network_ui_tertiary(struct ui *ui, uint32_t now)
{
	const struct network_access_point *point;
	struct network_ui_request request = { .mode = NETWORK_UI_WIFI };

	if (ui->network.selected != NETWORK_UI_WIFI ||
	    !ui->network.detail_active || ui->network.password_active)
		return;
	point = network_ui_state_selected_ap(&ui->network);
	if (point == NULL)
		return;
	if (point->active) {
		request.action = NETWORK_UI_ACTION_DISCONNECT;
	} else if (point->known) {
		request.action = NETWORK_UI_ACTION_FORGET;
		(void)snprintf(request.uuid, sizeof(request.uuid), "%s", point->uuid);
	} else {
		render_activate(ui, tr(ui, "network_not_saved"), now);
		return;
	}
	(void)submit_network_request(ui, &request, now);
}

bool network_ui_password_append(struct ui *ui, const char *text)
{
	size_t used;
	size_t incoming;

	if (!ui->network.password_active || text == NULL)
		return false;
	used = strlen(ui->network.password);
	incoming = strlen(text);
	if (incoming == 0U || used + incoming >= sizeof(ui->network.password))
		return false;
	for (size_t i = 0U; i < incoming; ++i) {
		if (!input_method_accept_byte(&ui->input_method,
		    (unsigned char)text[i]))
			return false;
	}
	memcpy(ui->network.password + used, text, incoming + 1U);
	return true;
}

void network_ui_password_backspace(struct ui *ui)
{
	size_t length = strlen(ui->network.password);

	if (!ui->network.password_active || length == 0U)
		return;
	ui->network.password[--length] = '\0';
}

void network_ui_password_cancel(struct ui *ui)
{
	if (!ui->network.password_active)
		return;
	clear_password(&ui->network);
	ui->network.password_active = false;
	ui->input_method.field = INPUT_FIELD_TEXT;
	input_method_reset(&ui->input_method);
	SDL_StopTextInput();
}

void network_ui_password_submit(struct ui *ui, uint32_t now)
{
	struct network_ui_request request = {
		.action = NETWORK_UI_ACTION_CONNECT,
		.mode = NETWORK_UI_WIFI,
	};
	int error;

	if (!ui->network.password_active)
		return;
	if (strlen(ui->network.password) < 8U) {
		render_activate(ui, tr(ui, "network_password_too_short"), now);
		return;
	}
	(void)snprintf(request.bssid, sizeof(request.bssid), "%s",
		ui->network.password_bssid);
	(void)snprintf(request.password, sizeof(request.password), "%s",
		ui->network.password);
	error = submit_network_request(ui, &request, now);
	{
		volatile unsigned char *bytes =
			(volatile unsigned char *)request.password;

		for (size_t i = 0U; i < sizeof(request.password); ++i)
			bytes[i] = 0U;
	}
	if (error == 0)
		network_ui_password_cancel(ui);
}

bool network_ui_back(struct ui *ui)
{
	if (ui->network.password_active) {
		network_ui_password_cancel(ui);
		return true;
	}
	if (ui->network.detail_active &&
	    ui->network.selected == NETWORK_UI_WIFI) {
		ui->network.detail_active = false;
		return true;
	}
	if (!ui->bluetooth.detail_active)
		return false;
	ui->bluetooth.detail_active = false;
	ui->bluetooth.selected_row = 0U;
	return true;
}

void network_ui_leave_page(struct ui *ui)
{
	network_ui_password_cancel(ui);
	ui->network.detail_active = false;
	ui->bluetooth.detail_active = false;
	ui->bluetooth.selected_row = 0U;
}

bool bluetooth_ui_update(struct ui *ui, uint32_t now)
{
	char selected_address[BLUETOOTH_BACKEND_ADDRESS_SIZE] = { 0 };

	if (ui->bluetooth.selected_row > 0U && ui->bluetooth.selected_row <=
	    ui->bluetooth.snapshot.device_count)
		(void)snprintf(selected_address, sizeof(selected_address), "%s",
			ui->bluetooth.snapshot.devices[
				ui->bluetooth.selected_row - 1U].address);
	int changed = bluetooth_backend_poll(ui->bluetooth.backend,
		&ui->bluetooth.snapshot, &ui->bluetooth.generation);
	uint64_t request_id = 0U;
	bool visible_change = changed > 0;

	if (changed > 0 && selected_address[0] != '\0') {
		bool found = false;

		for (size_t index = 0U;
		     index < ui->bluetooth.snapshot.device_count; ++index) {
			if (strcmp(selected_address,
			    ui->bluetooth.snapshot.devices[index].address) == 0) {
				ui->bluetooth.selected_row = index + 1U;
				found = true;
				break;
			}
		}
		if (!found)
			ui->bluetooth.selected_row =
				ui->bluetooth.snapshot.device_count;
	} else if (changed > 0 && ui->bluetooth.selected_row >
		   ui->bluetooth.snapshot.device_count) {
		ui->bluetooth.selected_row = ui->bluetooth.snapshot.device_count;
	}
	if (changed > 0 && ui->bluetooth.pending_request_id != 0U &&
	    ui->bluetooth.snapshot.completed_request_id ==
		ui->bluetooth.pending_request_id) {
		bool visible = ui->nav_index == NAV_PAGE_NETWORK &&
			ui->network.selected == NETWORK_UI_BLUETOOTH &&
			ui->navigation_epoch ==
				ui->bluetooth.pending_navigation_epoch;

		if (visible && !ui->bluetooth.pending_silent)
			render_activate(ui, tr(ui,
				ui->bluetooth.snapshot.phase == BLUETOOTH_PHASE_READY &&
				ui->bluetooth.snapshot.last_error == 0 ?
					"bluetooth_action_complete" :
					"bluetooth_action_failed"), now);
		ui->bluetooth.pending_request_id = 0U;
		ui->bluetooth.pending_silent = false;
		ui->bluetooth.status_refresh_at = now + 5000U;
	}
	if (ui->nav_index == NAV_PAGE_NETWORK &&
	    ui->network.selected == NETWORK_UI_BLUETOOTH &&
	    ui->bluetooth.detail_active &&
	    ui->bluetooth.pending_request_id == 0U &&
	    bluetooth_backend_available(ui->bluetooth.backend) &&
	    (ui->bluetooth.snapshot.phase == BLUETOOTH_PHASE_READY ||
	     ui->bluetooth.snapshot.phase == BLUETOOTH_PHASE_ERROR) &&
	    SDL_TICKS_PASSED(now, ui->bluetooth.status_refresh_at)) {
		int error = bluetooth_backend_request_status(ui->bluetooth.backend,
			&request_id);

		ui->bluetooth.status_refresh_at = now + 5000U;
		if (error == 0 && request_id != 0U) {
			ui->bluetooth.pending_request_id = request_id;
			ui->bluetooth.pending_navigation_epoch =
				ui->navigation_epoch;
			ui->bluetooth.pending_silent = true;
		}
	}
	return visible_change;
}
