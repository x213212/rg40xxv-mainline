#include "input_latch.h"
#include "input_latch_snapshot.h"

#include <assert.h>
#include <limits.h>
#include <linux/input.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct fake_evdev {
	unsigned long keys[(KEY_CNT + sizeof(unsigned long) * 8U - 1U) /
			   (sizeof(unsigned long) * 8U)];
	unsigned long abs_capabilities[
		(ABS_CNT + sizeof(unsigned long) * 8U - 1U) /
		(sizeof(unsigned long) * 8U)];
	struct input_absinfo abs[ABS_CNT];
	int fail_abs_code;
	bool fail_keys;
	bool fail_abs_capabilities;
};

static void set_bit(unsigned long *bits, unsigned int code)
{
	const size_t bits_per_word = sizeof(bits[0]) * 8U;

	bits[code / bits_per_word] |= 1UL << (code % bits_per_word);
}

static int fake_read_keys(void *context, unsigned long *bits, size_t size)
{
	struct fake_evdev *device = context;

	if (device->fail_keys || size != sizeof(device->keys))
		return -1;
	memcpy(bits, device->keys, size);
	return 0;
}

static int fake_read_abs_capabilities(void *context, unsigned long *bits,
				      size_t size)
{
	struct fake_evdev *device = context;

	if (device->fail_abs_capabilities ||
	    size != sizeof(device->abs_capabilities))
		return -1;
	memcpy(bits, device->abs_capabilities, size);
	return 0;
}

static int fake_read_abs(void *context, unsigned int code,
			 struct input_absinfo *info)
{
	struct fake_evdev *device = context;

	if (code >= ABS_CNT || device->fail_abs_code == (int)code)
		return -1;
	*info = device->abs[code];
	return 0;
}

static void configure_fake_axis(struct fake_evdev *device, unsigned int code,
				int value)
{
	set_bit(device->abs_capabilities, code);
	device->abs[code] = (struct input_absinfo) {
		.value = value,
		.minimum = -1800,
		.maximum = 1800,
		.flat = 32,
	};
}

static void test_startup_requires_stable_neutral(void)
{
	struct input_latch latch;

	input_latch_init(&latch, 1000U, INPUT_LATCH_NEUTRAL_STABLE_MS);
	assert(input_latch_waiting(&latch));
	assert(!input_latch_update(&latch, 1119U));
	assert(input_latch_update(&latch, 1120U));
	assert(!input_latch_waiting(&latch));
	assert(input_latch_update(&latch, 1121U));
}

static void test_held_activation_and_reacquire(void)
{
	struct input_latch latch;
	const size_t activation = 4U;

	input_latch_init(&latch, 2000U, INPUT_LATCH_NEUTRAL_STABLE_MS);
	input_latch_set(&latch, activation, true, 2000U);
	assert(input_latch_active_count(&latch) == 1U);
	assert(!input_latch_update(&latch, 9000U));
	input_latch_set(&latch, activation, false, 9010U);
	assert(!input_latch_update(&latch, 9129U));
	assert(input_latch_update(&latch, 9130U));

	/* Reopening after a child must gate the same still-held A again. */
	input_latch_init(&latch, 10000U, INPUT_LATCH_NEUTRAL_STABLE_MS);
	input_latch_set(&latch, activation, true, 10000U);
	assert(!input_latch_update(&latch, 20000U));
	input_latch_set(&latch, activation, false, 20010U);
	assert(!input_latch_update(&latch, 20129U));
	assert(input_latch_update(&latch, 20130U));
}

static void test_all_controls_must_release_and_bounce_restarts(void)
{
	struct input_latch latch;

	input_latch_init(&latch, 0U, INPUT_LATCH_NEUTRAL_STABLE_MS);
	input_latch_set(&latch, 1U, true, 1U);
	input_latch_set(&latch, 95U, true, 2U);
	assert(input_latch_active_count(&latch) == 2U);
	input_latch_set(&latch, 1U, false, 20U);
	assert(!input_latch_update(&latch, 1000U));
	input_latch_set(&latch, 95U, false, 1010U);
	assert(!input_latch_update(&latch, 1129U));
	input_latch_set(&latch, 1U, true, 1130U);
	input_latch_set(&latch, 1U, false, 1140U);
	assert(!input_latch_update(&latch, 1259U));
	assert(input_latch_update(&latch, 1260U));
}

