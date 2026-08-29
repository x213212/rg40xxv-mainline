#include "ui_layout.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static int right_edge(struct ui_layout_cover cover)
{
	return cover.x + cover.width;
}

static void test_horizontal_navigation_and_content(void)
{
	struct ui_layout_rect previous = { 0 };

	assert(UI_LAYOUT_NAV_COUNT == 7);
	assert(UI_LAYOUT_NAV_Y >= UI_LAYOUT_STATUS_BOTTOM);
	assert(UI_LAYOUT_NAV_X == UI_LAYOUT_CONTENT_X);
	assert(UI_LAYOUT_NAV_WIDTH == UI_LAYOUT_CONTENT_WIDTH);
	assert(UI_LAYOUT_CONTENT_WIDTH >= 600);
	assert(UI_LAYOUT_CONTENT_Y >= UI_LAYOUT_NAV_Y + UI_LAYOUT_NAV_HEIGHT);
	assert(UI_LAYOUT_CONTENT_Y + UI_LAYOUT_CONTENT_HEIGHT <
		UI_LAYOUT_CONTROLS_Y);
	for (int index = 0; index < UI_LAYOUT_NAV_COUNT; ++index) {
		struct ui_layout_rect tab = ui_layout_navigation_tab(index);

		assert(tab.width >= 86);
		assert(tab.y >= UI_LAYOUT_NAV_Y);
		assert(tab.y + tab.height <=
			UI_LAYOUT_NAV_Y + UI_LAYOUT_NAV_HEIGHT);
		if (index > 0)
			assert(previous.x + previous.width == tab.x);
		previous = tab;
	}
	assert(previous.x + previous.width ==
		UI_LAYOUT_NAV_X + UI_LAYOUT_NAV_WIDTH - 4);
}

static void test_cover_wall(void)
{
	struct ui_layout_cover outer_left = ui_layout_cover_at(-2.0);
	struct ui_layout_cover inner_left = ui_layout_cover_at(-1.0);
	struct ui_layout_cover selected = ui_layout_cover_at(0.0);
	struct ui_layout_cover inner_right = ui_layout_cover_at(1.0);
	struct ui_layout_cover outer_right = ui_layout_cover_at(2.0);

	assert(UI_LAYOUT_VISIBLE_COVER_COUNT == 5);
	assert(selected.width == 160 && selected.height == 232);
	assert(inner_left.width == 104 && inner_left.height == 160);
	assert(inner_right.width == 104 && inner_right.height == 160);
	assert(outer_left.width == 72 && outer_left.height == 112);
	assert(outer_right.width == 72 && outer_right.height == 112);
	assert(right_edge(outer_left) < inner_left.x);
	assert(right_edge(inner_left) < selected.x);
	assert(right_edge(selected) < inner_right.x);
	assert(right_edge(inner_right) < outer_right.x);
	assert(selected.y - 4 >= 145);
	assert(selected.y + selected.height + 4 < UI_LAYOUT_LIBRARY_INFO_Y);
	assert(UI_LAYOUT_LIBRARY_INFO_Y + UI_LAYOUT_LIBRARY_INFO_HEIGHT <
		UI_LAYOUT_CONTROLS_Y);

	for (int frame = 0; frame <= 100; ++frame) {
		double progress = (double)frame / 100.0;
		struct ui_layout_cover previous = ui_layout_cover_at(-2.0 - progress);

		for (int slot = -1; slot <= 2; ++slot) {
			struct ui_layout_cover current =
				ui_layout_cover_at((double)slot - progress);

			assert(right_edge(previous) < current.x);
			previous = current;
		}
	}
}

static void test_marquee(void)
{
	const int overflow = 120;
	uint32_t travel = ui_layout_marquee_travel_ms(overflow);
	uint32_t far_pause = UI_LAYOUT_MARQUEE_PAUSE_MS + travel;
	uint32_t reverse = far_pause + UI_LAYOUT_MARQUEE_PAUSE_MS;

	assert(UI_LAYOUT_MARQUEE_PAUSE_MS == 800);
	assert(ui_layout_marquee_offset(200, 220, UINT32_C(4000000000)) == 0);
	assert(ui_layout_marquee_offset(340, 220, 0) == 0);
	assert(ui_layout_marquee_offset(340, 220, 799) == 0);
	assert(ui_layout_marquee_offset(340, 220,
		UI_LAYOUT_MARQUEE_PAUSE_MS + travel / 2) < 0);
	assert(ui_layout_marquee_offset(340, 220, far_pause) == -overflow);
	assert(ui_layout_marquee_offset(340, 220, far_pause + 799) == -overflow);
	assert(ui_layout_marquee_offset(340, 220, reverse + travel / 2) < 0);
	assert(ui_layout_marquee_offset(340, 220, reverse + travel) == 0);
	assert(UI_LAYOUT_SETTINGS_NOTE_SECOND_Y +
		UI_LAYOUT_SETTINGS_NOTE_HEIGHT < UI_LAYOUT_CONTROLS_Y);
}

int main(void)
{
	test_horizontal_navigation_and_content();
	test_cover_wall();
	test_marquee();
	puts("UI_LAYOUT_TEST PASS tabs=7 covers=5 selected=160x232 marquee-pause=800ms");
	return 0;
}
