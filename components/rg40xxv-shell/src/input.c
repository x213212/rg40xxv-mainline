#define _POSIX_C_SOURCE 200809L

#include "ui.h"
#include "input_latch_snapshot.h"

#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

enum {
	INPUT_LATCH_KEYBOARD_BASE = 24,
	INPUT_LATCH_CONTROLLER_BUTTON_BASE = 48,
	INPUT_LATCH_CONTROLLER_AXIS_BASE = 64,
	INPUT_LATCH_JOYSTICK_BUTTON_BASE = 72,
	INPUT_LATCH_JOYSTICK_HAT = 80,
};

static const SDL_Keycode latch_keyboard_keys[] = {
	SDLK_UP, SDLK_DOWN, SDLK_LEFT, SDLK_RIGHT,
	SDLK_RETURN, SDLK_SPACE, SDLK_ESCAPE, SDLK_PAGEUP, SDLK_PAGEDOWN,
	SDLK_x, SDLK_F1, SDLK_y, SDLK_F2, SDLK_f, SDLK_F3, SDLK_KP_ENTER,
};

static const SDL_GameControllerButton latch_controller_buttons[] = {
	SDL_CONTROLLER_BUTTON_DPAD_UP,
	SDL_CONTROLLER_BUTTON_DPAD_DOWN,
	SDL_CONTROLLER_BUTTON_DPAD_LEFT,
	SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
	SDL_CONTROLLER_BUTTON_A,
	SDL_CONTROLLER_BUTTON_B,
	SDL_CONTROLLER_BUTTON_X,
	SDL_CONTROLLER_BUTTON_Y,
	SDL_CONTROLLER_BUTTON_START,
	SDL_CONTROLLER_BUTTON_BACK,
	SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
	SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
};

static const SDL_GameControllerAxis latch_controller_axes[] = {
	SDL_CONTROLLER_AXIS_LEFTX,
	SDL_CONTROLLER_AXIS_LEFTY,
	SDL_CONTROLLER_AXIS_RIGHTX,
	SDL_CONTROLLER_AXIS_RIGHTY,
};

_Static_assert((int)INPUT_LATCH_EVDEV_UNKNOWN <
	       (int)INPUT_LATCH_CONTROL_CAPACITY,
	       "input latch slot overflow");

static void snapshot_sdl_state(struct ui *ui, uint32_t now);

static int keyboard_latch_index(SDL_Keycode key)
{
	for (size_t i = 0; i < sizeof(latch_keyboard_keys) /
	     sizeof(latch_keyboard_keys[0]); ++i) {
		if (latch_keyboard_keys[i] == key)
			return (int)i;
	}
	return -1;
}

static int controller_button_latch_index(Uint8 button)
{
	for (size_t i = 0; i < sizeof(latch_controller_buttons) /
	     sizeof(latch_controller_buttons[0]); ++i) {
		if ((Uint8)latch_controller_buttons[i] == button)
			return (int)i;
	}
	return -1;
}

static int controller_axis_latch_index(Uint8 axis)
{
	for (size_t i = 0; i < sizeof(latch_controller_axes) /
	     sizeof(latch_controller_axes[0]); ++i) {
		if ((Uint8)latch_controller_axes[i] == axis)
			return (int)i;
	}
	return -1;
}

static enum input_latch_evdev_profile evdev_latch_profile(const struct ui *ui)
{
	return ui->input_profile == INPUT_PROFILE_H700_MAINLINE ?
		INPUT_LATCH_EVDEV_H700_MAINLINE :
		INPUT_LATCH_EVDEV_ANBERNIC_STOCK;
}

static bool sdl_axis_active(int value)
{
	/* Match the release side of the evdev navigation hysteresis. */
	return value < -SDL_JOYSTICK_AXIS_MAX / 4 ||
		value > SDL_JOYSTICK_AXIS_MAX / 4;
}

static void move_carousel(struct ui *ui, int delta, uint32_t now)
{
	size_t count = ui->catalog.visible_count;
	long long next;

	if (count < 2)
		return;
	next = (long long)ui->game_index + delta;
	while (next < 0)
		next += (long long)count;
	next %= (long long)count;
	ui->game_index = (size_t)next;
	ui->carousel_from = ui->carousel_position;
	ui->carousel_target += (double)delta;
	ui->carousel_started = now;
}

