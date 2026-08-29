#include "input_navigation.h"

#include <stdint.h>
#include <string.h>

static int analog_axis_index(unsigned int code)
{
	switch (code) {
	case ABS_X: return 0;
	case ABS_Y: return 1;
	case ABS_RX: return 2;
	case ABS_RY: return 3;
	default: return -1;
	}
}

static int64_t maximum_i64(int64_t first, int64_t second)
{
	return first > second ? first : second;
}

static int64_t absolute_i64(int64_t value)
{
	return value < 0 ? -value : value;
}

static void configure_thresholds(struct input_navigation_axis *axis,
				 int64_t negative_extent,
				 int64_t positive_extent)
{
	int64_t flat = axis->flat;
	int64_t negative_press = maximum_i64((negative_extent + 1) / 2,
					      flat + 1);
	int64_t positive_press = maximum_i64((positive_extent + 1) / 2,
					      flat + 1);
	int64_t negative_release = maximum_i64(negative_extent / 4, flat);
	int64_t positive_release = maximum_i64(positive_extent / 4, flat);

	if (negative_press > negative_extent)
		negative_press = negative_extent;
	if (positive_press > positive_extent)
		positive_press = positive_extent;
	if (negative_release >= negative_press)
		negative_release = negative_press > 0 ? negative_press - 1 : 0;
	if (positive_release >= positive_press)
		positive_release = positive_press > 0 ? positive_press - 1 : 0;
	axis->negative_press = (int)((int64_t)axis->center - negative_press);
	axis->negative_release = (int)((int64_t)axis->center - negative_release);
	axis->positive_release = (int)((int64_t)axis->center + positive_release);
	axis->positive_press = (int)((int64_t)axis->center + positive_press);
}

void input_navigation_init(struct input_navigation *navigation)
{
	memset(navigation, 0, sizeof(*navigation));
}

void input_navigation_reset_state(struct input_navigation *navigation)
{
	for (size_t i = 0; i < INPUT_NAVIGATION_ANALOG_AXIS_COUNT; ++i)
		navigation->analog[i].direction = 0;
	for (size_t i = 0; i < INPUT_NAVIGATION_HAT_AXIS_COUNT; ++i)
		navigation->hat[i] = 0;
}

bool input_navigation_configure_axis(struct input_navigation *navigation,
				     unsigned int code,
				     const struct input_absinfo *info)
{
	struct input_navigation_axis *axis;
	int64_t midpoint;
	int64_t initial;
	int64_t span;
	int64_t center_tolerance;
	int index = analog_axis_index(code);

	if (index < 0 || info == NULL)
		return false;
	axis = &navigation->analog[index];
	memset(axis, 0, sizeof(*axis));
	if (info->maximum <= info->minimum)
		return false;

	axis->minimum = info->minimum;
	axis->maximum = info->maximum;
	axis->flat = info->flat > 0 ? info->flat : 0;
	span = (int64_t)info->maximum - info->minimum;
	if (span < 2)
		return false;
	midpoint = (int64_t)info->minimum + span / 2;
	initial = info->value;
	center_tolerance = maximum_i64((int64_t)axis->flat * 4, span / 20);
	if (center_tolerance > span / 4)
		center_tolerance = span / 4;
	if (initial > info->minimum && initial < info->maximum &&
	    absolute_i64(initial - midpoint) <= center_tolerance)
		axis->center = (int)initial;
	else
		axis->center = (int)midpoint;
	configure_thresholds(axis,
		(int64_t)axis->center - axis->minimum,
		(int64_t)axis->maximum - axis->center);
	axis->configured = true;
	return true;
}

size_t input_navigation_configure_analog_axes(
	struct input_navigation *navigation, input_navigation_abs_reader reader,
	void *context)
{
	static const unsigned int codes[] = { ABS_X, ABS_Y, ABS_RX, ABS_RY };
	size_t configured = 0;

	if (reader == NULL)
		return 0;
	for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); ++i) {
		struct input_absinfo info;

		memset(&info, 0, sizeof(info));
		if (reader(context, codes[i], &info) == 0 &&
		    input_navigation_configure_axis(navigation, codes[i], &info))
			++configured;
	}
	return configured;
}

static int analog_direction(const struct input_navigation_axis *axis, int value)
{
	if (axis->direction < 0) {
		if (value < axis->negative_release)
			return -1;
		if (value >= axis->positive_press)
			return 1;
		return 0;
	}
	if (axis->direction > 0) {
		if (value > axis->positive_release)
			return 1;
		if (value <= axis->negative_press)
			return -1;
		return 0;
	}
	if (value <= axis->negative_press)
		return -1;
	if (value >= axis->positive_press)
		return 1;
	return 0;
}

static enum input_navigation_direction mapped_direction(int index, int direction)
{
	if (index == 0 || index == 2)
		return direction < 0 ? INPUT_NAVIGATION_LEFT :
			INPUT_NAVIGATION_RIGHT;
	return direction < 0 ? INPUT_NAVIGATION_UP : INPUT_NAVIGATION_DOWN;
}

enum input_navigation_direction input_navigation_handle_abs(
	struct input_navigation *navigation, unsigned int code, int value)
{
	struct input_navigation_axis *axis;
	int direction;
	int previous;
	int index;

	if (code == ABS_HAT0X || code == ABS_HAT0Y) {
		index = code == ABS_HAT0X ? 0 : 1;
		previous = navigation->hat[index];
		direction = value < 0 ? -1 : value > 0 ? 1 : 0;
		navigation->hat[index] = direction;
		if (direction == 0 || direction == previous)
			return INPUT_NAVIGATION_NONE;
		return mapped_direction(index, direction);
	}

	index = analog_axis_index(code);
	if (index < 0)
		return INPUT_NAVIGATION_NONE;
	axis = &navigation->analog[index];
	if (!axis->configured)
		return INPUT_NAVIGATION_NONE;
	previous = axis->direction;
	direction = analog_direction(axis, value);
	axis->direction = direction;
	if (direction == 0 || direction == previous)
		return INPUT_NAVIGATION_NONE;
	return mapped_direction(index, direction);
}

bool input_navigation_abs_is_neutral(const struct input_navigation *navigation,
				     unsigned int code, int value)
{
	const struct input_navigation_axis *axis;
	int index;

	if (code == ABS_HAT0X || code == ABS_HAT0Y)
		return value == 0;
	index = analog_axis_index(code);
	if (index < 0)
		return true;
	axis = &navigation->analog[index];
	if (!axis->configured)
		return true;
	return value >= axis->negative_release &&
		value <= axis->positive_release;
}
