#include "ui.h"

#include <errno.h>
#include <stdio.h>

static int clamp_percent(int value)
{
	return value < 0 ? 0 : value > 100 ? 100 : value;
}

static int stepped_percent(int current, int direction)
{
	int value = current < 0 ? 50 : current;

	if (direction == 0)
		direction = 1;
	value += direction * 5;
	return clamp_percent(value);
}

static int stepped_auto_screen_off(int current, int direction)
{
	static const int choices[] = { 0, 1, 3, 5, 10, 30 };
	size_t count = sizeof(choices) / sizeof(choices[0]);
	size_t index = 0U;

	for (size_t i = 0U; i < count; ++i) {
		if (choices[i] == current) {
			index = i;
			break;
		}
	}
	if (direction < 0)
		index = (index + count - 1U) % count;
	else
		index = (index + 1U) % count;
	return choices[index];
}

static const char *command_label(struct ui *ui,
				 enum settings_hardware_command command)
{
	switch (command) {
	case SETTINGS_HW_SCREEN_OFF:
	case SETTINGS_HW_SCREEN_ON:
		return tr(ui, "screen_power");
	case SETTINGS_HW_BRIGHTNESS:
		return tr(ui, "backlight");
	case SETTINGS_HW_JOYSTICK_RGB:
		return tr(ui, "joystick_light");
	case SETTINGS_HW_USB_DEBUG:
		return tr(ui, "usb_debug");
	case SETTINGS_HW_VOLUME:
	case SETTINGS_HW_MUTE_TOGGLE:
		return tr(ui, "volume");
	case SETTINGS_HW_ORDERLY_SHUTDOWN:
		return tr(ui, "orderly_shutdown");
	case SETTINGS_HW_REBOOT_CUSTOM:
		return tr(ui, "restart_normal");
	case SETTINGS_HW_NETWORK_STATUS:
		return tr(ui, "network_title");
	case SETTINGS_HW_WIFI_RECOVER:
	case SETTINGS_HW_WIFI_SCAN:
	case SETTINGS_HW_WIFI_CONNECT:
	case SETTINGS_HW_WIFI_DISCONNECT:
	case SETTINGS_HW_WIFI_FORGET:
		return tr(ui, "wifi");
	case SETTINGS_HW_HOTSPOT_SET:
		return tr(ui, "network_hotspot_mode");
	}
	return tr(ui, "hardware_control");
}

static struct settings_pending_state *pending_for_command(
	struct settings_state *settings, enum settings_hardware_command command)
{
	switch (command) {
	case SETTINGS_HW_BRIGHTNESS:
		return &settings->pending[SETTINGS_PENDING_BRIGHTNESS];
	case SETTINGS_HW_JOYSTICK_RGB:
		return &settings->pending[SETTINGS_PENDING_JOYSTICK_RGB];
	case SETTINGS_HW_USB_DEBUG:
		return &settings->pending[SETTINGS_PENDING_USB_DEBUG];
	case SETTINGS_HW_VOLUME:
		return &settings->pending[SETTINGS_PENDING_VOLUME];
	default:
		return NULL;
	}
}

static int pending_value(const struct settings_state *settings,
			 enum settings_pending_kind kind, int confirmed)
{
	return settings->pending[kind].value >= 0 ?
		settings->pending[kind].value : confirmed;
}

void settings_ui_track_pending(struct ui *ui,
			       enum settings_pending_kind kind, int value)
{
	struct settings_pending_state *pending = &ui->settings.pending[kind];

	if (ui->settings.last_request_id == 0U)
		return;
	pending->request_id = ui->settings.last_request_id;
	pending->navigation_epoch = ui->navigation_epoch;
	pending->value = value;
	pending->nav_index = ui->nav_index;
	(void)fprintf(stderr,
		"UI_HARDWARECTL_ENQUEUED request=%llu value=%d page=%d epoch=%llu\n",
		(unsigned long long)pending->request_id, value, pending->nav_index,
		(unsigned long long)pending->navigation_epoch);
}

static void clear_pending(struct settings_pending_state *pending)
{
	pending->request_id = 0U;
	pending->navigation_epoch = 0U;
	pending->value = -1;
	pending->nav_index = -1;
}

static bool pending_result_visible(const struct ui *ui,
				   const struct settings_pending_state *pending)
{
	return pending->nav_index == ui->nav_index &&
		pending->navigation_epoch == ui->navigation_epoch;
}

static void show_enqueue_error(struct ui *ui, const char *label, int error,
			       uint32_t now)
{
	(void)snprintf(ui->settings.status_text,
		       sizeof(ui->settings.status_text), "%s · %s · %s %d", label,
		       tr(ui, "hardware_failed"), tr(ui, "launch_error_code"),
		       error);
	render_activate(ui, ui->settings.status_text, now);
}

