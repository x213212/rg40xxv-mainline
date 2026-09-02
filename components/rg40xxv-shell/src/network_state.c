#define _POSIX_C_SOURCE 200809L

#include "network_ui.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

enum { NETWORK_SNAPSHOT_MAX_BYTES = 16 * 1024 };

#define NETWORK_SNAPSHOT_DEFAULT "/run/rg40xxv/network/snapshot.v1"
#define NETWORK_SNAPSHOT_HEADER "RG40XXV_NETWORK_SNAPSHOT\t1"

static bool valid_uuid(const char *value)
{
	static const size_t hyphens[] = { 8U, 13U, 18U, 23U };

	if (value == NULL || strlen(value) != NETWORK_UI_UUID_SIZE - 1U)
		return false;
	for (size_t i = 0U; i < NETWORK_UI_UUID_SIZE - 1U; ++i) {
		bool hyphen = false;

		for (size_t j = 0U; j < sizeof(hyphens) / sizeof(hyphens[0]); ++j)
			hyphen = hyphen || i == hyphens[j];
		if ((hyphen && value[i] != '-') ||
		    (!hyphen && !isxdigit((unsigned char)value[i])))
			return false;
	}
	return value[14] >= '1' && value[14] <= '5' &&
		strchr("89aAbB", value[19]) != NULL;
}

static bool valid_bssid(const char *value)
{
	if (value == NULL || strlen(value) != NETWORK_UI_BSSID_SIZE - 1U)
		return false;
	for (size_t i = 0U; i < NETWORK_UI_BSSID_SIZE - 1U; ++i) {
		if (i % 3U == 2U) {
			if (value[i] != ':')
				return false;
		} else if (!isxdigit((unsigned char)value[i])) {
			return false;
		}
	}
	return true;
}

static int hex_value(unsigned char value)
{
	if (value >= '0' && value <= '9') return value - '0';
	if (value >= 'a' && value <= 'f') return value - 'a' + 10;
	if (value >= 'A' && value <= 'F') return value - 'A' + 10;
	return -1;
}

static bool utf8_text_valid(const unsigned char *text, size_t length)
{
	size_t offset = 0U;

	while (offset < length) {
		uint32_t codepoint;
		size_t extra;
		unsigned char first = text[offset++];

		if (first < 0x80U) {
			if (first < 0x20U || first == 0x7fU) return false;
			continue;
		}
		if (first >= 0xc2U && first <= 0xdfU) {
			codepoint = first & 0x1fU; extra = 1U;
		} else if (first >= 0xe0U && first <= 0xefU) {
			codepoint = first & 0x0fU; extra = 2U;
		} else if (first >= 0xf0U && first <= 0xf4U) {
			codepoint = first & 0x07U; extra = 3U;
		} else return false;
		if (offset + extra > length) return false;
		for (size_t i = 0U; i < extra; ++i) {
			unsigned char continuation = text[offset++];

			if ((continuation & 0xc0U) != 0x80U) return false;
			codepoint = (codepoint << 6) | (continuation & 0x3fU);
		}
		if ((extra == 2U && codepoint < 0x800U) ||
		    (extra == 3U && codepoint < 0x10000U) ||
		    (codepoint >= 0xd800U && codepoint <= 0xdfffU) ||
		    codepoint > 0x10ffffU) return false;
	}
	return true;
}

static bool decode_text(const char *encoded, char *output, size_t size,
			bool optional)
{
	size_t used = 0U;

	if (optional && strcmp(encoded, "-") == 0) {
		output[0] = '\0';
		return true;
	}
	for (size_t i = 0U; encoded[i] != '\0'; ++i) {
		unsigned char value = (unsigned char)encoded[i];

		if (value == '%') {
			int high;
			int low;

			if (encoded[i + 1U] == '\0' || encoded[i + 2U] == '\0' ||
			    (high = hex_value((unsigned char)encoded[i + 1U])) < 0 ||
			    (low = hex_value((unsigned char)encoded[i + 2U])) < 0)
				return false;
			value = (unsigned char)((high << 4) | low);
			i += 2U;
		}
		if (used + 1U >= size) return false;
		output[used++] = (char)value;
	}
	if ((!optional && used == 0U) ||
	    !utf8_text_valid((const unsigned char *)output, used)) return false;
	output[used] = '\0';
	return true;
}

