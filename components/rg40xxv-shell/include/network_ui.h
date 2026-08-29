#ifndef RG40XXV_NETWORK_UI_H
#define RG40XXV_NETWORK_UI_H

#include <stdbool.h>

enum network_ui_mode {
	NETWORK_UI_WIFI,
	NETWORK_UI_HOTSPOT,
	NETWORK_UI_USB_DEBUG,
	NETWORK_UI_MODE_COUNT,
};

enum network_ui_action {
	NETWORK_UI_ACTION_OPEN,
	NETWORK_UI_ACTION_REFRESH,
	NETWORK_UI_ACTION_TOGGLE,
};

typedef int (*network_ui_action_callback)(void *context,
					   enum network_ui_mode mode,
					   enum network_ui_action action);

/*
 * This state is deliberately backend-neutral.  A future Wi-Fi/hotspot daemon
 * can publish state here and install a fixed-action callback without teaching
 * the renderer about commands, credentials, or shell strings.
 */
struct network_ui_state {
	enum network_ui_mode selected;
	network_ui_action_callback action;
	void *action_context;
	bool backend_available;
	bool wifi_enabled;
	bool hotspot_enabled;
};

void network_ui_state_init(struct network_ui_state *state);
void network_ui_state_set_backend(struct network_ui_state *state,
				  network_ui_action_callback action,
				  void *context);
void network_ui_state_select(struct network_ui_state *state, int direction);
int network_ui_state_activate(struct network_ui_state *state,
			      enum network_ui_action action);

#endif
