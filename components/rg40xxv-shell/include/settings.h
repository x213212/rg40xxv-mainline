#ifndef RG40XXV_SETTINGS_H
#define RG40XXV_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum joystick_rgb_color {
	JOYSTICK_RGB_COLOR_HARDWARE_DEFAULT,
	JOYSTICK_RGB_COLOR_WHITE,
	JOYSTICK_RGB_COLOR_CUSTOM,
};

enum joystick_rgb_effect {
	JOYSTICK_RGB_EFFECT_STATIC,
	JOYSTICK_RGB_EFFECT_BREATHING,
	JOYSTICK_RGB_EFFECT_CYCLE,
};

enum debug_log_category {
	DEBUG_LOG_UI,
	DEBUG_LOG_KERNEL,
	DEBUG_LOG_STREAM,
	DEBUG_LOG_BOOT,
	DEBUG_LOG_CATEGORY_COUNT,
};

enum settings_hardware_command {
	SETTINGS_HW_SCREEN_OFF,
	SETTINGS_HW_SCREEN_ON,
	SETTINGS_HW_BRIGHTNESS,
	SETTINGS_HW_JOYSTICK_RGB,
	SETTINGS_HW_USB_DEBUG,
	SETTINGS_HW_VOLUME,
	SETTINGS_HW_MUTE_TOGGLE,
	SETTINGS_HW_ORDERLY_SHUTDOWN,
	SETTINGS_HW_REBOOT_CUSTOM,
};

struct settings_hardware_result {
	enum settings_hardware_command command;
	int value;
	int spawn_error;
	int exit_code;
	int term_signal;
};

struct settings_worker;

struct device_preferences {
	int backlight_percent;
	bool screen_off;
	bool joystick_rgb_enabled;
	int joystick_rgb_brightness;
	enum joystick_rgb_color joystick_rgb_color;
	enum joystick_rgb_effect joystick_rgb_effect;
	bool usb_debug_enabled;
	bool screen_lock_enabled;
};

struct settings_backend_ops {
	void *context;
	/* Runtime callbacks enqueue work and return; they must not block UI frames. */
	int (*load)(void *context, struct device_preferences *preferences);
	int (*save)(void *context, const struct device_preferences *preferences);
	int (*apply_backlight)(void *context, int user_percent, bool screen_off);
	int (*set_screen_off)(void *context, bool screen_off);
	int (*apply_joystick_rgb)(void *context,
				  const struct device_preferences *preferences);
	/* Backend is restricted to RNDIS usb0 + CDC ACM ttyGS0; no mass storage. */
	int (*set_usb_debug)(void *context, bool enabled);
	int (*set_volume)(void *context, int percent);
	int (*toggle_mute)(void *context);
	int (*request_shutdown)(void *context);
	int (*request_reboot_custom)(void *context);
	int (*export_log)(void *context, enum debug_log_category category);
	int (*clear_log)(void *context, enum debug_log_category category,
			 bool preserve_last_boot_failure);
};

struct settings_state {
	struct device_preferences preferences;
	struct settings_backend_ops backend;
	struct settings_worker *worker;
	char status_text[256];
	int usb_debug_available;
	int volume_target;
	bool reboot_confirm_armed;
	bool reboot_pending;
	uint32_t reboot_confirm_until;
	int64_t log_sizes[DEBUG_LOG_CATEGORY_COUNT];
};

void settings_init(struct settings_state *settings);
int settings_backend_start(struct settings_state *settings,
			   const char *executable, const char *log_path);
void settings_backend_stop(struct settings_state *settings);
int settings_backend_poll(struct settings_state *settings,
			  struct settings_hardware_result *result);
bool settings_backend_available(const struct settings_state *settings);
int settings_export_log(struct settings_state *settings,
			enum debug_log_category category);
int settings_clear_log(struct settings_state *settings,
		       enum debug_log_category category, bool long_press_confirmed);

#endif