static void apply_navigation_filter(struct ui *ui)
{
	if (ui->nav_index == NAV_PAGE_APPS) {
		catalog_set_apps_view(ui, true);
		return;
	}
	if (ui->catalog.apps_only)
		catalog_set_apps_view(ui, false);
	if (ui->nav_index > NAV_PAGE_FAVORITES)
		return;
	ui->catalog.recent_only = ui->nav_index == NAV_PAGE_RECENT;
	ui->catalog.favorites_only = ui->nav_index == NAV_PAGE_FAVORITES;
	catalog_apply_filters(ui);
	persistence_request_filters(ui);
}

static bool focus_key_from_sdl(SDL_Keycode key, enum ui_focus_key *focus_key)
{
	switch (key) {
	case SDLK_UP: *focus_key = UI_FOCUS_KEY_UP; return true;
	case SDLK_DOWN: *focus_key = UI_FOCUS_KEY_DOWN; return true;
	case SDLK_LEFT: *focus_key = UI_FOCUS_KEY_LEFT; return true;
	case SDLK_RIGHT: *focus_key = UI_FOCUS_KEY_RIGHT; return true;
	case SDLK_RETURN:
	case SDLK_SPACE: *focus_key = UI_FOCUS_KEY_ACTIVATE; return true;
	case SDLK_ESCAPE: *focus_key = UI_FOCUS_KEY_BACK; return true;
	case SDLK_PAGEUP:
		*focus_key = UI_FOCUS_KEY_SHOULDER_PREVIOUS;
		return true;
	case SDLK_PAGEDOWN:
		*focus_key = UI_FOCUS_KEY_SHOULDER_NEXT;
		return true;
	default: return false;
	}
}

static bool handle_focus_key(struct ui *ui, SDL_Keycode key, uint32_t now)
{
	enum ui_focus_key focus_key;
	struct ui_focus_result result;

	if (!focus_key_from_sdl(key, &focus_key))
		return false;
	result = ui_focus_resolve(ui->focus_region, focus_key, ui->nav_index,
		NAV_PAGE_LIBRARY,
		ui->resident);
	ui->focus_region = result.region;
	switch (result.intent) {
	case UI_FOCUS_INTENT_PREVIOUS_TAB:
		ui->nav_index = (ui->nav_index + NAV_COUNT - 1) % NAV_COUNT;
		apply_navigation_filter(ui);
		audio_play_chime(ui, 1710.0);
		break;
	case UI_FOCUS_INTENT_NEXT_TAB:
		ui->nav_index = (ui->nav_index + 1) % NAV_COUNT;
		apply_navigation_filter(ui);
		audio_play_chime(ui, 1870.0);
		break;
	case UI_FOCUS_INTENT_CONTENT_UP:
		if (ui->nav_index == NAV_PAGE_SETTINGS) {
			settings_ui_move(ui, -1);
			audio_play_chime(ui, 1780.0);
		} else if (ui->nav_index == NAV_PAGE_NETWORK) {
			network_ui_select(ui, -1);
			audio_play_chime(ui, 1780.0);
		}
		break;
	case UI_FOCUS_INTENT_CONTENT_DOWN:
		if (ui->nav_index == NAV_PAGE_SETTINGS) {
			settings_ui_move(ui, 1);
			audio_play_chime(ui, 1640.0);
		} else if (ui->nav_index == NAV_PAGE_NETWORK) {
			network_ui_select(ui, 1);
			audio_play_chime(ui, 1640.0);
		}
		break;
	case UI_FOCUS_INTENT_CONTENT_LEFT:
		if (ui->nav_index == NAV_PAGE_SETTINGS)
			settings_ui_adjust(ui, -1, now);
		else if (ui->nav_index == NAV_PAGE_STREAMING) {
			stream_select_host(ui, -1);
			audio_play_chime(ui, 1710.0);
		} else if (ui->nav_index == NAV_PAGE_NETWORK) {
			network_ui_select(ui, -1);
			audio_play_chime(ui, 1710.0);
		} else {
			move_carousel(ui, -1, now);
			audio_play_chime(ui, 1710.0);
		}
		break;
	case UI_FOCUS_INTENT_CONTENT_RIGHT:
		if (ui->nav_index == NAV_PAGE_SETTINGS)
			settings_ui_adjust(ui, 1, now);
		else if (ui->nav_index == NAV_PAGE_STREAMING) {
			stream_select_host(ui, 1);
			audio_play_chime(ui, 1870.0);
		} else if (ui->nav_index == NAV_PAGE_NETWORK) {
			network_ui_select(ui, 1);
			audio_play_chime(ui, 1870.0);
		} else {
			move_carousel(ui, 1, now);
			audio_play_chime(ui, 1870.0);
		}
		break;
	case UI_FOCUS_INTENT_CONTENT_ACTIVATE:
		if (ui->nav_index == NAV_PAGE_SETTINGS)
			settings_ui_activate(ui, now);
		else if (ui->nav_index == NAV_PAGE_STREAMING)
			stream_activate_selected(ui, now);
		else if (ui->nav_index == NAV_PAGE_NETWORK)
			network_ui_activate(ui, now);
		else if (ui->catalog.visible_count > 0)
			launch_queue_selected(ui, now);
		break;
	case UI_FOCUS_INTENT_RETURN_LIBRARY:
		ui->nav_index = NAV_PAGE_LIBRARY;
		apply_navigation_filter(ui);
		break;
	case UI_FOCUS_INTENT_EXIT:
		ui->running = false;
		break;
	case UI_FOCUS_INTENT_NONE:
		break;
	}
	return true;
}

