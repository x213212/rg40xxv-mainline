#ifndef RG40XXV_INPUT_NAVIGATION_H
#define RG40XXV_INPUT_NAVIGATION_H

#include <linux/input.h>
#include <stdbool.h>
#include <stddef.h>

enum input_navigation_direction {
	INPUT_NAVIGATION_NONE,
	INPUT_NAVIGATION_LEFT,
	INPUT_NAVIGATION_RIGHT,
	INPUT_NAVIGATION_UP,
	INPUT_NAVIGATION_DOWN,
};

enum {
	INPUT_NAVIGATION_ANALOG_AXIS_COUNT = 4,
	INPUT_NAVIGATION_HAT_AXIS_COUNT = 2,
};

struct input_navigation_axis {
	int minimum;
	int maximum;
	int center;
	int flat;
	int negative_press;
	int negative_release;
	int positive_release;
	int positive_press;
	int direction;
	bool configured;
};

struct input_navigation {
	struct input_navigation_axis analog[INPUT_NAVIGATION_ANALOG_AXIS_COUNT];
	int hat[INPUT_NAVIGATION_HAT_AXIS_COUNT];
};

typedef int (*input_navigation_abs_reader)(void *context, unsigned int code,
					   struct input_absinfo *info);

void input_navigation_init(struct input_navigation *navigation);
void input_navigation_reset_state(struct input_navigation *navigation);
bool input_navigation_configure_axis(struct input_navigation *navigation,
				     unsigned int code,
				     const struct input_absinfo *info);
size_t input_navigation_configure_analog_axes(
	struct input_navigation *navigation, input_navigation_abs_reader reader,
	void *context);
enum input_navigation_direction input_navigation_handle_abs(
	struct input_navigation *navigation, unsigned int code, int value);
bool input_navigation_abs_is_neutral(const struct input_navigation *navigation,
				     unsigned int code, int value);

#endif
