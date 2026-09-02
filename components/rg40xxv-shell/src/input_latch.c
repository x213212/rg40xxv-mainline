#include "input_latch.h"

#include <linux/input.h>
#include <string.h>

static bool input_latch_any_active(const struct input_latch *latch)
{
	for (size_t i = 0; i < sizeof(latch->active) / sizeof(latch->active[0]);
	     ++i) {
		if (latch->active[i] != 0U)
			return true;
	}
	return false;
}

void input_latch_init(struct input_latch *latch, uint32_t now,
		      uint32_t stable_interval_ms)
{
	memset(latch, 0, sizeof(*latch));
	latch->neutral_since = now;
	latch->stable_interval_ms = stable_interval_ms;
	latch->neutral_since_valid = true;
	latch->waiting = true;
}

void input_latch_set(struct input_latch *latch, size_t control, bool active,
		     uint32_t now)
{
	size_t word;
	uint64_t mask;
	bool was_active;

	if (control >= INPUT_LATCH_CONTROL_CAPACITY)
		return;
	word = control / 64U;
	mask = UINT64_C(1) << (control % 64U);
	was_active = (latch->active[word] & mask) != 0U;
	if (active == was_active)
		return;
	if (active) {
		latch->active[word] |= mask;
		latch->neutral_since_valid = false;
		return;
	}
	latch->active[word] &= ~mask;
	if (!input_latch_any_active(latch)) {
		latch->neutral_since = now;
		latch->neutral_since_valid = true;
	}
}

bool input_latch_update(struct input_latch *latch, uint32_t now)
{
	if (!latch->waiting)
		return true;
	if (input_latch_any_active(latch))
		return false;
	if (!latch->neutral_since_valid) {
		latch->neutral_since = now;
		latch->neutral_since_valid = true;
		return false;
	}
	if ((uint32_t)(now - latch->neutral_since) <
	    latch->stable_interval_ms)
		return false;
	latch->waiting = false;
	return true;
}

bool input_latch_waiting(const struct input_latch *latch)
{
	return latch->waiting;
}

size_t input_latch_active_count(const struct input_latch *latch)
{
	size_t count = 0;

	for (size_t word = 0;
	     word < sizeof(latch->active) / sizeof(latch->active[0]); ++word) {
		uint64_t value = latch->active[word];

		while (value != 0U) {
			value &= value - 1U;
			++count;
		}
	}
	return count;
}

void input_latch_force_ready(struct input_latch *latch)
{
	memset(latch->active, 0, sizeof(latch->active));
	latch->neutral_since_valid = true;
	latch->waiting = false;
}

void input_activation_guard_init(struct input_activation_guard *guard)
{
	guard->armed = true;
}

void input_activation_guard_release(struct input_activation_guard *guard)
{
	guard->armed = true;
}

bool input_activation_guard_consume(struct input_activation_guard *guard)
{
	if (!guard->armed)
		return false;
	guard->armed = false;
	return true;
}

int input_latch_evdev_button_index(enum input_latch_evdev_profile profile,
				   unsigned int code)
{
	if (profile == INPUT_LATCH_EVDEV_H700_MAINLINE) {
		switch (code) {
		case BTN_DPAD_UP: return 0;
		case BTN_DPAD_DOWN: return 1;
		case BTN_DPAD_LEFT: return 2;
		case BTN_DPAD_RIGHT: return 3;
		case BTN_EAST: return 4;
		case BTN_SOUTH: return 5;
		case BTN_NORTH: return 6;
		case BTN_WEST: return 7;
		case BTN_START: return 8;
		case BTN_SELECT: return 9;
		case BTN_MODE: return 10;
		case BTN_TL: return 11;
		case BTN_TR: return 12;
		default: return -1;
		}
	}
	/* Vendor 4.9 DTS private A/B/X/Y/SELECT/START/MENU codes. */
	switch (code) {
	case 0x130: return 0;
	case 0x131: return 1;
	case 0x132: return 2;
	case 0x133: return 3;
	case 0x137: return 4;
	case 0x136: return 5;
	case 0x138: return 6;
	default: return -1;
	}
}

int input_latch_evdev_axis_index(unsigned int code)
{
	switch (code) {
	case ABS_X: return 0;
	case ABS_Y: return 1;
	case ABS_RX: return 2;
	case ABS_RY: return 3;
	case ABS_HAT0X: return 4;
	case ABS_HAT0Y: return 5;
	default: return -1;
	}
}
