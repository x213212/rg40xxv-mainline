#include "input_method.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

struct fake_dictionary {
	char query[64];
	uint64_t request_id;
};

static int fake_request(void *context, const char *query, uint64_t request_id)
{
	struct fake_dictionary *fake = context;

	(void)snprintf(fake->query, sizeof(fake->query), "%s", query);
	fake->request_id = request_id;
	return 0;
}

int main(void)
{
	struct input_method method;
	struct fake_dictionary fake = { 0 };
	struct input_method_backend backend = {
		.context = &fake, .name = "fake-test-only",
		.request_candidates = fake_request, .available = true,
	};
	const char *const candidates[] = { "中文", "中壢" };
	char masked[16];

	input_method_init(&method, INPUT_FIELD_TEXT);
	assert(method.layout == INPUT_METHOD_ENGLISH);
	assert(!input_method_set_layout(&method, INPUT_METHOD_NEW_CHEWING));
	input_method_set_backend(&method, &backend);
	assert(input_method_set_layout(&method, INPUT_METHOD_NEW_CHEWING));
	assert(input_method_compose(&method, "ㄓㄨㄥ") == 0);
	assert(strcmp(fake.query, "ㄓㄨㄥ") == 0 && method.candidate_pending);
	assert(input_method_submit_candidates(&method, fake.request_id - 1U,
		candidates, 2U) == -1);
	assert(input_method_submit_candidates(&method, fake.request_id,
		candidates, 2U) == 0);
	assert(strcmp(method.candidates[0].text, "中文") == 0);
	assert(input_method_backspace(&method) == 0);
	assert(strcmp(method.composition, "ㄓㄨ") == 0);
	input_method_init(&method, INPUT_FIELD_PASSWORD);
	input_method_set_backend(&method, &backend);
	assert(!input_method_set_layout(&method, INPUT_METHOD_NEW_CHEWING));
	assert(input_method_accept_byte(&method, 'A'));
	assert(!input_method_accept_byte(&method, 0xe4U));
	assert(input_method_mask_password("secret", masked, sizeof(masked)) == 6U);
	assert(strcmp(masked, "******") == 0);
	puts("INPUT_METHOD_TEST PASS backend=fake-test-only production=english-fallback");
	return 0;
}
