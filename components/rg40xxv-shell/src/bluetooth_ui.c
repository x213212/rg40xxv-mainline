#include "ui.h"

#include <errno.h>
#include <stdio.h>

static const SDL_Color bluetooth_primary = { 238, 238, 238, 255 };
static const SDL_Color bluetooth_secondary = { 160, 160, 160, 255 };
static const SDL_Color bluetooth_accent = { 220, 220, 220, 255 };
static const SDL_Color bluetooth_connected = { 150, 220, 160, 255 };

#define BLUETOOTH_UI_ICON MATERIAL_ICON_BLUETOOTH

#define BLUETOOTH_ROW_X 38
#define BLUETOOTH_ROW_Y 152
#define BLUETOOTH_ROW_W 564
#define BLUETOOTH_ROW_H 46
#define BLUETOOTH_ROW_GAP 6

static const char *device_status_text(struct ui *ui,
				      const struct bluetooth_ui_device *device)
{
	if (device->connected)
		return tr(ui, "bluetooth_state_connected");
	if (device->paired)
		return tr(ui, "bluetooth_state_paired");
	return tr(ui, "bluetooth_state_available");
}

static void draw_device_row(struct ui *ui,
			    const struct bluetooth_ui_device *device,
			    int y, bool selected)
{
	SDL_Color name_color = selected ? bluetooth_primary : bluetooth_secondary;
	SDL_Color state_color = device->connected ? bluetooth_connected :
		selected ? bluetooth_accent : bluetooth_secondary;
	SDL_Rect name_clip = { BLUETOOTH_ROW_X + 58, y + 6, 330, 20 };

	render_fill_round_rect(ui->renderer, BLUETOOTH_ROW_X, y,
		BLUETOOTH_ROW_W, BLUETOOTH_ROW_H, 12,
		selected ? (SDL_Color){ 52, 52, 52, 226 } :
		(SDL_Color){ 24, 24, 24, 200 });
	render_outline_round_rect(ui->renderer, BLUETOOTH_ROW_X, y,
		BLUETOOTH_ROW_W, BLUETOOTH_ROW_H, 12,
		selected ? (SDL_Color){ 220, 220, 220, 156 } :
		(SDL_Color){ 180, 180, 180, 44 });
	material_icon_draw(ui, BLUETOOTH_UI_ICON, BLUETOOTH_ROW_X + 16, y + 12,
		20, state_color, device->connected, selected);

	(void)SDL_RenderSetClipRect(ui->renderer, &name_clip);
	text_draw(ui, 1, device->name[0] != '\0' ? device->name :
		device->address, BLUETOOTH_ROW_X + 58, y + 6, name_color);
	(void)SDL_RenderSetClipRect(ui->renderer, NULL);

	text_draw(ui, 0, device->address, BLUETOOTH_ROW_X + 58, y + 26,
		bluetooth_secondary);
	text_draw(ui, 0, device_status_text(ui, device),
		BLUETOOTH_ROW_X + 424, y + 16, state_color);
}

