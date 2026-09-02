#define _POSIX_C_SOURCE 200809L

#include "hardware_internal.h"

#include <ctype.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/statvfs.h>
#include <time.h>

#define TIME_SYNC_RUNTIME_STAMP "/run/rg40xxv/time-sync-success"
#define TIME_SYNC_PERSISTENT_STAMP "/mnt/data/rg40xxv/state/time-sync-success"
#define TIME_SYNC_MINIMUM_EPOCH INT64_C(1704067200)
#define TIME_SYNC_MAXIMUM_EPOCH INT64_C(4102444800)

static int64_t snapshot_epoch(const struct hardware_backend *backend)
{
	char path[PATH_MAX];
	struct timespec now;

	if (backend->fixture_mode) {
		if (hw_path(path, sizeof(path), backend,
			    "/run/rg40xxv-ui/time.epoch") != 0)
			return -1;
		return hw_read_number(path, 946684800, INT64_MAX - 28800);
	}
	if (clock_gettime(CLOCK_REALTIME, &now) != 0 || now.tv_sec < 0)
		return -1;
	return (int64_t)now.tv_sec;
}

static int synchronization_stamp_valid(const struct hardware_backend *backend,
				       int64_t clock_epoch)
{
	static const char *const stamps[] = {
		TIME_SYNC_RUNTIME_STAMP,
		TIME_SYNC_PERSISTENT_STAMP,
	};
	char path[PATH_MAX];
	int64_t stamp_epoch;

	if (clock_epoch < TIME_SYNC_MINIMUM_EPOCH ||
	    clock_epoch >= TIME_SYNC_MAXIMUM_EPOCH)
		return 0;
	for (size_t index = 0; index < sizeof(stamps) / sizeof(stamps[0]);
	     ++index) {
		if (hw_path(path, sizeof(path), backend, stamps[index]) != 0)
			continue;
		stamp_epoch = hw_read_number(path, TIME_SYNC_MINIMUM_EPOCH,
					     TIME_SYNC_MAXIMUM_EPOCH - 1);
		/* The helper publishes only after ntpdate and hwclock both succeed. */
		if (stamp_epoch >= 0 && stamp_epoch <= clock_epoch)
			return 1;
	}
	return 0;
}

void hw_refresh_datetime(const struct hardware_backend *backend,
			 struct hardware_datetime *datetime)
{
	int64_t epoch = snapshot_epoch(backend);
	time_t taipei_epoch;

	if (epoch >= 0 && epoch <= INT64_MAX - 28800) {
		taipei_epoch = (time_t)(epoch + 28800);
		if ((int64_t)taipei_epoch == epoch + 28800 &&
		    gmtime_r(&taipei_epoch, &datetime->local) != NULL) {
			datetime->epoch_seconds = (time_t)epoch;
			datetime->available = 1;
		}
	}
	if (synchronization_stamp_valid(backend, epoch))
		datetime->time_synced = 1;
}

static int valid_slot(const char *slot)
{
	size_t index;

	if (*slot == '_')
		++slot;
	if (*slot == '\0')
		return 0;
	for (index = 0; slot[index] != '\0'; ++index)
		if (isalnum((unsigned char)slot[index]) == 0 && slot[index] != '-' &&
		    slot[index] != '_')
			return 0;
	return index < sizeof(((struct hardware_system *)0)->boot_slot);
}

static void parse_boot_slot(char *cmdline, char *out, size_t size)
{
	static const char *const keys[] = {
		"androidboot.slot_suffix=", "androidboot.slot=", "boot_slot=",
		"bootslot=", "slot_suffix=", "rauc.slot=",
	};
	char *save;
	char *token;

	for (token = strtok_r(cmdline, " \t", &save); token != NULL;
	     token = strtok_r(NULL, " \t", &save)) {
		for (size_t index = 0; index < sizeof(keys) / sizeof(keys[0]);
		     ++index) {
			size_t length = strlen(keys[index]);
			const char *slot = token + length;

			if (strncmp(token, keys[index], length) != 0 ||
			    !valid_slot(slot))
				continue;
			if (*slot == '_')
				++slot;
			(void)snprintf(out, size, "%s", slot);
			return;
		}
	}
}

void hw_refresh_system(const struct hardware_backend *backend,
		       struct hardware_system *system)
{
	char path[PATH_MAX];
	char text[HW_TEXT_SMALL];

