#include "ui.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static int reboot_requests;
static int reboot_enqueue_error;
static bool result_ready;
static struct settings_hardware_result queued_result;

static int fake_reboot(void *context)
{
	assert(context != NULL);
	++reboot_requests;
	return reboot_enqueue_error;
}

const char *tr(const struct ui *ui, const char *key)
{
	(void)ui;
	return key;
}

void render_activate(struct ui *ui, const char *message, uint32_t now)
{
	ui->action_text = message;
	ui->action_until = now + 1000U;
}

void audio_play_chime(struct ui *ui, double pitch)
{
	(void)ui;
	(void)pitch;
}

void persistence_request_locale(struct ui *ui)
{
	(void)ui;
}

void power_ui_toggle_screen_lock(struct ui *ui, uint32_t now)
{
	(void)ui;
	(void)now;
	assert(!"reboot test touched screen lock");
}

void power_ui_apply(struct ui *ui, unsigned int actions, uint32_t now)
{
	(void)ui;
	(void)actions;
	(void)now;
	assert(!"reboot test touched power UI");
}

unsigned int power_cancel_shutdown(struct power_state *state)
{
	(void)state;
	assert(!"reboot test cancelled shutdown");
	return 0U;
}

int settings_backend_poll(struct settings_state *settings,
			  struct settings_hardware_result *result)
{
	(void)settings;
	if (!result_ready)
		return 0;
	*result = queued_result;
	result_ready = false;
	return 1;
}

static void queue_reboot_result(int spawn_error, int exit_code,
				int term_signal)
{
	queued_result = (struct settings_hardware_result) {
		.command = SETTINGS_HW_REBOOT_CUSTOM,
		.spawn_error = spawn_error,
		.exit_code = exit_code,
		.term_signal = term_signal,
	};
	result_ready = true;
}

int main(void)
{
	struct ui ui = { 0 };

	ui.nav_index = NAV_PAGE_SETTINGS;
	ui.settings_index = 6;
	ui.settings.backend.context = &ui;
	ui.settings.backend.request_reboot_custom = fake_reboot;
	ui.settings.volume_target = -1;
	ui.hardware.audio.volume_percent = -1;

	/* Left/right can never trigger a destructive action. */
	settings_ui_adjust(&ui, 1, 90U);
	assert(reboot_requests == 0);

	/* First A arms a short confirmation window; second A submits exactly once. */
	settings_ui_activate(&ui, 100U);
	assert(ui.settings.reboot_confirm_armed);
	assert(strcmp(ui.action_text, "restart_confirm") == 0);
	assert(reboot_requests == 0);
	settings_ui_activate(&ui, 200U);
	assert(!ui.settings.reboot_confirm_armed);
	assert(ui.settings.reboot_pending);
	assert(strcmp(ui.action_text, "restart_queued") == 0);
	assert(reboot_requests == 1);

	settings_ui_activate(&ui, 300U);
	assert(strcmp(ui.action_text, "restart_busy") == 0);
	assert(reboot_requests == 1);

	/* A real helper failure clears busy state and exposes its exit code. */
	queue_reboot_result(0, 23, 0);
	settings_ui_update(&ui, 301U);
	assert(!ui.settings.reboot_pending);
	assert(strstr(ui.action_text, "hardware_failed") != NULL);
	assert(strstr(ui.action_text, "launch_exit_code 23") != NULL);

	/* An enqueue failure is immediate, visible, and never marks the request busy. */
	reboot_enqueue_error = EAGAIN;
	settings_ui_activate(&ui, 400U);
	settings_ui_activate(&ui, 401U);
	assert(reboot_requests == 2);
	assert(!ui.settings.reboot_pending);
	assert(strstr(ui.action_text, "launch_error_code 11") != NULL);
	reboot_enqueue_error = 0;

	/* Expiry and selection changes both require a fresh first confirmation. */
	settings_ui_activate(&ui, 1000U);
	settings_ui_update(&ui, 6000U);
	assert(!ui.settings.reboot_confirm_armed);
	settings_ui_activate(&ui, 6001U);
	assert(reboot_requests == 2);
	settings_ui_move(&ui, 1);
	assert(ui.settings_index == 0);
	settings_ui_move(&ui, -1);
	assert(ui.settings_index == 6);
	settings_ui_activate(&ui, 6002U);
	assert(reboot_requests == 2);

	settings_ui_activate(&ui, 6003U);
	assert(reboot_requests == 3);
	queue_reboot_result(0, 0, 0);
	settings_ui_update(&ui, 6004U);
	assert(!ui.settings.reboot_pending);
	assert(strcmp(ui.action_text, "restart_mode · hardware_applied") == 0);

	puts("REBOOT_UI_TEST PASS confirm=double-A expiry=5s argv=fixed busy=guarded errors=visible");
	return 0;
}