static int split_fields(char *line, char **fields, size_t capacity)
{
	size_t count = 0U;
	char *cursor = line;

	for (;;) {
		char *tab;

		if (count >= capacity) return -1;
		fields[count++] = cursor;
		tab = strchr(cursor, '\t');
		if (tab == NULL) break;
		*tab = '\0';
		cursor = tab + 1;
	}
	return (int)count;
}

static bool parse_bool(const char *text, bool *value)
{
	if (strcmp(text, "0") == 0) { *value = false; return true; }
	if (strcmp(text, "1") == 0) { *value = true; return true; }
	return false;
}

static bool parse_signal(const char *text, int *signal)
{
	char *end = NULL;
	long value;

	errno = 0;
	value = strtol(text, &end, 10);
	if (errno != 0 || end == text || *end != '\0' || value < 0 || value > 100)
		return false;
	*signal = (int)value;
	return true;
}

static int parse_status(char *line, struct network_ui_state *parsed)
{
	char *fields[10];
	int count = split_fields(line, fields,
		sizeof(fields) / sizeof(fields[0]));

	if (count != 10 || strcmp(fields[0], "S") != 0 ||
	    !parse_bool(fields[1], &parsed->wifi_present) ||
	    !parse_bool(fields[2], &parsed->wifi_enabled) ||
	    !parse_bool(fields[3], &parsed->wifi_connected) ||
	    !parse_bool(fields[4], &parsed->hotspot_enabled) ||
	    (strcmp(fields[5], "-") != 0 && !valid_uuid(fields[5])) ||
	    (strcmp(fields[6], "-") != 0 && !valid_bssid(fields[6])) ||
	    !decode_text(fields[7], parsed->connected_ssid,
		    sizeof(parsed->connected_ssid), true) ||
	    !decode_text(fields[8], parsed->ip_address,
		    sizeof(parsed->ip_address), true) ||
	    !decode_text(fields[9], parsed->hotspot_ssid,
		    sizeof(parsed->hotspot_ssid), true)) return EPROTO;
	if (strcmp(fields[5], "-") != 0)
		(void)snprintf(parsed->active_uuid, sizeof(parsed->active_uuid),
			"%s", fields[5]);
	if (strcmp(fields[6], "-") != 0)
		(void)snprintf(parsed->connected_bssid,
			sizeof(parsed->connected_bssid), "%s", fields[6]);
	if ((!parsed->wifi_present && (parsed->wifi_enabled ||
	     parsed->wifi_connected || parsed->hotspot_enabled)) ||
	    (parsed->hotspot_enabled && !parsed->wifi_connected)) return EPROTO;
	return 0;
}

static int parse_access_point(char *line, struct network_ui_state *parsed)
{
	char *fields[8];
	struct network_access_point *point;
	int count;

	if (parsed->access_point_count >= NETWORK_UI_MAX_ACCESS_POINTS) return E2BIG;
	count = split_fields(line, fields, sizeof(fields) / sizeof(fields[0]));
	if (count != 8 || strcmp(fields[0], "A") != 0 ||
	    !valid_bssid(fields[1])) return EPROTO;
	for (size_t i = 0U; i < parsed->access_point_count; ++i) {
		if (strcasecmp(parsed->access_points[i].bssid, fields[1]) == 0)
			return EEXIST;
	}
	point = &parsed->access_points[parsed->access_point_count];
	(void)snprintf(point->bssid, sizeof(point->bssid), "%s", fields[1]);
	if (!parse_signal(fields[2], &point->signal) ||
	    !decode_text(fields[3], point->security, sizeof(point->security), true) ||
	    !decode_text(fields[4], point->ssid, sizeof(point->ssid), false) ||
	    !parse_bool(fields[5], &point->known) ||
	    !parse_bool(fields[6], &point->active) ||
	    (strcmp(fields[7], "-") != 0 && !valid_uuid(fields[7])) ||
	    (point->known && strcmp(fields[7], "-") == 0) ||
	    (!point->known && strcmp(fields[7], "-") != 0)) return EPROTO;
	if (point->known)
		(void)snprintf(point->uuid, sizeof(point->uuid), "%s", fields[7]);
	++parsed->access_point_count;
	return 0;
}

