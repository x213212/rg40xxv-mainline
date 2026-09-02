#include "power.h"

#include <limits.h>
#include <string.h>

enum {
	POWER_HOLD_MS = 3000,
	POWER_COUNTDOWN_MS = 3000,
	UNLOCK_SEQUENCE_MS = 1200,
};

void power_state_init(struct power_state *state, bool lock_enabled)
{
	memset(state, 0, sizeof(*state));
	state->view = POWER_VIEW_ACTIVE;
	state->before_shutdown = POWER_VIEW_ACTIVE;
	state->lock_enabled = lock_enabled;
}

unsigned int power_set_lock_enabled(struct power_state *state, bool enabled)
{
	unsigned int action = POWER_ACTION_NONE;

	state->lock_enabled = enabled;
	if (!enabled && state->locked) {
		state->locked = false;
		state->unlock_progress = 0;
		if (state->view == POWER_VIEW_LOCKED) {
			state->view = POWER_VIEW_ACTIVE;
			action = POWER_ACTION_UNLOCKED;
		}
	}
	return action;
}

unsigned int power_button_press(struct power_state *state, uint32_t now)
{
	if (state->power_held)
		return POWER_ACTION_NONE;
	state->power_held = true;
	state->power_pressed_at = now;
	state->shutdown_requested = false;
	return POWER_ACTION_NONE;
}

static unsigned int short_press(struct power_state *state)
{
	if (state->view == POWER_VIEW_SCREEN_OFF) {
		if (state->locked) {
			state->view = POWER_VIEW_LOCKED;
			state->unlock_progress = 0;
			return POWER_ACTION_BACKLIGHT_ON;
		}
		state->view = POWER_VIEW_ACTIVE;
		return POWER_ACTION_BACKLIGHT_ON;
	}
	if (state->view == POWER_VIEW_ACTIVE || state->view == POWER_VIEW_LOCKED) {
		state->locked = state->lock_enabled;
		state->view = POWER_VIEW_SCREEN_OFF;
		state->unlock_progress = 0;
		return POWER_ACTION_BACKLIGHT_OFF;
	}
	return POWER_ACTION_NONE;
}

unsigned int power_button_release(struct power_state *state, uint32_t now)
{
	uint32_t held;

	if (!state->power_held)
		return POWER_ACTION_NONE;
	held = now - state->power_pressed_at;
	state->power_held = false;
	if (state->view == POWER_VIEW_SHUTDOWN_COUNTDOWN)
		return power_cancel_shutdown(state);
	if (held >= POWER_HOLD_MS)
		return POWER_ACTION_NONE;
	return short_press(state);
}

unsigned int power_auto_screen_off(struct power_state *state)
{
	if (state->power_held ||
	    (state->view != POWER_VIEW_ACTIVE &&
	     state->view != POWER_VIEW_LOCKED))
		return POWER_ACTION_NONE;
	state->locked = state->view == POWER_VIEW_LOCKED || state->lock_enabled;
	state->view = POWER_VIEW_SCREEN_OFF;
	state->unlock_progress = 0;
	return POWER_ACTION_BACKLIGHT_OFF;
}

unsigned int power_update(struct power_state *state, uint32_t now)
{
	if (!state->power_held)
		return POWER_ACTION_NONE;
	if (state->view != POWER_VIEW_SHUTDOWN_COUNTDOWN &&
	    now - state->power_pressed_at >= POWER_HOLD_MS) {
		unsigned int action = POWER_ACTION_SHOW_SHUTDOWN;

		state->before_shutdown = state->view;
		state->view = POWER_VIEW_SHUTDOWN_COUNTDOWN;
		state->shutdown_started_at = now;
		if (state->before_shutdown == POWER_VIEW_SCREEN_OFF)
			action |= POWER_ACTION_BACKLIGHT_ON;
		return action;
	}
	if (state->view == POWER_VIEW_SHUTDOWN_COUNTDOWN &&
	    !state->shutdown_requested &&
	    now - state->shutdown_started_at >= POWER_COUNTDOWN_MS) {
		state->shutdown_requested = true;
		return POWER_ACTION_REQUEST_SHUTDOWN;
	}
	return POWER_ACTION_NONE;
}

unsigned int power_lock_key(struct power_state *state, int key,
			    bool eligible, uint32_t now)
{
	if (state->view != POWER_VIEW_LOCKED || !eligible)
		return POWER_ACTION_NONE;
	if (state->unlock_progress == 0 || key != state->unlock_key ||
	    now - state->unlock_deadline < UINT32_C(0x80000000)) {
		state->unlock_progress = 0;
		state->unlock_key = key;
	}
	++state->unlock_progress;
	state->unlock_deadline = now + UNLOCK_SEQUENCE_MS;
	if (state->unlock_progress < 3)
		return POWER_ACTION_LOCK_PROGRESS;
	state->unlock_progress = 0;
	state->locked = false;
	state->view = POWER_VIEW_ACTIVE;
	return POWER_ACTION_LOCK_PROGRESS | POWER_ACTION_UNLOCKED;
}

unsigned int power_cancel_shutdown(struct power_state *state)
{
	if (state->view != POWER_VIEW_SHUTDOWN_COUNTDOWN)
		return POWER_ACTION_NONE;
	state->view = state->before_shutdown == POWER_VIEW_SCREEN_OFF &&
		state->locked ? POWER_VIEW_LOCKED : POWER_VIEW_ACTIVE;
	state->power_held = false;
	state->shutdown_requested = false;
	return POWER_ACTION_CANCEL_SHUTDOWN;
}

bool power_should_render(const struct power_state *state)
{
	return state->view != POWER_VIEW_SCREEN_OFF;
}

int power_next_timeout_ms(const struct power_state *state, uint32_t now)
{
	uint32_t deadline;

	if (!state->power_held)
		return -1;
	deadline = state->view == POWER_VIEW_SHUTDOWN_COUNTDOWN ?
		state->shutdown_started_at + POWER_COUNTDOWN_MS :
		state->power_pressed_at + POWER_HOLD_MS;
	if (now - deadline < UINT32_C(0x80000000))
		return 0;
	return (int)(deadline - now);
}

int power_idle_timeout_ms(const struct power_state *state,
			  uint32_t last_activity, uint32_t idle_ms,
			  uint32_t now)
{
	uint32_t deadline;
	uint32_t remaining;

	if (idle_ms == 0U || state->power_held ||
	    (state->view != POWER_VIEW_ACTIVE &&
	     state->view != POWER_VIEW_LOCKED))
		return -1;
	deadline = last_activity + idle_ms;
	if (now - deadline < UINT32_C(0x80000000))
		return 0;
	remaining = deadline - now;
	return remaining > (uint32_t)INT_MAX ? INT_MAX : (int)remaining;
}
