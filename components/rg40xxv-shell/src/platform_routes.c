#define _POSIX_C_SOURCE 200809L

#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static int add_route(struct platform_routes *routes, const char *line)
{
	struct platform_route route = { 0 };
	char system[64];
	char label[128];
	char primary[96];
	char fallback[96] = "";
	const char *second;
	const char *quote;

	if (extract_after(line, "    \"", system, sizeof(system)) != 0 ||
	    extract_after(line, "\"label_zh_TW\": \"", label, sizeof(label)) != 0 ||
	    extract_after(line, "\"routes\": [\"", primary, sizeof(primary)) != 0)
		return 0;
	second = strstr(line, "\"routes\": [\"");
	second = second == NULL ? NULL : strchr(second + 12, '"');
	second = second == NULL ? NULL : strchr(second + 1, ',');
	quote = second == NULL ? NULL : strchr(second, '"');
	if (quote != NULL) {
		const char *end = strchr(quote + 1, '"');

		if (end != NULL && (size_t)(end - quote - 1) < sizeof(fallback)) {
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
	route.playable = strstr(line, "\"playable\": false") == NULL;
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

	platform_routes_destroy(ui);
	(void)snprintf(ui->routes.source_path, sizeof(ui->routes.source_path),
		       "%s", path);
	stream = fopen(path, "r");
	if (stream == NULL)
		return -1;
	while (result == 0 && fgets(line, sizeof(line), stream) != NULL) {
		if (strstr(line, "\"label_zh_TW\"") != NULL &&
		    strstr(line, "\"routes\"") != NULL)
			result = add_route(&ui->routes, line);
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
