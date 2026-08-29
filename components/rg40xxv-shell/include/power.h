#ifndef RG40XXV_POWER_H
#define RG40XXV_POWER_H

#include <stdbool.h>
#include <stdint.h>

enum power_view_state {
	POWER_VIEW_ACTIVE,
	POWER_VIEW_SCREEN_OFF,
	POWER_VIEW_LOCKED,
	POWER_VIEW_SHUTDOWN_COUNTDOWN,
};

enum power_action {
	POWER_ACTION_NONE = 0,
	POWER_ACTION_BACKLIGHT_OFF = 1U << 0,
	POWER_ACTION_BACKLIGHT_ON = 1U << 1,
	POWER_ACTION_LOCK_PROGRESS = 1U << 2,
	POWER_ACTION_UNLOCKED = 1U << 3,
	POWER_ACTION_SHOW_SHUTDOWN = 1U << 4,
	POWER_ACTION_CANCEL_SHUTDOWN = 1U << 5,
	POWER_ACTION_REQUEST_SHUTDOWN = 1U << 6,
};

struct power_state {
	enum power_view_state view;
	enum power_view_state before_shutdown;
	uint32_t power_pressed_at;
	uint32_t shutdown_started_at;
	uint32_t unlock_deadline;
	int unlock_key;
	int unlock_progress;
	bool lock_enabled;
	bool locked;
	bool power_held;
	bool shutdown_requested;
};

void power_state_init(struct power_state *state, bool lock_enabled);
unsigned int power_set_lock_enabled(struct power_state *state, bool enabled);
unsigned int power_button_press(struct power_state *state, uint32_t now);
unsigned int power_button_release(struct power_state *state, uint32_t now);
unsigned int power_update(struct power_state *state, uint32_t now);
unsigned int power_lock_key(struct power_state *state, int key,
			    bool eligible, uint32_t now);
unsigned int power_cancel_shutdown(struct power_state *state);
bool power_should_render(const struct power_state *state);
int power_next_timeout_ms(const struct power_state *state, uint32_t now);

#endif
