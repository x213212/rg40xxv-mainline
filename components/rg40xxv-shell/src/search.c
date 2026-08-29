#include "ui.h"

#include <string.h>

void search_open(struct ui *ui)
{
	ui->search_active = true;
	ui->quick_menu_open = false;
	ui->keyboard_row = 0;
	ui->keyboard_column = 0;
	input_method_reset(&ui->input_method);
	(void)input_method_set_layout(&ui->input_method, INPUT_METHOD_ENGLISH);
	ui->keyboard_page = 0;
	SDL_StartTextInput();
}

void search_close(struct ui *ui)
{
	ui->search_active = false;
	input_method_reset(&ui->input_method);
	SDL_StopTextInput();
}

void search_append(struct ui *ui, const char *text)
{
	size_t used = strlen(ui->catalog.query);
	size_t incoming = strlen(text);

	if (!ui->search_active || incoming == 0 ||
	    used + incoming >= sizeof(ui->catalog.query))
		return;
	for (size_t i = 0; i < incoming; ++i) {
		if ((unsigned char)text[i] < 32U)
			return;
	}
	memcpy(ui->catalog.query + used, text, incoming + 1);
	catalog_apply_filters(ui);
}

void search_backspace(struct ui *ui)
{
	size_t length = strlen(ui->catalog.query);

	if (length == 0)
		return;
--length;
	while (length > 0 &&
	       ((unsigned char)ui->catalog.query[length] & 0xc0U) == 0x80U)
		--length;
	ui->catalog.query[length] = '\0';
	catalog_apply_filters(ui);
}

void search_clear(struct ui *ui)
{
	if (ui->catalog.query[0] == '\0')
		return;
	ui->catalog.query[0] = '\0';
	catalog_apply_filters(ui);
}
