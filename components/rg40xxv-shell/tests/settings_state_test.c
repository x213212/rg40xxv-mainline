#include "ui.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned int network_full_loads;
static unsigned int network_ap_loads;

int network_ui_state_load_snapshot(struct network_ui_state *state)
{
	(void)state;
	++network_full_loads;
	return 0;
}

int network_ui_state_load_access_points(struct network_ui_state *state)
{
	(void)state;
	++network_ap_loads;
	return 0;
}

static struct settings_hardware_result results[8];
static size_t result_head;
static size_t result_count;
static uint64_t next_request_id;
static unsigned int backend_calls;
static unsigned int overlay_calls;
static unsigned int persistence_calls;

static void push_result(uint64_t request_id,
			enum settings_hardware_command command, int value,
			int exit_code)
{
	assert(result_count < sizeof(results) / sizeof(results[0]));
	results[result_count++] = (struct settings_hardware_result) {
		.command = command,
		.request_id = request_id,
		.value = value,
		.exit_code = exit_code,
	};
}

static int fake_state_command(void *context, int value)
{
	struct ui *ui = context;

	++backend_calls;
	ui->settings.last_request_id = ++next_request_id;
	(void)value;
	return 0;
}

static int fake_backlight(void *context, int value, bool screen_off)
{
	assert(!screen_off);
	return fake_state_command(context, value);
}

static int fake_edge_command(void *context)
{
	return fake_state_command(context, 0);
}

int settings_backend_poll(struct settings_state *settings,
			  struct settings_hardware_result *result)
{
	(void)settings;
	if (result_head >= result_count)
		return 0;
	*result = results[result_head++];
	return 1;
}

const char *tr(const struct ui *ui, const char *key)
{
	(void)ui;
	return key;
}

void render_activate(struct ui *ui, const char *message, uint32_t now)
{
	++overlay_calls;
	ui->action_text = message;
	ui->action_until = now + 1U;
}

void persistence_request_locale(struct ui *ui)
{
	(void)ui;
	++persistence_calls;
}

void audio_play_chime(struct ui *ui, double pitch)
{
	(void)ui;
	(void)pitch;
}

void power_ui_toggle_screen_lock(struct ui *ui, uint32_t now)
{
	(void)ui;
	(void)now;
}

unsigned int power_cancel_shutdown(struct power_state *state)
{
	(void)state;
	return POWER_ACTION_NONE;
}

void power_ui_apply(struct ui *ui, unsigned int actions, uint32_t now)
{
	(void)ui;
	(void)actions;
	(void)now;
}

static void reset_results(void)
{
	result_head = 0U;
	result_count = 0U;
}

