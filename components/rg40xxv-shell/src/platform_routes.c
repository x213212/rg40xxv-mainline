#define _POSIX_C_SOURCE 200809L

#include "ui.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define RPG_CAPABILITY_DEFAULT \
	"/opt/rg40xxv/rpgmaker/runtime/admission.env"

#define CAP_SCHEMA (UINT64_C(1) << 0)
#define CAP_ROUTE (UINT64_C(1) << 1)
#define CAP_ARCH (UINT64_C(1) << 2)
#define CAP_NW_VERSION (UINT64_C(1) << 3)
#define CAP_NW_SHA (UINT64_C(1) << 4)
#define CAP_CAGE_VERSION (UINT64_C(1) << 5)
#define CAP_CAGE_SHA (UINT64_C(1) << 6)
#define CAP_CAGE_XWAYLAND (UINT64_C(1) << 7)
#define CAP_CAGE_PRIVILEGE (UINT64_C(1) << 8)
#define CAP_DISPLAY (UINT64_C(1) << 9)
#define CAP_PAYLOAD (UINT64_C(1) << 10)
#define CAP_CLOSURE (UINT64_C(1) << 11)
#define CAP_NW_QEMU (UINT64_C(1) << 12)
#define CAP_CAGE_QEMU (UINT64_C(1) << 13)
#define CAP_SEATD_QEMU (UINT64_C(1) << 14)
#define CAP_LOADER (UINT64_C(1) << 15)
#define CAP_QEMU_RENDER (UINT64_C(1) << 16)
#define CAP_QEMU_RENDER_EVIDENCE (UINT64_C(1) << 17)
#define CAP_QEMU_MV (UINT64_C(1) << 18)
#define CAP_QEMU_MZ (UINT64_C(1) << 19)
#define CAP_QEMU_REAL_EVIDENCE (UINT64_C(1) << 20)
#define CAP_DEVICE (UINT64_C(1) << 21)
#define CAP_DEVICE_DISPLAY (UINT64_C(1) << 22)
#define CAP_DEVICE_EGL (UINT64_C(1) << 23)
#define CAP_DEVICE_FULLSCREEN (UINT64_C(1) << 24)
#define CAP_DEVICE_MV (UINT64_C(1) << 25)
#define CAP_DEVICE_MZ (UINT64_C(1) << 26)
#define CAP_DEVICE_AUDIO (UINT64_C(1) << 27)
#define CAP_DEVICE_INPUT (UINT64_C(1) << 28)
#define CAP_DEVICE_EXIT (UINT64_C(1) << 29)
#define CAP_DEVICE_SAVE (UINT64_C(1) << 30)
#define CAP_DEVICE_MEMORY (UINT64_C(1) << 31)
#define CAP_DEVICE_EVIDENCE (UINT64_C(1) << 32)
#define CAP_TRUTH (UINT64_C(1) << 33)
#define CAP_REQUIRED ((UINT64_C(1) << 34) - UINT64_C(1))

enum capability_schema {
	SCHEMA_NONE = 0,
	SCHEMA_RMUT_V3,
};

enum rpg_truth_level {
	RPG_TRUTH_NONE = 0,
	RPG_TRUTH_LOADER,
	RPG_TRUTH_QEMU_RENDER,
	RPG_TRUTH_QEMU_REAL,
	RPG_TRUTH_DEVICE,
};

struct rpg_capability {
	bool launchable;
	enum rpg_truth_level truth_level;
};

static bool is_sha256(const char *value)
{
	if (strlen(value) != 64)
		return false;
	for (size_t i = 0; i < 64; ++i) {
		if (!isdigit((unsigned char)value[i]) &&
		    (value[i] < 'a' || value[i] > 'f'))
			return false;
	}
	return true;
}