static void handle_key(struct ui *ui, SDL_Keycode key, uint32_t now)
{
	metrics_note_input(ui);
	/* The physical power path remains live while controller input is gated. */
	if (key == SDLK_POWER) {
		(void)power_ui_handle_key(ui, key, now);
		return;
	}
	if (input_latch_waiting(&ui->input_latch))
		return;
	if (power_ui_handle_key(ui, key, now))
		return;
	if (ui->launch.pending || ui->launch.process.active)
		return;
	if (ui->launch.transition == LAUNCH_TRANSITION_ERROR) {
		if (key == SDLK_RETURN || key == SDLK_SPACE) {
			ui->launch.diagnostics_expanded =
				!ui->launch.diagnostics_expanded;
			audio_play_chime(ui, 1840.0);
		} else if (key == SDLK_ESCAPE) {
			ui->launch.transition = LAUNCH_TRANSITION_NONE;
			ui->launch.transition_presented = false;
			ui->launch.kind = LAUNCH_KIND_NONE;
		}
		return;
	}
	if (keyboard_handle_key(ui, key, now))
		return;
	if (ui->quick_menu_open) {
		switch (key) {
		case SDLK_UP:
			ui->quick_menu_index = (ui->quick_menu_index +
				QUICK_MENU_COUNT - 1) % QUICK_MENU_COUNT;
			audio_play_chime(ui, 1780.0);
			return;
		case SDLK_DOWN:
			ui->quick_menu_index = (ui->quick_menu_index + 1) %
				QUICK_MENU_COUNT;
			audio_play_chime(ui, 1640.0);
			return;
		case SDLK_LEFT:
			if (ui->quick_menu_index == 5)
				locale_toggle(ui);
			else
				catalog_cycle_filter(ui, ui->quick_menu_index, -1);
			audio_play_chime(ui, 1710.0);
			return;
		case SDLK_RIGHT:
		case SDLK_RETURN:
		case SDLK_SPACE:
			if (ui->quick_menu_index == 5)
				locale_toggle(ui);
			else
				catalog_cycle_filter(ui, ui->quick_menu_index, 1);
			audio_play_chime(ui, 1870.0);
			return;
		case SDLK_ESCAPE:
		case SDLK_F2:
			ui->quick_menu_open = false;
			return;
		default:
			return;
		}
	}
	if (handle_focus_key(ui, key, now))
		return;
	if (ui->focus_region != UI_FOCUS_CONTENT)
		return;
	switch (key) {
	case SDLK_x:
	case SDLK_F1:
		if (ui->nav_index == NAV_PAGE_STREAMING) {
			int result = stream_reload(ui);

			render_activate(ui, tr(ui, result == 0 ? "stream_reloaded" :
						"stream_load_failed"), now);
		} else if (ui->nav_index != NAV_PAGE_APPS &&
			   ui->nav_index != NAV_PAGE_NETWORK &&
			   ui->nav_index != NAV_PAGE_SETTINGS) {
			search_open(ui);
		}
		break;
	case SDLK_y:
	case SDLK_F2:
		if (ui->nav_index != NAV_PAGE_STREAMING &&
		    ui->nav_index != NAV_PAGE_APPS &&
		    ui->nav_index != NAV_PAGE_NETWORK &&
		    ui->nav_index != NAV_PAGE_SETTINGS) {
			ui->quick_menu_open = true;
			search_close(ui);
		}
		break;
	case SDLK_f:
	case SDLK_F3:
		if (ui->nav_index != NAV_PAGE_STREAMING &&
		    ui->nav_index != NAV_PAGE_APPS &&
		    ui->nav_index != NAV_PAGE_NETWORK)
			catalog_toggle_favorite(ui);
		break;
	default:
		break;
	}
}