static void test_tick_wrap_and_bounds(void)
{
	struct input_latch latch;
	uint32_t started = UINT32_MAX - 40U;

	input_latch_init(&latch, started, INPUT_LATCH_NEUTRAL_STABLE_MS);
	assert(!input_latch_update(&latch, 78U));
	assert(input_latch_update(&latch, 79U));
	input_latch_init(&latch, 7U, INPUT_LATCH_NEUTRAL_STABLE_MS);
	input_latch_set(&latch, INPUT_LATCH_CONTROL_CAPACITY, true, 8U);
	assert(input_latch_active_count(&latch) == 0U);
	input_latch_force_ready(&latch);
	assert(input_latch_update(&latch, 8U));
}

static void test_activation_release_guard(void)
{
	struct input_activation_guard guard;

	input_activation_guard_init(&guard);
	assert(input_activation_guard_consume(&guard));
	/* SDL and raw evdev may both report the same physical down edge. */
	assert(!input_activation_guard_consume(&guard));
	assert(!input_activation_guard_consume(&guard));
	input_activation_guard_release(&guard);
	assert(input_activation_guard_consume(&guard));
	assert(!input_activation_guard_consume(&guard));
}

static void test_evdev_control_classification(void)
{
	static const unsigned int mainline[] = {
		BTN_DPAD_UP, BTN_DPAD_DOWN, BTN_DPAD_LEFT, BTN_DPAD_RIGHT,
		BTN_EAST, BTN_SOUTH, BTN_NORTH, BTN_WEST, BTN_START,
		BTN_SELECT, BTN_MODE, BTN_TL, BTN_TR,
	};
	static const unsigned int stock[] = {
		0x130, 0x131, 0x132, 0x133, 0x137, 0x136, 0x138,
	};
	static const unsigned int axes[] = {
		ABS_X, ABS_Y, ABS_RX, ABS_RY, ABS_HAT0X, ABS_HAT0Y,
	};
	bool seen[INPUT_LATCH_EVDEV_BUTTON_COUNT] = { false };

	for (size_t i = 0; i < sizeof(mainline) / sizeof(mainline[0]); ++i) {
		int index = input_latch_evdev_button_index(
			INPUT_LATCH_EVDEV_H700_MAINLINE, mainline[i]);

		assert(index >= 0 && index < INPUT_LATCH_EVDEV_BUTTON_COUNT);
		assert(!seen[index]);
		seen[index] = true;
	}
	assert(input_latch_evdev_button_index(
		INPUT_LATCH_EVDEV_H700_MAINLINE, BTN_EAST) == 4);
	assert(input_latch_evdev_button_index(
		INPUT_LATCH_EVDEV_H700_MAINLINE, BTN_SOUTH) == 5);
	for (size_t i = 0; i < sizeof(stock) / sizeof(stock[0]); ++i)
		assert(input_latch_evdev_button_index(
			INPUT_LATCH_EVDEV_ANBERNIC_STOCK, stock[i]) >= 0);
	assert(input_latch_evdev_button_index(
		INPUT_LATCH_EVDEV_ANBERNIC_STOCK, 0x130) == 0);
	assert(input_latch_evdev_button_index(
		INPUT_LATCH_EVDEV_ANBERNIC_STOCK, 0x131) == 1);

	/* Power and volume remain owned by their existing service paths. */
	assert(input_latch_evdev_button_index(
		INPUT_LATCH_EVDEV_H700_MAINLINE, KEY_POWER) < 0);
	assert(input_latch_evdev_button_index(
		INPUT_LATCH_EVDEV_H700_MAINLINE, KEY_VOLUMEUP) < 0);
	assert(input_latch_evdev_button_index(
		INPUT_LATCH_EVDEV_H700_MAINLINE, KEY_VOLUMEDOWN) < 0);
	assert(input_latch_evdev_button_index(
		INPUT_LATCH_EVDEV_ANBERNIC_STOCK, KEY_VOLUMEUP) < 0);
	assert(input_latch_evdev_button_index(
		INPUT_LATCH_EVDEV_ANBERNIC_STOCK, KEY_VOLUMEDOWN) < 0);

	/* MODE+START stay visible to the supervisor and hold the UI latch. */
	assert(input_latch_evdev_button_index(
		INPUT_LATCH_EVDEV_H700_MAINLINE, BTN_MODE) >= 0);
	assert(input_latch_evdev_button_index(
		INPUT_LATCH_EVDEV_H700_MAINLINE, BTN_START) >= 0);
	assert(input_latch_evdev_button_index(
		INPUT_LATCH_EVDEV_ANBERNIC_STOCK, 0x138) >= 0);
	assert(input_latch_evdev_button_index(
		INPUT_LATCH_EVDEV_ANBERNIC_STOCK, 0x137) >= 0);

	for (size_t i = 0; i < sizeof(axes) / sizeof(axes[0]); ++i)
		assert(input_latch_evdev_axis_index(axes[i]) == (int)i);
	assert(input_latch_evdev_axis_index(ABS_Z) < 0);
	assert(input_latch_evdev_axis_index(ABS_RZ) < 0);
}