static bool capability_field(const char *key, const char *value,
				     enum capability_schema *schema,
				     enum rpg_truth_level *truth_level,
				     uint64_t *field, bool *passed)
{
	static const struct {
		const char *key;
		const char *value;
		uint64_t field;
	} rmut_exact[] = {
		{ "route", "standalone:rmut-nwjs-aarch64", CAP_ROUTE },
		{ "architecture", "aarch64", CAP_ARCH },
		{ "nwjs_version", "0.60.1", CAP_NW_VERSION },
		{ "cage_version", "0.1.4", CAP_CAGE_VERSION },
		{ "cage_xwayland", "DISABLED", CAP_CAGE_XWAYLAND },
		{ "cage_privilege_model", "rg40xxv-root-session", CAP_CAGE_PRIVILEGE },
		{ "display_backend", "wayland-drm", CAP_DISPLAY },
		{ "runtime_payload", "PASS", CAP_PAYLOAD },
		{ "dependency_closure", "PASS", CAP_CLOSURE },
		{ "nwjs_qemu_loader_smoke", "PASS", CAP_NW_QEMU },
		{ "cage_qemu_loader_smoke", "PASS", CAP_CAGE_QEMU },
		{ "seatd_qemu_loader_smoke", "PASS", CAP_SEATD_QEMU },
		{ "loader_validation", "PASS", CAP_LOADER },
	};
	static const struct {
		const char *key;
		uint64_t field;
	} rmut_status[] = {
		{ "qemu_render_validation", CAP_QEMU_RENDER },
		{ "qemu_real_mv_interactive", CAP_QEMU_MV },
		{ "qemu_real_mz_interactive", CAP_QEMU_MZ },
		{ "device_validation", CAP_DEVICE },
		{ "display_smoke", CAP_DEVICE_DISPLAY },
		{ "egl_panfrost_smoke", CAP_DEVICE_EGL },
		{ "fullscreen_smoke", CAP_DEVICE_FULLSCREEN },
		{ "device_mv_interactive", CAP_DEVICE_MV },
		{ "device_mz_interactive", CAP_DEVICE_MZ },
		{ "audio_smoke", CAP_DEVICE_AUDIO },
		{ "h700_gamepad_smoke", CAP_DEVICE_INPUT },
		{ "menu_start_supervisor_smoke", CAP_DEVICE_EXIT },
		{ "save_restart_smoke", CAP_DEVICE_SAVE },
		{ "peak_pss_under_640_mib", CAP_DEVICE_MEMORY },
	};
	static const struct {
		const char *key;
		uint64_t field;
	} rmut_evidence[] = {
		{ "qemu_render_evidence_sha256", CAP_QEMU_RENDER_EVIDENCE },
		{ "qemu_real_evidence_sha256", CAP_QEMU_REAL_EVIDENCE },
		{ "device_evidence_sha256", CAP_DEVICE_EVIDENCE },
	};
	*passed = false;
	*field = 0;
	if (strcmp(key, "schema") == 0) {
		if (*schema != SCHEMA_NONE)
			return false;
		if (strcmp(value, "rg40xxv-rmut-runtime-admission-v3") == 0) {
			*schema = SCHEMA_RMUT_V3;
			*field = CAP_SCHEMA;
		} else {
			return false;
		}
		*passed = true;
		return true;
	}
	/* The schema line must come first so the right table is selected. */
	if (*schema == SCHEMA_NONE)
		return false;
	if (strcmp(key, "runtime_sha256") == 0) {
		*field = CAP_NW_SHA;
		*passed = is_sha256(value);
		return *passed;
	}
	if (strcmp(key, "cage_sha256") == 0) {
		*field = CAP_CAGE_SHA;
		*passed = is_sha256(value);
		return *passed;
	}
	if (strcmp(key, "truth_level") == 0) {
		*field = CAP_TRUTH;
		if (strcmp(value, "LOADER_PASS") == 0)
			*truth_level = RPG_TRUTH_LOADER;
		else if (strcmp(value, "QEMU_RENDER_PASS") == 0)
			*truth_level = RPG_TRUTH_QEMU_RENDER;
		else if (strcmp(value, "QEMU_REAL_MV_MZ_INTERACTIVE_PASS") == 0)
			*truth_level = RPG_TRUTH_QEMU_REAL;
		else if (strcmp(value, "DEVICE_PASS") == 0)
			*truth_level = RPG_TRUTH_DEVICE;
		else
			return false;
		*passed = *truth_level == RPG_TRUTH_DEVICE;
		return true;
	}
	for (size_t i = 0; i < sizeof(rmut_exact) / sizeof(rmut_exact[0]); ++i) {
		if (strcmp(key, rmut_exact[i].key) == 0) {
			*field = rmut_exact[i].field;
			*passed = strcmp(value, rmut_exact[i].value) == 0;
			return *passed;
		}
	}
	for (size_t i = 0; i < sizeof(rmut_status) / sizeof(rmut_status[0]); ++i) {
		if (strcmp(key, rmut_status[i].key) == 0) {
			*field = rmut_status[i].field;
			*passed = strcmp(value, "PASS") == 0;
			return *passed || strcmp(value, "UNVERIFIED") == 0;
		}
	}
	for (size_t i = 0; i < sizeof(rmut_evidence) / sizeof(rmut_evidence[0]); ++i) {
		if (strcmp(key, rmut_evidence[i].key) == 0) {
			*field = rmut_evidence[i].field;
			*passed = is_sha256(value);
			return *passed || strcmp(value, "UNVERIFIED") == 0;
		}
	}
	return false;
}

