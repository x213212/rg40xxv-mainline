#include "ui.h"

#include <errno.h>
#include <stdio.h>

static void power_backend_error(struct ui *ui, const char *label, int error,
				uint32_t now)
{
	(void)snprintf(ui->settings.status_text,
		       sizeof(ui->settings.status_text), "%s · %s %d", label,
		       tr(ui, "launch_error_code"), error);
	render_activate(ui, ui->settings.status_text, now);
}

void power_ui_apply(struct ui *ui, unsigned int actions, uint32_t now)
{
	if ((actions & POWER_ACTION_BACKLIGHT_OFF) != 0U) {
		int error = ui->settings.backend.set_screen_off == NULL ? ENODEV :
			ui->settings.backend.set_screen_off(
				ui->settings.backend.context, true);

		if (error == 0)
			ui->settings.preferences.screen_off = true;
		else {
			ui->settings.preferences.screen_off = false;
			ui->power.view = POWER_VIEW_ACTIVE;
			ui->power.locked = false;
			power_backend_error(ui, tr(ui, "screen_power"), error, now);
		}
	}
	if ((actions & POWER_ACTION_BACKLIGHT_ON) != 0U) {
		int error = ui->settings.backend.set_screen_off == NULL ? ENODEV :
			ui->settings.backend.set_screen_off(
				ui->settings.backend.context, false);

		if (error == 0)
			ui->settings.preferences.screen_off = false;
		else
			power_backend_error(ui, tr(ui, "screen_power"), error, now);
	}
	if ((actions & POWER_ACTION_LOCK_PROGRESS) != 0U)
		audio_play_chime(ui, 1640.0 + ui->power.unlock_progress * 100.0);
	if ((actions & POWER_ACTION_REQUEST_SHUTDOWN) != 0U) {
		int error = ui->settings.backend.request_shutdown == NULL ? ENODEV :
			ui->settings.backend.request_shutdown(
				ui->settings.backend.context);

		if (error != 0) {
			(void)power_cancel_shutdown(&ui->power);
			power_backend_error(ui, tr(ui, "orderly_shutdown"), error, now);
		}
	}
	if ((actions & POWER_ACTION_CANCEL_SHUTDOWN) != 0U)
		render_activate(ui, tr(ui, "back"), now);
}

static bool unlock_eligible(SDL_Keycode key)
{
	return key != SDLK_POWER && key != SDLK_VOLUMEUP &&
		key != SDLK_VOLUMEDOWN && key != SDLK_AUDIOMUTE;
}

bool power_ui_handle_key(struct ui *ui, SDL_Keycode key, uint32_t now)
{
	if (key == SDLK_POWER) {
		power_ui_apply(ui, power_button_press(&ui->power, now), now);
		return true;
	}
	if (ui->power.view == POWER_VIEW_SHUTDOWN_COUNTDOWN) {
		if (key == SDLK_ESCAPE)
			power_ui_apply(ui, power_cancel_shutdown(&ui->power), now);
		return true;
	}
	if (ui->power.view == POWER_VIEW_LOCKED) {
		power_ui_apply(ui, power_lock_key(&ui->power, (int)key,
					       unlock_eligible(key), now), now);
		return true;
	}
	return ui->power.view == POWER_VIEW_SCREEN_OFF;
}

void power_ui_toggle_screen_lock(struct ui *ui, uint32_t now)
{
	bool enabled = !ui->settings.preferences.screen_lock_enabled;

	ui->settings.preferences.screen_lock_enabled = enabled;
	power_ui_apply(ui, power_set_lock_enabled(&ui->power, enabled), now);
	persistence_request_locale(ui);
	render_activate(ui, tr(ui, enabled ? "lock_on" : "lock_off"), now);
	audio_play_chime(ui, enabled ? 1870.0 : 1640.0);
}
