#include "network_ui.h"

#include <errno.h>
#include <string.h>

void network_ui_state_init(struct network_ui_state *state)
{
	memset(state, 0, sizeof(*state));
	state->selected = NETWORK_UI_WIFI;
}

void network_ui_state_set_backend(struct network_ui_state *state,
				  network_ui_action_callback action,
				  void *context)
{
	state->action = action;
	state->action_context = context;
	state->backend_available = action != NULL;
}

void network_ui_state_select(struct network_ui_state *state, int direction)
{
	int selected;

	if (direction == 0)
		return;
	selected = (int)state->selected + (direction > 0 ? 1 : -1);
	if (selected < 0)
		selected = NETWORK_UI_MODE_COUNT - 1;
	else if (selected >= NETWORK_UI_MODE_COUNT)
		selected = 0;
	state->selected = (enum network_ui_mode)selected;
}

int network_ui_state_activate(struct network_ui_state *state,
			      enum network_ui_action action)
{
	if (!state->backend_available || state->action == NULL)
		return ENODEV;
	return state->action(state->action_context, state->selected, action);
}
