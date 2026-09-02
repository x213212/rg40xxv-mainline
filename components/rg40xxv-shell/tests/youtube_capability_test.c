#define _POSIX_C_SOURCE 200809L

#include "ui.h"
#include "youtube_capability.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static unsigned int checks;

#define CHECK(condition)                                                        \
	do {                                                                      \
		++checks;                                                          \
		if (!(condition)) {                                                \
			(void)fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__,         \
				      __LINE__, #condition);                          \
			return 1;                                                     \
		}                                                                 \
	} while (0)

Uint32 SDLCALL SDL_GetTicks(void)
{
	return 1U;
}

void persistence_request_filters(struct ui *ui)
{
	(void)ui;
}

void persistence_request_favorites(struct ui *ui)
{
	(void)ui;
}

const char *tr(const struct ui *ui, const char *key)
{
	(void)ui;
	return key;
}

static int write_file(const char *path, const char *payload, mode_t mode)
{
	size_t length = strlen(payload);
	size_t offset = 0U;
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);

	if (fd < 0)
		return -1;
	while (offset < length) {
		ssize_t count = write(fd, payload + offset, length - offset);

		if (count <= 0) {
			(void)close(fd);
			return -1;
		}
		offset += (size_t)count;
	}
	return close(fd);
}

static void free_catalog(struct catalog_state *catalog)
{
	for (size_t item = 0; item < catalog->game_count; ++item) {
		free(catalog->games[item].title);
		free(catalog->games[item].path);
		free(catalog->games[item].cover_path);
		free(catalog->games[item].system);
		free(catalog->games[item].system_label);
		free(catalog->games[item].core);
		free(catalog->games[item].frontend);
		free(catalog->games[item].runtime);
		free(catalog->games[item].fallback);
	}
	for (size_t item = 0; item < catalog->system_count; ++item)
		free(catalog->systems[item]);
	for (size_t item = 0; item < catalog->core_count; ++item)
		free(catalog->cores[item]);
	free(catalog->games);
	free(catalog->systems);
	free(catalog->cores);
	free(catalog->visible);
	memset(catalog, 0, sizeof(*catalog));
}

static int format_admission(char *payload, size_t size, const char *launcher,
			    const char *core_gate, const char *memory_gate)
{
	static const char admission_template[] =
		"schema=rg40xxv-youtube-ui-admission-v3\n"
		"native_route=native-texture\n"
		"native_launcher=%s\n"
		"evidence_scope=COMPONENT_GATE\n"
		"native_controller_ui=%s\n"
		"url_resolver=%s\n"
		"range_bridge=%s\n"
		"h264_decode=%s\n"
		"aac_decode=%s\n"
		"drm_kms_display=%s\n"
		"alsa_audio=%s\n"
		"input=%s\n"
		"session_return=%s\n"
		"memory_budget=%s\n";

	return snprintf(payload, size, admission_template, launcher, core_gate,
			core_gate, core_gate, core_gate, core_gate, core_gate,
			core_gate, core_gate, core_gate, memory_gate);
}

