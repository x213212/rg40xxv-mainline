#define _POSIX_C_SOURCE 200809L

#include "ui.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

static const char *core_for_system(const char *system)
{
	if (strcasecmp(system, "GBA") == 0 || strcasecmp(system, "GB") == 0 ||
	    strcasecmp(system, "GBC") == 0)
		return "mGBA";
	if (strcasecmp(system, "FC") == 0 || strcasecmp(system, "FDS") == 0)
		return "FCEUmm";
	if (strcasecmp(system, "SFC") == 0)
		return "Snes9x";
	if (strcasecmp(system, "MD") == 0 || strcasecmp(system, "SMS") == 0 ||
	    strcasecmp(system, "GG") == 0)
		return "Genesis Plus GX";
	if (strcasecmp(system, "PS") == 0)
		return "PCSX-ReARMed";
	if (strcasecmp(system, "N64") == 0)
		return "Mupen64Plus";
	if (strcasecmp(system, "NDS") == 0)
		return "standalone:drastic";
	if (strcasecmp(system, "PSP") == 0)
		return "standalone:ppsspp";
	if (strcasecmp(system, "EASYRPG") == 0)
		return "libretro:easyrpg";
	if (strcasecmp(system, "DOS") == 0)
		return "libretro:dosbox_pure";
	if (strcasecmp(system, "PORTS") == 0 || strcasecmp(system, "APPS") == 0)
		return "native";
	if (strcasecmp(system, "PS2") == 0)
		return "experimental:unavailable";
	return "RetroArch";
}

static const char *frontend_for_system(const char *system, const char *core)
{
	if (strcasecmp(system, "PSP") == 0)
		return "PPSSPP standalone";
	if (strcasecmp(system, "NDS") == 0)
		return "DraStic";
	if (strcmp(core, "native") == 0)
		return "Native port";
	return "RetroArch";
}

static const char *runtime_for_system(const char *system, const char *core)
{
	if (strcasecmp(system, "PSP") == 0)
		return "PPSSPP";
	if (strcasecmp(system, "NDS") == 0)
		return "ARMhf";
	if (strcmp(core, "native") == 0)
		return "AArch64/Linux";
	return core;
}

static const char *fallback_for_system(const char *system)
{
	if (strcasecmp(system, "PSP") == 0)
		return "libretro:ppsspp";
	if (strcasecmp(system, "NDS") == 0)
		return "libretro:melonds (ARMhf)";
	return "";
}

static bool is_rom_file(const char *name)
{
	static const char *const extensions[] = {
		".zip", ".7z", ".rar", ".nes", ".fds", ".sfc", ".smc",
		".gba", ".gb", ".gbc", ".nds", ".n64", ".z64", ".v64",
		".md", ".gen", ".sms", ".gg", ".32x", ".cue", ".chd",
		".iso", ".pbp", ".cso", ".gdi", ".bin", ".a26", ".a52",
		".a78", ".atr", ".xex", ".d64", ".adf", ".lha", ".hdf",
		".rom", ".pce", ".ngp", ".ngc", ".ws", ".wsc", ".col",
		".vec", ".wad", ".pak", ".m3u", ".sh", ".love", ".elf",
		".exe", ".vb", ".p8", ".mgw", ".dosz", ".jar", ".lnx",
		".d88", ".tap", ".tzx", ".prg", ".crt", ".scummvm",
	};
	const char *extension = strrchr(name, '.');

	if (extension == NULL)
		return false;
	for (size_t i = 0; i < sizeof(extensions) / sizeof(extensions[0]); ++i) {
		if (strcasecmp(extension, extensions[i]) == 0)
			return true;
	}
	return false;
}

static int grow_games(struct catalog_state *catalog)
{
	if (catalog->game_count < catalog->game_capacity)
		return 0;
	size_t capacity = catalog->game_capacity == 0 ? 256 :
		catalog->game_capacity * 2;
	struct game_entry *games = realloc(catalog->games,
					 capacity * sizeof(*games));

	if (games == NULL)
		return -1;
	catalog->games = games;
	catalog->game_capacity = capacity;
	return 0;
}

