#define _POSIX_C_SOURCE 200809L

#include "ui.h"

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

struct scan_budget {
	size_t entries;
	size_t max_entries;
	uint64_t deadline_ns;
};

struct scan_source {
	const char *root;
	const char *preferred_root;
};

struct cover_index_item {
	char *stem;
	char *path;
	int preference;
};

struct cover_index {
	struct cover_index_item *items;
	size_t count;
	size_t capacity;
};

static uint64_t monotonic_ns(void)
{
	struct timespec value;

	if (clock_gettime(CLOCK_MONOTONIC, &value) != 0)
		return 0;
	return (uint64_t)value.tv_sec * 1000000000ULL + (uint64_t)value.tv_nsec;
}

static bool scan_budget_expired(struct scan_budget *budget)
{
	if (budget->entries >= budget->max_entries)
		return true;
	++budget->entries;
	return budget->deadline_ns != 0U && monotonic_ns() >= budget->deadline_ns;
}

static void cover_index_destroy(struct cover_index *index)
{
	for (size_t item = 0; item < index->count; ++item) {
		free(index->items[item].stem);
		free(index->items[item].path);
	}
	free(index->items);
	memset(index, 0, sizeof(*index));
}

static int compare_cover(const void *left, const void *right)
{
	const struct cover_index_item *a = left;
	const struct cover_index_item *b = right;
	int stem = strcmp(a->stem, b->stem);

	return stem != 0 ? stem : a->preference - b->preference;
}

static int cover_preference(const char *name, size_t *stem_length)
{
	const char *extension = strrchr(name, '.');

	if (extension == NULL || extension == name)
		return -1;
	*stem_length = (size_t)(extension - name);
	if (strcasecmp(extension, ".jpg") == 0 ||
	    strcasecmp(extension, ".jpeg") == 0)
		return 0;
	if (strcasecmp(extension, ".png") == 0)
		return 1;
	if (strcasecmp(extension, ".bmp") == 0)
		return 2;
	return -1;
}

static int cover_index_add(struct cover_index *index, const char *directory,
			   const char *name)
{
	struct cover_index_item *item;
	size_t stem_length = 0U;
	int preference = cover_preference(name, &stem_length);
	int path_length;

	if (preference < 0)
		return 0;
	if (index->count >= index->capacity) {
		size_t next = index->capacity == 0U ? 64U : index->capacity * 2U;
		struct cover_index_item *grown = realloc(index->items,
			next * sizeof(*grown));

		if (grown == NULL)
			return -1;
		index->items = grown;
		index->capacity = next;
	}
	item = &index->items[index->count];
	memset(item, 0, sizeof(*item));
	item->stem = strndup(name, stem_length);
	item->path = malloc(PATH_MAX);
	item->preference = preference;
	if (item->stem == NULL || item->path == NULL)
		goto fail;
	path_length = snprintf(item->path, PATH_MAX, "%s/%s", directory, name);
	if (path_length < 0 || path_length >= PATH_MAX)
		goto fail;
	++index->count;
	return 0;

fail:
	free(item->stem);
	free(item->path);
	memset(item, 0, sizeof(*item));
	return -1;
}

static int cover_index_build(struct cover_index *index, const char *system_dir,
			     struct scan_budget *budget)
{
	char directory[PATH_MAX];
	DIR *stream;
	struct dirent *entry;
	int length = snprintf(directory, sizeof(directory), "%s/Imgs", system_dir);
	int result = 0;

	if (length < 0 || (size_t)length >= sizeof(directory))
		return -1;
	stream = opendir(directory);
	if (stream == NULL)
		return 0;
	while ((entry = readdir(stream)) != NULL) {
		if (entry->d_name[0] == '.')
			continue;
		if (scan_budget_expired(budget)) {
			result = 1;
			break;
		}
		if (cover_index_add(index, directory, entry->d_name) != 0) {
			result = -1;
			break;
		}
	}
	(void)closedir(stream);
	if (result == 0)
		qsort(index->items, index->count, sizeof(*index->items),
			compare_cover);
	return result;
}

