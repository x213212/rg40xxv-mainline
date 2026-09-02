#include "power_input_filter.h"

#include <assert.h>
#include <linux/input-event-codes.h>
#include <stdio.h>

int main(void)
{
	struct power_input_filter filter;

	power_input_filter_init(&filter);
	assert(power_input_filter_event(&filter, EV_KEY, KEY_POWER, 1) ==
		POWER_INPUT_FILTER_PRESS);
	assert(power_input_filter_event(&filter, EV_KEY, KEY_POWER, 2) ==
		POWER_INPUT_FILTER_NONE);
	assert(power_input_filter_event(&filter, EV_KEY, KEY_POWER, 0) ==
		POWER_INPUT_FILTER_RELEASE);

	assert(power_input_ignore_sdl(true, true, true));
	assert(!power_input_ignore_sdl(false, true, true));
	assert(power_input_ignore_sdl(false, true, false));
	assert(!power_input_ignore_sdl(false, false, false));

	assert(power_input_filter_event(&filter, EV_SYN, SYN_DROPPED, 0) ==
		POWER_INPUT_FILTER_CANCEL);
	assert(filter.sync_lost && filter.suppress_until_up);
	assert(power_input_filter_event(&filter, EV_KEY, KEY_POWER, 0) ==
		POWER_INPUT_FILTER_NONE);
	assert(power_input_filter_event(&filter, EV_SYN, SYN_REPORT, 0) ==
		POWER_INPUT_FILTER_RESYNC);
	power_input_filter_resync(&filter, false);
	assert(!filter.sync_lost && !filter.suppress_until_up);
	assert(power_input_filter_event(&filter, EV_KEY, KEY_POWER, 1) ==
		POWER_INPUT_FILTER_PRESS);

	assert(power_input_filter_event(&filter, EV_SYN, SYN_DROPPED, 0) ==
		POWER_INPUT_FILTER_CANCEL);
	assert(power_input_filter_event(&filter, EV_SYN, SYN_REPORT, 0) ==
		POWER_INPUT_FILTER_RESYNC);
	power_input_filter_resync(&filter, true);
	assert(!filter.sync_lost && filter.suppress_until_up);
	assert(power_input_filter_event(&filter, EV_KEY, KEY_POWER, 1) ==
		POWER_INPUT_FILTER_NONE);
	assert(power_input_filter_event(&filter, EV_KEY, KEY_POWER, 0) ==
		POWER_INPUT_FILTER_NONE);
	assert(!filter.suppress_until_up);
	assert(power_input_filter_event(&filter, EV_KEY, KEY_POWER, 1) ==
		POWER_INPUT_FILTER_PRESS);

	assert(power_input_filter_fault(&filter) == POWER_INPUT_FILTER_CANCEL);
	assert(!filter.sync_lost && !filter.suppress_until_up);

	puts("POWER_INPUT_FILTER_TEST PASS normal=PASS duplicate=PASS "
	     "syn_dropped=PASS hotplug_fault=PASS");
	return 0;
}