int main(void)
{
	struct ui ui;
	uint64_t first;
	uint64_t latest;
	uint64_t off_page;
	uint64_t volume;
	unsigned int overlays_before;

	memset(&ui, 0, sizeof(ui));
	for (int i = 0; i < SETTINGS_PENDING_COUNT; ++i) {
		ui.settings.pending[i].value = -1;
		ui.settings.pending[i].nav_index = -1;
	}
	ui.settings.mute_pending.value = -1;
	ui.settings.mute_pending.nav_index = -1;
	ui.settings.reboot_pending.value = -1;
	ui.settings.reboot_pending.nav_index = -1;
	ui.settings.volume_target = -1;
	ui.nav_index = NAV_PAGE_SETTINGS;
	ui.focus_region = UI_FOCUS_CONTENT;
	ui.settings.preferences.backlight_percent = 8;
	ui.settings.backend.context = &ui;
	ui.settings.backend.apply_backlight = fake_backlight;
	ui.settings.backend.set_volume = fake_state_command;
	ui.settings.backend.request_reboot_custom = fake_edge_command;
	ui.hardware.audio.volume_percent = 95;

	/* A selected row cannot write hardware until A enters detail. */
	settings_ui_adjust(&ui, 1, 10U);
	assert(backend_calls == 0U);

	/* Auto screen-off persists immediately and never writes hardware here. */
	ui.settings_index = 6;
	ui.settings_detail_active = true;
	ui.settings.preferences.auto_screen_off_minutes = 0;
	backend_calls = 0U;
	persistence_calls = 0U;
	settings_ui_adjust(&ui, 1, 10U);
	assert(ui.settings.preferences.auto_screen_off_minutes == 1);
	assert(backend_calls == 0U);
	assert(persistence_calls == 1U);
	settings_ui_adjust(&ui, -1, 11U);
	assert(ui.settings.preferences.auto_screen_off_minutes == 0);
	assert(persistence_calls == 2U);
	ui.settings_detail_active = false;
	persistence_calls = 0U;

	/* Normal restart is a confirmed, fixed custom-target edge command. */
	reset_results();
	ui.settings_detail_active = false;
	ui.settings_index = 4;
	settings_ui_activate(&ui, 43U);
	assert(ui.settings_detail_active);
	backend_calls = 0U;
	settings_ui_activate(&ui, 44U);
	assert(backend_calls == 1U);
	assert(ui.settings.reboot_pending.request_id != 0U);
	push_result(ui.settings.reboot_pending.request_id,
		SETTINGS_HW_REBOOT_CUSTOM, 0, 0);
	assert(settings_ui_update(&ui, 45U));
	assert(ui.settings.reboot_pending.request_id == 0U);
	ui.settings_detail_active = false;
	ui.settings_index = 0;
	backend_calls = 0U;
	overlay_calls = 0U;
	settings_ui_activate(&ui, 11U);
	assert(ui.settings_detail_active);
	settings_ui_adjust(&ui, 1, 12U);
	first = ui.settings.last_request_id;
	assert(ui.settings.pending[SETTINGS_PENDING_BRIGHTNESS].value == 13);
	settings_ui_adjust(&ui, 1, 13U);
	latest = ui.settings.last_request_id;
	assert(latest != first);
	assert(ui.settings.pending[SETTINGS_PENDING_BRIGHTNESS].value == 18);

	/* An older success cannot commit or display over the newer request. */
	push_result(first, SETTINGS_HW_BRIGHTNESS, 13, 0);
	assert(!settings_ui_update(&ui, 20U));
	assert(ui.settings.preferences.backlight_percent == 8);
	assert(persistence_calls == 0U);
	assert(overlay_calls == 0U);

	/* Latest failure preserves the last confirmed value and wins the OSD. */
	push_result(latest, SETTINGS_HW_BRIGHTNESS, 18, 7);
	assert(settings_ui_update(&ui, 21U));
	assert(ui.settings.preferences.backlight_percent == 8);
	assert(persistence_calls == 0U);
	assert(overlay_calls == 1U);
	assert(strstr(ui.settings.status_text, "hardware_failed") != NULL);

	/* Success commits state, but an epoch change suppresses old-page OSD. */
	reset_results();
	settings_ui_adjust(&ui, -1, 30U);
	off_page = ui.settings.last_request_id;
	settings_ui_leave_page(&ui);
	++ui.navigation_epoch;
	ui.nav_index = NAV_PAGE_LIBRARY;
	overlay_calls = 0U;
	push_result(off_page, SETTINGS_HW_BRIGHTNESS, 3, 0);
	assert(!settings_ui_update(&ui, 31U));
	assert(ui.settings.preferences.backlight_percent == 3);
	assert(persistence_calls == 1U);
	assert(overlay_calls == 0U);

	/* 0..100 volume uses the same confirmed-only result contract. */
	reset_results();
	ui.nav_index = NAV_PAGE_SETTINGS;
	++ui.navigation_epoch;
	ui.settings_index = 5;
	ui.settings_detail_active = true;
	settings_ui_adjust(&ui, 1, 40U);
	volume = ui.settings.last_request_id;
	assert(ui.settings.pending[SETTINGS_PENDING_VOLUME].value == 100);
	push_result(volume, SETTINGS_HW_VOLUME, 100, 0);
	assert(settings_ui_update(&ui, 41U));
	assert(ui.settings.volume_target == 100);
	assert(overlay_calls == 1U);
	backend_calls = 0U;
	settings_ui_adjust(&ui, 1, 42U);
	assert(backend_calls == 0U);

	/* Scan completion is AP-only and belongs to its originating Wi-Fi view. */
	reset_results();
	ui.nav_index = NAV_PAGE_NETWORK;
	++ui.navigation_epoch;
	ui.network.selected = NETWORK_UI_WIFI;
	ui.network.detail_active = true;
	ui.network.scan_pending = true;
	ui.network.scan_navigation_epoch = ui.navigation_epoch;
	overlays_before = overlay_calls;
	push_result(++next_request_id, SETTINGS_HW_WIFI_SCAN, 0, 0);
	assert(settings_ui_update(&ui, 50U));
	assert(network_ap_loads == 1U);
	assert(network_full_loads == 0U);
	assert(!ui.network.scan_pending);
	assert(ui.nav_index == NAV_PAGE_NETWORK);
	assert(ui.network.detail_active);
	assert(overlay_calls == overlays_before + 1U);

	/* A late background scan updates AP data without painting over a new page. */
	reset_results();
	ui.network.scan_pending = true;
	ui.network.scan_navigation_epoch = ui.navigation_epoch;
	ui.network.detail_active = false;
	ui.nav_index = NAV_PAGE_LIBRARY;
	++ui.navigation_epoch;
	overlays_before = overlay_calls;
	push_result(++next_request_id, SETTINGS_HW_WIFI_SCAN, 0, 0);
	assert(!settings_ui_update(&ui, 51U));
	assert(network_ap_loads == 2U);
	assert(network_full_loads == 0U);
	assert(!ui.network.scan_pending);
	assert(ui.nav_index == NAV_PAGE_LIBRARY);
	assert(overlay_calls == overlays_before);

	/* Non-scan network actions still commit the full confirmed snapshot. */
	reset_results();
	push_result(++next_request_id, SETTINGS_HW_WIFI_CONNECT, 0, 0);
	assert(settings_ui_update(&ui, 52U));
	assert(network_ap_loads == 2U);
	assert(network_full_loads == 1U);

	/* Silent periodic status commits state but never steals another page OSD. */
	reset_results();
	ui.network.status_pending = true;
	ui.network.status_request_id = ++next_request_id;
	overlays_before = overlay_calls;
	push_result(ui.network.status_request_id, SETTINGS_HW_NETWORK_STATUS, 0, 0);
	assert(settings_ui_update(&ui, 53U));
	assert(!ui.network.status_pending);
	assert(network_full_loads == 2U);
	assert(overlay_calls == overlays_before);

	puts("SETTINGS_STATE_TEST PASS enter-before-write stale=ignored off-page=suppressed confirmed-only=1 bounds=0..100 wifi-scan=AP-only reboot=custom status-refresh=silent");
	return 0;
}