int main(int argc, char **argv)
{
	struct youtube_capability capability;
	struct youtube_capability unavailable = {0};
	struct ui ui = {0};
	char native[PATH_MAX];
	char native_alias[PATH_MAX];
	char native_symlink[PATH_MAX];
	char admission[PATH_MAX];
	char admission_alias[PATH_MAX];
	char invalid[PATH_MAX];
	char payload[8192];
	struct stat status;
	size_t payload_length;

	CHECK(argc == 2);
	CHECK(snprintf(native, sizeof(native), "%s/native", argv[1]) > 0);
	CHECK(snprintf(native_alias, sizeof(native_alias), "%s/native.alias",
		       argv[1]) > 0);
	CHECK(snprintf(native_symlink, sizeof(native_symlink), "%s/native.symlink",
		       argv[1]) > 0);
	CHECK(snprintf(admission, sizeof(admission), "%s/admission.env",
		       argv[1]) > 0);
	CHECK(snprintf(admission_alias, sizeof(admission_alias),
		       "%s/admission.alias", argv[1]) > 0);
	CHECK(snprintf(invalid, sizeof(invalid), "%s/invalid.env", argv[1]) > 0);
	CHECK(write_file(native, "#!/bin/sh\nexit 0\n", 0700) == 0);
	CHECK(link(native, native_alias) == 0);
	CHECK(stat(native, &status) == 0);
	CHECK(status.st_nlink == 2);

	/* Every route gate is proven except the bounded-memory observation.  The
	 * exact UNVERIFIED state remains visible and permits its user-driven run. */
	CHECK(format_admission(payload, sizeof(payload), native, "PASS",
			       "UNVERIFIED") > 0);
	CHECK(write_file(admission, payload, 0600) == 0);
	CHECK(link(admission, admission_alias) == 0);
	CHECK(stat(admission, &status) == 0);
	CHECK(status.st_nlink == 2);
	CHECK(youtube_capability_load(&capability, admission) == 0);
	CHECK(capability.native_available);
	CHECK(capability.native_launchable);
	CHECK(!capability.native_device_verified);
	CHECK(strcmp(capability.native_launcher, native) == 0);
	CHECK(youtube_catalog_add(&ui, &capability) == 0);
	CHECK(ui.catalog.game_count == 1U);
	CHECK(ui.catalog.visible_count == 0U);
	catalog_set_apps_view(&ui, true);
	CHECK(ui.catalog.visible_count == 1U);
	CHECK(ui.catalog.games[0].playable);
	CHECK(strcmp(ui.catalog.games[0].title, "YouTube") == 0);
	CHECK(strcmp(ui.catalog.games[0].path, native) == 0);
	CHECK(strstr(ui.catalog.games[0].runtime,
		"device acceptance pending") != NULL);
	free_catalog(&ui.catalog);

	/* Even a complete component gate is still a candidate; this admission
	 * namespace cannot manufacture device acceptance. */
	CHECK(format_admission(payload, sizeof(payload), native_alias, "PASS",
			       "PASS") > 0);
	CHECK(write_file(admission, payload, 0600) == 0);
	CHECK(youtube_capability_load(&capability, admission) == 0);
	CHECK(capability.native_available);
	CHECK(capability.native_launchable);
	CHECK(!capability.native_device_verified);
	CHECK(strcmp(capability.native_launcher, native_alias) == 0);
	CHECK(youtube_catalog_add(&ui, &capability) == 0);
	CHECK(ui.catalog.game_count == 1U);
	CHECK(ui.catalog.games[0].playable);
	CHECK(strcmp(ui.catalog.games[0].title, "YouTube") == 0);
	CHECK(strstr(ui.catalog.games[0].runtime,
		"device acceptance pending") != NULL);
	free_catalog(&ui.catalog);

	/* A durable memory-budget failure exposes the diagnosis but blocks launch. */
	CHECK(format_admission(payload, sizeof(payload), native, "PASS", "FAIL") > 0);
	CHECK(write_file(admission, payload, 0600) == 0);
	CHECK(youtube_capability_load(&capability, admission) == 0);
	CHECK(capability.native_available);
	CHECK(!capability.native_launchable);
	CHECK(!capability.native_device_verified);
	CHECK(youtube_catalog_add(&ui, &capability) == 0);
	CHECK(ui.catalog.game_count == 1U);
	CHECK(!ui.catalog.games[0].playable);
	CHECK(strcmp(ui.catalog.games[0].title,
		"YouTube · UNAVAILABLE") == 0);
	CHECK(strstr(ui.catalog.games[0].runtime,
		"recorded memory failure") != NULL);
	free_catalog(&ui.catalog);

	/* UNVERIFIED is narrowly accepted for memory only.  Any unproven core route
	 * gate remains fail-closed, while the one native tile stays informative. */
	CHECK(format_admission(payload, sizeof(payload), native, "UNVERIFIED",
			       "UNVERIFIED") > 0);
	CHECK(write_file(admission, payload, 0600) == 0);
	CHECK(youtube_capability_load(&capability, admission) != 0);
	CHECK(!capability.native_available);
	CHECK(!capability.native_launchable);
	CHECK(!capability.native_device_verified);
	CHECK(youtube_catalog_add(&ui, &capability) == 0);
	CHECK(ui.catalog.game_count == 1U);
	catalog_set_apps_view(&ui, true);
	CHECK(ui.catalog.visible_count == 1U);
	CHECK(!ui.catalog.games[0].playable);
	CHECK(strstr(ui.catalog.games[0].runtime,
		"admission/launcher route missing") != NULL);
	free_catalog(&ui.catalog);

	/* The v3 namespace is closed: old Web fields and duplicate keys are not
	 * silently ignored. */
	CHECK(format_admission(payload, sizeof(payload), native, "PASS", "PASS") > 0);
	payload_length = strlen(payload);
	CHECK(snprintf(payload + payload_length, sizeof(payload) - payload_length,
		       "web_route=mobile-web\n") > 0);
	CHECK(write_file(invalid, payload, 0600) == 0);
	CHECK(youtube_capability_load(&capability, invalid) != 0);
	CHECK(!capability.native_available);
	CHECK(write_file(invalid,
		"schema=rg40xxv-youtube-ui-admission-v3\n"
		"schema=rg40xxv-youtube-ui-admission-v3\n", 0600) == 0);
	CHECK(youtube_capability_load(&capability, invalid) != 0);

	/* Release deduplication hardlinks are allowed; writable or symlink launchers
	 * are not. */
	CHECK(chmod(native, 0720) == 0);
	CHECK(format_admission(payload, sizeof(payload), native, "PASS", "PASS") > 0);
	CHECK(write_file(admission, payload, 0600) == 0);
	CHECK(youtube_capability_load(&capability, admission) != 0);
	CHECK(chmod(native, 0700) == 0);
	CHECK(symlink(native, native_symlink) == 0);
	CHECK(format_admission(payload, sizeof(payload), native_symlink, "PASS",
			       "PASS") > 0);
	CHECK(write_file(admission, payload, 0600) == 0);
	CHECK(youtube_capability_load(&capability, admission) != 0);

	/* A synthetic YouTube tile must be the first Application even though it is
	 * appended after an on-disk catalog snapshot has already been loaded. */
	ui.catalog.games = calloc(1U, sizeof(*ui.catalog.games));
	CHECK(ui.catalog.games != NULL);
	ui.catalog.game_capacity = 1U;
	ui.catalog.game_count = 1U;
	ui.catalog.games[0].title = strdup("PortMaster");
	ui.catalog.games[0].system = strdup("APPS");
	ui.catalog.games[0].system_label = strdup("Applications");
	CHECK(ui.catalog.games[0].title != NULL);
	CHECK(ui.catalog.games[0].system != NULL);
	CHECK(ui.catalog.games[0].system_label != NULL);
	CHECK(youtube_catalog_add(&ui, &unavailable) == 0);
	CHECK(ui.catalog.game_count == 2U);
	CHECK(strcmp(ui.catalog.games[0].title,
		"YouTube · UNAVAILABLE") == 0);
	CHECK(strcmp(ui.catalog.games[1].title, "PortMaster") == 0);
	catalog_set_apps_view(&ui, true);
	CHECK(ui.catalog.visible_count == 2U);
	CHECK(!ui.catalog.games[0].playable);
	CHECK(strcmp(ui.catalog.games[0].path,
		"/opt/rg40xxv/youtube/bin/rg40xxv-youtube-native") == 0);
	free_catalog(&ui.catalog);

	(void)printf("YOUTUBE_CAPABILITY_TEST PASS checks=%u route=native-texture memory-unverified=launchable memory-fail=blocked web=absent\n",
		checks);
	return 0;
}