static const char *cover_index_find(const struct cover_index *index,
				    const char *stem)
{
	size_t left = 0U;
	size_t right = index->count;

	while (left < right) {
		size_t middle = left + (right - left) / 2U;
		int order = strcmp(index->items[middle].stem, stem);

		if (order < 0)
			left = middle + 1U;
		else
			right = middle;
	}
	return left < index->count && strcmp(index->items[left].stem, stem) == 0 ?
		index->items[left].path : NULL;
}

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
	if (strcasecmp(system, "RPGMV") == 0 ||
	    strcasecmp(system, "RPGMZ") == 0)
		return "standalone:rmut-nwjs-aarch64";
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
	if (strcasecmp(system, "RPGMV") == 0 ||
	    strcasecmp(system, "RPGMZ") == 0)
		return "Standalone";
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

static const char *relative_to_root(const char *root, const char *path)
{
	size_t length;
	const char *relative;

	if (root == NULL || path == NULL || root[0] != '/' || path[0] != '/')
		return NULL;
	length = strlen(root);
	while (length > 1U && root[length - 1U] == '/')
		--length;
	if (strncmp(root, path, length) != 0 ||
	    (length > 1U && path[length] != '/'))
		return NULL;
	relative = path + length;
	while (*relative == '/')
		++relative;
	return *relative == '\0' ? NULL : relative;
}

static bool preferred_source_has_game(const struct ui *ui,
				      const struct scan_source *source,
				      const char *path, mode_t mode)
{
	const char *relative;
	char preferred_path[PATH_MAX];
	struct stat status;
	int length;

	if (source->preferred_root == NULL)
		return false;
	relative = relative_to_root(source->root, path);
	if (relative == NULL)
		return false;
	for (size_t index = 0; index < ui->catalog.game_count; ++index) {
		const char *preferred_relative = relative_to_root(
			source->preferred_root, ui->catalog.games[index].path);

		if (preferred_relative != NULL &&
		    strcasecmp(preferred_relative, relative) == 0)
			return true;
	}
	length = snprintf(preferred_path, sizeof(preferred_path), "%s/%s",
		source->preferred_root, relative);
	if (length < 0 || (size_t)length >= sizeof(preferred_path) ||
	    lstat(preferred_path, &status) != 0)
		return false;
	return (S_ISREG(mode) && S_ISREG(status.st_mode)) ||
		(S_ISDIR(mode) && S_ISDIR(status.st_mode));
}

static bool easyrpg_project_is_launchable(const char *path,
					   const struct stat *directory_status)
{
	char database[PATH_MAX];
	struct stat database_status;
	int length;

	/*
	 * EasyRPG accepts a project directory, but the launch helper deliberately
	 * rejects aliases and cross-filesystem trees.  Apply the cheap, bounded
	 * part of that contract while cataloguing so a broken directory is not
	 * advertised as a playable RPG Maker 2000/2003 title.  The launch helper
	 * remains the final fail-closed authority and performs the full tree check.
	 */
	if (directory_status == NULL || !S_ISDIR(directory_status->st_mode))
		return false;
	length = snprintf(database, sizeof(database), "%s/RPG_RT.ldb", path);
	if (length < 0 || (size_t)length >= sizeof(database) ||
	    lstat(database, &database_status) != 0)
		return false;
	return S_ISREG(database_status.st_mode) &&
		database_status.st_dev == directory_status->st_dev;
}

static const char *catalog_system_name(const struct catalog_state *catalog,
				       const char *system)
{
	for (size_t index = 0; index < catalog->system_count; ++index) {
		if (strcasecmp(catalog->systems[index], system) == 0)
			return catalog->systems[index];
	}
	return system;
}

