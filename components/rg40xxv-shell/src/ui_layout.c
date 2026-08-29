#include "ui_layout.h"

#include <math.h>

struct ui_layout_rect ui_layout_navigation_tab(int index)
{
	const int inner_x = UI_LAYOUT_NAV_X + 4;
	const int inner_width = UI_LAYOUT_NAV_WIDTH - 8;
	const int base_width = inner_width / UI_LAYOUT_NAV_COUNT;
	struct ui_layout_rect result = {
		.x = inner_x + index * base_width,
		.y = UI_LAYOUT_NAV_Y + 3,
		.width = index == UI_LAYOUT_NAV_COUNT - 1 ?
			inner_width - base_width * index : base_width,
		.height = UI_LAYOUT_NAV_HEIGHT - 6,
	};

	return result;
}

static double cover_dimension(double distance, double selected,
			      double inner, double outer)
{
	double absolute = fabs(distance);

	if (absolute <= 1.0)
		return selected + (inner - selected) * absolute;
	if (absolute < 2.0)
		return inner + (outer - inner) * (absolute - 1.0);
	return outer;
}

static double cover_center_offset(double distance)
{
	double absolute = fabs(distance);
	double offset = absolute <= 1.0 ? absolute * 138.0 :
		138.0 + (absolute - 1.0) * 96.0;

	return distance < 0.0 ? -offset : offset;
}

struct ui_layout_cover ui_layout_cover_at(double distance)
{
	int width = (int)lrint(cover_dimension(distance,
		UI_LAYOUT_COVER_SELECTED_WIDTH, UI_LAYOUT_COVER_INNER_WIDTH,
		UI_LAYOUT_COVER_OUTER_WIDTH));
	int height = (int)lrint(cover_dimension(distance,
		UI_LAYOUT_COVER_SELECTED_HEIGHT, UI_LAYOUT_COVER_INNER_HEIGHT,
		UI_LAYOUT_COVER_OUTER_HEIGHT));
	int center = UI_LAYOUT_COVER_CENTER_X +
		(int)lrint(cover_center_offset(distance));
	struct ui_layout_cover result = {
		.x = center - width / 2,
		.y = UI_LAYOUT_COVER_CENTER_Y - height / 2,
		.width = width,
		.height = height,
	};

	return result;
}

uint32_t ui_layout_marquee_travel_ms(int overflow)
{
	uint32_t duration;

	if (overflow <= 0)
		return 0;
	duration = (uint32_t)overflow * 1000U / 36U;
	return duration < 1200U ? 1200U : duration;
}

static double smoothstep(double progress)
{
	return progress * progress * (3.0 - 2.0 * progress);
}

int ui_layout_marquee_offset(int text_width, int viewport_width, uint32_t now)
{
	int overflow = text_width - viewport_width;
	uint32_t travel;
	uint32_t cycle;
	uint32_t phase;
	double progress;

	if (overflow <= 0 || viewport_width <= 0)
		return 0;
	travel = ui_layout_marquee_travel_ms(overflow);
	cycle = 2U * (UI_LAYOUT_MARQUEE_PAUSE_MS + travel);
	phase = now % cycle;
	if (phase < UI_LAYOUT_MARQUEE_PAUSE_MS)
		return 0;
	phase -= UI_LAYOUT_MARQUEE_PAUSE_MS;
	if (phase < travel) {
		progress = (double)phase / travel;
		return -(int)lrint((double)overflow * smoothstep(progress));
	}
	phase -= travel;
	if (phase < UI_LAYOUT_MARQUEE_PAUSE_MS)
		return -overflow;
	phase -= UI_LAYOUT_MARQUEE_PAUSE_MS;
	progress = (double)phase / travel;
	return -(int)lrint((double)overflow * (1.0 - smoothstep(progress)));
}
