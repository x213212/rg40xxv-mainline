#ifndef RG40XXV_UI_LAYOUT_H
#define RG40XXV_UI_LAYOUT_H

#include <stdint.h>

enum {
	UI_LAYOUT_STATUS_BOTTOM = 36,
	UI_LAYOUT_NAV_X = 12,
	UI_LAYOUT_NAV_Y = 42,
	UI_LAYOUT_NAV_WIDTH = 616,
	UI_LAYOUT_NAV_HEIGHT = 36,
	UI_LAYOUT_NAV_COUNT = 7,
	UI_LAYOUT_CONTENT_X = 12,
	UI_LAYOUT_CONTENT_Y = 85,
	UI_LAYOUT_CONTENT_WIDTH = 616,
	UI_LAYOUT_CONTENT_HEIGHT = 342,
	UI_LAYOUT_CONTROLS_Y = 435,
	UI_LAYOUT_VISIBLE_COVER_COUNT = 5,
	UI_LAYOUT_COVER_CENTER_X = 320,
	UI_LAYOUT_COVER_CENTER_Y = 265,
	UI_LAYOUT_COVER_SELECTED_WIDTH = 160,
	UI_LAYOUT_COVER_SELECTED_HEIGHT = 232,
	UI_LAYOUT_COVER_INNER_WIDTH = 104,
	UI_LAYOUT_COVER_INNER_HEIGHT = 160,
	UI_LAYOUT_COVER_OUTER_WIDTH = 72,
	UI_LAYOUT_COVER_OUTER_HEIGHT = 112,
	UI_LAYOUT_LIBRARY_INFO_X = 26,
	UI_LAYOUT_LIBRARY_INFO_Y = 386,
	UI_LAYOUT_LIBRARY_INFO_WIDTH = 588,
	UI_LAYOUT_LIBRARY_INFO_HEIGHT = 40,
	UI_LAYOUT_SETTINGS_NOTE_X = 28,
	UI_LAYOUT_SETTINGS_NOTE_WIDTH = 584,
	UI_LAYOUT_SETTINGS_NOTE_HEIGHT = 18,
	UI_LAYOUT_SETTINGS_NOTE_FIRST_Y = 388,
	UI_LAYOUT_SETTINGS_NOTE_SECOND_Y = 407,
	UI_LAYOUT_MARQUEE_PAUSE_MS = 800,
};

struct ui_layout_rect {
	int x;
	int y;
	int width;
	int height;
};

struct ui_layout_cover {
	int x;
	int y;
	int width;
	int height;
};

struct ui_layout_rect ui_layout_navigation_tab(int index);
struct ui_layout_cover ui_layout_cover_at(double distance);
uint32_t ui_layout_marquee_travel_ms(int overflow);
int ui_layout_marquee_offset(int text_width, int viewport_width,
			     uint32_t now);

#endif
