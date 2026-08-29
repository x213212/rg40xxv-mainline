#ifndef RG40XXV_HARDWARE_H
#define RG40XXV_HARDWARE_H

#include <limits.h>
#include <stdint.h>
#include <time.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define HARDWARE_UNAVAILABLE (-1)
#define HARDWARE_TEXT_UNAVAILABLE "unavailable"

enum hardware_network_state {
	HARDWARE_NETWORK_UNAVAILABLE = -1,
	HARDWARE_NETWORK_UNKNOWN,
	HARDWARE_NETWORK_DOWN,
	HARDWARE_NETWORK_DORMANT,
	HARDWARE_NETWORK_UP,
};

enum hardware_battery_status {
	HARDWARE_BATTERY_UNAVAILABLE = -1,
	HARDWARE_BATTERY_UNKNOWN,
	HARDWARE_BATTERY_CHARGING,
	HARDWARE_BATTERY_DISCHARGING,
	HARDWARE_BATTERY_FULL,
	HARDWARE_BATTERY_NOT_CHARGING,
};

enum hardware_voltage_source {
	HARDWARE_VOLTAGE_UNAVAILABLE = -1,
	HARDWARE_VOLTAGE_MEASURED_REGULATOR,
	HARDWARE_VOLTAGE_MEASURED_HWMON,
};

struct hardware_datetime {
	time_t epoch_seconds;
	struct tm local;
	int available;
	int time_synced;
};

struct hardware_wifi {
	char interface[32];
	enum hardware_network_state operstate;
	int link_quality;
	int signal_dbm;
};

struct hardware_battery {
	char device[64];
	int percent;
	enum hardware_battery_status status;
};

struct hardware_audio {
	int volume_percent;
	int muted;
};

/* Brightness and blanking are deliberately independent: zero is not "off". */
struct hardware_backlight {
	char device[64];
	int percent;
	int safe_min_percent;
	int screen_off;
	int brightness_control;
	int screen_off_control;
};

struct hardware_joystick_rgb {
	char device[64];
	int capable;
	int actual_percent;
	int configured_percent;
};

struct hardware_system {
	char kernel_release[128];
	char kernel_build[256];
	char boot_slot[32];
};

struct hardware_cpu {
	int64_t frequency_khz;
	int temperature_millic;
	double load_1m;
	double load_5m;
	double load_15m;
	int64_t voltage_uv;
	enum hardware_voltage_source voltage_source;
};

struct hardware_memory {
	int64_t total_bytes;
	int64_t used_bytes;
	int64_t available_bytes;
	int64_t cache_bytes;
	int64_t swap_total_bytes;
	int64_t swap_used_bytes;
};

struct hardware_storage {
	char mountpoint[128];
	int64_t total_bytes;
	int64_t used_bytes;
	int64_t available_bytes;
};

struct hardware_snapshot {
	struct hardware_datetime datetime;
	struct hardware_wifi wifi;
	struct hardware_battery battery;
	struct hardware_audio audio;
	struct hardware_backlight backlight;
	struct hardware_joystick_rgb joystick_rgb;
	struct hardware_system system;
	struct hardware_cpu cpu;
	struct hardware_memory memory;
	struct hardware_storage storage;
};

/* Persistence callbacks own their storage. They must return 0 on success. */
struct hardware_persistence_ops {
	void *context;
	int (*load_rgb_percent)(void *context, int *percent);
	int (*save_rgb_percent)(void *context, int percent);
};

/*
 * All hardware discovery is read-only.  fixture_root is owned by the backend
 * so argv or test-temporary lifetimes cannot leave dangling root pointers.
 */
struct hardware_backend {
	char fixture_root[PATH_MAX];
	char storage_path[PATH_MAX];
	int fixture_mode;
	struct hardware_persistence_ops persistence;
};

void hardware_backend_init(struct hardware_backend *backend);
int hardware_backend_set_fixture_root(struct hardware_backend *backend,
				      const char *root);
void hardware_snapshot_init(struct hardware_snapshot *snapshot);
int hardware_refresh(const struct hardware_backend *backend,
		     struct hardware_snapshot *snapshot, int include_wifi);
int hardware_rgb_load_setting(const struct hardware_backend *backend,
			      int *percent);
int hardware_rgb_save_setting(const struct hardware_backend *backend,
			      int percent);
const char *hardware_voltage_source_label(enum hardware_voltage_source source);
const char *hardware_network_state_label(enum hardware_network_state state);
const char *hardware_battery_status_label(enum hardware_battery_status status);

#endif