	if (hw_path(path, sizeof(path), backend,
		    "/proc/sys/kernel/osrelease") == 0 &&
	    hw_read_text(path, text, sizeof(text)) == 0)
		(void)snprintf(system->kernel_release,
			       sizeof(system->kernel_release), "%.127s", text);
	if (hw_path(path, sizeof(path), backend,
		    "/proc/sys/kernel/version") == 0 &&
	    hw_read_text(path, text, sizeof(text)) == 0)
		(void)snprintf(system->kernel_build,
			       sizeof(system->kernel_build), "%.255s", text);
	if (hw_path(path, sizeof(path), backend, "/proc/cmdline") == 0 &&
	    hw_read_text(path, text, sizeof(text)) == 0)
		parse_boot_slot(text, system->boot_slot,
				sizeof(system->boot_slot));
	if (strcmp(system->boot_slot, HARDWARE_TEXT_UNAVAILABLE) == 0 &&
	    hw_path(path, sizeof(path), backend,
		    "/sys/firmware/devicetree/base/chosen/bootargs") == 0 &&
	    hw_read_text(path, text, sizeof(text)) == 0)
		parse_boot_slot(text, system->boot_slot,
				sizeof(system->boot_slot));
}

static void refresh_cpu_basic(const struct hardware_backend *backend,
			      struct hardware_cpu *cpu)
{
	static const char *const paths[] = {
		"/sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq",
		"/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq",
		"/sys/devices/system/cpu/cpufreq/policy0/cpuinfo_cur_freq",
	};
	char path[PATH_MAX];
	char text[HW_TEXT_SMALL];

	for (size_t index = 0; index < sizeof(paths) / sizeof(paths[0]); ++index) {
		if (hw_path(path, sizeof(path), backend, paths[index]) == 0)
			cpu->frequency_khz = hw_read_number(path, 1, INT64_MAX);
		if (cpu->frequency_khz >= 0)
			break;
	}
	if (hw_path(path, sizeof(path), backend, "/proc/loadavg") == 0 &&
	    hw_read_text(path, text, sizeof(text)) == 0 &&
	    sscanf(text, "%lf %lf %lf", &cpu->load_1m, &cpu->load_5m,
		   &cpu->load_15m) != 3)
		cpu->load_1m = cpu->load_5m = cpu->load_15m = -1.0;
}

static void refresh_cpu_temperature(struct hardware_backend *backend,
				    struct hardware_cpu *cpu)
{
	char root[PATH_MAX];
	char path[PATH_MAX];
	char name[64];
	char type[64];
	struct dirent *entry;
	DIR *directory;
	unsigned int seen = 0;
	int remembered = 0;

	if (hw_path(root, sizeof(root), backend, "/sys/class/thermal") != 0)
		return;
	if (hw_discovery_cached_device(backend, HW_DISCOVERY_THERMAL, root,
				       name, sizeof(name), NULL, 0) == 0) {
		int64_t value;

		if (hw_device_path(path, sizeof(path), root, name, "type") == 0 &&
		    hw_read_text(path, type, sizeof(type)) == 0 &&
		    hw_contains_casefold(type, "cpu") &&
		    (value = hw_device_number(root, name, "temp", -40000, 200000)) >=
			-40000) {
			cpu->temperature_millic = (int)value;
			return;
		}
		hw_discovery_forget(backend, HW_DISCOVERY_THERMAL);
	}
	if (!hw_discovery_should_scan(backend, HW_DISCOVERY_THERMAL))
		return;
	directory = hw_discovery_open_directory(backend, root);
	if (directory == NULL) {
		hw_discovery_mark_missing(backend, HW_DISCOVERY_THERMAL);
		return;
	}
	while (seen++ < HW_DIRECTORY_ENTRY_MAX &&
	       (entry = readdir(directory)) != NULL) {
		int64_t value;

		if (strncmp(entry->d_name, "thermal_zone", 12) != 0 ||
		    !hw_valid_component(entry->d_name) ||
		    hw_device_path(path, sizeof(path), root, entry->d_name, "type") != 0 ||
		    hw_read_text(path, type, sizeof(type)) != 0 ||
		    !hw_contains_casefold(type, "cpu"))
			continue;
		value = hw_device_number(root, entry->d_name, "temp", -40000, 200000);
		if (value >= -40000 &&
		    hw_discovery_remember(backend, HW_DISCOVERY_THERMAL, root,
					  entry->d_name, "temp") == 0) {
			cpu->temperature_millic = (int)value;
			remembered = 1;
		}
		break;
	}
	(void)closedir(directory);
	if (!remembered)
		hw_discovery_mark_missing(backend, HW_DISCOVERY_THERMAL);
}

