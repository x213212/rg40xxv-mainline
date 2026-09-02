#ifndef RG40XXV_RPG_TRANSLATION_H
#define RG40XXV_RPG_TRANSLATION_H

#include <limits.h>
#include <stdint.h>

struct ui;

enum rpg_translation_mode {
	RPG_TRANSLATION_OFF,
	RPG_TRANSLATION_STATIC,
};

struct rpg_translation_state {
	char path[PATH_MAX];
	enum rpg_translation_mode mode;
};

/* Invalid/missing state always fails closed to OFF. */
int rpg_translation_init(struct rpg_translation_state *state,
			 const char *path);
void rpg_translation_toggle_selected(struct ui *ui, uint32_t now);

#endif
