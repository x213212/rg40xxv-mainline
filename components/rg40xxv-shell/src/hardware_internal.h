#ifndef RG40XXV_HARDWARE_INTERNAL_H
#define RG40XXV_HARDWARE_INTERNAL_H

#include "hardware.h"

#include <dirent.h>
#include <stddef.h>
#include <stdint.h>

enum {
	HW_TEXT_SMALL = 512,
	HW_TEXT_MEDIUM = 8192,
	HW_TEXT_LARGE = 16384,
	HW_DIRECTORY_ENTRY_MAX = 64,
};

int hw_path(char *out, size_t size, const struct hardware_backend *backend,
	    const char *absolute_tail);
int hw_read_text(const char *path, char *out, size_t size);
int64_t hw_read_number(const char *path, int64_t low, int64_t high);
int hw_regular_exists(const char *path);
DIR *hw_open_directory(const char *path);
int hw_valid_component(const char *name);
int hw_device_path(char *path, size_t size, const char *root,
		   const char *name, const char *attribute);
int64_t hw_device_number(const char *root, const char *name,
			 const char *attribute, int64_t low, int64_t high);
int hw_contains_casefold(const char *text, const char *needle);
int hw_percent_of(int64_t value, int64_t maximum);

void hw_refresh_datetime(const struct hardware_backend *backend,
			 struct hardware_datetime *datetime);
void hw_refresh_system(const struct hardware_backend *backend,
		       struct hardware_system *system);
void hw_refresh_cpu(const struct hardware_backend *backend,
		    struct hardware_cpu *cpu);
void hw_refresh_memory(const struct hardware_backend *backend,
		       struct hardware_memory *memory);
void hw_refresh_storage(const struct hardware_backend *backend,
			struct hardware_storage *storage);
void hw_refresh_wifi(const struct hardware_backend *backend,
		     struct hardware_wifi *wifi);
void hw_refresh_battery(const struct hardware_backend *backend,
			struct hardware_battery *battery);
void hw_refresh_audio(const struct hardware_backend *backend,
		      struct hardware_audio *audio);
void hw_refresh_backlight(const struct hardware_backend *backend,
			  struct hardware_backlight *backlight);
void hw_refresh_rgb(const struct hardware_backend *backend,
		    struct hardware_joystick_rgb *rgb);
int hw_alsa_read(struct hardware_audio *audio);

#endif
