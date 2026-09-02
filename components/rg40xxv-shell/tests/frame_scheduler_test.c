#include "frame_scheduler.h"

#include <stdio.h>

static unsigned int checks;

#define CHECK(condition)                                                        \
	do {                                                                      \
		++checks;                                                          \
		if (!(condition)) {                                                \
			(void)fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__,         \
				      __LINE__, #condition);                          \
			return 1;                                                     \
		}                                                                 \
	} while (0)

static struct frame_scheduler_query query_at(uint32_t now)
{
	return (struct frame_scheduler_query) {
		.now = now,
		.required_wake_ms = -1,
		.visible = true,
	};
}

int main(void)
{
	struct frame_scheduler scheduler;
	struct frame_scheduler_query query;

	frame_scheduler_init(&scheduler, 1000U);
	query = query_at(1000U);
	CHECK(frame_scheduler_render_due(&scheduler, &query));
	CHECK(frame_scheduler_next_wait_ms(&scheduler, &query) == 0);
	frame_scheduler_presented(&scheduler, 1000U);
	CHECK(!frame_scheduler_render_due(&scheduler, &query));
	CHECK(frame_scheduler_next_wait_ms(&scheduler, &query) == 1000);
	query.now = 2000U;
	CHECK(frame_scheduler_render_due(&scheduler, &query));

	frame_scheduler_presented(&scheduler, 3000U);
	query = query_at(3015U);
	query.animate_60hz = true;
	CHECK(frame_scheduler_next_wait_ms(&scheduler, &query) == 1);
	query.now = 3016U;
	CHECK(frame_scheduler_render_due(&scheduler, &query));

	frame_scheduler_presented(&scheduler, 4000U);
	query = query_at(4099U);
	query.marquee_10hz = true;
	CHECK(frame_scheduler_next_wait_ms(&scheduler, &query) == 1);
	query.now = 4100U;
	CHECK(frame_scheduler_render_due(&scheduler, &query));

	frame_scheduler_presented(&scheduler, 5000U);
	query = query_at(5000U);
	query.action_active = true;
	query.action_until = 6000U;
	CHECK(frame_scheduler_next_wait_ms(&scheduler, &query) == 580);
	query.now = 5580U;
	CHECK(frame_scheduler_render_due(&scheduler, &query));
	frame_scheduler_presented(&scheduler, 5580U);
	query.now = 5612U;
	CHECK(frame_scheduler_next_wait_ms(&scheduler, &query) == 1);
	query.now = 5613U;
	CHECK(frame_scheduler_render_due(&scheduler, &query));
	frame_scheduler_presented(&scheduler, 5980U);
	query.now = 6000U;
	CHECK(frame_scheduler_render_due(&scheduler, &query));
	frame_scheduler_presented(&scheduler, 6000U);
	CHECK(frame_scheduler_next_wait_ms(&scheduler, &query) == 1000);

	frame_scheduler_presented(&scheduler, 7000U);
	query = query_at(7000U);
	query.raw_evdev = true;
	CHECK(frame_scheduler_next_wait_ms(&scheduler, &query) == 50);
	query.required_wake_ms = 27;
	CHECK(frame_scheduler_next_wait_ms(&scheduler, &query) == 27);
	query.visible = false;
	query.required_wake_ms = -1;
	query.raw_evdev = false;
	CHECK(frame_scheduler_next_wait_ms(&scheduler, &query) == 1000);
	query.raw_evdev = true;
	CHECK(frame_scheduler_next_wait_ms(&scheduler, &query) == 50);
	frame_scheduler_invalidate(&scheduler);
	CHECK(!frame_scheduler_render_due(&scheduler, &query));
	CHECK(frame_scheduler_next_wait_ms(&scheduler, &query) == 50);
	frame_scheduler_clear(&scheduler);
	query.required_wake_ms = 0;
	CHECK(!frame_scheduler_render_due(&scheduler, &query));
	CHECK(frame_scheduler_next_wait_ms(&scheduler, &query) == 0);

	frame_scheduler_presented(&scheduler, UINT32_MAX - 7U);
	query = query_at(8U);
	query.animate_60hz = true;
	CHECK(frame_scheduler_render_due(&scheduler, &query));

	(void)printf("FRAME_SCHEDULER_TEST PASS checks=%u\n", checks);
	return 0;
}