static void test_evdev_snapshot_and_fail_closed_recovery(void)
{
	struct input_latch latch;
	struct input_navigation navigation;
	struct fake_evdev device = { .fail_abs_code = -1 };

	input_navigation_init(&navigation);
	set_bit(device.keys, BTN_EAST);
	set_bit(device.keys, KEY_VOLUMEUP);
	configure_fake_axis(&device, ABS_X, 1000);
	configure_fake_axis(&device, ABS_HAT0Y, -1);
	assert(input_latch_evdev_reacquire(&latch, &navigation,
		INPUT_LATCH_EVDEV_H700_MAINLINE, 1000U,
		INPUT_LATCH_NEUTRAL_STABLE_MS, fake_read_keys,
		fake_read_abs_capabilities, fake_read_abs, &device));
	assert(input_latch_active_count(&latch) == 3U);
	assert(input_latch_waiting(&latch));
	assert(navigation.analog[0].configured);
	assert(navigation.analog[0].direction == 1);
	assert(navigation.hat[1] == -1);
	assert(!input_latch_update(&latch, 9000U));

	memset(device.keys, 0, sizeof(device.keys));
	device.abs[ABS_X].value = 0;
	device.abs[ABS_HAT0Y].value = 0;
	assert(input_latch_evdev_reacquire(&latch, &navigation,
		INPUT_LATCH_EVDEV_H700_MAINLINE, 9010U,
		INPUT_LATCH_NEUTRAL_STABLE_MS, fake_read_keys,
		fake_read_abs_capabilities, fake_read_abs, &device));
	assert(input_latch_active_count(&latch) == 0U);
	assert(!input_latch_update(&latch, 9129U));
	assert(input_latch_update(&latch, 9130U));

	/* A supported axis read failure cannot be mistaken for neutral. */
	set_bit(device.keys, BTN_EAST);
	device.fail_abs_code = ABS_X;
	assert(!input_latch_evdev_reacquire(&latch, &navigation,
		INPUT_LATCH_EVDEV_H700_MAINLINE, 10000U,
		INPUT_LATCH_NEUTRAL_STABLE_MS, fake_read_keys,
		fake_read_abs_capabilities, fake_read_abs, &device));
	assert(input_latch_active_count(&latch) == 1U);
	assert(!input_latch_update(&latch, 30000U));

	/* A later complete snapshot clears the unknown-state sentinel. */
	memset(device.keys, 0, sizeof(device.keys));
	device.fail_abs_code = -1;
	assert(input_latch_evdev_reacquire(&latch, &navigation,
		INPUT_LATCH_EVDEV_H700_MAINLINE, 30010U,
		INPUT_LATCH_NEUTRAL_STABLE_MS, fake_read_keys,
		fake_read_abs_capabilities, fake_read_abs, &device));
	assert(input_latch_active_count(&latch) == 0U);
	assert(input_latch_update(&latch, 30130U));

	device.fail_keys = true;
	assert(!input_latch_evdev_reacquire(&latch, &navigation,
		INPUT_LATCH_EVDEV_H700_MAINLINE, 40000U,
		INPUT_LATCH_NEUTRAL_STABLE_MS, fake_read_keys,
		fake_read_abs_capabilities, fake_read_abs, &device));
	assert(input_latch_active_count(&latch) == 1U);
	device.fail_keys = false;
	device.fail_abs_capabilities = true;
	assert(!input_latch_evdev_reacquire(&latch, &navigation,
		INPUT_LATCH_EVDEV_H700_MAINLINE, 50000U,
		INPUT_LATCH_NEUTRAL_STABLE_MS, fake_read_keys,
		fake_read_abs_capabilities, fake_read_abs, &device));
	assert(input_latch_active_count(&latch) == 1U);
}

int main(void)
{
	test_startup_requires_stable_neutral();
	test_held_activation_and_reacquire();
	test_all_controls_must_release_and_bounce_restarts();
	test_tick_wrap_and_bounds();
	test_activation_release_guard();
	test_evdev_control_classification();
	test_evdev_snapshot_and_fail_closed_recovery();
	puts("INPUT_LATCH_TEST PASS boot=reacquire neutral_ms=120 controls=buttons+dpad+axes");
	return 0;
}
