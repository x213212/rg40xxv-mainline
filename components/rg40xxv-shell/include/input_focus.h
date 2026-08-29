#ifndef RG40XXV_INPUT_FOCUS_H
#define RG40XXV_INPUT_FOCUS_H

#include <stdbool.h>

enum ui_focus_region {
	UI_FOCUS_TOP_NAV,
	UI_FOCUS_CONTENT,
};

enum ui_focus_key {
	UI_FOCUS_KEY_UP,
	UI_FOCUS_KEY_DOWN,
	UI_FOCUS_KEY_LEFT,
	UI_FOCUS_KEY_RIGHT,
	UI_FOCUS_KEY_ACTIVATE,
	UI_FOCUS_KEY_BACK,
	UI_FOCUS_KEY_SHOULDER_PREVIOUS,
	UI_FOCUS_KEY_SHOULDER_NEXT,
};

enum ui_focus_intent {
	UI_FOCUS_INTENT_NONE,
	UI_FOCUS_INTENT_PREVIOUS_TAB,
	UI_FOCUS_INTENT_NEXT_TAB,
	UI_FOCUS_INTENT_CONTENT_UP,
	UI_FOCUS_INTENT_CONTENT_DOWN,
	UI_FOCUS_INTENT_CONTENT_LEFT,
	UI_FOCUS_INTENT_CONTENT_RIGHT,
	UI_FOCUS_INTENT_CONTENT_ACTIVATE,
	UI_FOCUS_INTENT_RETURN_LIBRARY,
	UI_FOCUS_INTENT_EXIT,
};

struct ui_focus_result {
	enum ui_focus_region region;
	enum ui_focus_intent intent;
};

struct ui_focus_result ui_focus_resolve(enum ui_focus_region region,
					enum ui_focus_key key,
					int nav_index, int library_index,
					bool resident);

#endif
