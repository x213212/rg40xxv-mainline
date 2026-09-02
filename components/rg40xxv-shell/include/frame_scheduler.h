#ifndef RG40XXV_FRAME_SCHEDULER_H
#define RG40XXV_FRAME_SCHEDULER_H

#include <stdbool.h>
#include <stdint.h>

enum {
	FRAME_SCHEDULER_ANIMATION_MS = 16,
	FRAME_SCHEDULER_FADE_MS = 33,
	FRAME_SCHEDULER_MARQUEE_MS = 100,
	FRAME_SCHEDULER_STATIC_MS = 1000,
	FRAME_SCHEDULER_ACTION_FADE_WINDOW_MS = 420,
	FRAME_SCHEDULER_RAW_EVDEV_MAX_WAIT_MS = 50,
};

struct frame_scheduler {
	uint32_t last_present;
	bool dirty;
};

struct frame_scheduler_query {
	uint32_t now;
	uint32_t action_until;
	int required_wake_ms;
	bool visible;
	bool animate_60hz;
	bool marquee_10hz;
	bool action_active;
	bool raw_evdev;
};

void frame_scheduler_init(struct frame_scheduler *scheduler, uint32_t now);
void frame_scheduler_invalidate(struct frame_scheduler *scheduler);
void frame_scheduler_clear(struct frame_scheduler *scheduler);
void frame_scheduler_presented(struct frame_scheduler *scheduler,
			       uint32_t now);
bool frame_scheduler_render_due(const struct frame_scheduler *scheduler,
				const struct frame_scheduler_query *query);
int frame_scheduler_next_wait_ms(const struct frame_scheduler *scheduler,
				 const struct frame_scheduler_query *query);

#endif
