#include "ui.h"

#include <ctype.h>
#include <string.h>

static const char *const keys[KEYBOARD_PAGES][KEYBOARD_ROWS]
	[KEYBOARD_COLUMNS] = {
	{
		{ "q", "w", "e", "r", "t", "y", "u", "i", "o", "p" },
		{ "a", "s", "d", "f", "g", "h", "j", "k", "l", "-" },
		{ "z", "x", "c", "v", "b", "n", "m", "_", ".", "'" },
		{ "0", "1", "2", "3", "4", "5", "6", "7", "8", "space" },
	},
	{
		{ "1", "2", "3", "4", "5", "6", "7", "8", "9", "0" },
		{ ".", ",", ":", ";", "+", "-", "*", "/", "=", "%" },
		{ "(", ")", "[", "]", "{", "}", "<", ">", "_", "#" },
		{ "@", "!", "?", "&", "|", "\\", "~", "^", "$", "space" },
	},
	{
		{ "!", "@", "#", "$", "%", "^", "&", "*", "(", ")" },
		{ "[", "]", "{", "}", "<", ">", "/", "\\", "|", "~" },
		{ "+", "-", "=", "_", ":", ";", "\"", "'", "`", "." },
		{ ",", "?", "€", "£", "¥", "°", "·", "…", "_", "space" },
	},
	{
		{ "ㄅ", "ㄉ", "ˇ", "ˋ", "ㄓ", "ˊ", "˙", "ㄚ", "ㄞ", "ㄢ" },
		{ "ㄆ", "ㄊ", "ㄍ", "ㄐ", "ㄔ", "ㄗ", "ㄧ", "ㄛ", "ㄟ", "ㄣ" },
		{ "ㄇ", "ㄋ", "ㄎ", "ㄑ", "ㄕ", "ㄘ", "ㄨ", "ㄜ", "ㄠ", "ㄤ" },
		{ "ㄈ", "ㄌ", "ㄏ", "ㄒ", "ㄖ", "ㄙ", "ㄩ", "ㄝ", "ㄡ", "ㄥ" },
	},
};

static void move_cursor(struct ui *ui, int row, int column)
{
	ui->keyboard_row = (ui->keyboard_row + row + KEYBOARD_ROWS) % KEYBOARD_ROWS;
	ui->keyboard_column = (ui->keyboard_column + column + KEYBOARD_COLUMNS) %
		KEYBOARD_COLUMNS;
	audio_play_chime(ui, row != 0 ? 1640.0 : 1780.0);
}

static const char *key_label(struct ui *ui, int row, int column, char output[8])
{
	const char *value = keys[ui->keyboard_page][row][column];

	if (strcmp(value, "space") == 0)
		return " ";
	if (ui->input_method.shift && ui->keyboard_page == INPUT_METHOD_ENGLISH &&
	    value[0] != '\0' && value[1] == '\0') {
		output[0] = (char)toupper((unsigned char)value[0]);
		output[1] = '\0';
		return output;
	}
	return value;
}

bool keyboard_handle_key(struct ui *ui, SDL_Keycode key, uint32_t now)
{
	char shifted[8];
	const char *selected;

	(void)now;
	if (!ui->search_active)
		return false;
	switch (key) {
	case SDLK_UP: move_cursor(ui, -1, 0); break;
	case SDLK_DOWN: move_cursor(ui, 1, 0); break;
	case SDLK_LEFT: move_cursor(ui, 0, -1); break;
	case SDLK_RIGHT: move_cursor(ui, 0, 1); break;
	case SDLK_RETURN:
		selected = key_label(ui, ui->keyboard_row, ui->keyboard_column, shifted);
		if (ui->input_method.layout == INPUT_METHOD_NEW_CHEWING)
			(void)input_method_compose(&ui->input_method, selected);
		else
			search_append(ui, selected);
		audio_play_chime(ui, 1870.0);
		break;
	case SDLK_KP_ENTER:
		if (ui->input_method.layout == INPUT_METHOD_NEW_CHEWING &&
		    ui->input_method.candidate_count > 0U) {
			search_append(ui, ui->input_method.candidates[0].text);
			input_method_reset(&ui->input_method);
		} else
			search_close(ui);
		break;
	case SDLK_BACKSPACE:
		if (ui->input_method.layout == INPUT_METHOD_NEW_CHEWING &&
		    ui->input_method.composition[0] != '\0')
			(void)input_method_backspace(&ui->input_method);
		else if (ui->catalog.query[0] != '\0')
			search_backspace(ui);
		else
			search_close(ui);
		break;
	case SDLK_ESCAPE:
		search_close(ui);
		break;
	case SDLK_F1:
	case SDLK_LSHIFT:
	case SDLK_RSHIFT:
		input_method_toggle_shift(&ui->input_method);
		audio_play_chime(ui, 1840.0);
		break;
	case SDLK_F2:
	case SDLK_TAB:
		input_method_cycle_layout(&ui->input_method);
		ui->keyboard_page = (int)ui->input_method.layout;
		audio_play_chime(ui, 1710.0);
		break;
	case SDLK_F3:
		ui->catalog.search_all_systems = !ui->catalog.search_all_systems;
		catalog_apply_filters(ui);
		persistence_request_filters(ui);
		audio_play_chime(ui, 1780.0);
		break;
	default: break;
	}
	return true;
}

