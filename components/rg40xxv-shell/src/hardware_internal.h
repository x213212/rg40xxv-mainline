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

enum hw_discovery_slot {
	HW_DISCOVERY_THERMAL,
	HW_DISCOVERY_REGULATOR,
	HW_DISCOVERY_HWMON,
	HW_DISCOVERY_BATTERY,
	HW_DISCOVERY_BACKLIGHT,
	HW_DISCOVERY_SLOT_COUNT,
};

int hw_path(char *out, size_t size, const struct hardware_backend *backend,
	    const char *absolute_tail);
int hw_read_text(const char *path, char *out, size_t size);
int64_t hw_read_number(const char *path, int64_t low, int64_t high);
int hw_regular_exists(const char *path);
DIR *hw_open_directory(const char *path);
DIR *hw_discovery_open_directory(struct hardware_backend *backend,
				 const char *path);
void hw_discovery_begin_refresh(struct hardware_backend *backend);
void hw_discovery_cache_reset(struct hardware_backend *backend);
int hw_discovery_cached_device(struct hardware_backend *backend,
			       enum hw_discovery_slot slot, const char *root,
			       char *name, size_t size, char *leaf,
			       size_t leaf_size);
int hw_discovery_should_scan(const struct hardware_backend *backend,
			     enum hw_discovery_slot slot);
int hw_discovery_remember(struct hardware_backend *backend,
			  enum hw_discovery_slot slot, const char *root,
			  const char *name, const char *leaf);
void hw_discovery_forget(struct hardware_backend *backend,
			 enum hw_discovery_slot slot);
void hw_discovery_mark_missing(struct hardware_backend *backend,
			       enum hw_discovery_slot slot);
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
void hw_refresh_cpu(struct hardware_backend *backend,
		    struct hardware_cpu *cpu);
void hw_refresh_memory(const struct hardware_backend *backend,
		       struct hardware_memory *memory);
void hw_refresh_storage(const struct hardware_backend *backend,
			struct hardware_storage *storage);
void hw_refresh_wifi(const struct hardware_backend *backend,
		     struct hardware_wifi *wifi);
void hw_refresh_battery(struct hardware_backend *backend,
			struct hardware_battery *battery);
void hw_refresh_audio(const struct hardware_backend *backend,
		      struct hardware_audio *audio);
void hw_refresh_backlight(struct hardware_backend *backend,
			  struct hardware_backlight *backlight);
void hw_refresh_rgb(const struct hardware_backend *backend,
		    struct hardware_joystick_rgb *rgb);
int hw_alsa_read(struct hardware_audio *audio);

#endif