static int add_game(struct ui *ui, const char *path, const char *name,
		    const char *system_dir, const char *system, time_t modified,
		    const struct cover_index *covers)
{
	struct catalog_state *catalog = &ui->catalog;
	struct game_entry game = { 0 };
	char cover[PATH_MAX];
	const char *system_name = catalog_system_name(catalog, system);
	const struct platform_route *route = platform_route_find(ui, system_name);
	const char *core = route == NULL ? core_for_system(system_name) :
		route->primary;
	const char *frontend = route == NULL ? frontend_for_system(system_name, core) :
		platform_route_frontend(core);
	const char *runtime = route == NULL ? runtime_for_system(system_name, core) :
		platform_route_runtime(core);
	const char *fallback = route == NULL ? fallback_for_system(system_name) :
		route->fallback;
	int cover_length;

	game.title = title_from_name(name);
	if (game.title == NULL)
		goto fail;
	{
		const char *indexed = cover_index_find(covers, game.title);

		cover_length = snprintf(cover, sizeof(cover), "%s", indexed != NULL ?
			indexed : "");
		if (indexed == NULL)
			cover_length = snprintf(cover, sizeof(cover), "%s/Imgs/%s.png",
				system_dir, game.title);
		if (cover_length < 0 || (size_t)cover_length >= sizeof(cover))
			goto fail;
	}
	game.path = realpath(path, NULL);
	if (game.path == NULL)
		game.path = strdup(path);
	game.cover_path = strdup(cover);
	game.system = strdup(system_name);
	game.system_label = strdup(route == NULL ? system_name : route->label_zh_tw);
	game.core = strdup(core);
	game.frontend = strdup(frontend);
	game.runtime = strdup(runtime);
	game.fallback = strdup(fallback);
	game.playable = route == NULL ?
		(strcasecmp(system_name, "RPGMV") != 0 &&
		 strcasecmp(system_name, "RPGMZ") != 0) : route->playable;
	game.modified = modified;
	if (game.path == NULL || game.cover_path == NULL || game.system == NULL ||
	    game.system_label == NULL ||
	    game.core == NULL || game.frontend == NULL || game.runtime == NULL ||
	    game.fallback == NULL || grow_games(catalog) != 0 ||
	    add_unique(&catalog->systems, &catalog->system_count,
		       &catalog->system_capacity, system_name) != 0 ||
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
			  const char *system_dir, const char *system, int depth,
			  struct scan_budget *budget,
			  const struct cover_index *covers,
			  uint64_t *direct_entries,
			  const struct scan_source *source)
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
		if (depth == 0 && direct_entries != NULL)
			++*direct_entries;
		if (scan_budget_expired(budget)) {
			result = 1;
			break;
		}
		length = snprintf(path, sizeof(path), "%s/%s", directory,
				  entry->d_name);
		if (length < 0 || (size_t)length >= sizeof(path) ||
		    lstat(path, &status) != 0)
			continue;
		if (S_ISDIR(status.st_mode) && depth == 0 &&
		    strcasecmp(system, "EASYRPG") == 0 &&
		    !easyrpg_project_is_launchable(path, &status))
			continue;
		if (S_ISDIR(status.st_mode) && depth == 0 &&
		    (strcasecmp(system, "EASYRPG") == 0 ||
		     strcasecmp(system, "RPGMV") == 0 ||
		     strcasecmp(system, "RPGMZ") == 0))
			result = preferred_source_has_game(ui, source, path,
				status.st_mode) ? 0 : add_game(ui, path, entry->d_name,
					system_dir, system, status.st_mtime, covers);
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
				result = preferred_source_has_game(ui, source, launcher,
					launcher_status.st_mode) ? 0 :
					add_game(ui, launcher, entry->d_name,
						system_dir, system,
						launcher_status.st_mtime, covers);
		}
		else if (S_ISDIR(status.st_mode) && depth < 8 &&
			 strcasecmp(system, "PORTS") != 0)
			result = scan_directory(ui, path, system_dir, system, depth + 1,
						budget, covers, direct_entries, source);
		else if (S_ISREG(status.st_mode) && is_rom_file(entry->d_name) &&
			 (strcasecmp(system, "PORTS") != 0 ||
			  strcasecmp(strrchr(entry->d_name, '.'), ".sh") == 0))
			result = preferred_source_has_game(ui, source, path,
				status.st_mode) ? 0 : add_game(ui, path, entry->d_name,
					system_dir, system, status.st_mtime, covers);
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

static void catalog_sort_and_filter(struct ui *ui)
{
	qsort(ui->catalog.games, ui->catalog.game_count,
	      sizeof(*ui->catalog.games), compare_game);
	qsort(ui->catalog.systems, ui->catalog.system_count,
	      sizeof(*ui->catalog.systems), compare_text);
	qsort(ui->catalog.cores, ui->catalog.core_count,
	      sizeof(*ui->catalog.cores), compare_text);
	catalog_apply_filters(ui);
}

