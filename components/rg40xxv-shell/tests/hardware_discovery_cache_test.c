#define _POSIX_C_SOURCE 200809L

#include "hardware.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void replace_thermal_device(const char *root)
{
	char old_path[PATH_MAX];
	char retired_path[PATH_MAX];
	char new_path[PATH_MAX];
	char leaf[PATH_MAX];
	FILE *stream;

	assert(snprintf(old_path, sizeof(old_path),
			"%s/sys/class/thermal/thermal_zone0", root) <
	       (int)sizeof(old_path));
	assert(snprintf(retired_path, sizeof(retired_path),
			"%s/sys/class/thermal/.retired", root) <
	       (int)sizeof(retired_path));
	assert(rename(old_path, retired_path) == 0);
	assert(snprintf(new_path, sizeof(new_path),
			"%s/sys/class/thermal/thermal_zone1", root) <
	       (int)sizeof(new_path));
	assert(mkdir(new_path, 0700) == 0);
	assert(snprintf(leaf, sizeof(leaf), "%s/type", new_path) <
	       (int)sizeof(leaf));
	stream = fopen(leaf, "w");
	assert(stream != NULL);
	assert(fputs("cpu\n", stream) >= 0 && fclose(stream) == 0);
	assert(snprintf(leaf, sizeof(leaf), "%s/temp", new_path) <
	       (int)sizeof(leaf));
	stream = fopen(leaf, "w");
	assert(stream != NULL);
	assert(fputs("61250\n", stream) >= 0 && fclose(stream) == 0);
}

int main(int argc, char **argv)
{
	struct hardware_backend backend;
	struct hardware_snapshot snapshot;
	uint64_t first;
	uint64_t before;
	uint64_t steady_delta;
	uint64_t rediscover_delta;
	uint64_t negative_window_delta;
	uint64_t negative_retry_delta;
	uint64_t root_change_delta;

	assert(argc == 3);
	hardware_backend_init(&backend);
	assert(HARDWARE_DISCOVERY_CACHE_MAX == 5);
	assert(hardware_backend_set_fixture_root(&backend, argv[1]) == 0);
	assert(hardware_refresh(&backend, &snapshot, 0) == 0);
	assert(snapshot.cpu.temperature_millic == 53750);
	assert(snapshot.cpu.voltage_uv == 905000);
	assert(snapshot.cpu.voltage_source == HARDWARE_VOLTAGE_MEASURED_HWMON);
	assert(snapshot.battery.percent == 73);
	assert(snapshot.backlight.percent == 50);
	first = hardware_backend_discovery_scans(&backend);
	assert(first == 5);

	assert(hardware_refresh(&backend, &snapshot, 0) == 0);
	steady_delta = hardware_backend_discovery_scans(&backend) - first;
	assert(steady_delta == 0);

	replace_thermal_device(argv[1]);
	before = hardware_backend_discovery_scans(&backend);
	assert(hardware_refresh(&backend, &snapshot, 0) == 0);
	rediscover_delta = hardware_backend_discovery_scans(&backend) - before;
	assert(rediscover_delta == 1);
	assert(snapshot.cpu.temperature_millic == 61250);

	/* regulator was absent at generation 1: do not rescan before 61. */
	before = hardware_backend_discovery_scans(&backend);
	for (unsigned int refresh = 0; refresh < 57; ++refresh)
		assert(hardware_refresh(&backend, &snapshot, 0) == 0);
	negative_window_delta = hardware_backend_discovery_scans(&backend) - before;
	assert(negative_window_delta == 0);
	assert(hardware_refresh(&backend, &snapshot, 0) == 0);
	negative_retry_delta = hardware_backend_discovery_scans(&backend) - before;
	assert(negative_retry_delta == 1);

	before = hardware_backend_discovery_scans(&backend);
	assert(hardware_backend_set_fixture_root(&backend, argv[2]) == 0);
	assert(hardware_refresh(&backend, &snapshot, 0) == 0);
	root_change_delta = hardware_backend_discovery_scans(&backend) - before;
	assert(root_change_delta == 5);
	assert(snapshot.cpu.temperature_millic == 48750);

	printf("HARDWARE_DISCOVERY_CACHE_TEST first_scans=%llu "
	       "steady_delta=%llu rediscover_delta=%llu "
	       "negative_window_delta=%llu negative_retry_delta=%llu "
	       "root_change_delta=%llu PASS\n",
	       (unsigned long long)first,
	       (unsigned long long)steady_delta,
	       (unsigned long long)rediscover_delta,
	       (unsigned long long)negative_window_delta,
	       (unsigned long long)negative_retry_delta,
	       (unsigned long long)root_change_delta);
	return 0;
}
