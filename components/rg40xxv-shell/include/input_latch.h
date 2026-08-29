#ifndef RG40XXV_INPUT_LATCH_H
#define RG40XXV_INPUT_LATCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	INPUT_LATCH_NEUTRAL_STABLE_MS = 120,
	INPUT_LATCH_CONTROL_CAPACITY = 128,
	INPUT_LATCH_EVDEV_BUTTON_COUNT = 13,
	INPUT_LATCH_EVDEV_AXIS_COUNT = 6,
};

enum input_latch_evdev_profile {
	INPUT_LATCH_EVDEV_H700_MAINLINE,
	INPUT_LATCH_EVDEV_ANBERNIC_STOCK,
};

struct input_latch {
	uint64_t active[INPUT_LATCH_CONTROL_CAPACITY / 64];
	uint32_t neutral_since;
	uint32_t stable_interval_ms;
	bool neutral_since_valid;
	bool waiting;
};

void input_latch_init(struct input_latch *latch, uint32_t now,
		      uint32_t stable_interval_ms);
void input_latch_set(struct input_latch *latch, size_t control, bool active,
		     uint32_t now);
bool input_latch_update(struct input_latch *latch, uint32_t now);
bool input_latch_waiting(const struct input_latch *latch);
size_t input_latch_active_count(const struct input_latch *latch);
void input_latch_force_ready(struct input_latch *latch);

int input_latch_evdev_button_index(enum input_latch_evdev_profile profile,
				   unsigned int code);
int input_latch_evdev_axis_index(unsigned int code);

#endif
