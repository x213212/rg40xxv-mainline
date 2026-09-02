#include "hardware.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void check_valid(const struct hardware_snapshot *snapshot)
{
	assert(snapshot->datetime.available == 1);
	assert(snapshot->datetime.local.tm_year + 1900 == 2026);
	assert(snapshot->datetime.local.tm_mon + 1 == 8);
	assert(snapshot->datetime.local.tm_mday == 25);
	assert(snapshot->datetime.local.tm_hour == 12);
	assert(snapshot->datetime.local.tm_min == 34);
	assert(snapshot->datetime.time_synced == 1);
	assert(strcmp(snapshot->system.kernel_release,
		      "7.2.0-rg40xxv-fixture") == 0);
	assert(snapshot->memory.total_bytes == 1048576000);
	assert(snapshot->memory.available_bytes == 262144000);
	assert(snapshot->memory.used_bytes == 786432000);
	assert(strcmp(snapshot->wifi.interface, "wlan0") == 0);
	assert(snapshot->wifi.operstate == HARDWARE_NETWORK_UP);
	assert(snapshot->wifi.link_quality == 90);
	assert(snapshot->wifi.signal_dbm == -41);
	assert(snapshot->battery.percent == 73);
	assert(snapshot->battery.status == HARDWARE_BATTERY_CHARGING);
	assert(snapshot->backlight.percent == 50);
	assert(snapshot->audio.volume_percent == 64);
	assert(snapshot->audio.muted == 0);
	assert(snapshot->cpu.voltage_uv == 920000);
	assert(snapshot->cpu.voltage_source ==
	       HARDWARE_VOLTAGE_MEASURED_REGULATOR);
}

static void check_persistent_sync(const struct hardware_snapshot *snapshot)
{
	assert(snapshot->datetime.available == 1);
	assert(snapshot->datetime.time_synced == 1);
}

static void check_hwmon(const struct hardware_snapshot *snapshot)
{
	assert(snapshot->cpu.voltage_uv == 905000);
	assert(snapshot->cpu.voltage_source == HARDWARE_VOLTAGE_MEASURED_HWMON);
}

static void check_unavailable(const struct hardware_snapshot *snapshot)
{
	assert(snapshot->datetime.available == -1);
	assert(snapshot->datetime.time_synced == -1);
	assert(strcmp(snapshot->system.kernel_release,
		      HARDWARE_TEXT_UNAVAILABLE) == 0);
	assert(snapshot->memory.total_bytes == -1);
	assert(snapshot->wifi.operstate == HARDWARE_NETWORK_UNAVAILABLE);
	assert(snapshot->battery.percent == -1);
	assert(snapshot->backlight.percent == -1);
	assert(snapshot->audio.volume_percent == -1);
	/* A debugfs OPP target must never be presented as measured voltage. */
	assert(snapshot->cpu.voltage_uv == -1);
	assert(snapshot->cpu.voltage_source == HARDWARE_VOLTAGE_UNAVAILABLE);
}

static void check_unsynced(const struct hardware_snapshot *snapshot)
{
	assert(snapshot->datetime.available == 1);
	assert(snapshot->datetime.time_synced == -1);
}

int main(int argc, char **argv)
{
	struct hardware_backend backend;
	struct hardware_snapshot snapshot;

	assert(argc == 3);
	hardware_backend_init(&backend);
	assert(hardware_backend_set_fixture_root(&backend, "relative") == -1);
	assert(hardware_backend_set_fixture_root(&backend, argv[1]) == 0);
	assert(backend.fixture_mode == 1);
	assert(hardware_refresh(&backend, &snapshot, 1) == 0);
	if (strcmp(argv[2], "valid") == 0)
		check_valid(&snapshot);
	else if (strcmp(argv[2], "hwmon") == 0)
		check_hwmon(&snapshot);
	else if (strcmp(argv[2], "unavailable") == 0)
		check_unavailable(&snapshot);
	else if (strcmp(argv[2], "persistent-sync") == 0)
		check_persistent_sync(&snapshot);
	else
		check_unsynced(&snapshot);
	puts("HARDWARE_FIXTURE_TEST PASS");
	return 0;
}