static int add_unique(char ***values, size_t *count, size_t *capacity,
		      const char *value)
{
	for (size_t i = 0; i < *count; ++i) {
		if (strcmp((*values)[i], value) == 0)
			return 0;
	}
	if (*count >= *capacity) {
		size_t next = *capacity == 0 ? 16 : *capacity * 2;
		char **grown = realloc(*values, next * sizeof(*grown));

		if (grown == NULL)
			return -1;
		*values = grown;
		*capacity = next;
	}
	(*values)[*count] = strdup(value);
	if ((*values)[*count] == NULL)
		return -1;
	++*count;
	return 0;
}

static char *title_from_name(const char *name)
{
	const char *extension = strrchr(name, '.');
	size_t length = extension == NULL ? strlen(name) : (size_t)(extension - name);
	char *title = malloc(length + 1);

	if (title != NULL) {
		memcpy(title, name, length);
		title[length] = '\0';
	}
	return title;
}

static int add_game(struct ui *ui, const char *path, const char *name,
		    const char *system_dir, const char *system, time_t modified)
{
	struct catalog_state *catalog = &ui->catalog;
	struct game_entry game = { 0 };
	char cover[PATH_MAX];
	const struct platform_route *route = platform_route_find(ui, system);
	const char *core = route == NULL ? core_for_system(system) : route->primary;
	const char *frontend = route == NULL ? frontend_for_system(system, core) :
		platform_route_frontend(core);
	const char *runtime = route == NULL ? runtime_for_system(system, core) :
		platform_route_runtime(core);
	const char *fallback = route == NULL ? fallback_for_system(system) :
		route->fallback;
	int cover_length;

	game.title = title_from_name(name);
	if (game.title == NULL)
		goto fail;
	cover[0] = '\0';
	for (size_t i = 0; i < 3; ++i) {
		static const char *const extensions[] = { ".jpg", ".png", ".bmp" };
		struct stat cover_status;

		cover_length = snprintf(cover, sizeof(cover), "%s/Imgs/%s%s",
					system_dir, game.title, extensions[i]);
		if (cover_length < 0 || (size_t)cover_length >= sizeof(cover))
			goto fail;
		if (lstat(cover, &cover_status) == 0 && S_ISREG(cover_status.st_mode))
			break;
		if (i == 2) {
			cover_length = snprintf(cover, sizeof(cover), "%s/Imgs/%s.png",
						system_dir, game.title);
			if (cover_length < 0 ||
			    (size_t)cover_length >= sizeof(cover))
				goto fail;
		}
	}
	game.path = realpath(path, NULL);
	if (game.path == NULL)
		game.path = strdup(path);
	game.cover_path = strdup(cover);
	game.system = strdup(system);
	game.system_label = strdup(route == NULL ? system : route->label_zh_tw);
	game.core = strdup(core);
	game.frontend = strdup(frontend);
	game.runtime = strdup(runtime);
	game.fallback = strdup(fallback);
	game.playable = route == NULL || route->playable;
	game.modified = modified;
	if (game.path == NULL || game.cover_path == NULL || game.system == NULL ||
	    game.system_label == NULL ||
	    game.core == NULL || game.frontend == NULL || game.runtime == NULL ||
	    game.fallback == NULL || grow_games(catalog) != 0 ||
	    add_unique(&catalog->systems, &catalog->system_count,
		       &catalog->system_capacity, system) != 0 ||
	    add_unique(&catalog->cores, &catalog->core_count,
		       &catalog->core_capacity, core) != 0)
		goto fail;
	catalog->games[catalog->game_count++] = game;
	return 0;

fail:
	free(game.title);
	free(game.path);
	free(game.cover_path);
	free(game.system);
	free(game.system_label);
	free(game.core);
	free(game.frontend);
	free(game.runtime);
	free(game.fallback);
	return -1;
}

