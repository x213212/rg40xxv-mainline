#include "power.h"

#include <assert.h>
#include <stdio.h>

static void short_power(struct power_state *state, uint32_t now)
{
	(void)power_button_press(state, now);
	(void)power_button_release(state, now + 80U);
}

int main(void)
{
	struct power_state state;
	unsigned int action;

	power_state_init(&state, true);
	short_power(&state, 0U);
	assert(state.view == POWER_VIEW_SCREEN_OFF && state.locked);
	assert(!power_should_render(&state));
	short_power(&state, 200U);
	assert(state.view == POWER_VIEW_LOCKED && state.unlock_progress == 0);
	(void)power_lock_key(&state, 99, false, 300U);
	assert(state.unlock_progress == 0);
	action = power_lock_key(&state, 10, true, 400U);
	assert(action == POWER_ACTION_LOCK_PROGRESS && state.unlock_progress == 1);
	(void)power_lock_key(&state, 11, true, 500U);
	assert(state.unlock_progress == 1);
	(void)power_lock_key(&state, 11, true, 600U);
	action = power_lock_key(&state, 11, true, 700U);
	assert((action & POWER_ACTION_UNLOCKED) != 0 && !state.locked);
	power_state_init(&state, true);
	short_power(&state, 800U);
	short_power(&state, 900U);
	(void)power_lock_key(&state, 7, true, 1000U);
	(void)power_lock_key(&state, 7, true, 2300U);
	assert(state.unlock_progress == 1);
	power_state_init(&state, false);
	assert(power_idle_timeout_ms(&state, 1000U, 60000U, 1000U) == 60000);
	assert(power_idle_timeout_ms(&state, 1000U, 60000U, 60999U) == 1);
	assert(power_idle_timeout_ms(&state, 1000U, 60000U, 61000U) == 0);
	action = power_auto_screen_off(&state);
	assert(action == POWER_ACTION_BACKLIGHT_OFF);
	assert(state.view == POWER_VIEW_SCREEN_OFF && !state.locked);
	assert(power_idle_timeout_ms(&state, 1000U, 60000U, 61000U) == -1);
	short_power(&state, 61100U);
	assert(state.view == POWER_VIEW_ACTIVE);
	short_power(&state, 1000U);
	assert(state.view == POWER_VIEW_SCREEN_OFF && !state.locked);
	short_power(&state, 1200U);
	assert(state.view == POWER_VIEW_ACTIVE);
	(void)power_button_press(&state, 2000U);
	action = power_update(&state, 5000U);
	assert((action & POWER_ACTION_SHOW_SHUTDOWN) != 0);
	action = power_button_release(&state, 5100U);
	assert(action == POWER_ACTION_CANCEL_SHUTDOWN);
	(void)power_button_press(&state, 6000U);
	(void)power_update(&state, 9000U);
	action = power_update(&state, 12000U);
	assert(action == POWER_ACTION_REQUEST_SHUTDOWN);
	puts("POWER_STATE_TEST PASS");
	return 0;
}