void settings_ui_move(struct ui *ui, int direction)
{
	if (ui->settings_detail_active)
		return;
	if (direction < 0)
		ui->settings_index = (ui->settings_index +
			SETTINGS_UI_ROW_COUNT - 1) % SETTINGS_UI_ROW_COUNT;
	else if (direction > 0)
		ui->settings_index = (ui->settings_index + 1) %
			SETTINGS_UI_ROW_COUNT;
}

void settings_ui_adjust(struct ui *ui, int direction, uint32_t now)
{
	struct device_preferences desired = ui->settings.preferences;
	int requested;
	int current;
	int error = 0;
	const char *label;
	enum settings_pending_kind kind;

	/* A selected row is inert until A explicitly enters its detail view. */
	if (!ui->settings_detail_active)
		return;
	if (ui->settings_index == 6) {
		requested = stepped_auto_screen_off(
			ui->settings.preferences.auto_screen_off_minutes, direction);
		ui->settings.preferences.auto_screen_off_minutes = requested;
		ui->last_user_activity_at = now;
		persistence_request_locale(ui);
		render_activate(ui, tr(ui, "auto_screen_off_saved"), now);
		audio_play_chime(ui, requested == 0 ? 1640.0 : 1870.0);
		return;
	}
	switch (ui->settings_index) {
	case 0:
		kind = SETTINGS_PENDING_BRIGHTNESS;
		current = pending_value(&ui->settings, kind,
			ui->settings.preferences.backlight_percent);
		requested = stepped_percent(current, direction);
		if (requested == current)
			return;
		label = tr(ui, "backlight");
		error = ui->settings.backend.apply_backlight == NULL ? ENODEV :
			ui->settings.backend.apply_backlight(
				ui->settings.backend.context, requested, false);
		break;
	case 1:
		kind = SETTINGS_PENDING_JOYSTICK_RGB;
		current = pending_value(&ui->settings, kind,
			ui->settings.preferences.joystick_rgb_brightness);
		requested = stepped_percent(current, direction);
		if (requested == current)
			return;
		desired.joystick_rgb_brightness = requested;
		desired.joystick_rgb_enabled = requested > 0;
		label = tr(ui, "joystick_light");
		error = ui->settings.backend.apply_joystick_rgb == NULL ? ENODEV :
			ui->settings.backend.apply_joystick_rgb(
				ui->settings.backend.context, &desired);
		break;
	case 2:
		kind = SETTINGS_PENDING_USB_DEBUG;
		current = pending_value(&ui->settings, kind,
			ui->settings.preferences.usb_debug_enabled ? 1 : 0);
		requested = current == 0 ? 1 : 0;
		label = tr(ui, "usb_debug");
		error = ui->settings.backend.set_usb_debug == NULL ? ENODEV :
			ui->settings.backend.set_usb_debug(
				ui->settings.backend.context, requested != 0);
		break;
	case 5:
		kind = SETTINGS_PENDING_VOLUME;
		current = pending_value(&ui->settings, kind,
			ui->settings.volume_target >= 0 ?
			ui->settings.volume_target :
			ui->hardware.audio.volume_percent);
		requested = stepped_percent(current, direction);
		if (requested == current)
			return;
		label = tr(ui, "volume");
		error = ui->settings.backend.set_volume == NULL ? ENODEV :
			ui->settings.backend.set_volume(
				ui->settings.backend.context, requested);
		break;
	case 3:
	case 4:
	default:
		return;
	}
	if (error != 0) {
		show_enqueue_error(ui, label, error, now);
		return;
	}
	settings_ui_track_pending(ui, kind, requested);
}

void settings_ui_activate(struct ui *ui, uint32_t now)
{
	int error;

	if (!ui->settings_detail_active) {
		ui->settings_detail_active = true;
		(void)fprintf(stderr, "UI_SETTINGS_DETAIL ENTER index=%d\n",
			ui->settings_index);
		audio_play_chime(ui, 1840.0);
		return;
	}
	switch (ui->settings_index) {
	case 2:
		settings_ui_adjust(ui, 1, now);
		return;
	case 6:
		settings_ui_adjust(ui, 1, now);
		return;
	case 3:
		power_ui_toggle_screen_lock(ui, now);
		return;
	case 4:
		if (ui->settings.reboot_pending.request_id != 0U) {
			render_activate(ui, tr(ui, "restart_busy"), now);
			return;
		}
		error = ui->settings.backend.request_reboot_custom == NULL ? ENODEV :
			ui->settings.backend.request_reboot_custom(
				ui->settings.backend.context);
		if (error != 0) {
			show_enqueue_error(ui, tr(ui, "restart_normal"), error, now);
			return;
		}
		ui->settings.reboot_pending.request_id =
			ui->settings.last_request_id;
		ui->settings.reboot_pending.navigation_epoch = ui->navigation_epoch;
		ui->settings.reboot_pending.nav_index = ui->nav_index;
		ui->settings.reboot_pending.value = 0;
		render_activate(ui, tr(ui, "restart_queued"), now);
		return;
	case 5:
		break;
	default:
		/* Sliders use left/right after entering; A has no hidden write. */
		return;
	}
	if (ui->settings.backend.toggle_mute == NULL)
		error = ENODEV;
	else
		error = ui->settings.backend.toggle_mute(
			ui->settings.backend.context);
	if (error != 0) {
		show_enqueue_error(ui, tr(ui, "volume"), error, now);
		return;
	}
	if (ui->settings.last_request_id != 0U) {
		ui->settings.mute_pending.request_id =
			ui->settings.last_request_id;
		ui->settings.mute_pending.navigation_epoch = ui->navigation_epoch;
		ui->settings.mute_pending.nav_index = ui->nav_index;
		ui->settings.mute_pending.value = 0;
	}
}

