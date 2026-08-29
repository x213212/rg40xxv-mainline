#define _POSIX_C_SOURCE 200809L

#include "hardware_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static void unavailable_text(char *out, size_t size)
{
	(void)snprintf(out, size, "%s", HARDWARE_TEXT_UNAVAILABLE);
}

void hardware_backend_init(struct hardware_backend *backend)
{
	if (backend == NULL)
		return;
	memset(backend, 0, sizeof(*backend));
	(void)snprintf(backend->fixture_root, sizeof(backend->fixture_root), "/");
	(void)snprintf(backend->storage_path, sizeof(backend->storage_path), "/");
}

int hardware_backend_set_fixture_root(struct hardware_backend *backend,
				      const char *root)
{
	char resolved[PATH_MAX];
	struct stat status;

	if (backend == NULL || root == NULL || root[0] != '/' ||
	    realpath(root, resolved) == NULL || lstat(resolved, &status) != 0 ||
	    !S_ISDIR(status.st_mode))
		return -1;
	if (snprintf(backend->fixture_root, sizeof(backend->fixture_root), "%s",
		     resolved) >= (int)sizeof(backend->fixture_root) ||
	    snprintf(backend->storage_path, sizeof(backend->storage_path), "%s",
		     resolved) >= (int)sizeof(backend->storage_path))
		return -1;
	backend->fixture_mode = strcmp(resolved, "/") != 0;
	return 0;
}

void hardware_snapshot_init(struct hardware_snapshot *snapshot)
{
	struct hardware_snapshot *s = snapshot;

	if (s == NULL)
		return;
	memset(s, 0, sizeof(*s));
	s->datetime.epoch_seconds = (time_t)-1;
	s->datetime.local = (struct tm){ .tm_sec = -1, .tm_min = -1,
		.tm_hour = -1, .tm_mday = -1, .tm_mon = -1, .tm_year = -1,
		.tm_wday = -1, .tm_yday = -1, .tm_isdst = -1 };
	s->datetime.available = s->datetime.time_synced = -1;
	s->wifi.operstate = HARDWARE_NETWORK_UNAVAILABLE;
	s->wifi.link_quality = s->wifi.signal_dbm = -1;
	s->battery.percent = -1;
	s->battery.status = HARDWARE_BATTERY_UNAVAILABLE;
	s->audio.volume_percent = s->audio.muted = -1;
	s->backlight.percent = s->backlight.safe_min_percent = -1;
	s->backlight.screen_off = s->backlight.brightness_control = -1;
	s->backlight.screen_off_control = -1;
	s->joystick_rgb.capable = s->joystick_rgb.actual_percent = -1;
	s->joystick_rgb.configured_percent = -1;
	s->cpu.frequency_khz = s->cpu.temperature_millic = -1;
	s->cpu.load_1m = s->cpu.load_5m = s->cpu.load_15m = -1.0;
	s->cpu.voltage_uv = -1;
	s->cpu.voltage_source = HARDWARE_VOLTAGE_UNAVAILABLE;
	s->memory.total_bytes = s->memory.used_bytes = -1;
	s->memory.available_bytes = s->memory.cache_bytes = -1;
	s->memory.swap_total_bytes = s->memory.swap_used_bytes = -1;
	s->storage.total_bytes = s->storage.used_bytes = -1;
	s->storage.available_bytes = -1;
	unavailable_text(s->wifi.interface, sizeof(s->wifi.interface));
	unavailable_text(s->battery.device, sizeof(s->battery.device));
	unavailable_text(s->backlight.device, sizeof(s->backlight.device));
	unavailable_text(s->joystick_rgb.device, sizeof(s->joystick_rgb.device));
	unavailable_text(s->system.kernel_release,
			 sizeof(s->system.kernel_release));
	unavailable_text(s->system.kernel_build, sizeof(s->system.kernel_build));
	unavailable_text(s->system.boot_slot, sizeof(s->system.boot_slot));
	unavailable_text(s->storage.mountpoint, sizeof(s->storage.mountpoint));
}

int hardware_rgb_load_setting(const struct hardware_backend *backend,
			      int *percent)
{
	int value;

	if (backend == NULL || percent == NULL ||
	    backend->persistence.load_rgb_percent == NULL ||
	    backend->persistence.load_rgb_percent(
		backend->persistence.context, &value) != 0 ||
	    value < 0 || value > 100)
		return -1;
	*percent = value;
	return 0;
}

int hardware_rgb_save_setting(const struct hardware_backend *backend,
			      int percent)
{
	if (backend == NULL || percent < 0 || percent > 100 ||
	    backend->persistence.save_rgb_percent == NULL)
		return -1;
	return backend->persistence.save_rgb_percent(
		backend->persistence.context, percent) == 0 ? 0 : -1;
}

const char *hardware_voltage_source_label(enum hardware_voltage_source source)
{
	if (source == HARDWARE_VOLTAGE_MEASURED_REGULATOR)
		return "regulator";
	if (source == HARDWARE_VOLTAGE_MEASURED_HWMON)
		return "hwmon";
	return HARDWARE_TEXT_UNAVAILABLE;
}

const char *hardware_network_state_label(enum hardware_network_state state)
{
	if (state == HARDWARE_NETWORK_UP)
		return "up";
	if (state == HARDWARE_NETWORK_DOWN)
		return "down";
	if (state == HARDWARE_NETWORK_DORMANT)
		return "dormant";
	if (state == HARDWARE_NETWORK_UNKNOWN)
		return "unknown";
	return HARDWARE_TEXT_UNAVAILABLE;
}

const char *hardware_battery_status_label(enum hardware_battery_status status)
{
	if (status == HARDWARE_BATTERY_CHARGING)
		return "charging";
	if (status == HARDWARE_BATTERY_DISCHARGING)
		return "discharging";
	if (status == HARDWARE_BATTERY_FULL)
		return "full";
	if (status == HARDWARE_BATTERY_NOT_CHARGING)
		return "not-charging";
	if (status == HARDWARE_BATTERY_UNKNOWN)
		return "unknown";
	return HARDWARE_TEXT_UNAVAILABLE;
}

int hardware_refresh(const struct hardware_backend *backend,
		     struct hardware_snapshot *snapshot, int include_wifi)
{
	if (backend == NULL || snapshot == NULL ||
	    backend->fixture_root[0] != '/' || backend->storage_path[0] != '/')
		return -1;
	hardware_snapshot_init(snapshot);
	hw_refresh_datetime(backend, &snapshot->datetime);
	if (include_wifi)
		hw_refresh_wifi(backend, &snapshot->wifi);
	hw_refresh_battery(backend, &snapshot->battery);
	hw_refresh_audio(backend, &snapshot->audio);
	hw_refresh_backlight(backend, &snapshot->backlight);
	hw_refresh_rgb(backend, &snapshot->joystick_rgb);
	(void)hardware_rgb_load_setting(backend,
					&snapshot->joystick_rgb.configured_percent);
	hw_refresh_system(backend, &snapshot->system);
	hw_refresh_cpu(backend, &snapshot->cpu);
	hw_refresh_memory(backend, &snapshot->memory);
	hw_refresh_storage(backend, &snapshot->storage);
	return 0;
}
