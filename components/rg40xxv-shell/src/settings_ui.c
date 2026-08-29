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
		return tr(ui, "restart_mode");
	}
	return tr(ui, "hardware_control");
}

static void show_enqueue_error(struct ui *ui, const char *label, int error,
			       uint32_t now)
{
	(void)snprintf(ui->settings.status_text,
		       sizeof(ui->settings.status_text), "%s · %s %d", label,
		       tr(ui, "launch_error_code"), error);
	render_activate(ui, ui->settings.status_text, now);
}

void settings_ui_move(struct ui *ui, int direction)
{
	ui->settings.reboot_confirm_armed = false;
	if (direction < 0)
		ui->settings_index = (ui->settings_index + 6) % 7;
	else if (direction > 0)
		ui->settings_index = (ui->settings_index + 1) % 7;
}

void settings_ui_adjust(struct ui *ui, int direction, uint32_t now)
{
	struct device_preferences desired = ui->settings.preferences;
	int error = 0;
	const char *label;

	switch (ui->settings_index) {
	case 0:
		desired.backlight_percent = stepped_percent(
			ui->settings.preferences.backlight_percent, direction);
		label = tr(ui, "backlight");
		if (ui->settings.backend.apply_backlight == NULL)
			error = ENODEV;
		else
			error = ui->settings.backend.apply_backlight(
				ui->settings.backend.context,
				desired.backlight_percent, false);
		break;
	case 1:
		desired.joystick_rgb_brightness = stepped_percent(
			ui->settings.preferences.joystick_rgb_brightness,
			direction);
		desired.joystick_rgb_enabled =
			desired.joystick_rgb_brightness > 0;
		label = tr(ui, "joystick_light");
		if (ui->settings.backend.apply_joystick_rgb == NULL)
			error = ENODEV;
		else
			error = ui->settings.backend.apply_joystick_rgb(
				ui->settings.backend.context, &desired);
		break;
	case 2:
		desired.usb_debug_enabled =
			!ui->settings.preferences.usb_debug_enabled;
		label = tr(ui, "usb_debug");
		if (ui->settings.backend.set_usb_debug == NULL)
			error = ENODEV;
		else
			error = ui->settings.backend.set_usb_debug(
				ui->settings.backend.context,
				desired.usb_debug_enabled);
		break;
	case 3:
		power_ui_toggle_screen_lock(ui, now);
		return;
	case 4:
		render_activate(ui, tr(ui, "display_mode_backend_required"), now);
		audio_play_chime(ui, 1640.0);
		return;
	case 5: {
		int current = ui->settings.volume_target >= 0 ?
			ui->settings.volume_target : ui->hardware.audio.volume_percent;
		int requested = stepped_percent(current, direction);

		label = tr(ui, "volume");
		if (ui->settings.backend.set_volume == NULL)
			error = ENODEV;
		else
			error = ui->settings.backend.set_volume(
				ui->settings.backend.context, requested);
		if (error == 0)
			ui->settings.volume_target = requested;
		break;
	}
	case 6:
		/* A destructive action is never bound to left/right adjustment. */
		return;
	default:
		return;
	}
	if (error != 0) {
		show_enqueue_error(ui, label, error, now);
		return;
	}
	ui->settings.preferences = desired;
	persistence_request_locale(ui);
	if (ui->settings_index == 0)
		(void)snprintf(ui->settings.status_text,
			sizeof(ui->settings.status_text), "%s · %d%% · %s", label,
			desired.backlight_percent, tr(ui, "hardware_queued"));
	else if (ui->settings_index == 1)
		(void)snprintf(ui->settings.status_text,
			sizeof(ui->settings.status_text), "%s · %d%% · %s", label,
			desired.joystick_rgb_brightness, tr(ui, "hardware_queued"));
	else if (ui->settings_index == 5)
		(void)snprintf(ui->settings.status_text,
			sizeof(ui->settings.status_text), "%s · %d%% · %s", label,
			ui->settings.volume_target, tr(ui, "hardware_queued"));
	else
		(void)snprintf(ui->settings.status_text,
			sizeof(ui->settings.status_text), "%s · %s", label,
			tr(ui, "hardware_queued"));
	render_activate(ui, ui->settings.status_text, now);
	audio_play_chime(ui, 1840.0);
}

