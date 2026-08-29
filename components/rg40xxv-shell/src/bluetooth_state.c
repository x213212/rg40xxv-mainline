#include "bluetooth_ui.h"

#include <errno.h>
#include <string.h>

void bluetooth_ui_state_init(struct bluetooth_ui_state *state)
{
	memset(state, 0, sizeof(*state));
}

void bluetooth_ui_state_set_backend(struct bluetooth_ui_state *state,
				    bluetooth_ui_action_callback action,
				    void *context)
{
	state->action = action;
	state->action_context = context;
	state->backend_available = action != NULL;
}

void bluetooth_ui_state_set_adapter(struct bluetooth_ui_state *state,
				    bool present, bool powered)
{
	state->adapter_present = present;
	if (!present) {
		state->powered = false;
		state->scanning = false;
		state->device_count = 0;
		state->selected = 0;
		state->scroll_top = 0;
		return;
	}
	state->powered = powered;
	if (!powered) {
		state->scanning = false;
		state->device_count = 0;
		state->selected = 0;
		state->scroll_top = 0;
	}
}

void bluetooth_ui_state_set_devices(struct bluetooth_ui_state *state,
				    const struct bluetooth_ui_device *devices,
				    unsigned int count)
{
	char keep[BLUETOOTH_UI_ADDRESS_LEN] = { 0 };
	unsigned int i;

	/*
	 * Remember the highlighted address before replacing the list.  A scan
	 * republishes the whole array every couple of seconds, and an index-
	 * based selection would jump around under the user's thumb as devices
	 * appear and reorder.
	 */
	if (state->selected < state->device_count)
		memcpy(keep, state->devices[state->selected].address,
		       sizeof(keep));

	if (count > BLUETOOTH_UI_MAX_DEVICES)
		count = BLUETOOTH_UI_MAX_DEVICES;
	if (count > 0)
		memcpy(state->devices, devices,
		       count * sizeof(state->devices[0]));
	state->device_count = count;

	state->selected = 0;
	if (keep[0] != '\0') {
		for (i = 0; i < count; ++i) {
			if (strncmp(state->devices[i].address, keep,
				    sizeof(keep)) == 0) {
				state->selected = i;
				break;
			}
		}
	}
	bluetooth_ui_state_scroll_into_view(state);
}

void bluetooth_ui_state_select(struct bluetooth_ui_state *state, int direction)
{
	int selected;

	if (direction == 0 || state->device_count == 0)
		return;
	selected = (int)state->selected + (direction > 0 ? 1 : -1);
	if (selected < 0)
		selected = (int)state->device_count - 1;
	else if (selected >= (int)state->device_count)
		selected = 0;
	state->selected = (unsigned int)selected;
	bluetooth_ui_state_scroll_into_view(state);
}

void bluetooth_ui_state_scroll_into_view(struct bluetooth_ui_state *state)
{
	unsigned int last;

	if (state->device_count <= BLUETOOTH_UI_VISIBLE_ROWS) {
		state->scroll_top = 0;
		return;
	}
	if (state->selected < state->scroll_top) {
		state->scroll_top = state->selected;
		return;
	}
	last = state->scroll_top + BLUETOOTH_UI_VISIBLE_ROWS - 1;
	if (state->selected > last)
		state->scroll_top = state->selected -
			(BLUETOOTH_UI_VISIBLE_ROWS - 1);
	if (state->scroll_top + BLUETOOTH_UI_VISIBLE_ROWS > state->device_count)
		state->scroll_top = state->device_count -
			BLUETOOTH_UI_VISIBLE_ROWS;
}

const struct bluetooth_ui_device *
bluetooth_ui_state_selected(const struct bluetooth_ui_state *state)
{
	if (state->device_count == 0 || state->selected >= state->device_count)
		return NULL;
	return &state->devices[state->selected];
}

enum bluetooth_ui_action
bluetooth_ui_state_primary_action(const struct bluetooth_ui_state *state)
{
	const struct bluetooth_ui_device *device =
		bluetooth_ui_state_selected(state);

	if (device == NULL)
		return BLUETOOTH_UI_ACTION_SCAN;
	if (device->connected)
		return BLUETOOTH_UI_ACTION_DISCONNECT;
	if (device->paired)
		return BLUETOOTH_UI_ACTION_CONNECT;
	return BLUETOOTH_UI_ACTION_PAIR;
}

int bluetooth_ui_state_activate(struct bluetooth_ui_state *state,
				enum bluetooth_ui_action action)
{
	const struct bluetooth_ui_device *device;
	const char *address = NULL;

	if (!state->backend_available || state->action == NULL)
		return ENODEV;

	switch (action) {
	case BLUETOOTH_UI_ACTION_POWER_ON:
	case BLUETOOTH_UI_ACTION_POWER_OFF:
	case BLUETOOTH_UI_ACTION_SCAN:
		break;
	default:
		device = bluetooth_ui_state_selected(state);
		if (device == NULL)
			return ENOENT;
		address = device->address;
		break;
	}

	return state->action(state->action_context, action, address);
}