void render_bluetooth_page(struct ui *ui, uint32_t now)
{
	const struct bluetooth_ui_state *bt = &ui->bluetooth;
	const char *adapter_status;
	const char *message;
	SDL_Rect footer_clip = { 28, 374, 584, 34 };
	unsigned int i;
	int y = BLUETOOTH_ROW_Y;

	if (!bt->adapter_present)
		adapter_status = tr(ui, "bluetooth_adapter_missing");
	else if (!bt->powered)
		adapter_status = tr(ui, "bluetooth_adapter_off");
	else if (bt->scanning)
		adapter_status = tr(ui, "bluetooth_scanning");
	else
		adapter_status = tr(ui, "bluetooth_adapter_on");

	render_glass_panel(ui->renderer, UI_LAYOUT_CONTENT_X, UI_LAYOUT_CONTENT_Y,
		UI_LAYOUT_CONTENT_WIDTH, UI_LAYOUT_CONTENT_HEIGHT,
		ui->focus_region == UI_FOCUS_CONTENT);
	text_draw(ui, 2, tr(ui, "bluetooth_title"), 28, 94,
		ui->focus_region == UI_FOCUS_CONTENT ? bluetooth_primary :
		bluetooth_secondary);
	text_draw(ui, 0, adapter_status, 28, 121, bluetooth_secondary);

	if (bt->device_count == 0) {
		const char *empty = !bt->adapter_present ?
			tr(ui, "bluetooth_adapter_missing_detail") :
			!bt->powered ? tr(ui, "bluetooth_adapter_off_detail") :
			bt->scanning ? tr(ui, "bluetooth_scanning_detail") :
			tr(ui, "bluetooth_no_devices");

		text_draw(ui, 1, empty, BLUETOOTH_ROW_X, BLUETOOTH_ROW_Y + 40,
			bluetooth_secondary);
	} else {
		for (i = bt->scroll_top;
		     i < bt->device_count &&
		     i < bt->scroll_top + BLUETOOTH_UI_VISIBLE_ROWS; ++i) {
			draw_device_row(ui, &bt->devices[i], y,
				ui->focus_region == UI_FOCUS_CONTENT &&
				i == bt->selected);
			y += BLUETOOTH_ROW_H + BLUETOOTH_ROW_GAP;
		}
		if (bt->device_count > BLUETOOTH_UI_VISIBLE_ROWS) {
			char counter[32];

			(void)snprintf(counter, sizeof(counter), "%u / %u",
				bt->selected + 1, bt->device_count);
			text_draw(ui, 0, counter, 546, 121, bluetooth_secondary);
		}
	}

	message = ui->action_until > now && ui->action_text != NULL ?
		ui->action_text : tr(ui, "bluetooth_hint");
	(void)SDL_RenderSetClipRect(ui->renderer, &footer_clip);
	text_draw(ui, 0, message, 28, 374, bluetooth_secondary);
	(void)SDL_RenderSetClipRect(ui->renderer, NULL);
}

void bluetooth_ui_select(struct ui *ui, int direction)
{
	bluetooth_ui_state_select(&ui->bluetooth, direction);
}

static void report(struct ui *ui, int error, uint32_t now, const char *queued)
{
	if (error == ENODEV)
		render_activate(ui, tr(ui, "bluetooth_backend_required"), now);
	else if (error == ENOENT)
		render_activate(ui, tr(ui, "bluetooth_no_selection"), now);
	else if (error != 0)
		render_activate(ui, tr(ui, "bluetooth_action_failed"), now);
	else
		render_activate(ui, tr(ui, queued), now);
}

void bluetooth_ui_activate(struct ui *ui, uint32_t now)
{
	enum bluetooth_ui_action action;
	const char *queued;

	if (!ui->bluetooth.adapter_present) {
		render_activate(ui, tr(ui, "bluetooth_adapter_missing"), now);
		return;
	}
	if (!ui->bluetooth.powered) {
		report(ui, bluetooth_ui_state_activate(&ui->bluetooth,
			BLUETOOTH_UI_ACTION_POWER_ON), now,
			"bluetooth_powering_on");
		return;
	}
	if (ui->bluetooth.device_count == 0) {
		report(ui, bluetooth_ui_state_activate(&ui->bluetooth,
			BLUETOOTH_UI_ACTION_SCAN), now, "bluetooth_scan_queued");
		return;
	}

	action = bluetooth_ui_state_primary_action(&ui->bluetooth);
	switch (action) {
	case BLUETOOTH_UI_ACTION_DISCONNECT:
		queued = "bluetooth_disconnect_queued";
		break;
	case BLUETOOTH_UI_ACTION_CONNECT:
		queued = "bluetooth_connect_queued";
		break;
	default:
		queued = "bluetooth_pair_queued";
		break;
	}
	report(ui, bluetooth_ui_state_activate(&ui->bluetooth, action), now,
		queued);
}

void bluetooth_ui_forget(struct ui *ui, uint32_t now)
{
	report(ui, bluetooth_ui_state_activate(&ui->bluetooth,
		BLUETOOTH_UI_ACTION_FORGET), now, "bluetooth_forget_queued");
}

void bluetooth_ui_scan(struct ui *ui, uint32_t now)
{
	report(ui, bluetooth_ui_state_activate(&ui->bluetooth,
		BLUETOOTH_UI_ACTION_SCAN), now, "bluetooth_scan_queued");
}