bool settings_ui_back(struct ui *ui)
{
	if (!ui->settings_detail_active)
		return false;
	ui->settings_detail_active = false;
	(void)fprintf(stderr, "UI_SETTINGS_DETAIL LEAVE reason=back index=%d\n",
		ui->settings_index);
	return true;
}

void settings_ui_leave_page(struct ui *ui)
{
	if (ui->settings_detail_active)
		(void)fprintf(stderr,
			"UI_SETTINGS_DETAIL LEAVE reason=page index=%d\n",
			ui->settings_index);
	ui->settings_detail_active = false;
	if (ui->action_text == ui->settings.status_text) {
		ui->action_text = NULL;
		ui->action_until = 0U;
	}
}

static void commit_success(struct ui *ui,
			   const struct settings_hardware_result *result)
{
	switch (result->command) {
	case SETTINGS_HW_BRIGHTNESS:
		ui->settings.preferences.backlight_percent = result->value;
		persistence_request_locale(ui);
		break;
	case SETTINGS_HW_JOYSTICK_RGB:
		ui->settings.preferences.joystick_rgb_brightness = result->value;
		ui->settings.preferences.joystick_rgb_enabled = result->value > 0;
		persistence_request_locale(ui);
		break;
	case SETTINGS_HW_USB_DEBUG:
		ui->settings.preferences.usb_debug_enabled = result->value != 0;
		persistence_request_locale(ui);
		break;
	case SETTINGS_HW_VOLUME:
		ui->settings.volume_target = result->value;
		break;
	default:
		break;
	}
}

static bool result_failed(const struct settings_hardware_result *result)
{
	return result->spawn_error != 0 || result->term_signal != 0 ||
		result->exit_code != 0;
}

static void format_result_status(struct ui *ui,
				 const struct settings_hardware_result *result,
				 bool failed, char *output, size_t output_size)
{
	const char *label = command_label(ui, result->command);

	if (!failed) {
		if (result->command == SETTINGS_HW_BRIGHTNESS ||
		    result->command == SETTINGS_HW_JOYSTICK_RGB ||
		    result->command == SETTINGS_HW_VOLUME)
			(void)snprintf(output, output_size, "%s · %d%% · %s",
				label, result->value, tr(ui, "hardware_applied"));
		else
			(void)snprintf(output, output_size, "%s · %s", label,
				tr(ui, "hardware_applied"));
		return;
	}
	if (result->spawn_error != 0)
		(void)snprintf(output, output_size, "%s · %s · %s %d", label,
			tr(ui, "hardware_failed"), tr(ui, "launch_error_code"),
			result->spawn_error);
	else if (result->term_signal != 0)
		(void)snprintf(output, output_size, "%s · %s · %s %d", label,
			tr(ui, "hardware_failed"), tr(ui, "launch_exit_signal"),
			result->term_signal);
	else
		(void)snprintf(output, output_size, "%s · %s · %s %d", label,
			tr(ui, "hardware_failed"), tr(ui, "launch_exit_code"),
			result->exit_code);
}