static int scan_root(struct ui *ui, const char *rom_root,
		     const char *preferred_root, struct scan_budget *budget)
{
	DIR *root;
	struct dirent *entry;
	struct scan_source source = {
		.root = rom_root,
		.preferred_root = preferred_root,
	};
	int result = 0;

	root = opendir(rom_root);
	if (root == NULL)
		return -1;
	while (result == 0 && (entry = readdir(root)) != NULL) {
		char path[PATH_MAX];
		struct stat status;
		struct cover_index covers = { 0 };
		uint64_t direct_entries = 0U;
		int length;

		if (entry->d_name[0] == '.' || strcasecmp(entry->d_name, "Imgs") == 0)
			continue;
		if (scan_budget_expired(budget)) {
			result = 1;
			break;
		}
		length = snprintf(path, sizeof(path), "%s/%s", rom_root, entry->d_name);
		if (length < 0 || (size_t)length >= sizeof(path) ||
		    lstat(path, &status) != 0 || !S_ISDIR(status.st_mode))
			continue;
		result = cover_index_build(&covers, path, budget);
		if (result == 0)
			result = scan_directory(ui, path, path, entry->d_name, 0,
				budget, &covers, &direct_entries, &source);
		cover_index_destroy(&covers);
	}
	closedir(root);
	return result;
}

int catalog_scan_bounded(struct ui *ui, const char *rom_root,
			 size_t max_entries, uint32_t max_ms)
{
	struct scan_budget budget = {
		.entries = 0,
		.max_entries = max_entries == 0U ? SIZE_MAX : max_entries,
		.deadline_ns = max_ms == 0U ? 0U :
			monotonic_ns() + (uint64_t)max_ms * 1000000ULL,
	};
	int result;

	catalog_destroy(ui);
	if (rom_root == NULL || rom_root[0] != '/' ||
	    snprintf(ui->catalog.rom_root, sizeof(ui->catalog.rom_root), "%s",
		rom_root) < 0 || strlen(rom_root) >= sizeof(ui->catalog.rom_root)) {
		catalog_apply_filters(ui);
		return -1;
	}
	result = scan_root(ui, rom_root, NULL, &budget);
	if (result < 0) {
		catalog_destroy(ui);
		catalog_apply_filters(ui);
		return -1;
	}
	catalog_sort_and_filter(ui);
	return result;
}

int catalog_append_source_bounded(struct ui *ui, const char *rom_root,
				  const char *preferred_root,
				  size_t max_entries, uint32_t max_ms)
{
	struct scan_budget budget = {
		.entries = 0,
		.max_entries = max_entries == 0U ? SIZE_MAX : max_entries,
		.deadline_ns = max_ms == 0U ? 0U :
			monotonic_ns() + (uint64_t)max_ms * 1000000ULL,
	};
	char *resolved_root;
	char *resolved_preferred;
	int result;

	if (ui == NULL || rom_root == NULL || preferred_root == NULL ||
	    rom_root[0] != '/' || preferred_root[0] != '/')
		return -1;
	resolved_root = realpath(rom_root, NULL);
	resolved_preferred = realpath(preferred_root, NULL);
	if (resolved_root == NULL || resolved_preferred == NULL ||
	    strcmp(resolved_root, resolved_preferred) == 0) {
		free(resolved_root);
		free(resolved_preferred);
		return -1;
	}
	result = scan_root(ui, resolved_root, resolved_preferred, &budget);
	free(resolved_root);
	free(resolved_preferred);
	/* Keep the primary catalog coherent even if TF2 disappears mid-scan. */
	catalog_sort_and_filter(ui);
	return result;
}

static int move_cached_game(struct ui *ui, struct game_entry *game)
{
	struct catalog_state *catalog = &ui->catalog;
	const char *canonical_system = catalog_system_name(catalog, game->system);

	if (canonical_system != game->system) {
		char *replacement = strdup(canonical_system);

		if (replacement == NULL)
			return -1;
		free(game->system);
		game->system = replacement;
	}
	if (grow_games(catalog) != 0 ||
	    add_unique(&catalog->systems, &catalog->system_count,
		       &catalog->system_capacity, game->system) != 0 ||
	    add_unique(&catalog->cores, &catalog->core_count,
		       &catalog->core_capacity, game->core) != 0)
		return -1;
	catalog->games[catalog->game_count++] = *game;
	memset(game, 0, sizeof(*game));
	return 0;
}