static void handle_evdev_key(struct ui *ui, unsigned int code, uint32_t now)
{
	if (ui->input_profile == INPUT_PROFILE_H700_MAINLINE) {
		switch (code) {
		case BTN_DPAD_UP: handle_key(ui, SDLK_UP, now); return;
		case BTN_DPAD_DOWN: handle_key(ui, SDLK_DOWN, now); return;
		case BTN_DPAD_LEFT: handle_key(ui, SDLK_LEFT, now); return;
		case BTN_DPAD_RIGHT: handle_key(ui, SDLK_RIGHT, now); return;
		case BTN_EAST: handle_key(ui, SDLK_RETURN, now); return;
		case BTN_SOUTH: handle_key(ui, SDLK_ESCAPE, now); return;
		case BTN_NORTH: handle_key(ui, SDLK_F1, now); return;
		case BTN_WEST: handle_key(ui, SDLK_F2, now); return;
		case BTN_START: handle_key(ui, SDLK_KP_ENTER, now); return;
		case BTN_SELECT: handle_key(ui, SDLK_F3, now); return;
		case BTN_MODE: handle_key(ui, SDLK_F2, now); return;
		case BTN_TL: handle_key(ui, SDLK_PAGEUP, now); return;
		case BTN_TR: handle_key(ui, SDLK_PAGEDOWN, now); return;
		default: return;
		}
	}
	/* Vendor 4.9 DTS uses private numeric codes 0x130..0x138. */
	switch (code) {
	case 0x130: handle_key(ui, SDLK_RETURN, now); break;
	case 0x131: handle_key(ui, SDLK_ESCAPE, now); break;
	case 0x132: handle_key(ui, SDLK_F1, now); break;
	case 0x133: handle_key(ui, SDLK_F2, now); break;
	case 0x137: handle_key(ui, SDLK_KP_ENTER, now); break;
	case 0x136: handle_key(ui, SDLK_F3, now); break;
	case 0x138: handle_key(ui, SDLK_F2, now); break;
	default: break;
	}
}

static void handle_evdev_axis(struct ui *ui, unsigned int code, int value,
				      uint32_t now)
{
	int latch_index = input_latch_evdev_axis_index(code);
	enum input_navigation_direction direction = input_navigation_handle_abs(
		&ui->input_navigation, code, value);

	if (latch_index >= 0)
		input_latch_set(&ui->input_latch,
			INPUT_LATCH_EVDEV_AXIS_BASE + (size_t)latch_index,
			!input_navigation_abs_is_neutral(&ui->input_navigation,
				code, value), now);
	switch (direction) {
	case INPUT_NAVIGATION_LEFT: handle_key(ui, SDLK_LEFT, now); break;
	case INPUT_NAVIGATION_RIGHT: handle_key(ui, SDLK_RIGHT, now); break;
	case INPUT_NAVIGATION_UP: handle_key(ui, SDLK_UP, now); break;
	case INPUT_NAVIGATION_DOWN: handle_key(ui, SDLK_DOWN, now); break;
	case INPUT_NAVIGATION_NONE: break;
	}
}

