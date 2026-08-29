#include "input_method.h"

#include <stdio.h>
#include <string.h>

void input_method_init(struct input_method *method, enum input_field_kind field)
{
	memset(method, 0, sizeof(*method));
	method->field = field;
	method->layout = INPUT_METHOD_ENGLISH;
}

void input_method_set_backend(struct input_method *method,
			      const struct input_method_backend *backend)
{
	if (backend == NULL || !backend->available ||
	    backend->request_candidates == NULL) {
		memset(&method->backend, 0, sizeof(method->backend));
		if (method->layout == INPUT_METHOD_NEW_CHEWING)
			method->layout = INPUT_METHOD_ENGLISH;
		return;
	}
	method->backend = *backend;
}

bool input_method_set_layout(struct input_method *method,
			     enum input_method_layout layout)
{
	if (method->field == INPUT_FIELD_PASSWORD &&
	    layout != INPUT_METHOD_ENGLISH && layout != INPUT_METHOD_NUMBER &&
	    layout != INPUT_METHOD_SYMBOL)
		return false;
	if (layout == INPUT_METHOD_NEW_CHEWING && !method->backend.available)
		return false;
	method->layout = layout;
	input_method_reset(method);
	return true;
}

void input_method_cycle_layout(struct input_method *method)
{
	enum input_method_layout next;

	switch (method->layout) {
	case INPUT_METHOD_ENGLISH: next = INPUT_METHOD_NUMBER; break;
	case INPUT_METHOD_NUMBER: next = INPUT_METHOD_SYMBOL; break;
	case INPUT_METHOD_SYMBOL:
		next = method->backend.available && method->field != INPUT_FIELD_PASSWORD ?
			INPUT_METHOD_NEW_CHEWING : INPUT_METHOD_ENGLISH;
		break;
	default: next = INPUT_METHOD_ENGLISH; break;
	}
	(void)input_method_set_layout(method, next);
}

void input_method_toggle_shift(struct input_method *method)
{
	if (method->layout == INPUT_METHOD_ENGLISH)
		method->shift = !method->shift;
}

bool input_method_accept_byte(const struct input_method *method,
			      unsigned char value)
{
	if (method->field == INPUT_FIELD_PASSWORD)
		return value >= 0x20U && value <= 0x7eU;
	return value >= 0x20U;
}

size_t input_method_mask_password(const char *value, char *output, size_t size)
{
	size_t length = value == NULL ? 0U : strlen(value);
	size_t shown = size == 0U ? 0U :
		(length < size - 1U ? length : size - 1U);

	if (size > 0U) {
		memset(output, '*', shown);
		output[shown] = '\0';
	}
	return length;
}

int input_method_compose(struct input_method *method, const char *symbol)
{
	size_t used;
	size_t incoming;

	if (method->layout != INPUT_METHOD_NEW_CHEWING ||
	    !method->backend.available || symbol == NULL)
		return -1;
	used = strlen(method->composition);
	incoming = strlen(symbol);
	if (incoming == 0U || used + incoming >= sizeof(method->composition))
		return -1;
	memcpy(method->composition + used, symbol, incoming + 1U);
	method->candidate_count = 0U;
	method->candidate_pending = true;
	++method->request_id;
	return method->backend.request_candidates(method->backend.context,
		method->composition, method->request_id);
}

int input_method_backspace(struct input_method *method)
{
	size_t length = strlen(method->composition);

	if (method->layout != INPUT_METHOD_NEW_CHEWING || length == 0U)
		return -1;
	--length;
	while (length > 0U &&
	       ((unsigned char)method->composition[length] & 0xc0U) == 0x80U)
		--length;
	method->composition[length] = '\0';
	method->candidate_count = 0U;
	++method->request_id;
	if (length == 0U) {
		method->candidate_pending = false;
		return 0;
	}
	method->candidate_pending = true;
	return method->backend.request_candidates(method->backend.context,
		method->composition, method->request_id);
}

int input_method_submit_candidates(struct input_method *method,
				   uint64_t request_id,
				   const char *const *candidates, size_t count)
{
	if (request_id != method->request_id || !method->candidate_pending)
		return -1;
	method->candidate_count = count < INPUT_METHOD_CANDIDATE_MAX ? count :
		INPUT_METHOD_CANDIDATE_MAX;
	for (size_t i = 0; i < method->candidate_count; ++i) {
		(void)snprintf(method->candidates[i].text,
			sizeof(method->candidates[i].text), "%s", candidates[i]);
	}
	method->candidate_pending = false;
	return 0;
}

void input_method_reset(struct input_method *method)
{
	method->composition[0] = '\0';
	method->candidate_count = 0U;
	method->candidate_pending = false;
	++method->request_id;
}