static int parse_snapshot(char *content, size_t length,
			  struct network_ui_state *parsed)
{
	char *cursor = content;
	size_t line_number = 0U;
	bool status_seen = false;

	if (length == 0U || content[length - 1U] != '\n' ||
	    memchr(content, '\0', length) != NULL ||
	    memchr(content, '\r', length) != NULL) return EPROTO;
	while (*cursor != '\0') {
		char *newline = strchr(cursor, '\n');
		int error = 0;

		if (newline == NULL || newline == cursor) return EPROTO;
		*newline = '\0';
		if (line_number == 0U) {
			if (strcmp(cursor, NETWORK_SNAPSHOT_HEADER) != 0) return EPROTO;
		} else if (line_number == 1U) {
			error = parse_status(cursor, parsed);
			status_seen = error == 0;
		} else error = parse_access_point(cursor, parsed);
		if (error != 0) return error;
		++line_number;
		cursor = newline + 1;
	}
	return status_seen && line_number >= 2U ? 0 : EPROTO;
}

void network_ui_state_init(struct network_ui_state *state)
{
	memset(state, 0, sizeof(*state));
	state->selected = NETWORK_UI_WIFI;
	(void)snprintf(state->snapshot_path, sizeof(state->snapshot_path), "%s",
		NETWORK_SNAPSHOT_DEFAULT);
}

int network_ui_state_set_snapshot_path(struct network_ui_state *state,
				       const char *path)
{
	if (path == NULL || path[0] != '/' || strlen(path) >=
	    sizeof(state->snapshot_path)) return EINVAL;
	for (const unsigned char *cursor = (const unsigned char *)path;
	     *cursor != '\0'; ++cursor) {
		if (*cursor < 0x20U || *cursor == 0x7fU) return EINVAL;
	}
	(void)snprintf(state->snapshot_path, sizeof(state->snapshot_path), "%s",
		path);
	return 0;
}

static int read_snapshot(struct network_ui_state *state,
			 struct network_ui_state *parsed)
{
	struct stat status;
	char content[NETWORK_SNAPSHOT_MAX_BYTES + 1U];
	ssize_t total = 0;
	int fd;
	int error = 0;