static int read_evdev_keys(void *context, unsigned long *bits, size_t size)
{
	int fd = *(const int *)context;

	return ioctl(fd, EVIOCGKEY(size), bits) < 0 ? -1 : 0;
}

static int read_evdev_abs_capabilities(void *context, unsigned long *bits,
				       size_t size)
{
	int fd = *(const int *)context;

	return ioctl(fd, EVIOCGBIT(EV_ABS, size), bits) < 0 ? -1 : 0;
}

static int read_evdev_abs(void *context, unsigned int code,
			  struct input_absinfo *info)
{
	int fd = *(const int *)context;

	return ioctl(fd, EVIOCGABS(code), info) < 0 ? -1 : 0;
}

static bool reacquire_evdev_state(struct ui *ui, uint32_t now)
{
	return input_latch_evdev_reacquire(&ui->input_latch,
		&ui->input_navigation, evdev_latch_profile(ui), now,
		INPUT_LATCH_NEUTRAL_STABLE_MS, read_evdev_keys,
		read_evdev_abs_capabilities, read_evdev_abs, &ui->input_fd);
}

static void handle_evdev(struct ui *ui, uint32_t now)
{
	struct input_event events[16];
	ssize_t count;

	if (ui->input_fd < 0)
		return;
	while ((count = read(ui->input_fd, events, sizeof(events))) > 0) {
		size_t event_count = (size_t)count / sizeof(events[0]);

		for (size_t i = 0; i < event_count; ++i) {
			int latch_index;

			if (events[i].type == EV_SYN &&
			    events[i].code == SYN_DROPPED) {
				input_latch_init(&ui->input_latch, now,
					INPUT_LATCH_NEUTRAL_STABLE_MS);
				input_latch_set(&ui->input_latch,
					INPUT_LATCH_EVDEV_UNKNOWN, true, now);
				ui->input_evdev_sync_lost = true;
				continue;
			}
			if (ui->input_evdev_sync_lost) {
				if (events[i].type == EV_SYN &&
				    events[i].code == SYN_REPORT) {
					if (reacquire_evdev_state(ui, now)) {
						snapshot_sdl_state(ui, now);
						if (ui->benchmark)
							input_latch_force_ready(
								&ui->input_latch);
						ui->input_evdev_sync_lost = false;
					} else {
						/* Unknown state is fail-closed; retry next report. */
						input_latch_set(&ui->input_latch,
							INPUT_LATCH_EVDEV_UNKNOWN, true, now);
					}
				}
				continue;
			}
			latch_index = events[i].type == EV_KEY ?
				input_latch_evdev_button_index(
					evdev_latch_profile(ui), events[i].code) : -1;
			if (latch_index >= 0)
				input_latch_set(&ui->input_latch,
					INPUT_LATCH_EVDEV_BUTTON_BASE +
					(size_t)latch_index,
					events[i].value != 0, now);
			if (events[i].type == EV_KEY && events[i].code == KEY_POWER &&
			    events[i].value == 1)
				power_ui_apply(ui,
					power_button_press(&ui->power, now), now);
			else if (events[i].type == EV_KEY &&
				 events[i].code == KEY_POWER && events[i].value == 0)
				power_ui_apply(ui,
					power_button_release(&ui->power, now), now);
			else if (events[i].type == EV_KEY &&
				 (events[i].value == 1 ||
				  (events[i].value == 2 &&
				   ui->focus_region == UI_FOCUS_CONTENT &&
				   ui->nav_index == NAV_PAGE_SETTINGS &&
				   ui->settings_index == 5 &&
				   (events[i].code == BTN_DPAD_LEFT ||
				    events[i].code == BTN_DPAD_RIGHT))))
				handle_evdev_key(ui, events[i].code, now);
			else if (events[i].type == EV_ABS)
				handle_evdev_axis(ui, events[i].code, events[i].value, now);
		}
	}
}

