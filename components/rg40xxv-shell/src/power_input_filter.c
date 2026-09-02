#include "power_input_filter.h"

#include <linux/input-event-codes.h>

void power_input_filter_init(struct power_input_filter *filter)
{
	filter->sync_lost = false;
	filter->suppress_until_up = false;
}

enum power_input_filter_action power_input_filter_event(
	struct power_input_filter *filter, uint16_t type, uint16_t code,
	int32_t value)
{
	if (type == EV_SYN && code == SYN_DROPPED) {
		filter->sync_lost = true;
		filter->suppress_until_up = true;
		return POWER_INPUT_FILTER_CANCEL;
	}
	if (filter->sync_lost) {
		if (type == EV_SYN && code == SYN_REPORT)
			return POWER_INPUT_FILTER_RESYNC;
		return POWER_INPUT_FILTER_NONE;
	}
	if (type != EV_KEY || code != KEY_POWER)
		return POWER_INPUT_FILTER_NONE;
	if (filter->suppress_until_up) {
		if (value == 0)
			filter->suppress_until_up = false;
		return POWER_INPUT_FILTER_NONE;
	}
	if (value == 1)
		return POWER_INPUT_FILTER_PRESS;
	if (value == 0)
		return POWER_INPUT_FILTER_RELEASE;
	return POWER_INPUT_FILTER_NONE;
}

void power_input_filter_resync(struct power_input_filter *filter,
	bool pressed)
{
	filter->sync_lost = false;
	filter->suppress_until_up = pressed;
}

enum power_input_filter_action power_input_filter_fault(
	struct power_input_filter *filter)
{
	power_input_filter_init(filter);
	return POWER_INPUT_FILTER_CANCEL;
}

bool power_input_ignore_sdl(bool dedicated_power_available,
	bool raw_gamepad_available, bool is_power_key)
{
	return is_power_key ? dedicated_power_available : raw_gamepad_available;
}