	fd = open(state->snapshot_path,
		O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
	if (fd < 0) { state->last_snapshot_error = errno; return errno; }
	if (fstat(fd, &status) != 0) error = errno;
	else if (!S_ISREG(status.st_mode) || status.st_uid != geteuid() ||
		 status.st_nlink != 1 || (status.st_mode & 022) != 0 ||
		 status.st_size <= 0 || status.st_size > NETWORK_SNAPSHOT_MAX_BYTES)
		error = EACCES;
	while (error == 0 && total < status.st_size) {
		ssize_t count = read(fd, content + total,
			(size_t)(status.st_size - total));

		if (count < 0 && errno == EINTR) continue;
		if (count <= 0) { error = count < 0 ? errno : EIO; break; }
		total += count;
	}
	(void)close(fd);
	if (error != 0) { state->last_snapshot_error = error; return error; }
	content[total] = '\0';
	memset(parsed, 0, sizeof(*parsed));
	error = parse_snapshot(content, (size_t)total, parsed);
	if (error != 0) { state->last_snapshot_error = error; return error; }
	return 0;
}

static void apply_access_points(struct network_ui_state *state,
				const struct network_ui_state *parsed)
{
	char selected_bssid[NETWORK_UI_BSSID_SIZE] = { 0 };
	size_t previous_index = state->selected_access_point;

	if (state->access_point_count > 0U &&
	    state->selected_access_point < state->access_point_count)
		(void)snprintf(selected_bssid, sizeof(selected_bssid), "%s",
			state->access_points[state->selected_access_point].bssid);
	state->access_point_count = parsed->access_point_count;
	memcpy(state->access_points, parsed->access_points,
		sizeof(state->access_points));
	state->selected_access_point = 0U;
	if (state->access_point_count == 0U)
		return;
	if (selected_bssid[0] != '\0') {
		for (size_t index = 0U; index < state->access_point_count; ++index) {
			if (strcasecmp(state->access_points[index].bssid,
			    selected_bssid) == 0) {
				state->selected_access_point = index;
				return;
			}
		}
	}
	state->selected_access_point = previous_index < state->access_point_count ?
		previous_index : state->access_point_count - 1U;
}

static void advance_generation(struct network_ui_state *state)
{
	state->last_snapshot_error = 0;
	++state->generation;
	if (state->generation == 0U)
		state->generation = 1U;
}

int network_ui_state_load_snapshot(struct network_ui_state *state)
{
	struct network_ui_state parsed;
	int error = read_snapshot(state, &parsed);

	if (error != 0)
		return error;
	apply_access_points(state, &parsed);
	(void)snprintf(state->active_uuid, sizeof(state->active_uuid), "%s",
		parsed.active_uuid);
	(void)snprintf(state->connected_bssid, sizeof(state->connected_bssid), "%s",
		parsed.connected_bssid);
	(void)snprintf(state->connected_ssid, sizeof(state->connected_ssid), "%s",
		parsed.connected_ssid);
	(void)snprintf(state->ip_address, sizeof(state->ip_address), "%s",
		parsed.ip_address);
	(void)snprintf(state->hotspot_ssid, sizeof(state->hotspot_ssid), "%s",
		parsed.hotspot_ssid);
	state->wifi_present = parsed.wifi_present;
	state->wifi_enabled = parsed.wifi_enabled;
	state->wifi_connected = parsed.wifi_connected;
	state->hotspot_enabled = parsed.hotspot_enabled;
	advance_generation(state);
	return 0;
}

int network_ui_state_load_access_points(struct network_ui_state *state)
{
	struct network_ui_state parsed;
	int error = read_snapshot(state, &parsed);

	if (error != 0)
		return error;
	apply_access_points(state, &parsed);
	advance_generation(state);
	return 0;
}

void network_ui_state_set_backend(struct network_ui_state *state,
				  network_ui_action_callback action,
				  void *context)
{
	state->action = action;
	state->action_context = context;
	state->backend_available = action != NULL;
}

void network_ui_state_select(struct network_ui_state *state, int direction)
{
	int selected;

	if (direction == 0) return;
	selected = (int)state->selected + (direction > 0 ? 1 : -1);
	if (selected < 0) selected = NETWORK_UI_MODE_COUNT - 1;
	else if (selected >= NETWORK_UI_MODE_COUNT) selected = 0;
	state->selected = (enum network_ui_mode)selected;
}

const struct network_access_point *network_ui_state_selected_ap(
	const struct network_ui_state *state)
{
	if (state->access_point_count == 0U ||
	    state->selected_access_point >= state->access_point_count) return NULL;
	return &state->access_points[state->selected_access_point];
}

int network_ui_state_request(struct network_ui_state *state,
			     const struct network_ui_request *request)
{
	if (!state->backend_available || state->action == NULL) return ENODEV;
	if (request == NULL || request->mode < NETWORK_UI_WIFI ||
	    request->mode >= NETWORK_UI_MODE_COUNT) return EINVAL;
	return state->action(state->action_context, request);
}