static bool open_evdev_gamepad(struct ui *ui)
{
	for (int index = 0; index < 32; ++index) {
		char path[64];
		char name[sizeof(ui->input_name)] = { 0 };
		int fd;

		(void)snprintf(path, sizeof(path), "/dev/input/event%d", index);
		fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			continue;
		if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0) {
			close(fd);
			continue;
		}
		if (strstr(name, "H700 Gamepad") != NULL)
			ui->input_profile = INPUT_PROFILE_H700_MAINLINE;
		else if (strstr(name, "ANBERNIC-keys") != NULL)
			ui->input_profile = INPUT_PROFILE_ANBERNIC_STOCK;
		else {
			close(fd);
			continue;
		}
		ui->input_fd = fd;
		input_navigation_init(&ui->input_navigation);
		if (!reacquire_evdev_state(ui, SDL_GetTicks())) {
			close(fd);
			ui->input_fd = -1;
			ui->input_profile = INPUT_PROFILE_NONE;
			continue;
		}
		(void)snprintf(ui->input_name, sizeof(ui->input_name), "%s", name);
		return true;
	}
	return false;
}

static void snapshot_sdl_state(struct ui *ui, uint32_t now)
{
	const Uint8 *keyboard;
	int key_count = 0;

	SDL_PumpEvents();
	keyboard = SDL_GetKeyboardState(&key_count);
	for (size_t i = 0;
	     i < sizeof(latch_keyboard_keys) / sizeof(latch_keyboard_keys[0]);
	     ++i) {
		SDL_Scancode code = SDL_GetScancodeFromKey(latch_keyboard_keys[i]);
		bool active = keyboard != NULL && code != SDL_SCANCODE_UNKNOWN &&
			(int)code < key_count && keyboard[code] != 0;

		input_latch_set(&ui->input_latch,
			INPUT_LATCH_KEYBOARD_BASE + i, active, now);
	}
	if (ui->input_fd >= 0)
		return;
	if (ui->controller != NULL) {
		for (size_t i = 0; i < sizeof(latch_controller_buttons) /
		     sizeof(latch_controller_buttons[0]); ++i)
			input_latch_set(&ui->input_latch,
				INPUT_LATCH_CONTROLLER_BUTTON_BASE + i,
				SDL_GameControllerGetButton(ui->controller,
					latch_controller_buttons[i]) != 0, now);
		for (size_t i = 0; i < sizeof(latch_controller_axes) /
		     sizeof(latch_controller_axes[0]); ++i)
			input_latch_set(&ui->input_latch,
				INPUT_LATCH_CONTROLLER_AXIS_BASE + i,
				sdl_axis_active(SDL_GameControllerGetAxis(ui->controller,
					latch_controller_axes[i])), now);
		return;
	}
	if (ui->joystick == NULL)
		return;
	for (int i = 0; i < SDL_JoystickNumButtons(ui->joystick) && i < 8; ++i)
		input_latch_set(&ui->input_latch,
			INPUT_LATCH_JOYSTICK_BUTTON_BASE + (size_t)i,
			SDL_JoystickGetButton(ui->joystick, i) != 0, now);
	if (SDL_JoystickNumHats(ui->joystick) > 0)
		input_latch_set(&ui->input_latch, INPUT_LATCH_JOYSTICK_HAT,
			SDL_JoystickGetHat(ui->joystick, 0) != SDL_HAT_CENTERED,
			now);
}

