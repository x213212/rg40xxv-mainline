#ifndef RG40XXV_INPUT_LATCH_SNAPSHOT_H
#define RG40XXV_INPUT_LATCH_SNAPSHOT_H

#include "input_latch.h"
#include "input_navigation.h"

#include <stddef.h>

enum {
	INPUT_LATCH_EVDEV_BUTTON_BASE = 0,
	INPUT_LATCH_EVDEV_AXIS_BASE = 16,
	INPUT_LATCH_EVDEV_UNKNOWN = 127,
};

typedef int (*input_latch_bits_reader)(void *context, unsigned long *bits,
				       size_t size);

bool input_latch_evdev_reacquire(
	struct input_latch *latch, struct input_navigation *navigation,
	enum input_latch_evdev_profile profile, uint32_t now,
	uint32_t stable_interval_ms, input_latch_bits_reader key_reader,
	input_latch_bits_reader abs_capability_reader,
	input_navigation_abs_reader abs_reader, void *context);

#endif
