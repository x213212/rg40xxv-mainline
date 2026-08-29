#include "input_focus.h"

#include <assert.h>
#include <stdio.h>

static void test_top_navigation_isolated_from_content(void)
{
	struct ui_focus_result result;

	result = ui_focus_resolve(UI_FOCUS_TOP_NAV, UI_FOCUS_KEY_LEFT,
		5, 1, true);
	assert(result.region == UI_FOCUS_TOP_NAV);
	assert(result.intent == UI_FOCUS_INTENT_PREVIOUS_TAB);
	result = ui_focus_resolve(UI_FOCUS_TOP_NAV, UI_FOCUS_KEY_RIGHT,
		5, 1, true);
	assert(result.intent == UI_FOCUS_INTENT_NEXT_TAB);
	result = ui_focus_resolve(UI_FOCUS_TOP_NAV,
		UI_FOCUS_KEY_SHOULDER_PREVIOUS, 5, 1, true);
	assert(result.region == UI_FOCUS_TOP_NAV);
	assert(result.intent == UI_FOCUS_INTENT_PREVIOUS_TAB);
	result = ui_focus_resolve(UI_FOCUS_TOP_NAV,
		UI_FOCUS_KEY_SHOULDER_NEXT, 5, 1, true);
	assert(result.intent == UI_FOCUS_INTENT_NEXT_TAB);
	result = ui_focus_resolve(UI_FOCUS_TOP_NAV, UI_FOCUS_KEY_DOWN,
		5, 1, true);
	assert(result.region == UI_FOCUS_CONTENT);
	assert(result.intent == UI_FOCUS_INTENT_NONE);
	result = ui_focus_resolve(UI_FOCUS_TOP_NAV, UI_FOCUS_KEY_ACTIVATE,
		5, 1, true);
	assert(result.region == UI_FOCUS_CONTENT);
	assert(result.intent == UI_FOCUS_INTENT_NONE);
	result = ui_focus_resolve(UI_FOCUS_TOP_NAV, UI_FOCUS_KEY_UP,
		5, 1, true);
	assert(result.region == UI_FOCUS_TOP_NAV);
	assert(result.intent == UI_FOCUS_INTENT_NONE);
}

static void test_content_back_and_home_policy(void)
{
	struct ui_focus_result result;

	result = ui_focus_resolve(UI_FOCUS_CONTENT, UI_FOCUS_KEY_LEFT,
		5, 1, true);
	assert(result.region == UI_FOCUS_CONTENT);
	assert(result.intent == UI_FOCUS_INTENT_CONTENT_LEFT);
	result = ui_focus_resolve(UI_FOCUS_CONTENT,
		UI_FOCUS_KEY_SHOULDER_NEXT, 5, 1, true);
	assert(result.region == UI_FOCUS_CONTENT);
	assert(result.intent == UI_FOCUS_INTENT_NONE);
	result = ui_focus_resolve(UI_FOCUS_CONTENT, UI_FOCUS_KEY_BACK,
		5, 1, true);
	assert(result.region == UI_FOCUS_TOP_NAV);
	assert(result.intent == UI_FOCUS_INTENT_NONE);
	result = ui_focus_resolve(UI_FOCUS_TOP_NAV, UI_FOCUS_KEY_BACK,
		5, 1, true);
	assert(result.intent == UI_FOCUS_INTENT_RETURN_LIBRARY);
	result = ui_focus_resolve(UI_FOCUS_TOP_NAV, UI_FOCUS_KEY_BACK,
		1, 1, true);
	assert(result.intent == UI_FOCUS_INTENT_NONE);
	result = ui_focus_resolve(UI_FOCUS_TOP_NAV, UI_FOCUS_KEY_BACK,
		1, 1, false);
	assert(result.intent == UI_FOCUS_INTENT_EXIT);
}

int main(void)
{
	test_top_navigation_isolated_from_content();
	test_content_back_and_home_policy();
	puts("INPUT_FOCUS_TEST PASS top-nav/content/back hierarchy");
	return 0;
}