static struct rpg_capability rpg_capability_load(void)
{
	const char *path = getenv("RG40XXV_UI_RPGMAKER_CAPABILITY");
	struct stat status;
	FILE *stream;
	char line[512];
	uint64_t seen = 0;
	uint64_t passed = 0;
	enum capability_schema schema = SCHEMA_NONE;
	enum rpg_truth_level truth_level = RPG_TRUTH_NONE;
	bool valid = true;
	struct rpg_capability capability = { false, RPG_TRUTH_NONE };

	if (path == NULL || path[0] == '\0')
		path = RPG_CAPABILITY_DEFAULT;
	if (path[0] != '/' || lstat(path, &status) != 0 ||
	    !S_ISREG(status.st_mode) || (status.st_mode & 022) != 0 ||
	    status.st_size <= 0 || status.st_size > 16384)
		return capability;
	stream = fopen(path, "r");
	if (stream == NULL)
		return capability;
	while (valid && fgets(line, sizeof(line), stream) != NULL) {
		char *equals;
		char *newline;
		uint64_t field;
		bool field_passed;

		newline = strchr(line, '\n');
		if (newline == NULL && !feof(stream)) {
			valid = false;
			break;
		}
		if (newline != NULL)
			*newline = '\0';
		if (line[0] == '\0' || line[0] == '#')
			continue;
		if (strchr(line, '\r') != NULL) {
			valid = false;
			break;
		}
		equals = strchr(line, '=');
		if (equals == NULL || equals == line || equals[1] == '\0') {
			valid = false;
			break;
		}
		*equals = '\0';
		valid = capability_field(line, equals + 1, &schema, &truth_level, &field,
					 &field_passed);
		if (field != 0 && (seen & field) != 0)
			valid = false;
		seen |= field;
		if (field_passed)
			passed |= field;
	}
	if (ferror(stream) != 0)
		valid = false;
	fclose(stream);
	valid = valid && schema == SCHEMA_RMUT_V3 && seen == CAP_REQUIRED;
	capability.launchable = valid && passed == CAP_REQUIRED &&
		truth_level == RPG_TRUTH_DEVICE;
	capability.truth_level = valid ? truth_level : RPG_TRUTH_NONE;
	return capability;
}

static const char *rpg_capability_suffix(struct rpg_capability capability)
{
	if (capability.launchable)
		return "";
	switch (capability.truth_level) {
	case RPG_TRUTH_LOADER:
		return "（僅 Loader 診斷）";
	case RPG_TRUTH_QEMU_RENDER:
		return "（僅 QEMU 渲染診斷）";
	case RPG_TRUTH_QEMU_REAL:
		return "（僅 QEMU 遊戲診斷）";
	case RPG_TRUTH_DEVICE:
	case RPG_TRUTH_NONE:
	default:
		return "（執行器未就緒）";
	}
}

static bool is_rpg_web_system(const char *system)
{
	return strcmp(system, "RPGMV") == 0 || strcmp(system, "RPGMZ") == 0;
}

static int extract_after(const char *line, const char *marker,
			 char *buffer, size_t size)
{
	const char *start = strstr(line, marker);
	const char *end;
	size_t length;

	if (start == NULL)
		return -1;
	start += strlen(marker);
	end = strchr(start, '"');
	if (end == NULL)
		return -1;
	length = (size_t)(end - start);
	if (length >= size)
		return -1;
	memcpy(buffer, start, length);
	buffer[length] = '\0';
	return 0;
}

static int grow_routes(struct platform_routes *routes)
{
	if (routes->count < routes->capacity)
		return 0;
	size_t capacity = routes->capacity == 0 ? 32 : routes->capacity * 2;
	struct platform_route *items = realloc(routes->items,
						     capacity * sizeof(*items));

	if (items == NULL)
		return -1;
	routes->items = items;
	routes->capacity = capacity;
	return 0;
}

static int add_route(struct platform_routes *routes, const char *line,
		     struct rpg_capability rpg_capability)
{
	struct platform_route route = { 0 };
	char system[64];
	char label[128];
	char primary[96];
	char fallback[96] = "";
	const char *second;
	const char *quote;
	const char *routes_end;