int catalog_append_cached_source(struct ui *ui, struct ui *cached,
				 const char *rom_root,
				 const char *preferred_root)
{
	struct scan_source source;
	char *resolved_root;
	char *resolved_preferred;
	int result = 0;

	if (ui == NULL || cached == NULL || rom_root == NULL ||
	    preferred_root == NULL || rom_root[0] != '/' ||
	    preferred_root[0] != '/')
		return -1;
	resolved_root = realpath(rom_root, NULL);
	resolved_preferred = realpath(preferred_root, NULL);
	if (resolved_root == NULL || resolved_preferred == NULL ||
	    strcmp(resolved_root, resolved_preferred) == 0) {
		free(resolved_root);
		free(resolved_preferred);
		return -1;
	}
	source.root = resolved_root;
	source.preferred_root = resolved_preferred;
	for (size_t index = 0U; index < cached->catalog.game_count; ++index) {
		struct game_entry *game = &cached->catalog.games[index];
		struct stat metadata;

		if (game->path == NULL ||
		    relative_to_root(resolved_root, game->path) == NULL ||
		    lstat(game->path, &metadata) != 0 ||
		    (!S_ISREG(metadata.st_mode) && !S_ISDIR(metadata.st_mode)) ||
		    preferred_source_has_game(ui, &source, game->path,
			metadata.st_mode))
			continue;
		if (move_cached_game(ui, game) != 0) {
			result = -1;
			break;
		}
	}
	free(resolved_root);
	free(resolved_preferred);
	catalog_sort_and_filter(ui);
	return result;
}

bool catalog_optional_source_available(const char *mount_point,
				       const char *rom_root)
{
	struct stat mount_status;
	struct stat root_status;
	FILE *stream;
	char *line = NULL;
	size_t capacity = 0U;
	size_t mount_length;
	size_t root_length;
	bool mounted = false;

	if (mount_point == NULL || rom_root == NULL || mount_point[0] != '/' ||
	    rom_root[0] != '/')
		return false;
	mount_length = strlen(mount_point);
	while (mount_length > 1U && mount_point[mount_length - 1U] == '/')
		--mount_length;
	root_length = strlen(rom_root);
	if (root_length <= mount_length ||
	    strncmp(mount_point, rom_root, mount_length) != 0 ||
	    rom_root[mount_length] != '/' ||
	    lstat(mount_point, &mount_status) != 0 ||
	    !S_ISDIR(mount_status.st_mode) || S_ISLNK(mount_status.st_mode) ||
	    lstat(rom_root, &root_status) != 0 || !S_ISDIR(root_status.st_mode) ||
	    S_ISLNK(root_status.st_mode))
		return false;
	stream = fopen("/proc/self/mountinfo", "r");
	if (stream == NULL)
		return false;
	while (getline(&line, &capacity, stream) >= 0) {
		char *save = NULL;
		char *field = NULL;

		for (unsigned int index = 0U; index < 5U; ++index) {
			field = strtok_r(index == 0U ? line : NULL, " ", &save);
			if (field == NULL)
				break;
		}
		if (field != NULL && strlen(field) == mount_length &&
		    strncmp(field, mount_point, mount_length) == 0) {
			mounted = true;
			break;
		}
	}
	free(line);
	(void)fclose(stream);
	return mounted;
}

int catalog_scan_platform(struct ui *ui, const char *platform_path,
			  const char *system, size_t max_entries, uint32_t max_ms,
			  uint64_t *direct_entries)
{
	struct scan_budget budget = {
		.entries = 0U,
		.max_entries = max_entries == 0U ? SIZE_MAX : max_entries,
		.deadline_ns = max_ms == 0U ? 0U :
			monotonic_ns() + (uint64_t)max_ms * 1000000ULL,
	};
	struct cover_index covers = { 0 };
	int result;

	if (direct_entries != NULL)
		*direct_entries = 0U;
	result = cover_index_build(&covers, platform_path, &budget);
	if (result == 0)
		result = scan_directory(ui, platform_path, platform_path, system, 0,
			&budget, &covers, direct_entries,
			&(const struct scan_source) { .root = platform_path });
	cover_index_destroy(&covers);
	if (result < 0)
		return -1;
	catalog_sort_and_filter(ui);
	return result;
}

int catalog_scan(struct ui *ui, const char *rom_root)
{
	int result = catalog_scan_bounded(ui, rom_root, 0U, 0U);

	return result < 0 ? -1 : 0;
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
	for (size_t i = 0; i < catalog->platform_count; ++i)
		free(catalog->platforms[i].name);
	free(catalog->games);
	free(catalog->visible);
	free(catalog->systems);
	free(catalog->cores);
	free(catalog->platforms);
	memset(catalog, 0, sizeof(*catalog));
}
