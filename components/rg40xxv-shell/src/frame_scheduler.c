#include "frame_scheduler.h"

#include <limits.h>

static bool deadline_passed(uint32_t now, uint32_t deadline)
{
	return (int32_t)(now - deadline) >= 0;
}

static int time_until(uint32_t now, uint32_t deadline)
{
	uint32_t remaining;

	if (deadline_passed(now, deadline))
		return 0;
	remaining = deadline - now;
	return remaining > (uint32_t)INT_MAX ? INT_MAX : (int)remaining;
}

static int sooner(int current, int candidate)
{
	if (candidate < 0)
		return current;
	return current < 0 || candidate < current ? candidate : current;
}

static int render_wait_ms(const struct frame_scheduler *scheduler,
			  const struct frame_scheduler_query *query)
{
	int wait;

	if (!query->visible)
		return -1;
	if (scheduler->dirty)
		return 0;
	wait = time_until(query->now,
			  scheduler->last_present + FRAME_SCHEDULER_STATIC_MS);
	if (query->animate_60hz)
		wait = sooner(wait, time_until(query->now,
			scheduler->last_present +
			FRAME_SCHEDULER_ANIMATION_MS));
	if (query->marquee_10hz)
		wait = sooner(wait, time_until(query->now,
			scheduler->last_present +
			FRAME_SCHEDULER_MARQUEE_MS));
	if (query->action_active &&
	    !deadline_passed(scheduler->last_present, query->action_until)) {
		int until_end = time_until(query->now, query->action_until);

		/* Present once at expiry so the last translucent OSD is removed. */
		wait = sooner(wait, until_end);
		if (!deadline_passed(query->now, query->action_until)) {
			uint32_t remaining = query->action_until - query->now;

			if (remaining > FRAME_SCHEDULER_ACTION_FADE_WINDOW_MS)
				wait = sooner(wait, (int)(remaining -
					FRAME_SCHEDULER_ACTION_FADE_WINDOW_MS));
			else
				wait = sooner(wait, time_until(query->now,
					scheduler->last_present +
					FRAME_SCHEDULER_FADE_MS));
		}
	}
	return wait;
}

void frame_scheduler_init(struct frame_scheduler *scheduler, uint32_t now)
{
	scheduler->last_present = now;
	scheduler->dirty = true;
}

void frame_scheduler_invalidate(struct frame_scheduler *scheduler)
{
	scheduler->dirty = true;
}

void frame_scheduler_clear(struct frame_scheduler *scheduler)
{
	scheduler->dirty = false;
}

void frame_scheduler_presented(struct frame_scheduler *scheduler,
			       uint32_t now)
{
	scheduler->last_present = now;
	scheduler->dirty = false;
}

bool frame_scheduler_render_due(const struct frame_scheduler *scheduler,
				const struct frame_scheduler_query *query)
{
	return query->visible && render_wait_ms(scheduler, query) == 0;
}

int frame_scheduler_next_wait_ms(const struct frame_scheduler *scheduler,
				 const struct frame_scheduler_query *query)
{
	int wait = render_wait_ms(scheduler, query);

	wait = sooner(wait, query->required_wake_ms);
	/* Keep signal/monitor progress bounded even while the panel is off. */
	if (wait < 0)
		wait = FRAME_SCHEDULER_STATIC_MS;
	if (query->raw_evdev &&
	    wait > FRAME_SCHEDULER_RAW_EVDEV_MAX_WAIT_MS)
		wait = FRAME_SCHEDULER_RAW_EVDEV_MAX_WAIT_MS;
	return wait;
}
