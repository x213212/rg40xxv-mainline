#include "input_navigation.h"

#include <assert.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>

struct fake_abs_device {
	unsigned int calls[INPUT_NAVIGATION_ANALOG_AXIS_COUNT];
	size_t call_count;
};

static int read_h700_axis(void *context, unsigned int code,
			  struct input_absinfo *info)
{
	struct fake_abs_device *device = context;

	assert(device->call_count < INPUT_NAVIGATION_ANALOG_AXIS_COUNT);
	device->calls[device->call_count++] = code;
	info->value = 0;
	info->minimum = -1800;
	info->maximum = 1800;
	info->flat = 32;
	return 0;
}

static void test_h700_calibration_and_hysteresis(void)
{
	struct input_navigation navigation;
	struct fake_abs_device device = { 0 };
	const struct input_navigation_axis *axis;

	input_navigation_init(&navigation);
	assert(input_navigation_configure_analog_axes(&navigation,
		read_h700_axis, &device) == INPUT_NAVIGATION_ANALOG_AXIS_COUNT);
	assert(device.call_count == INPUT_NAVIGATION_ANALOG_AXIS_COUNT);
	assert(device.calls[0] == ABS_X);
	assert(device.calls[1] == ABS_Y);
	assert(device.calls[2] == ABS_RX);
	assert(device.calls[3] == ABS_RY);

	axis = &navigation.analog[0];
	assert(axis->configured);
	assert(axis->minimum == -1800);
	assert(axis->maximum == 1800);
	assert(axis->center == 0);
	assert(axis->flat == 32);
	assert(axis->negative_press == -900);
	assert(axis->negative_release == -450);
	assert(axis->positive_release == 450);
	assert(axis->positive_press == 900);
	assert(input_navigation_abs_is_neutral(&navigation, ABS_X, -450));
	assert(input_navigation_abs_is_neutral(&navigation, ABS_X, 450));
	assert(!input_navigation_abs_is_neutral(&navigation, ABS_X, -451));
	assert(!input_navigation_abs_is_neutral(&navigation, ABS_X, 451));

	assert(input_navigation_handle_abs(&navigation, ABS_X, 899) ==
		INPUT_NAVIGATION_NONE);
	assert(input_navigation_handle_abs(&navigation, ABS_X, 900) ==
		INPUT_NAVIGATION_RIGHT);
	assert(input_navigation_handle_abs(&navigation, ABS_X, 700) ==
		INPUT_NAVIGATION_NONE);
	assert(navigation.analog[0].direction == 1);
	assert(input_navigation_handle_abs(&navigation, ABS_X, 451) ==
		INPUT_NAVIGATION_NONE);
	assert(navigation.analog[0].direction == 1);
	assert(input_navigation_handle_abs(&navigation, ABS_X, 450) ==
		INPUT_NAVIGATION_NONE);
	assert(navigation.analog[0].direction == 0);
	assert(input_navigation_handle_abs(&navigation, ABS_X, -899) ==
		INPUT_NAVIGATION_NONE);
	assert(input_navigation_handle_abs(&navigation, ABS_X, -900) ==
		INPUT_NAVIGATION_LEFT);
	assert(input_navigation_handle_abs(&navigation, ABS_X, -100) ==
		INPUT_NAVIGATION_NONE);
	assert(navigation.analog[0].direction == 0);

	for (int value = -100; value <= 100; value += 10)
		assert(input_navigation_handle_abs(&navigation, ABS_X, value) ==
			INPUT_NAVIGATION_NONE);
}

static void test_both_sticks_and_hat(void)
{
	struct input_navigation navigation;
	struct fake_abs_device device = { 0 };

	input_navigation_init(&navigation);
	assert(input_navigation_configure_analog_axes(&navigation,
		read_h700_axis, &device) == INPUT_NAVIGATION_ANALOG_AXIS_COUNT);

	assert(input_navigation_handle_abs(&navigation, ABS_Y, -1000) ==
		INPUT_NAVIGATION_UP);
	assert(input_navigation_handle_abs(&navigation, ABS_Y, 0) ==
		INPUT_NAVIGATION_NONE);
	assert(input_navigation_handle_abs(&navigation, ABS_RX, 1000) ==
		INPUT_NAVIGATION_RIGHT);
	assert(input_navigation_handle_abs(&navigation, ABS_RX, 0) ==
		INPUT_NAVIGATION_NONE);
	assert(input_navigation_handle_abs(&navigation, ABS_RY, 1000) ==
		INPUT_NAVIGATION_DOWN);

	assert(input_navigation_handle_abs(&navigation, ABS_HAT0X, -1) ==
		INPUT_NAVIGATION_LEFT);
	assert(input_navigation_handle_abs(&navigation, ABS_HAT0X, -1) ==
		INPUT_NAVIGATION_NONE);
	assert(input_navigation_handle_abs(&navigation, ABS_HAT0X, 0) ==
		INPUT_NAVIGATION_NONE);
	assert(input_navigation_handle_abs(&navigation, ABS_HAT0X, 1) ==
		INPUT_NAVIGATION_RIGHT);
	assert(input_navigation_handle_abs(&navigation, ABS_HAT0Y, -1) ==
		INPUT_NAVIGATION_UP);
	assert(input_navigation_handle_abs(&navigation, ABS_HAT0Y, 0) ==
		INPUT_NAVIGATION_NONE);
	assert(input_navigation_handle_abs(&navigation, ABS_HAT0Y, 1) ==
		INPUT_NAVIGATION_DOWN);
	assert(input_navigation_abs_is_neutral(&navigation, ABS_HAT0X, 0));
	assert(!input_navigation_abs_is_neutral(&navigation, ABS_HAT0X, -1));
	input_navigation_reset_state(&navigation);
	assert(navigation.analog[1].direction == 0);
	assert(navigation.analog[2].direction == 0);
	assert(navigation.analog[3].direction == 0);
	assert(navigation.hat[0] == 0);
	assert(navigation.hat[1] == 0);
}

static void test_center_guard_and_invalid_axis(void)
{
	struct input_navigation navigation;
	struct input_absinfo info = {
		.value = 1800,
		.minimum = -1800,
		.maximum = 1800,
		.flat = 32,
	};

	input_navigation_init(&navigation);
	assert(input_navigation_configure_axis(&navigation, ABS_X, &info));
	assert(navigation.analog[0].center == 0);
	info.value = 24;
	assert(input_navigation_configure_axis(&navigation, ABS_X, &info));
	assert(navigation.analog[0].center == 24);
	assert(!input_navigation_configure_axis(&navigation, ABS_Z, &info));
	info.maximum = info.minimum;
	assert(!input_navigation_configure_axis(&navigation, ABS_X, &info));
	assert(!navigation.analog[0].configured);
	assert(input_navigation_handle_abs(&navigation, ABS_X, INT_MAX) ==
		INPUT_NAVIGATION_NONE);
	assert(input_navigation_abs_is_neutral(&navigation, ABS_X, INT_MAX));
	assert(input_navigation_abs_is_neutral(&navigation, ABS_Z, INT_MAX));
}

int main(void)
{
	test_h700_calibration_and_hysteresis();
	test_both_sticks_and_hat();
	test_center_guard_and_invalid_axis();
	puts("INPUT_NAVIGATION_TEST PASS axes=X/Y/RX/RY range=-1800..1800");
	return 0;
}