static void track_sdl_latch_event(struct ui *ui, const SDL_Event *event,
				  uint32_t now)
{
	int index;

	switch (event->type) {
	case SDL_KEYDOWN:
	case SDL_KEYUP:
		index = keyboard_latch_index(event->key.keysym.sym);
		if (index >= 0)
			input_latch_set(&ui->input_latch,
				INPUT_LATCH_KEYBOARD_BASE + (size_t)index,
				event->type == SDL_KEYDOWN, now);
		break;
	case SDL_CONTROLLERBUTTONDOWN:
	case SDL_CONTROLLERBUTTONUP:
		if (ui->input_fd >= 0)
			break;
		index = controller_button_latch_index(event->cbutton.button);
		if (index >= 0)
			input_latch_set(&ui->input_latch,
				INPUT_LATCH_CONTROLLER_BUTTON_BASE + (size_t)index,
				event->type == SDL_CONTROLLERBUTTONDOWN, now);
		break;
	case SDL_CONTROLLERAXISMOTION:
		if (ui->input_fd >= 0)
			break;
		index = controller_axis_latch_index(event->caxis.axis);
		if (index >= 0)
			input_latch_set(&ui->input_latch,
				INPUT_LATCH_CONTROLLER_AXIS_BASE + (size_t)index,
				sdl_axis_active(event->caxis.value), now);
		break;
	case SDL_JOYBUTTONDOWN:
	case SDL_JOYBUTTONUP:
		if (ui->input_fd >= 0 || ui->controller != NULL ||
		    event->jbutton.button >= 8)
			break;
		input_latch_set(&ui->input_latch,
			INPUT_LATCH_JOYSTICK_BUTTON_BASE + event->jbutton.button,
			event->type == SDL_JOYBUTTONDOWN, now);
		break;
	case SDL_JOYHATMOTION:
		if (ui->input_fd < 0 && ui->controller == NULL &&
		    event->jhat.hat == 0)
			input_latch_set(&ui->input_latch,
				INPUT_LATCH_JOYSTICK_HAT,
				event->jhat.value != SDL_HAT_CENTERED, now);
		break;
	default:
		break;
	}
}

static void handle_controller_event(struct ui *ui, const SDL_Event *event,
					uint32_t now)
{
	if (ui->input_fd >= 0)
		return;
	if (event->type == SDL_CONTROLLERBUTTONDOWN) {
		switch (event->cbutton.button) {
		case SDL_CONTROLLER_BUTTON_DPAD_UP: handle_key(ui, SDLK_UP, now); break;
		case SDL_CONTROLLER_BUTTON_DPAD_DOWN: handle_key(ui, SDLK_DOWN, now); break;
		case SDL_CONTROLLER_BUTTON_DPAD_LEFT: handle_key(ui, SDLK_LEFT, now); break;
		case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: handle_key(ui, SDLK_RIGHT, now); break;
		case SDL_CONTROLLER_BUTTON_A: handle_key(ui, SDLK_RETURN, now); break;
		case SDL_CONTROLLER_BUTTON_B: handle_key(ui, SDLK_ESCAPE, now); break;
		case SDL_CONTROLLER_BUTTON_X: handle_key(ui, SDLK_F1, now); break;
		case SDL_CONTROLLER_BUTTON_Y: handle_key(ui, SDLK_F2, now); break;
		case SDL_CONTROLLER_BUTTON_START: handle_key(ui, SDLK_KP_ENTER, now); break;
		case SDL_CONTROLLER_BUTTON_BACK: handle_key(ui, SDLK_F3, now); break;
		case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
			handle_key(ui, SDLK_PAGEUP, now); break;
		case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
			handle_key(ui, SDLK_PAGEDOWN, now); break;
		default: break;
		}
	} else if (event->type == SDL_JOYHATMOTION && ui->controller == NULL && event->jhat.hat == 0) {
		if (event->jhat.value & SDL_HAT_UP) handle_key(ui, SDLK_UP, now);
		if (event->jhat.value & SDL_HAT_DOWN) handle_key(ui, SDLK_DOWN, now);
		if (event->jhat.value & SDL_HAT_LEFT) handle_key(ui, SDLK_LEFT, now);
		if (event->jhat.value & SDL_HAT_RIGHT) handle_key(ui, SDLK_RIGHT, now);
	} else if (event->type == SDL_JOYBUTTONDOWN && ui->controller == NULL) {
		/* Stock ANBERNIC-keys.cfg: A=0, B=1, Y=2, X=3. */
		switch (event->jbutton.button) {
		case 0: case 7: handle_key(ui, SDLK_RETURN, now); break;
		case 1: handle_key(ui, SDLK_ESCAPE, now); break;
		case 2: handle_key(ui, SDLK_F2, now); break;
		case 3: handle_key(ui, SDLK_F1, now); break;
		case 4: handle_key(ui, SDLK_PAGEUP, now); break;
		case 5: handle_key(ui, SDLK_PAGEDOWN, now); break;
		case 6: handle_key(ui, SDLK_F3, now); break;
		default: break;
		}
	}
}

