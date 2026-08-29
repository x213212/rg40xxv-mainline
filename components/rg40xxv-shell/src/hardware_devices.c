#define _POSIX_C_SOURCE 200809L

#include "hardware_internal.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

static int quality_percent(double quality)
{
	if (quality < 0.0)
		return -1;
	if (quality <= 70.0)
		return (int)((quality * 100.0 + 35.0) / 70.0);
	if (quality <= 100.0)
		return (int)(quality + 0.5);
	return -1;
}

static void wifi_interface_fallback(const struct hardware_backend *backend,
				    struct hardware_wifi *wifi)
{
	char root[PATH_MAX];
	struct dirent *entry;
	DIR *directory;
	unsigned int seen = 0;

	if (hw_path(root, sizeof(root), backend, "/sys/class/net") != 0 ||
	    (directory = hw_open_directory(root)) == NULL)
		return;
	while (seen++ < HW_DIRECTORY_ENTRY_MAX &&
	       (entry = readdir(directory)) != NULL) {
		char path[PATH_MAX];
		DIR *wireless;

		if (!hw_valid_component(entry->d_name) ||
		    snprintf(path, sizeof(path), "%s/%s/wireless", root,
			     entry->d_name) >= (int)sizeof(path))
			continue;
		wireless = hw_open_directory(path);
		if (wireless == NULL)
			continue;
		(void)closedir(wireless);
		(void)snprintf(wifi->interface, sizeof(wifi->interface), "%.31s",
			       entry->d_name);
		break;
	}
	(void)closedir(directory);
}

void hw_refresh_wifi(const struct hardware_backend *backend,
		     struct hardware_wifi *wifi)
{
	char path[PATH_MAX];
	char text[HW_TEXT_MEDIUM];
	char *line;
	char *save;
	unsigned int lines = 0;

	if (hw_path(path, sizeof(path), backend, "/proc/net/wireless") == 0 &&
	    hw_read_text(path, text, sizeof(text)) == 0) {
		for (line = strtok_r(text, "\n", &save);
		     line != NULL && lines++ < 128;
		     line = strtok_r(NULL, "\n", &save)) {
			char interface[32];
			char *colon = strchr(line, ':');
			double quality;
			double level;

			if (colon == NULL || sscanf(line, " %31[^:]", interface) != 1 ||
			    !hw_valid_component(interface) ||
			    sscanf(colon + 1, " %*x %lf %lf", &quality, &level) != 2)
				continue;
			(void)snprintf(wifi->interface, sizeof(wifi->interface), "%s",
				       interface);
			wifi->link_quality = quality_percent(quality);
			if (level > 0.0 && level <= 255.0)
				level -= 256.0;
			if (level >= -127.0 && level <= 0.0)
				wifi->signal_dbm = (int)level;
			break;
		}
	}
	if (strcmp(wifi->interface, HARDWARE_TEXT_UNAVAILABLE) == 0)
		wifi_interface_fallback(backend, wifi);
	if (strcmp(wifi->interface, HARDWARE_TEXT_UNAVAILABLE) == 0 ||
	    snprintf(path, sizeof(path), "%s/sys/class/net/%s/operstate",
		     strcmp(backend->fixture_root, "/") == 0 ? "" :
		     backend->fixture_root, wifi->interface) >= (int)sizeof(path) ||
	    hw_read_text(path, text, sizeof(text)) != 0)
		return;
	if (strcmp(text, "up") == 0)
		wifi->operstate = HARDWARE_NETWORK_UP;
	else if (strcmp(text, "down") == 0)
		wifi->operstate = HARDWARE_NETWORK_DOWN;
	else if (strcmp(text, "dormant") == 0)
		wifi->operstate = HARDWARE_NETWORK_DORMANT;
	else if (strcmp(text, "unknown") == 0)
		wifi->operstate = HARDWARE_NETWORK_UNKNOWN;
}

static int find_typed_device(const char *root, const char *wanted,
			     char *name, size_t size)
{
	char path[PATH_MAX];
	char type[64];
	struct dirent *entry;
	DIR *directory = hw_open_directory(root);
	unsigned int seen = 0;

	if (directory == NULL)
		return -1;
	while (seen++ < HW_DIRECTORY_ENTRY_MAX &&
	       (entry = readdir(directory)) != NULL) {
		if (!hw_valid_component(entry->d_name) ||
		    hw_device_path(path, sizeof(path), root, entry->d_name, "type") != 0 ||
		    hw_read_text(path, type, sizeof(type)) != 0 ||
		    strcmp(type, wanted) != 0)
			continue;
		(void)snprintf(name, size, "%.63s", entry->d_name);
		(void)closedir(directory);
		return 0;
	}
	(void)closedir(directory);
	return -1;
}

void hw_refresh_battery(const struct hardware_backend *backend,
			struct hardware_battery *battery)
{
	char root[PATH_MAX];
	char path[PATH_MAX];
	char text[64];
	int64_t value;

