#include "input_latch_snapshot.h"

#include <linux/input.h>
#include <string.h>

static bool bit_is_set(const unsigned long *bits, unsigned int code)
{
	const size_t bits_per_word = sizeof(bits[0]) * 8U;

	return (bits[code / bits_per_word] &
		(1UL << (code % bits_per_word))) != 0UL;
}

static bool snapshot_evdev(
	struct input_latch *latch, struct input_navigation *navigation,
	enum input_latch_evdev_profile profile, uint32_t now,
	input_latch_bits_reader key_reader,
	input_latch_bits_reader abs_capability_reader,
	input_navigation_abs_reader abs_reader, void *context)
{
	unsigned long key_bits[(KEY_CNT + sizeof(unsigned long) * 8U - 1U) /
			       (sizeof(unsigned long) * 8U)] = { 0 };
	unsigned long abs_bits[(ABS_CNT + sizeof(unsigned long) * 8U - 1U) /
			       (sizeof(unsigned long) * 8U)] = { 0 };
	static const unsigned int axes[] = {
		ABS_X, ABS_Y, ABS_RX, ABS_RY, ABS_HAT0X, ABS_HAT0Y,
	};
	bool button_active[INPUT_LATCH_EVDEV_BUTTON_COUNT] = { false };
	bool axis_active[INPUT_LATCH_EVDEV_AXIS_COUNT] = { false };

	if (key_reader == NULL || abs_capability_reader == NULL ||
	    abs_reader == NULL || key_reader(context, key_bits,
	    sizeof(key_bits)) != 0 || abs_capability_reader(context, abs_bits,
	    sizeof(abs_bits)) != 0)
		return false;
	for (unsigned int code = 0; code < KEY_CNT; ++code) {
		int index = input_latch_evdev_button_index(profile, code);

		if (index >= 0)
			button_active[index] = bit_is_set(key_bits, code);
	}
	input_navigation_reset_state(navigation);
	for (size_t i = 0; i < sizeof(axes) / sizeof(axes[0]); ++i) {
		struct input_absinfo info;
		int index = input_latch_evdev_axis_index(axes[i]);

		if (!bit_is_set(abs_bits, axes[i]))
			continue;
		memset(&info, 0, sizeof(info));
		if (abs_reader(context, axes[i], &info) != 0)
			return false;
		if (i < (size_t)INPUT_NAVIGATION_ANALOG_AXIS_COUNT &&
		    !navigation->analog[i].configured &&
		    !input_navigation_configure_axis(navigation, axes[i], &info))
			return false;
		if (index < 0)
			return false;
		axis_active[index] = !input_navigation_abs_is_neutral(navigation,
			axes[i], info.value);
		(void)input_navigation_handle_abs(navigation, axes[i], info.value);
	}
	for (size_t i = 0; i < (size_t)INPUT_LATCH_EVDEV_BUTTON_COUNT; ++i)
		input_latch_set(latch, INPUT_LATCH_EVDEV_BUTTON_BASE + i,
			button_active[i], now);
	for (size_t i = 0; i < (size_t)INPUT_LATCH_EVDEV_AXIS_COUNT; ++i)
		input_latch_set(latch, INPUT_LATCH_EVDEV_AXIS_BASE + i,
			axis_active[i], now);
	return true;
}

bool input_latch_evdev_reacquire(
	struct input_latch *latch, struct input_navigation *navigation,
	enum input_latch_evdev_profile profile, uint32_t now,
	uint32_t stable_interval_ms, input_latch_bits_reader key_reader,
	input_latch_bits_reader abs_capability_reader,
	input_navigation_abs_reader abs_reader, void *context)
{
	input_latch_init(latch, now, stable_interval_ms);
	if (snapshot_evdev(latch, navigation, profile, now, key_reader,
		abs_capability_reader, abs_reader, context))
		return true;
	input_latch_set(latch, INPUT_LATCH_EVDEV_UNKNOWN, true, now);
	return false;
}
