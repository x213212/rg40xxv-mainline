#ifndef RG40XXV_INPUT_METHOD_H
#define RG40XXV_INPUT_METHOD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum input_method_layout {
	INPUT_METHOD_ENGLISH,
	INPUT_METHOD_NUMBER,
	INPUT_METHOD_SYMBOL,
	INPUT_METHOD_NEW_CHEWING,
};

enum input_field_kind {
	INPUT_FIELD_TEXT,
	INPUT_FIELD_PASSWORD,
};

enum {
	INPUT_METHOD_COMPOSITION_MAX = 64,
	INPUT_METHOD_CANDIDATE_MAX = 8,
	INPUT_METHOD_CANDIDATE_TEXT_MAX = 48,
};

struct input_method_candidate {
	char text[INPUT_METHOD_CANDIDATE_TEXT_MAX];
};

/* Request must return immediately. Submit results on the UI thread. */
struct input_method_backend {
	void *context;
	const char *name;
	int (*request_candidates)(void *context, const char *composition,
				  uint64_t request_id);
	bool available;
};

struct input_method {
	struct input_method_backend backend;
	enum input_method_layout layout;
	enum input_field_kind field;
	char composition[INPUT_METHOD_COMPOSITION_MAX];
	struct input_method_candidate candidates[INPUT_METHOD_CANDIDATE_MAX];
	size_t candidate_count;
	uint64_t request_id;
	bool shift;
	bool candidate_pending;
};

void input_method_init(struct input_method *method, enum input_field_kind field);
void input_method_set_backend(struct input_method *method,
			      const struct input_method_backend *backend);
bool input_method_set_layout(struct input_method *method,
			     enum input_method_layout layout);
void input_method_cycle_layout(struct input_method *method);
void input_method_toggle_shift(struct input_method *method);
bool input_method_accept_byte(const struct input_method *method,
			      unsigned char value);
size_t input_method_mask_password(const char *value, char *output, size_t size);
int input_method_compose(struct input_method *method, const char *symbol);
int input_method_backspace(struct input_method *method);
int input_method_submit_candidates(struct input_method *method,
				   uint64_t request_id,
				   const char *const *candidates, size_t count);
void input_method_reset(struct input_method *method);

#endif