	if (hw_path(root, sizeof(root), backend, "/sys/class/power_supply") != 0 ||
	    find_typed_device(root, "Battery", battery->device,
			      sizeof(battery->device)) != 0)
		return;
	value = hw_device_number(root, battery->device, "capacity", 0, 100);
	if (value >= 0)
		battery->percent = (int)value;
	if (hw_device_path(path, sizeof(path), root, battery->device, "status") != 0 ||
	    hw_read_text(path, text, sizeof(text)) != 0)
		return;
	if (strcmp(text, "Charging") == 0)
		battery->status = HARDWARE_BATTERY_CHARGING;
	else if (strcmp(text, "Discharging") == 0)
		battery->status = HARDWARE_BATTERY_DISCHARGING;
	else if (strcmp(text, "Full") == 0)
		battery->status = HARDWARE_BATTERY_FULL;
	else if (strcmp(text, "Not charging") == 0)
		battery->status = HARDWARE_BATTERY_NOT_CHARGING;
	else if (strcmp(text, "Unknown") == 0)
		battery->status = HARDWARE_BATTERY_UNKNOWN;
}

static int first_entry(const char *root, char *name, size_t size)
{
	struct dirent *entry;
	DIR *directory = hw_open_directory(root);
	unsigned int seen = 0;

	if (directory == NULL)
		return -1;
	while (seen++ < HW_DIRECTORY_ENTRY_MAX &&
	       (entry = readdir(directory)) != NULL) {
		if (!hw_valid_component(entry->d_name))
			continue;
		(void)snprintf(name, size, "%.63s", entry->d_name);
		(void)closedir(directory);
		return 0;
	}
	(void)closedir(directory);
	return -1;
}

void hw_refresh_backlight(const struct hardware_backend *backend,
			  struct hardware_backlight *light)
{
	char root[PATH_MAX];
	char path[PATH_MAX];
	int64_t current;
	int64_t maximum;
	int64_t minimum;
	int64_t power;

	if (hw_path(root, sizeof(root), backend, "/sys/class/backlight") != 0 ||
	    first_entry(root, light->device, sizeof(light->device)) != 0)
		return;
	maximum = hw_device_number(root, light->device, "max_brightness", 1,
				   INT_MAX);
	current = hw_device_number(root, light->device, "actual_brightness", 0,
				   INT_MAX);
	if (current < 0)
		current = hw_device_number(root, light->device, "brightness", 0,
					   INT_MAX);
	light->percent = hw_percent_of(current, maximum);
	if (hw_device_path(path, sizeof(path), root, light->device, "brightness") == 0 &&
	    maximum > 0 && hw_regular_exists(path))
		light->brightness_control = 1;
	minimum = hw_device_number(root, light->device, "min_brightness", 0,
				   INT_MAX);
	light->safe_min_percent = hw_percent_of(minimum, maximum);
	power = hw_device_number(root, light->device, "bl_power", 0, INT_MAX);
	if (power >= 0) {
		light->screen_off = power == 0 ? 0 : 1;
		light->screen_off_control = 1;
	}
}

void hw_refresh_rgb(const struct hardware_backend *backend,
		    struct hardware_joystick_rgb *rgb)
{
	char root[PATH_MAX];
	char path[PATH_MAX];
	int64_t current;
	int64_t maximum;

	if (hw_path(root, sizeof(root), backend, "/sys/class/leds") != 0 ||
	    hw_device_path(path, sizeof(path), root, "rgb:kbd_backlight",
			   "brightness") != 0 || !hw_regular_exists(path))
		return;
	(void)snprintf(rgb->device, sizeof(rgb->device), "rgb:kbd_backlight");
	rgb->capable = 1;
	maximum = hw_device_number(root, rgb->device, "max_brightness", 1,
				   INT_MAX);
	current = hw_device_number(root, rgb->device, "brightness", 0, INT_MAX);
	rgb->actual_percent = hw_percent_of(current, maximum);
}

static int read_audio_snapshot(const struct hardware_backend *backend,
			       struct hardware_audio *audio)
{
	char path[PATH_MAX];
	char text[HW_TEXT_SMALL];
	char *line;
	char *save;
	int volume = -1;
	int muted = -1;
	unsigned int lines = 0;

	if (hw_path(path, sizeof(path), backend,
		    "/run/rg40xxv-ui/alsa-volume") != 0 ||
	    hw_read_text(path, text, sizeof(text)) != 0)
		return -1;
	for (line = strtok_r(text, "\n", &save); line != NULL && lines++ < 16;
	     line = strtok_r(NULL, "\n", &save)) {
		int value;
		char extra;

		if (sscanf(line, "volume_percent=%d%c", &value, &extra) == 1 &&
		    value >= 0 && value <= 100)
			volume = value;
		else if (sscanf(line, "muted=%d%c", &value, &extra) == 1 &&
			 value >= 0 && value <= 1)
			muted = value;
	}
	if (volume < 0)
		return -1;
	audio->volume_percent = volume;
	audio->muted = muted;
	return 0;
}

void hw_refresh_audio(const struct hardware_backend *backend,
		      struct hardware_audio *audio)
{
	char path[PATH_MAX];
	char cards[HW_TEXT_SMALL];

	if (read_audio_snapshot(backend, audio) == 0 || backend->fixture_mode)
		return;
	if (hw_path(path, sizeof(path), backend, "/proc/asound/cards") != 0 ||
	    hw_read_text(path, cards, sizeof(cards)) != 0 ||
	    hw_contains_casefold(cards, "no soundcards"))
		return;
	(void)hw_alsa_read(audio);
}
