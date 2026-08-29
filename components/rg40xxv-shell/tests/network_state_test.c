#include "network_ui.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>

struct capture {
	enum network_ui_mode mode;
	enum network_ui_action action;
	int calls;
};

static int capture_action(void *context, enum network_ui_mode mode,
			  enum network_ui_action action)
{
	struct capture *capture = context;

	capture->mode = mode;
	capture->action = action;
	++capture->calls;
	return 0;
}

int main(void)
{
	struct network_ui_state state;
	struct capture capture = { 0 };

	network_ui_state_init(&state);
	assert(state.selected == NETWORK_UI_WIFI);
	assert(!state.backend_available);
	assert(network_ui_state_activate(&state, NETWORK_UI_ACTION_OPEN) == ENODEV);
	network_ui_state_select(&state, 1);
	assert(state.selected == NETWORK_UI_HOTSPOT);
	network_ui_state_select(&state, -1);
	assert(state.selected == NETWORK_UI_WIFI);
	network_ui_state_select(&state, -1);
	assert(state.selected == NETWORK_UI_USB_DEBUG);
	network_ui_state_select(&state, 1);
	assert(state.selected == NETWORK_UI_WIFI);
	network_ui_state_set_backend(&state, capture_action, &capture);
	assert(state.backend_available);
	assert(network_ui_state_activate(&state, NETWORK_UI_ACTION_REFRESH) == 0);
	assert(capture.calls == 1);
	assert(capture.mode == NETWORK_UI_WIFI);
	assert(capture.action == NETWORK_UI_ACTION_REFRESH);
	puts("NETWORK_STATE_TEST PASS fixed action/state API");
	return 0;
}