static int cpu_voltage_label(const char *name)
{
	return hw_contains_casefold(name, "vdd-cpu") ||
		hw_contains_casefold(name, "vdd_cpu") ||
		hw_contains_casefold(name, "cpu vcore") ||
		hw_contains_casefold(name, "cpu voltage") ||
		strcmp(name, "CPU") == 0 || strcmp(name, "cpu") == 0;
}

static int64_t regulator_voltage(struct hardware_backend *backend)
{
	char root[PATH_MAX];
	char path[PATH_MAX];
	char device[64];
	char name[128];
	struct dirent *entry;
	DIR *directory;
	unsigned int seen = 0;
	int64_t value = -1;

	if (hw_path(root, sizeof(root), backend, "/sys/class/regulator") != 0)
		return -1;
	if (hw_discovery_cached_device(backend, HW_DISCOVERY_REGULATOR, root,
				       device, sizeof(device), NULL, 0) == 0) {
		if (hw_device_path(path, sizeof(path), root, device, "name") == 0 &&
		    hw_read_text(path, name, sizeof(name)) == 0 &&
		    cpu_voltage_label(name) &&
		    (value = hw_device_number(root, device, "microvolts", 1,
					      INT_MAX)) > 0)
			return value;
		hw_discovery_forget(backend, HW_DISCOVERY_REGULATOR);
	}
	if (!hw_discovery_should_scan(backend, HW_DISCOVERY_REGULATOR))
		return -1;
	directory = hw_discovery_open_directory(backend, root);
	if (directory == NULL) {
		hw_discovery_mark_missing(backend, HW_DISCOVERY_REGULATOR);
		return -1;
	}
	while (seen++ < HW_DIRECTORY_ENTRY_MAX &&
	       (entry = readdir(directory)) != NULL) {
		if (!hw_valid_component(entry->d_name) ||
		    hw_device_path(path, sizeof(path), root, entry->d_name, "name") != 0 ||
		    hw_read_text(path, name, sizeof(name)) != 0 ||
		    !cpu_voltage_label(name))
			continue;
		value = hw_device_number(root, entry->d_name, "microvolts", 1,
					 INT_MAX);
		if (value > 0 &&
		    hw_discovery_remember(backend, HW_DISCOVERY_REGULATOR, root,
					  entry->d_name, "microvolts") == 0)
			break;
		value = -1;
	}
	(void)closedir(directory);
	if (value < 0)
		hw_discovery_mark_missing(backend, HW_DISCOVERY_REGULATOR);
	return value;
}

static int64_t hwmon_voltage(struct hardware_backend *backend)
{
	char root[PATH_MAX];
	char device[64];
	char input_name[32];
	struct dirent *entry;
	DIR *directory;
	unsigned int seen = 0;
	int64_t result = -1;

	if (hw_path(root, sizeof(root), backend, "/sys/class/hwmon") != 0)
		return -1;
	if (hw_discovery_cached_device(backend, HW_DISCOVERY_HWMON, root,
				       device, sizeof(device), input_name,
				       sizeof(input_name)) == 0) {
		unsigned int channel;
		char label_name[32];
		char path[PATH_MAX];
		char label[128];
		int64_t millivolts;

		if (sscanf(input_name, "in%u_input", &channel) == 1 && channel < 32) {
			(void)snprintf(label_name, sizeof(label_name), "in%u_label", channel);
			if (hw_device_path(path, sizeof(path), root, device, label_name) == 0 &&
			    hw_read_text(path, label, sizeof(label)) == 0 &&
			    cpu_voltage_label(label) &&
			    (millivolts = hw_device_number(root, device, input_name, 1,
							100000)) > 0)
				return millivolts * 1000;
		}
		hw_discovery_forget(backend, HW_DISCOVERY_HWMON);
	}
	if (!hw_discovery_should_scan(backend, HW_DISCOVERY_HWMON))
		return -1;
	directory = hw_discovery_open_directory(backend, root);
	if (directory == NULL) {
		hw_discovery_mark_missing(backend, HW_DISCOVERY_HWMON);
		return -1;
	}
	while (seen++ < HW_DIRECTORY_ENTRY_MAX &&
	       (entry = readdir(directory)) != NULL && result < 0) {
		if (!hw_valid_component(entry->d_name))
			continue;
		for (unsigned int channel = 0; channel < 32; ++channel) {
			char label_name[32];
			char input_name[32];
			char path[PATH_MAX];
			char label[128];
			int64_t millivolts;

			(void)snprintf(label_name, sizeof(label_name), "in%u_label", channel);
			if (hw_device_path(path, sizeof(path), root, entry->d_name,
					   label_name) != 0 ||
			    hw_read_text(path, label, sizeof(label)) != 0 ||
			    !cpu_voltage_label(label))
				continue;
			(void)snprintf(input_name, sizeof(input_name), "in%u_input", channel);
			millivolts = hw_device_number(root, entry->d_name, input_name,
						     1, 100000);
			if (millivolts > 0 &&
			    hw_discovery_remember(backend, HW_DISCOVERY_HWMON, root,
						  entry->d_name, input_name) == 0)
				result = millivolts * 1000;
			break;
		}
	}
	(void)closedir(directory);
	if (result < 0)
		hw_discovery_mark_missing(backend, HW_DISCOVERY_HWMON);
	return result;
}