void input_init(struct ui *ui)
{
	uint32_t now = SDL_GetTicks();

	ui->input_fd = -1;
	ui->input_profile = INPUT_PROFILE_NONE;
	ui->input_evdev_sync_lost = false;
	input_navigation_init(&ui->input_navigation);
	input_latch_init(&ui->input_latch, now,
		INPUT_LATCH_NEUTRAL_STABLE_MS);
	(void)snprintf(ui->input_name, sizeof(ui->input_name), "none");
	if (!open_evdev_gamepad(ui))
		input_latch_init(&ui->input_latch, SDL_GetTicks(),
			INPUT_LATCH_NEUTRAL_STABLE_MS);
	for (int i = 0; ui->input_fd < 0 && i < SDL_NumJoysticks(); ++i) {
		if (SDL_IsGameController(i)) {
			ui->controller = SDL_GameControllerOpen(i);
			if (ui->controller != NULL) {
				(void)snprintf(ui->input_name, sizeof(ui->input_name), "%s",
					SDL_GameControllerName(ui->controller));
				break;
			}
		}
	}
	if (ui->input_fd < 0 && ui->controller == NULL && SDL_NumJoysticks() > 0) {
		ui->joystick = SDL_JoystickOpen(0);
		if (ui->joystick != NULL)
			(void)snprintf(ui->input_name, sizeof(ui->input_name), "%s",
				SDL_JoystickName(ui->joystick));
	}
	snapshot_sdl_state(ui, SDL_GetTicks());
	/* Synthetic benchmark events do not have matching release events. */
	if (ui->benchmark)
		input_latch_force_ready(&ui->input_latch);
}

void input_handle_events(struct ui *ui, uint32_t now)
{
	SDL_Event event;

	while (SDL_PollEvent(&event)) {
		track_sdl_latch_event(ui, &event, now);
		if (event.type == SDL_TEXTINPUT || event.type == SDL_KEYDOWN ||
		    event.type == SDL_CONTROLLERBUTTONDOWN ||
		    event.type == SDL_JOYBUTTONDOWN ||
		    event.type == SDL_JOYHATMOTION)
			metrics_note_input(ui);
		if (event.type == SDL_QUIT) {
			if (!ui->resident)
				ui->running = false;
		} else if (event.type == SDL_TEXTINPUT && ui->search_active)
			search_append(ui, event.text.text);
		else if (event.type == SDL_KEYUP &&
			 event.key.keysym.sym == SDLK_POWER)
			power_ui_apply(ui,
				power_button_release(&ui->power, now), now);
		else if (event.type == SDL_KEYDOWN &&
			 (event.key.repeat == 0 || ui->search_active ||
			  (ui->focus_region == UI_FOCUS_CONTENT &&
			   ui->nav_index == NAV_PAGE_SETTINGS &&
			   ui->settings_index == 5 &&
			   (event.key.keysym.sym == SDLK_LEFT ||
			    event.key.keysym.sym == SDLK_RIGHT))))
			handle_key(ui, event.key.keysym.sym, now);
		else
			handle_controller_event(ui, &event, now);
	}
	handle_evdev(ui, now);
	/* Advance only after queued events have refreshed the physical state. */
	(void)input_latch_update(&ui->input_latch, now);
	power_ui_apply(ui, power_update(&ui->power, now), now);
}

void input_close(struct ui *ui)
{
	if (ui->input_fd >= 0)
		close(ui->input_fd);
	ui->input_fd = -1;
	if (ui->controller != NULL)
		SDL_GameControllerClose(ui->controller);
	else if (ui->joystick != NULL)
		SDL_JoystickClose(ui->joystick);
	ui->controller = NULL;
	ui->joystick = NULL;
	ui->input_evdev_sync_lost = false;
}
