#ifndef RG40XXV_POWER_INPUT_FILTER_H
#define RG40XXV_POWER_INPUT_FILTER_H

#include <stdbool.h>
#include <stdint.h>

struct power_input_filter {
	bool sync_lost;
	bool suppress_until_up;
};

enum power_input_filter_action {
	POWER_INPUT_FILTER_NONE,
	POWER_INPUT_FILTER_PRESS,
	POWER_INPUT_FILTER_RELEASE,
	POWER_INPUT_FILTER_CANCEL,
	POWER_INPUT_FILTER_RESYNC,
};

void power_input_filter_init(struct power_input_filter *filter);
enum power_input_filter_action power_input_filter_event(
	struct power_input_filter *filter, uint16_t type, uint16_t code,
	int32_t value);
void power_input_filter_resync(struct power_input_filter *filter,
	bool pressed);
enum power_input_filter_action power_input_filter_fault(
	struct power_input_filter *filter);
bool power_input_ignore_sdl(bool dedicated_power_available,
	bool raw_gamepad_available, bool is_power_key);

#endif