static const char *layout_label(struct ui *ui)
{
	if (ui->input_method.layout == INPUT_METHOD_NUMBER)
		return tr(ui, "keyboard_numbers");
	if (ui->input_method.layout == INPUT_METHOD_SYMBOL)
		return tr(ui, "keyboard_symbols");
	if (ui->input_method.layout == INPUT_METHOD_NEW_CHEWING)
		return tr(ui, "keyboard_new_chewing");
	return tr(ui, ui->input_method.shift ? "keyboard_english_shift" :
		  "keyboard_english");
}

void keyboard_render(struct ui *ui)
{
	const SDL_Color panel = { 12, 12, 12, 250 };
	const SDL_Color line = { 82, 82, 82, 255 };
	const SDL_Color selected_color = { 220, 220, 220, 255 };
	const SDL_Color text = { 224, 224, 224, 255 };
	const SDL_Color muted = { 150, 150, 150, 255 };

	render_fill_rect(ui->renderer, UI_LAYOUT_CONTENT_X, 258,
		UI_LAYOUT_CONTENT_WIDTH, 169, panel);
	render_outline_rect(ui->renderer, UI_LAYOUT_CONTENT_X, 258,
		UI_LAYOUT_CONTENT_WIDTH, 169, line);
	text_draw(ui, 0, layout_label(ui), 28, 266, text);
	if (ui->input_method.layout == INPUT_METHOD_NEW_CHEWING) {
		const char *candidate = ui->input_method.candidate_count == 0U ? "" :
			ui->input_method.candidates[0].text;

		text_draw(ui, 0, ui->input_method.composition, 212, 266, text);
		text_draw(ui, 0, candidate, 382, 266, muted);
	} else
		text_draw(ui, 0, tr(ui, ui->input_method.backend.available ?
			  "new_chewing_ready" : "ime_english_short"), 260, 266, muted);
	text_draw(ui, 0, tr(ui, ui->catalog.search_all_systems ?
		  "scope_all_short" : "scope_current_short"), 536, 266, muted);
	for (int row = 0; row < KEYBOARD_ROWS; ++row) {
		for (int column = 0; column < KEYBOARD_COLUMNS; ++column) {
			char shifted[8];
			int x = 28 + column * 58;
			int y = 290 + row * 29;
			bool active = row == ui->keyboard_row &&
				column == ui->keyboard_column;
			const char *label = key_label(ui, row, column, shifted);
			int width;

			if (strcmp(label, " ") == 0)
				label = tr(ui, "space");
			render_fill_rect(ui->renderer, x, y, 54, 25,
				active ? (SDL_Color){ 35, 35, 35, 255 } : panel);
			render_outline_rect(ui->renderer, x, y, 54, 25,
					    active ? selected_color : line);
			width = text_width(ui, 0, label,
				active ? selected_color : text);
			text_draw(ui, 0, label, x + (54 - width) / 2, y + 2,
				  active ? selected_color : text);
		}
	}
	text_draw(ui, 0, tr(ui, "keyboard_controls_new"), 28, 409, muted);
}
