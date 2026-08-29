#include "ui.h"

#include <errno.h>
#include <stdio.h>

static const SDL_Color network_primary = { 238, 238, 238, 255 };
static const SDL_Color network_secondary = { 160, 160, 160, 255 };
static const SDL_Color network_accent = { 220, 220, 220, 255 };

static void draw_network_card(struct ui *ui, enum network_ui_mode mode,
			      enum material_icon_id icon, int x, const char *title,
			      const char *status, const char *detail)
{
	bool selected = ui->focus_region == UI_FOCUS_CONTENT &&
		ui->network.selected == mode;
	SDL_Rect detail_clip = { x + 14, 260, 148, 46 };

	render_fill_round_rect(ui->renderer, x + 3, 153, 176, 166, 18,
		(SDL_Color){ 0, 0, 0, 72 });
	render_fill_round_rect(ui->renderer, x, 148, 176, 166, 18,
		selected ? (SDL_Color){ 52, 52, 52, 226 } :
		(SDL_Color){ 24, 24, 24, 214 });
	render_outline_round_rect(ui->renderer, x, 148, 176, 166, 18,
		selected ? (SDL_Color){ 220, 220, 220, 156 } :
		(SDL_Color){ 180, 180, 180, 52 });
	render_fill_round_rect(ui->renderer, x + 13, 164, 40, 40, 13,
		selected ? (SDL_Color){ 78, 78, 78, 152 } :
		(SDL_Color){ 48, 48, 48, 130 });
	material_icon_draw(ui, icon, x + 21, 172, 24,
		selected ? network_accent : network_secondary, selected, selected);
	text_draw(ui, 1, title, x + 62, 169,
		selected ? network_primary : network_secondary);
	text_draw(ui, 1, status, x + 14, 221,
		selected ? network_accent : network_primary);
	(void)SDL_RenderSetClipRect(ui->renderer, &detail_clip);
	text_draw(ui, 0, detail, x + 18, 260, network_secondary);
	(void)SDL_RenderSetClipRect(ui->renderer, NULL);
}

void render_network_page(struct ui *ui, uint32_t now)
{
	const char *wifi_status = ui->hardware.wifi.operstate ==
		HARDWARE_NETWORK_UP ? tr(ui, "connected") : tr(ui, "disconnected");
	const char *usb_status = ui->settings.usb_debug_available > 0 ?
		tr(ui, ui->settings.preferences.usb_debug_enabled ?
			"enabled" : "disabled") : tr(ui, "unavailable");
	const char *message = ui->action_until > now && ui->action_text != NULL ?
		ui->action_text : tr(ui, "network_backend_reserved");
	SDL_Rect clip = { 28, 374, 584, 34 };

	render_glass_panel(ui->renderer, UI_LAYOUT_CONTENT_X, UI_LAYOUT_CONTENT_Y,
		UI_LAYOUT_CONTENT_WIDTH, UI_LAYOUT_CONTENT_HEIGHT,
		ui->focus_region == UI_FOCUS_CONTENT);
	text_draw(ui, 2, tr(ui, "network_title"), 28, 94,
		ui->focus_region == UI_FOCUS_CONTENT ? network_primary :
		network_secondary);
	text_draw(ui, 0, tr(ui, "network_subtitle"), 28, 121,
		network_secondary);
	draw_network_card(ui, NETWORK_UI_WIFI, MATERIAL_ICON_WIFI, 38,
		tr(ui, "network_wifi_mode"), wifi_status,
		tr(ui, "network_wifi_detail"));
	draw_network_card(ui, NETWORK_UI_HOTSPOT, MATERIAL_ICON_WIFI, 232,
		tr(ui, "network_hotspot_mode"), tr(ui, "not_configured"),
		tr(ui, "network_hotspot_detail"));
	draw_network_card(ui, NETWORK_UI_USB_DEBUG, MATERIAL_ICON_TUNE, 426,
		tr(ui, "usb_debug"), usb_status,
		tr(ui, "network_usb_detail"));
	(void)SDL_RenderSetClipRect(ui->renderer, &clip);
	text_draw(ui, 0, message, 28, 374, network_secondary);
	(void)SDL_RenderSetClipRect(ui->renderer, NULL);
}

void network_ui_select(struct ui *ui, int direction)
{
	network_ui_state_select(&ui->network, direction);
}

void network_ui_activate(struct ui *ui, uint32_t now)
{
	int error;

	if (ui->network.selected == NETWORK_UI_USB_DEBUG) {
		bool enabled = !ui->settings.preferences.usb_debug_enabled;

		error = ui->settings.backend.set_usb_debug == NULL ? ENODEV :
			ui->settings.backend.set_usb_debug(
				ui->settings.backend.context, enabled);
		if (error == 0) {
			ui->settings.preferences.usb_debug_enabled = enabled;
			persistence_request_locale(ui);
		}
	} else {
		error = network_ui_state_activate(&ui->network,
			NETWORK_UI_ACTION_OPEN);
	}

	if (error == ENODEV)
		render_activate(ui, tr(ui, "network_backend_required"), now);
	else if (error != 0)
		render_activate(ui, tr(ui, "network_action_failed"), now);
	else
		render_activate(ui, tr(ui, "network_action_queued"), now);
}
