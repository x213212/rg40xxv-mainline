#define _POSIX_C_SOURCE 200809L

#include "settings.h"

#include <SDL.h>
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static uint64_t monotonic_ms(void)
{
	struct timespec value;

	assert(clock_gettime(CLOCK_MONOTONIC, &value) == 0);
	return (uint64_t)value.tv_sec * 1000U +
		(uint64_t)value.tv_nsec / 1000000U;
}

static struct settings_hardware_result wait_result(
	struct settings_state *settings)
{
	struct settings_hardware_result result;

	for (int attempt = 0; attempt < 2000; ++attempt) {
		if (settings_backend_poll(settings, &result) > 0)
			return result;
		SDL_Delay(5U);
	}
	assert(!"hardware result timeout");
	return (struct settings_hardware_result){ 0 };
}

static void assert_success(struct settings_hardware_result result,
			   enum settings_hardware_command command, int value)
{
	assert(result.command == command);
	assert(result.value == value);
	assert(result.spawn_error == 0);
	assert(result.exit_code == 0);
	assert(result.term_signal == 0);
}

static void marker_path(char path[4096], const char *helper,
			const char *suffix)
{
	int length = snprintf(path, 4096, "%s%s", helper, suffix);

	assert(length >= 0 && length < 4096);
}

static void wait_for_path(const char *path)
{
	for (int attempt = 0; attempt < 1000; ++attempt) {
		if (access(path, F_OK) == 0)
			return;
		SDL_Delay(5U);
	}
	assert(!"helper marker timeout");
}

static void create_marker(const char *path)
{
	FILE *marker = fopen(path, "w");

	assert(marker != NULL);
	assert(fclose(marker) == 0);
}

