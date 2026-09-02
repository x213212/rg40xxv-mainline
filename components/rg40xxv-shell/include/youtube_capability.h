#ifndef RG40XXV_YOUTUBE_CAPABILITY_H
#define RG40XXV_YOUTUBE_CAPABILITY_H

#include <limits.h>
#include <stdbool.h>

struct ui;

struct youtube_capability {
	char native_launcher[PATH_MAX];
	bool native_available;
	bool native_launchable;
	bool native_device_verified;
};

/*
 * The admission file is an installer-owned assertion, not a runtime probe.
 * Missing, malformed, writable, or incomplete files fail closed.  The UI
 * never runs a probe or a shell command on its render thread.
 */
int youtube_capability_load(struct youtube_capability *capability,
			    const char *path);

/* The sole YouTube tile is the release-owned native texture route.  It remains
 * visible but disabled when its admission is absent or incomplete.  A sole
 * UNVERIFIED memory-budget gate permits the user-driven verification run;
 * evidence_scope=COMPONENT_GATE never promotes final device acceptance, and
 * a recorded memory FAIL remains blocked. */
int youtube_catalog_add(struct ui *ui, const struct youtube_capability *capability);

#endif
