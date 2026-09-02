#include "input_focus.h"

struct ui_focus_result ui_focus_resolve(enum ui_focus_region region,
					enum ui_focus_key key,
					int nav_index, int library_index,
					bool resident)
{
	struct ui_focus_result result = {
		.region = region,
		.intent = UI_FOCUS_INTENT_NONE,
	};

	if (region == UI_FOCUS_TOP_NAV) {
		switch (key) {
		case UI_FOCUS_KEY_LEFT:
		case UI_FOCUS_KEY_SHOULDER_PREVIOUS:
			result.intent = UI_FOCUS_INTENT_PREVIOUS_TAB;
			break;
		case UI_FOCUS_KEY_RIGHT:
		case UI_FOCUS_KEY_SHOULDER_NEXT:
			result.intent = UI_FOCUS_INTENT_NEXT_TAB;
			break;
		case UI_FOCUS_KEY_DOWN:
		case UI_FOCUS_KEY_ACTIVATE:
			result.region = UI_FOCUS_CONTENT;
			break;
		case UI_FOCUS_KEY_BACK:
			if (nav_index != library_index)
				result.intent = UI_FOCUS_INTENT_RETURN_LIBRARY;
			else if (!resident)
				result.intent = UI_FOCUS_INTENT_EXIT;
			break;
		case UI_FOCUS_KEY_UP:
			break;
		}
		return result;
	}

	switch (key) {
	case UI_FOCUS_KEY_UP:
		result.intent = UI_FOCUS_INTENT_CONTENT_UP;
		break;
	case UI_FOCUS_KEY_DOWN:
		result.intent = UI_FOCUS_INTENT_CONTENT_DOWN;
		break;
	case UI_FOCUS_KEY_LEFT:
		result.intent = UI_FOCUS_INTENT_CONTENT_LEFT;
		break;
	case UI_FOCUS_KEY_RIGHT:
		result.intent = UI_FOCUS_INTENT_CONTENT_RIGHT;
		break;
	case UI_FOCUS_KEY_ACTIVATE:
		result.intent = UI_FOCUS_INTENT_CONTENT_ACTIVATE;
		break;
	case UI_FOCUS_KEY_BACK:
		result.region = UI_FOCUS_TOP_NAV;
		break;
	case UI_FOCUS_KEY_SHOULDER_PREVIOUS:
		result.intent = UI_FOCUS_INTENT_PREVIOUS_TAB;
		break;
	case UI_FOCUS_KEY_SHOULDER_NEXT:
		result.intent = UI_FOCUS_INTENT_NEXT_TAB;
		break;
	}
	return result;
}