int main(int argc, char **argv)
{
	struct settings_state settings;
	struct device_preferences preferences;
	struct settings_hardware_result result;
	uint64_t started;
	uint64_t enqueue_elapsed;
	uint64_t stop_elapsed;
	char blocked_path[4096];
	char release_path[4096];

	assert(argc == 6);
	assert(SDL_Init(SDL_INIT_EVENTS | SDL_INIT_TIMER) == 0);
	assert(setenv("DEVICE_CONTROL_TESTING", "1", 1) == 0);
	assert(setenv("DEVICE_CONTROL_TEST_ROOT", "/must-not-leak", 1) == 0);
	assert(setenv("LD_PRELOAD", "/must-not-leak.so", 1) == 0);
	assert(setenv("LD_LIBRARY_PATH", "/must-not-leak", 1) == 0);
	settings_init(&settings);
	assert(settings.preferences.usb_debug_enabled);
	assert(settings_backend_start(&settings, argv[1], argv[2]) == 0);
	assert(settings_backend_available(&settings));

	preferences = settings.preferences;
	started = monotonic_ms();
	assert(settings.backend.set_screen_off(settings.backend.context, true) == 0);
	assert(settings.backend.set_screen_off(settings.backend.context, false) == 0);
	assert(settings.backend.apply_backlight(settings.backend.context, 0,
						 false) == 0);
	assert(settings.backend.apply_backlight(settings.backend.context, 100,
						 false) == 0);
	preferences.joystick_rgb_brightness = 100;
	assert(settings.backend.apply_joystick_rgb(settings.backend.context,
						   &preferences) == 0);
	assert(settings.backend.set_usb_debug(settings.backend.context, false) == 0);
	assert(settings.backend.request_shutdown(settings.backend.context) == 0);
	assert(settings.backend.request_reboot_custom(settings.backend.context) == 0);
	assert(settings.backend.set_volume(settings.backend.context, 35) == 0);
	assert(settings.backend.toggle_mute(settings.backend.context) == 0);
	enqueue_elapsed = monotonic_ms() - started;
	assert(enqueue_elapsed < 50U);

	assert_success(wait_result(&settings), SETTINGS_HW_SCREEN_OFF, 0);
	assert_success(wait_result(&settings), SETTINGS_HW_SCREEN_ON, 0);
	assert_success(wait_result(&settings), SETTINGS_HW_BRIGHTNESS, 0);
	assert_success(wait_result(&settings), SETTINGS_HW_BRIGHTNESS, 100);
	assert_success(wait_result(&settings), SETTINGS_HW_JOYSTICK_RGB, 100);
	assert_success(wait_result(&settings), SETTINGS_HW_USB_DEBUG, 0);
	assert_success(wait_result(&settings), SETTINGS_HW_ORDERLY_SHUTDOWN, 0);
	assert_success(wait_result(&settings), SETTINGS_HW_REBOOT_CUSTOM, 0);
	assert_success(wait_result(&settings), SETTINGS_HW_VOLUME, 35);
	assert_success(wait_result(&settings), SETTINGS_HW_MUTE_TOGGLE, 0);

	assert(settings.backend.apply_backlight(settings.backend.context, 13,
						 false) == 0);
	result = wait_result(&settings);
	assert(result.command == SETTINGS_HW_BRIGHTNESS);
	assert(result.value == 13);
	assert(result.spawn_error == 0);
	assert(result.exit_code == 7);

	/*
	 * Hold the worker in hardwarectl while more than one full queue of
	 * alternating RGB/USB state changes arrives.  Only each final value may
	 * remain pending; edge-triggered mute and shutdown requests must survive.
	 */
	marker_path(blocked_path, argv[1], ".blocked");
	marker_path(release_path, argv[1], ".release");
	assert(settings.backend.apply_backlight(settings.backend.context, 97,
						 false) == 0);
	wait_for_path(blocked_path);
	for (int value = 0; value < 64; ++value) {
		preferences.joystick_rgb_brightness = value;
		assert(settings.backend.apply_joystick_rgb(
			settings.backend.context, &preferences) == 0);
		assert(settings.backend.set_usb_debug(settings.backend.context,
						      (value & 1) != 0) == 0);
	}
	assert(settings.backend.toggle_mute(settings.backend.context) == 0);
	for (int value = 0; value < 64; ++value) {
		preferences.joystick_rgb_brightness = value;
		assert(settings.backend.apply_joystick_rgb(
			settings.backend.context, &preferences) == 0);
		assert(settings.backend.set_usb_debug(settings.backend.context,
						      (value & 1) != 0) == 0);
	}
	assert(settings.backend.toggle_mute(settings.backend.context) == 0);
	assert(settings.backend.request_shutdown(settings.backend.context) == 0);
	create_marker(release_path);
	assert_success(wait_result(&settings), SETTINGS_HW_BRIGHTNESS, 97);
	assert_success(wait_result(&settings), SETTINGS_HW_MUTE_TOGGLE, 0);
	assert_success(wait_result(&settings), SETTINGS_HW_JOYSTICK_RGB, 63);
	assert_success(wait_result(&settings), SETTINGS_HW_USB_DEBUG, 1);
	assert_success(wait_result(&settings), SETTINGS_HW_MUTE_TOGGLE, 0);
	assert_success(wait_result(&settings), SETTINGS_HW_ORDERLY_SHUTDOWN, 0);

	assert(settings.backend.apply_backlight(settings.backend.context, 98,
						 false) == 0);
	result = wait_result(&settings);
	assert(result.command == SETTINGS_HW_BRIGHTNESS);
	assert(result.value == 98);
	assert(result.spawn_error == ETIMEDOUT);
	assert(result.term_signal == SIGTERM || result.term_signal == SIGKILL ||
	       result.exit_code != 0);

	assert(settings.backend.apply_backlight(settings.backend.context, 99,
						 false) == 0);
	SDL_Delay(100U);
	started = monotonic_ms();
	settings_backend_stop(&settings);
	stop_elapsed = monotonic_ms() - started;
	assert(stop_elapsed < 1000U);
	assert(!settings_backend_available(&settings));

	(void)unlink(argv[3]);
	assert(symlink(argv[4], argv[3]) == 0);
	settings_init(&settings);
	assert(settings_backend_start(&settings, argv[1], argv[3]) == 0);
	assert(settings.backend.set_screen_off(settings.backend.context, false) == 0);
	result = wait_result(&settings);
	assert(result.command == SETTINGS_HW_SCREEN_ON);
	assert(result.spawn_error == ELOOP);
	settings_backend_stop(&settings);

	settings_init(&settings);
	assert(settings_backend_start(&settings, argv[1], argv[5]) == 0);
	assert(settings.backend.set_screen_off(settings.backend.context, false) == 0);
	result = wait_result(&settings);
	assert(result.command == SETTINGS_HW_SCREEN_ON);
	assert(result.spawn_error != 0);
	started = monotonic_ms();
	settings_backend_stop(&settings);
	assert(monotonic_ms() - started < 1000U);

	SDL_Quit();
	printf("SETTINGS_WORKER_TEST PASS enqueue_ms=%llu stop_ms=%llu\n",
	       (unsigned long long)enqueue_elapsed,
	       (unsigned long long)stop_elapsed);
	return 0;
}