static int scan_directory(struct ui *ui, const char *directory,
			  const char *system_dir, const char *system, int depth)
{
	DIR *stream = opendir(directory);
	struct dirent *entry;
	int result = 0;

	if (stream == NULL)
		return 0;
	while (result == 0 && (entry = readdir(stream)) != NULL) {
		char path[PATH_MAX];
		struct stat status;
		int length;

		if (entry->d_name[0] == '.' || strcasecmp(entry->d_name, "Imgs") == 0)
			continue;
		length = snprintf(path, sizeof(path), "%s/%s", directory,
				  entry->d_name);
		if (length < 0 || (size_t)length >= sizeof(path) ||
		    lstat(path, &status) != 0)
			continue;
		if (S_ISDIR(status.st_mode) && depth == 0 &&
		    strcasecmp(system, "EASYRPG") == 0)
			result = add_game(ui, path, entry->d_name, system_dir, system,
					  status.st_mtime);
		else if (S_ISDIR(status.st_mode) && depth == 0 &&
			 strcasecmp(system, "APPS") == 0) {
			char launcher[PATH_MAX];
			struct stat launcher_status;
			int launcher_length = snprintf(launcher, sizeof(launcher),
						       "%s/launch.sh", path);

			if (launcher_length >= 0 &&
			    (size_t)launcher_length < sizeof(launcher) &&
			    lstat(launcher, &launcher_status) == 0 &&
			    S_ISREG(launcher_status.st_mode))
				result = add_game(ui, launcher, entry->d_name,
						  system_dir, system,
						  launcher_status.st_mtime);
		}
		else if (S_ISDIR(status.st_mode) && depth < 8 &&
			 strcasecmp(system, "PORTS") != 0)
			result = scan_directory(ui, path, system_dir, system, depth + 1);
		else if (S_ISREG(status.st_mode) && is_rom_file(entry->d_name) &&
			 (strcasecmp(system, "PORTS") != 0 ||
			  strcasecmp(strrchr(entry->d_name, '.'), ".sh") == 0))
			result = add_game(ui, path, entry->d_name, system_dir, system,
					  status.st_mtime);
	}
	closedir(stream);
	return result;
}

static int compare_text(const void *left, const void *right)
{
	const char *const *a = left;
	const char *const *b = right;

	return strcmp(*a, *b);
}

static int compare_game(const void *left, const void *right)
{
	const struct game_entry *a = left;
	const struct game_entry *b = right;
	int system = strcmp(a->system, b->system);

	return system != 0 ? system : strcmp(a->title, b->title);
}

int catalog_scan(struct ui *ui, const char *rom_root)
{
	DIR *root;
	struct dirent *entry;
	int result = 0;

	catalog_destroy(ui);
	(void)snprintf(ui->catalog.rom_root, sizeof(ui->catalog.rom_root), "%s",
		       rom_root);
	root = opendir(rom_root);
	if (root == NULL) {
		catalog_apply_filters(ui);
		return -1;
	}
	while (result == 0 && (entry = readdir(root)) != NULL) {
		char path[PATH_MAX];
		struct stat status;
		int length;

		if (entry->d_name[0] == '.' || strcasecmp(entry->d_name, "Imgs") == 0)
			continue;
		length = snprintf(path, sizeof(path), "%s/%s", rom_root, entry->d_name);
		if (length < 0 || (size_t)length >= sizeof(path) ||
		    lstat(path, &status) != 0 || !S_ISDIR(status.st_mode))
			continue;
		result = scan_directory(ui, path, path, entry->d_name, 0);
	}
	closedir(root);
	if (result != 0) {
		catalog_destroy(ui);
		return -1;
	}
	qsort(ui->catalog.games, ui->catalog.game_count,
	      sizeof(*ui->catalog.games), compare_game);
	qsort(ui->catalog.systems, ui->catalog.system_count,
	      sizeof(*ui->catalog.systems), compare_text);
	qsort(ui->catalog.cores, ui->catalog.core_count,
	      sizeof(*ui->catalog.cores), compare_text);
	catalog_apply_filters(ui);
	return 0;
}

void catalog_destroy(struct ui *ui)
{
	struct catalog_state *catalog = &ui->catalog;

	for (size_t i = 0; i < catalog->game_count; ++i) {
		free(catalog->games[i].title);
		free(catalog->games[i].path);
		free(catalog->games[i].cover_path);
		free(catalog->games[i].system);
		free(catalog->games[i].system_label);
		free(catalog->games[i].core);
		free(catalog->games[i].frontend);
		free(catalog->games[i].runtime);
		free(catalog->games[i].fallback);
	}
	for (size_t i = 0; i < catalog->system_count; ++i)
		free(catalog->systems[i]);
	for (size_t i = 0; i < catalog->core_count; ++i)
		free(catalog->cores[i]);
	free(catalog->games);
	free(catalog->visible);
	free(catalog->systems);
	free(catalog->cores);
	memset(catalog, 0, sizeof(*catalog));
}