void hw_refresh_cpu(struct hardware_backend *backend,
		    struct hardware_cpu *cpu)
{
	refresh_cpu_basic(backend, cpu);
	refresh_cpu_temperature(backend, cpu);
	cpu->voltage_uv = regulator_voltage(backend);
	if (cpu->voltage_uv > 0)
		cpu->voltage_source = HARDWARE_VOLTAGE_MEASURED_REGULATOR;
	else if ((cpu->voltage_uv = hwmon_voltage(backend)) > 0)
		cpu->voltage_source = HARDWARE_VOLTAGE_MEASURED_HWMON;
}

void hw_refresh_memory(const struct hardware_backend *backend,
		       struct hardware_memory *memory)
{
	char path[PATH_MAX];
	char text[HW_TEXT_LARGE];
	char *line;
	char *save;
	int64_t total = -1, available = -1, cached = -1, reclaim = -1;
	int64_t swap_total = -1, swap_free = -1;
	unsigned int lines = 0;

	if (hw_path(path, sizeof(path), backend, "/proc/meminfo") != 0 ||
	    hw_read_text(path, text, sizeof(text)) != 0)
		return;
	for (line = strtok_r(text, "\n", &save); line != NULL && lines++ < 256;
	     line = strtok_r(NULL, "\n", &save)) {
		char key[64];
		int64_t value;

		if (sscanf(line, "%63[^:]: %" SCNd64 " kB", key, &value) != 2 ||
		    value < 0 || value > INT64_MAX / 1024)
			continue;
		value *= 1024;
		if (strcmp(key, "MemTotal") == 0)
			total = value;
		else if (strcmp(key, "MemAvailable") == 0)
			available = value;
		else if (strcmp(key, "Cached") == 0)
			cached = value;
		else if (strcmp(key, "SReclaimable") == 0)
			reclaim = value;
		else if (strcmp(key, "SwapTotal") == 0)
			swap_total = value;
		else if (strcmp(key, "SwapFree") == 0)
			swap_free = value;
	}
	memory->total_bytes = total;
	memory->available_bytes = available;
	if (total >= 0 && available >= 0 && available <= total)
		memory->used_bytes = total - available;
	if (cached >= 0 && reclaim >= 0 && cached <= INT64_MAX - reclaim)
		memory->cache_bytes = cached + reclaim;
	memory->swap_total_bytes = swap_total;
	if (swap_total >= 0 && swap_free >= 0 && swap_free <= swap_total)
		memory->swap_used_bytes = swap_total - swap_free;
}

static int64_t blocks_to_bytes(uint64_t blocks, uint64_t block_size)
{
	return block_size != 0 && blocks <= (uint64_t)INT64_MAX / block_size ?
		(int64_t)(blocks * block_size) : -1;
}

void hw_refresh_storage(const struct hardware_backend *backend,
			struct hardware_storage *storage)
{
	struct statvfs info;
	int64_t total;
	int64_t free_bytes;

	if (statvfs(backend->storage_path, &info) != 0)
		return;
	total = blocks_to_bytes(info.f_blocks, info.f_frsize);
	free_bytes = blocks_to_bytes(info.f_bfree, info.f_frsize);
	storage->total_bytes = total;
	storage->available_bytes = blocks_to_bytes(info.f_bavail, info.f_frsize);
	if (total >= 0 && free_bytes >= 0 && free_bytes <= total)
		storage->used_bytes = total - free_bytes;
	(void)snprintf(storage->mountpoint, sizeof(storage->mountpoint), "%.127s",
		       backend->storage_path);
}