void settings_ui_activate(struct ui *ui, uint32_t now)
{
	int error;

	if (ui->settings_index == 6) {
		if (ui->settings.reboot_pending) {
			render_activate(ui, tr(ui, "restart_busy"), now);
			return;
		}
		if (!ui->settings.reboot_confirm_armed ||
		    SDL_TICKS_PASSED(now, ui->settings.reboot_confirm_until)) {
			ui->settings.reboot_confirm_armed = true;
			ui->settings.reboot_confirm_until = now + 5000U;
			render_activate(ui, tr(ui, "restart_confirm"), now);
			audio_play_chime(ui, 1640.0);
			return;
		}
		ui->settings.reboot_confirm_armed = false;
		if (ui->settings.backend.request_reboot_custom == NULL)
			error = ENODEV;
		else
			error = ui->settings.backend.request_reboot_custom(
				ui->settings.backend.context);
		if (error != 0) {
			show_enqueue_error(ui, tr(ui, "restart_mode"), error, now);
			return;
		}
		ui->settings.reboot_pending = true;
		render_activate(ui, tr(ui, "restart_queued"), now);
		audio_play_chime(ui, 1840.0);
		return;
	}
	ui->settings.reboot_confirm_armed = false;
	if (ui->settings_index != 5) {
		settings_ui_adjust(ui, 1, now);
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
	(void)snprintf(ui->settings.status_text,
		sizeof(ui->settings.status_text), "%s · %s", tr(ui, "volume"),
		tr(ui, "mute_toggle_queued"));
	render_activate(ui, ui->settings.status_text, now);
	audio_play_chime(ui, 1840.0);
}

void settings_ui_update(struct ui *ui, uint32_t now)
{
	struct settings_hardware_result result;

	if (ui->nav_index != NAV_PAGE_SETTINGS || ui->settings_index != 6 ||
	    SDL_TICKS_PASSED(now, ui->settings.reboot_confirm_until))
		ui->settings.reboot_confirm_armed = false;

	if (ui->settings.volume_target >= 0 &&
	    ui->hardware.audio.volume_percent == ui->settings.volume_target)
		ui->settings.volume_target = -1;

	while (settings_backend_poll(&ui->settings, &result) > 0) {
		const char *label = command_label(ui, result.command);
		bool failed = result.spawn_error != 0 || result.term_signal != 0 ||
			result.exit_code != 0;

		if (result.command == SETTINGS_HW_REBOOT_CUSTOM)
			ui->settings.reboot_pending = false;

		(void)fprintf(stderr,
			      "UI_HARDWARECTL_RESULT command=%d value=%d spawn_error=%d exit=%d signal=%d\n",
			      result.command, result.value, result.spawn_error,
			      result.exit_code, result.term_signal);
		if (failed) {
			const char *kind;
			int code;

			if (result.spawn_error != 0) {
				kind = tr(ui, "launch_error_code");
				code = result.spawn_error;
			} else if (result.term_signal != 0) {
				kind = tr(ui, "launch_exit_signal");
				code = result.term_signal;
			} else {
				kind = tr(ui, "launch_exit_code");
				code = result.exit_code;
			}
			(void)snprintf(ui->settings.status_text,
				       sizeof(ui->settings.status_text),
				       "%s · %s · %s %d", label,
				       tr(ui, "hardware_failed"), kind, code);
			if (result.command == SETTINGS_HW_SCREEN_OFF) {
				ui->settings.preferences.screen_off = false;
				ui->power.view = POWER_VIEW_ACTIVE;
				ui->power.locked = false;
			}
			if (result.command == SETTINGS_HW_ORDERLY_SHUTDOWN)
				power_ui_apply(ui,
					power_cancel_shutdown(&ui->power), now);
			if (result.command == SETTINGS_HW_VOLUME)
				ui->settings.volume_target = -1;
		} else {
			(void)snprintf(ui->settings.status_text,
				       sizeof(ui->settings.status_text), "%s · %s",
				       label, tr(ui, "hardware_applied"));
		}
		render_activate(ui, ui->settings.status_text, now);
		(void)fprintf(stderr, "UI_HARDWARECTL_STATUS %s\n",
			      ui->settings.status_text);
	}
}