	if (extract_after(line, "    \"", system, sizeof(system)) != 0 ||
	    extract_after(line, "\"label_zh_TW\": \"", label, sizeof(label)) != 0 ||
	    extract_after(line, "\"routes\": [\"", primary, sizeof(primary)) != 0)
		return 0;
	if (is_rpg_web_system(system)) {
		const char *suffix = rpg_capability_suffix(rpg_capability);
		size_t used = strlen(label);
		size_t suffix_size = strlen(suffix) + 1;

		if (used + suffix_size > sizeof(label))
			return -1;
		memcpy(label + used, suffix, suffix_size);
	}
	second = strstr(line, "\"routes\": [\"");
	second = second == NULL ? NULL : strchr(second + 12, '"');
	routes_end = second == NULL ? NULL : strchr(second + 1, ']');
	second = second == NULL ? NULL : strchr(second + 1, ',');
	quote = second == NULL || routes_end == NULL || second >= routes_end ?
		NULL : strchr(second, '"');
	if (quote != NULL && quote < routes_end) {
		const char *end = strchr(quote + 1, '"');

		if (end != NULL && end < routes_end &&
		    (size_t)(end - quote - 1) < sizeof(fallback)) {
			memcpy(fallback, quote + 1, (size_t)(end - quote - 1));
			fallback[end - quote - 1] = '\0';
		}
	}
	if (grow_routes(routes) != 0)
		return -1;
	route.system = strdup(system);
	route.label_zh_tw = strdup(label);
	route.primary = strdup(primary);
	route.fallback = strdup(fallback);
	route.playable = strstr(line, "\"playable\": false") == NULL ||
		(rpg_capability.launchable && is_rpg_web_system(system));
	if (route.system == NULL || route.label_zh_tw == NULL ||
	    route.primary == NULL || route.fallback == NULL) {
		free(route.system);
		free(route.label_zh_tw);
		free(route.primary);
		free(route.fallback);
		return -1;
	}
	routes->items[routes->count++] = route;
	return 0;
}

int platform_routes_load(struct ui *ui, const char *path)
{
	FILE *stream;
	char line[1024];
	int result = 0;
	struct rpg_capability rpg_capability = rpg_capability_load();

	platform_routes_destroy(ui);
	ui->routes.rpg_truth_level = (unsigned int)rpg_capability.truth_level;
	ui->routes.rpg_launchable = rpg_capability.launchable;
	(void)snprintf(ui->routes.source_path, sizeof(ui->routes.source_path),
		       "%s", path);
	stream = fopen(path, "r");
	if (stream == NULL)
		return -1;
	while (result == 0 && fgets(line, sizeof(line), stream) != NULL) {
		if (strstr(line, "\"label_zh_TW\"") != NULL &&
		    strstr(line, "\"routes\"") != NULL)
			result = add_route(&ui->routes, line, rpg_capability);
	}
	fclose(stream);
	if (result != 0)
		platform_routes_destroy(ui);
	return result;
}

void platform_routes_destroy(struct ui *ui)
{
	for (size_t i = 0; i < ui->routes.count; ++i) {
		free(ui->routes.items[i].system);
		free(ui->routes.items[i].label_zh_tw);
		free(ui->routes.items[i].primary);
		free(ui->routes.items[i].fallback);
	}
	free(ui->routes.items);
	memset(&ui->routes, 0, sizeof(ui->routes));
}

const struct platform_route *platform_route_find(const struct ui *ui,
						  const char *system)
{
	for (size_t i = 0; i < ui->routes.count; ++i) {
		if (strcmp(ui->routes.items[i].system, system) == 0)
			return &ui->routes.items[i];
	}
	return NULL;
}

const char *platform_route_frontend(const char *selector)
{
	if (strcmp(selector, "standalone:ppsspp") == 0)
		return "PPSSPP standalone";
	if (strcmp(selector, "standalone:drastic") == 0)
		return "DraStic";
	if (strncmp(selector, "standalone:", 11) == 0)
		return "Standalone";
	if (strncmp(selector, "script:", 7) == 0)
		return "Native script";
	if (strncmp(selector, "aarch64:", 8) == 0)
		return "RetroArch AArch64";
	if (strncmp(selector, "libretro:", 9) == 0)
		return "RetroArch";
	return "Unavailable";
}

const char *platform_route_runtime(const char *selector)
{
	const char *separator = strchr(selector, ':');

	if (strcmp(selector, "standalone:ppsspp") == 0)
		return "PPSSPP";
	if (strcmp(selector, "standalone:drastic") == 0)
		return "ARMhf";
	if (strncmp(selector, "script:", 7) == 0)
		return "AArch64/Linux";
	return separator == NULL ? selector : separator + 1;
}
