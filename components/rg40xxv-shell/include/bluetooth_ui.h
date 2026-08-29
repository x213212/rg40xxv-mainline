#ifndef RG40XXV_BLUETOOTH_UI_H
#define RG40XXV_BLUETOOTH_UI_H

#include <stdbool.h>
#include <stdint.h>

/*
 * The adapter itself is known good on this device: once hci0 exists the RTL
 * firmware loads normally and the session reports
 *   BLUETOOTH_RESULT capability=PASS powered=yes adapter=yes
 * What was missing was any way to drive it.  render_scene.c had call sites for
 * a Bluetooth page committed without the page, which is why the whole UI
 * stopped compiling.
 *
 * Like network_ui, this state is backend-neutral: it never holds a command
 * string, a shell fragment, or a device path.  A backend publishes the device
 * list and installs one fixed-action callback; the renderer only ever sees
 * already-formatted fields.
 */

#define BLUETOOTH_UI_MAX_DEVICES 16
#define BLUETOOTH_UI_ADDRESS_LEN 18	/* aa:bb:cc:dd:ee:ff + NUL */
#define BLUETOOTH_UI_NAME_LEN 48

enum bluetooth_ui_action {
	BLUETOOTH_UI_ACTION_POWER_ON,
	BLUETOOTH_UI_ACTION_POWER_OFF,
	BLUETOOTH_UI_ACTION_SCAN,
	BLUETOOTH_UI_ACTION_CONNECT,
	BLUETOOTH_UI_ACTION_DISCONNECT,
	BLUETOOTH_UI_ACTION_PAIR,
	BLUETOOTH_UI_ACTION_FORGET,
};

struct bluetooth_ui_device {
	char address[BLUETOOTH_UI_ADDRESS_LEN];
	char name[BLUETOOTH_UI_NAME_LEN];
	bool paired;
	bool connected;
	bool trusted;
};

/*
 * address may be NULL for adapter-wide actions (power, scan).  The backend
 * owns all blocking work; it must return promptly and report completion by
 * publishing a new device list.
 */
typedef int (*bluetooth_ui_action_callback)(void *context,
					    enum bluetooth_ui_action action,
					    const char *address);

struct bluetooth_ui_state {
	bool open;
	bool adapter_present;
	bool powered;
	bool scanning;
	uint32_t scan_started_ms;

	struct bluetooth_ui_device devices[BLUETOOTH_UI_MAX_DEVICES];
	unsigned int device_count;
	unsigned int selected;
	unsigned int scroll_top;

	bluetooth_ui_action_callback action;
	void *action_context;
	bool backend_available;
};

void bluetooth_ui_state_init(struct bluetooth_ui_state *state);
void bluetooth_ui_state_set_backend(struct bluetooth_ui_state *state,
				    bluetooth_ui_action_callback action,
				    void *context);

/* Adapter status published by the backend. */
void bluetooth_ui_state_set_adapter(struct bluetooth_ui_state *state,
				    bool present, bool powered);

/*
 * Replace the whole device list in one go.  Selection is preserved by address
 * where possible so a refresh arriving mid-navigation does not move the
 * highlight out from under the user.
 */
void bluetooth_ui_state_set_devices(struct bluetooth_ui_state *state,
				    const struct bluetooth_ui_device *devices,
				    unsigned int count);

void bluetooth_ui_state_select(struct bluetooth_ui_state *state, int direction);
const struct bluetooth_ui_device *
bluetooth_ui_state_selected(const struct bluetooth_ui_state *state);

/* Returns 0, ENODEV when no backend is installed, or the backend's error. */
int bluetooth_ui_state_activate(struct bluetooth_ui_state *state,
				enum bluetooth_ui_action action);

/*
 * A on the selected row: connect when paired-but-disconnected, disconnect when
 * connected, pair otherwise.  Kept in the state machine rather than the
 * renderer so the same rule is testable without SDL.
 */
enum bluetooth_ui_action
bluetooth_ui_state_primary_action(const struct bluetooth_ui_state *state);

/* Visible rows for the current scroll window. */
#define BLUETOOTH_UI_VISIBLE_ROWS 5
void bluetooth_ui_state_scroll_into_view(struct bluetooth_ui_state *state);

#endif