bool settings_ui_update(struct ui *ui, uint32_t now)
{
	struct settings_hardware_result result;
	char selected_status[sizeof(ui->settings.status_text)] = { 0 };
	int selected_priority = 0;
	bool state_changed = false;

	if (ui->settings.volume_target >= 0 &&
	    ui->hardware.audio.volume_percent == ui->settings.volume_target)
		ui->settings.volume_target = -1;

	while (settings_backend_poll(&ui->settings, &result) > 0) {
		struct settings_pending_state *pending =
			pending_for_command(&ui->settings, result.command);
		bool failed = result_failed(&result);
		bool visible = true;
		bool stale = false;
		char status[sizeof(ui->settings.status_text)];
		int priority = failed ? 2 : 1;
		bool network_command = result.command >= SETTINGS_HW_NETWORK_STATUS &&
			result.command <= SETTINGS_HW_HOTSPOT_SET;

		(void)fprintf(stderr,
			"UI_HARDWARECTL_RESULT request=%llu command=%d value=%d spawn_error=%d exit=%d signal=%d\n",
			(unsigned long long)result.request_id, result.command,
			result.value, result.spawn_error, result.exit_code,
			result.term_signal);
		if (pending != NULL) {
			stale = pending->request_id == 0U ||
				pending->request_id != result.request_id ||
				pending->value != result.value;
			if (!stale) {
				visible = pending_result_visible(ui, pending);
				if (!failed)
					commit_success(ui, &result);
				clear_pending(pending);
			}
		} else if (result.command == SETTINGS_HW_MUTE_TOGGLE) {
			struct settings_pending_state *mute =
				&ui->settings.mute_pending;

			stale = mute->request_id == 0U ||
				mute->request_id != result.request_id;
			if (!stale) {
				visible = pending_result_visible(ui, mute);
				clear_pending(mute);
			}
		} else if (result.command == SETTINGS_HW_REBOOT_CUSTOM) {
			struct settings_pending_state *reboot =
				&ui->settings.reboot_pending;

			stale = reboot->request_id == 0U ||
				reboot->request_id != result.request_id;
			if (!stale) {
				visible = pending_result_visible(ui, reboot);
				clear_pending(reboot);
			}
		} else if (result.command == SETTINGS_HW_NETWORK_STATUS &&
			   ui->network.status_pending &&
			   ui->network.status_request_id == result.request_id) {
			/* Periodic status refresh is silent and never steals another OSD. */
			visible = false;
			ui->network.status_pending = false;
			ui->network.status_request_id = 0U;
			ui->network.status_refresh_at = now + 5000U;
		} else if (result.command == SETTINGS_HW_WIFI_SCAN) {
			/*
			 * The worker may finish after the user has changed pages.  Always
			 * consume the real result, but only surface it in the Wi-Fi detail
			 * view that submitted this scan.  This prevents a late scan from
			 * replacing another page's action OSD/visual state.
			 */
			visible = ui->network.scan_pending &&
				ui->nav_index == NAV_PAGE_NETWORK &&
				ui->network.selected == NETWORK_UI_WIFI &&
				ui->network.detail_active &&
				ui->network.scan_navigation_epoch ==
					ui->navigation_epoch;
			ui->network.scan_pending = false;
			ui->network.scan_navigation_epoch = 0U;
		}
		if (stale) {
			(void)fprintf(stderr,
				"UI_HARDWARECTL_STALE request=%llu command=%d ignored=1\n",
				(unsigned long long)result.request_id,
				result.command);
			continue;
		}
		if (failed) {
			if (result.command == SETTINGS_HW_SCREEN_OFF) {
				ui->settings.preferences.screen_off = false;
				ui->power.view = POWER_VIEW_ACTIVE;
				ui->power.locked = false;
			}
			if (result.command == SETTINGS_HW_SCREEN_ON) {
				ui->settings.preferences.screen_off = true;
				ui->power.view = POWER_VIEW_SCREEN_OFF;
			}
			if (result.command == SETTINGS_HW_ORDERLY_SHUTDOWN)
				power_ui_apply(ui,
					power_cancel_shutdown(&ui->power), now);
		}
		if (network_command && !failed) {
			if ((result.command == SETTINGS_HW_WIFI_SCAN ?
			     network_ui_state_load_access_points(&ui->network) :
			     network_ui_state_load_snapshot(&ui->network)) != 0) {
				failed = true;
				priority = 2;
					result.exit_code = ui->network.last_snapshot_error;
				} else {
					/*
					 * A late AP-only scan has no pixels to change after the user
					 * leaves the Network page.  Keep the cache fresh without
					 * waking the renderer; full status/actions can change the
					 * global Wi-Fi indicator and therefore still invalidate.
					 */
					if (result.command != SETTINGS_HW_WIFI_SCAN || visible)
						state_changed = true;
				}
		}
		format_result_status(ui, &result, failed, status, sizeof(status));
		(void)fprintf(stderr, "UI_HARDWARECTL_STATUS %s visible=%d\n",
			status, visible ? 1 : 0);
		/* Preserve a failure when several completed results drain together. */
		if (visible && priority >= selected_priority) {
			(void)snprintf(selected_status, sizeof(selected_status), "%s",
				status);
			selected_priority = priority;
		}
	}
	if (selected_priority != 0) {
		(void)snprintf(ui->settings.status_text,
			sizeof(ui->settings.status_text), "%s", selected_status);
		render_activate(ui, ui->settings.status_text, now);
		if (selected_priority == 1)
			audio_play_chime(ui, 1840.0);
	}
	return selected_priority != 0 || state_changed;
}
