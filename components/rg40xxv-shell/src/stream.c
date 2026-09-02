#include "ui.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static const SDL_Color stream_primary = { 238, 238, 238, 255 };
static const SDL_Color stream_secondary = { 157, 157, 157, 255 };
static const SDL_Color stream_focus = { 220, 220, 220, 255 };
static const SDL_Color stream_line = { 75, 75, 75, 220 };

static void stream_refresh_launcher_gate(struct ui *ui)
{
	ui->streaming.launcher_error =
		launcher_executable_validate(ui->streaming.launcher_path);
	ui->streaming.moonlight_deployed =
		ui->streaming.launcher_error == 0;
}

static void stream_draw_clipped(struct ui *ui, int font, const char *text,
				int x, int y, int width, SDL_Color color)
{
	SDL_Rect clip = { x, y, width, 27 };

	(void)SDL_RenderSetClipRect(ui->renderer, &clip);
	text_draw(ui, font, text, x, y, color);
	(void)SDL_RenderSetClipRect(ui->renderer, NULL);
}

static void stream_draw_row(struct ui *ui, int x, int y, int width,
			    const char *label, const char *value, bool selected)
{
	if (selected)
		render_outline_rect(ui->renderer, x - 4, y - 3, width + 8, 42,
				    stream_focus);
	stream_draw_clipped(ui, 0, label, x, y, width,
			    selected ? stream_focus : stream_secondary);
	stream_draw_clipped(ui, 1, value, x, y + 18, width, stream_primary);
}

static const char *stream_codec_label(NsCodec codec)
{
	switch (codec) {
	case NS_CODEC_H264:
		return "H.264";
	case NS_CODEC_H265:
		return "H.265";
	case NS_CODEC_AV1:
		return "AV1";
	}
	return "--";
}

static const char *stream_aspect_label(const struct ui *ui, NsAspect aspect)
{
	switch (aspect) {
	case NS_ASPECT_FIT:
		return tr(ui, "stream_aspect_fit");
	case NS_ASPECT_FILL:
		return tr(ui, "stream_aspect_fill");
	case NS_ASPECT_STRETCH:
		return tr(ui, "stream_aspect_stretch");
	}
	return "--";
}

const NsHost *stream_selected_host(const struct ui *ui)
{
	if (ui->streaming.hosts.count == 0 ||
	    ui->streaming.selected_index >= ui->streaming.hosts.count)
		return NULL;
	return &ui->streaming.hosts.hosts[ui->streaming.selected_index];
}

int stream_reload(struct ui *ui)
{
	int error;

	if (ui->streaming.backend == NULL) {
		error = stream_backend_start(&ui->streaming.backend,
					     ui->streaming.state_dir,
					     ui->streaming.launcher_path);
		if (error != 0) {
			ui->streaming.phase = STREAM_BACKEND_ERROR;
			(void)snprintf(ui->streaming.load_error,
				       sizeof(ui->streaming.load_error), "%s",
				       "stream worker unavailable");
			return -1;
		}
	}
	error = stream_backend_request_discovery(ui->streaming.backend);
	if (error == 0) {
		ui->streaming.phase = STREAM_BACKEND_DISCOVERING;
		ui->streaming.discovery_retry_at = 0U;
		ui->streaming.discovery_retry_count = 0U;
	}
	return error == 0 ? 0 : -1;
}

bool stream_update(struct ui *ui, uint32_t now)
{
	struct stream_backend_snapshot snapshot;
	char selected_name[NS_HOST_NAME_MAX_BYTES + 1] = { 0 };
	char selected_address[NS_HOST_ADDRESS_MAX_BYTES + 1] = { 0 };
	const NsHost *selected = stream_selected_host(ui);
	int updated;
	int index = -1;

	if (ui->streaming.backend == NULL)
		return false;
	if (ui->streaming.phase == STREAM_BACKEND_PENDING &&
	    ui->streaming.discovery_retry_at != 0U &&
	    SDL_TICKS_PASSED(now, ui->streaming.discovery_retry_at)) {
		if (stream_backend_request_discovery(ui->streaming.backend) == 0) {
			ui->streaming.phase = STREAM_BACKEND_DISCOVERING;
			ui->streaming.discovery_retry_at = 0U;
		}
	}
	updated = stream_backend_poll(ui->streaming.backend, &snapshot,
				      &ui->streaming.backend_generation);
	if (updated <= 0)
		return false;
	if (selected != NULL) {
		(void)snprintf(selected_name, sizeof(selected_name), "%s",
			       selected->name);
		(void)snprintf(selected_address, sizeof(selected_address), "%s",
			       selected->address);
	}
	ui->streaming.hosts = snapshot.hosts;
	ui->streaming.phase = snapshot.phase;
	ui->streaming.loaded = snapshot.store_loaded;
	ui->streaming.discovered_count = snapshot.discovered_count;
	ui->streaming.discovery_ms = snapshot.discovery_ms;
	(void)snprintf(ui->streaming.pair_pin,
		       sizeof(ui->streaming.pair_pin), "%s", snapshot.pin);
	(void)snprintf(ui->streaming.load_error,
		       sizeof(ui->streaming.load_error), "%s", snapshot.detail);
	if (snapshot.phase == STREAM_BACKEND_PENDING &&
	    (strncmp(snapshot.detail, "mDNS interface", 14) == 0 ||
	     strncmp(snapshot.detail, "mDNS join", 9) == 0) &&
	    ui->streaming.discovery_retry_count < 5U) {
		unsigned int shift = ui->streaming.discovery_retry_count > 3U ? 3U :
			ui->streaming.discovery_retry_count;

		++ui->streaming.discovery_retry_count;
		ui->streaming.discovery_retry_at = now + (1000U << shift);
	} else if (snapshot.phase == STREAM_BACKEND_READY) {
		ui->streaming.discovery_retry_count = 0U;
		ui->streaming.discovery_retry_at = 0U;
	}
	if (ui->streaming.settings_save_pending &&
	    (strcmp(snapshot.detail, "settings-saved") == 0 ||
	     snapshot.phase == STREAM_BACKEND_ERROR)) {
		bool visible = ui->nav_index == NAV_PAGE_STREAMING &&
			ui->streaming.settings_save_navigation_epoch ==
				ui->navigation_epoch;

		ui->streaming.settings_save_pending = false;
		if (visible)
			render_activate(ui, tr(ui,
				snapshot.phase == STREAM_BACKEND_ERROR ?
				"stream_settings_failed" : "stream_settings_saved"),
				now);
	}
	if (selected_name[0] != '\0')
		index = ns_host_find(&ui->streaming.hosts, selected_name);
	if (index < 0 && selected_address[0] != '\0') {
		for (size_t i = 0U; i < ui->streaming.hosts.count; ++i) {
			if (strcmp(ui->streaming.hosts.hosts[i].address,
				   selected_address) == 0) {
				index = (int)i;
				break;
			}
		}
	}
	if (index < 0 && ui->streaming.hosts.default_name[0] != '\0')
		index = ns_host_find(&ui->streaming.hosts,
				     ui->streaming.hosts.default_name);
	ui->streaming.selected_index = index >= 0 ? (size_t)index : 0U;
	(void)fprintf(stderr,
		"UI_STREAM_BACKEND phase=%d hosts=%zu discovered=%zu discovery_ms=%u store=%s detail=%s\n",
		(int)snapshot.phase, snapshot.hosts.count,
		snapshot.discovered_count, snapshot.discovery_ms,
		snapshot.store_loaded ? "ready" : "unavailable",
		snapshot.detail[0] != '\0' ? snapshot.detail : "none");
	return true;
}

void stream_init(struct ui *ui, const char *state_dir,
		 const char *launcher_path)
{
	int length;

	memset(&ui->streaming, 0, sizeof(ui->streaming));
	ui->streaming.setting_index = -1;
	ui->streaming.phase = STREAM_BACKEND_LOADING;
	length = snprintf(ui->streaming.state_dir,
			  sizeof(ui->streaming.state_dir), "%s", state_dir);
	if (length < 0 || (size_t)length >= sizeof(ui->streaming.state_dir)) {
		(void)snprintf(ui->streaming.load_error,
			       sizeof(ui->streaming.load_error), "%s",
			       "state directory path is too long");
		return;
	}
	length = snprintf(ui->streaming.launcher_path,
			  sizeof(ui->streaming.launcher_path), "%s", launcher_path);
	if (length < 0 ||
	    (size_t)length >= sizeof(ui->streaming.launcher_path)) {
		ui->streaming.launcher_path[0] = '\0';
	}
	stream_refresh_launcher_gate(ui);
}

void stream_select_host(struct ui *ui, int direction)
{
	size_t count = ui->streaming.hosts.count;

	if (count < 2 || direction == 0)
		return;
	if (direction < 0)
		ui->streaming.selected_index =
			(ui->streaming.selected_index + count - 1) % count;
	else
		ui->streaming.selected_index =
			(ui->streaming.selected_index + 1) % count;
}

void stream_move_setting(struct ui *ui, int direction)
{
	if (direction < 0)
		ui->streaming.setting_index = ui->streaming.setting_index <= -1 ?
			4 : ui->streaming.setting_index - 1;
	else if (direction > 0)
		ui->streaming.setting_index = ui->streaming.setting_index >= 4 ?
			-1 : ui->streaming.setting_index + 1;
}

static size_t closest_u32(const uint32_t *values, size_t count,
			  uint32_t current)
{
	size_t best = 0U;
	uint32_t difference = values[0] > current ? values[0] - current :
		current - values[0];

	for (size_t i = 1U; i < count; ++i) {
		uint32_t candidate = values[i] > current ? values[i] - current :
			current - values[i];

		if (candidate < difference) {
			best = i;
			difference = candidate;
		}
	}
	return best;
}

void stream_adjust_setting(struct ui *ui, int direction, uint32_t now)
{
	static const uint32_t frame_rates[] = { 30U, 60U };
	static const uint32_t bitrates[] = { 3000U, 5000U, 8000U, 12000U };
	NsHost *host;
	NsHost previous;
	size_t index;
	int error;

	if (ui->streaming.hosts.count == 0U ||
	    ui->streaming.selected_index >= ui->streaming.hosts.count)
		return;
	/* One persisted mutation at a time prevents stale save results. */
	if (ui->streaming.settings_save_pending)
		return;
	if (ui->streaming.setting_index < 0) {
		stream_select_host(ui, direction);
		return;
	}
	host = &ui->streaming.hosts.hosts[ui->streaming.selected_index];
	previous = *host;
	if (ui->streaming.setting_index == 0) {
		/* Output is deliberately fixed to the native panel.  Touching this
		 * row only repairs an older persisted profile; it never cycles back
		 * to a larger host resolution. */
		bool already_native = previous.resolution.width == 640U &&
			previous.resolution.height == 480U &&
			previous.resolution.custom == 0;

		host->resolution.width = 640U;
		host->resolution.height = 480U;
		host->resolution.custom = 0;
		if (already_native) {
			render_activate(ui, tr(ui, "stream_resolution_fixed"), now);
			return;
		}
	} else if (ui->streaming.setting_index == 1) {
		index = closest_u32(frame_rates,
			sizeof(frame_rates) / sizeof(frame_rates[0]), host->fps);
		if (direction < 0)
			index = (index + sizeof(frame_rates) /
				sizeof(frame_rates[0]) - 1U) %
				(sizeof(frame_rates) / sizeof(frame_rates[0]));
		else
			index = (index + 1U) %
				(sizeof(frame_rates) / sizeof(frame_rates[0]));
		host->fps = frame_rates[index];
	} else if (ui->streaming.setting_index == 2) {
		int aspect = (int)host->aspect + (direction < 0 ? -1 : 1);

		if (aspect < (int)NS_ASPECT_FIT)
			aspect = (int)NS_ASPECT_STRETCH;
		if (aspect > (int)NS_ASPECT_STRETCH)
			aspect = (int)NS_ASPECT_FIT;
		host->aspect = (NsAspect)aspect;
	} else if (ui->streaming.setting_index == 3) {
		int codec = (int)host->codec + (direction < 0 ? -1 : 1);

		if (codec < (int)NS_CODEC_H264)
			codec = (int)NS_CODEC_AV1;
		if (codec > (int)NS_CODEC_AV1)
			codec = (int)NS_CODEC_H264;
		host->codec = (NsCodec)codec;
	} else if (ui->streaming.setting_index == 4) {
		index = closest_u32(bitrates,
			sizeof(bitrates) / sizeof(bitrates[0]), host->bitrate_kbps);
		if (direction < 0)
			index = (index + sizeof(bitrates) / sizeof(bitrates[0]) - 1U) %
				(sizeof(bitrates) / sizeof(bitrates[0]));
		else
			index = (index + 1U) %
				(sizeof(bitrates) / sizeof(bitrates[0]));
		host->bitrate_kbps = bitrates[index];
	} else {
		return;
	}
	error = stream_backend_request_settings_save(ui->streaming.backend, host);
	if (error != 0) {
		*host = previous;
		render_activate(ui, tr(ui, "stream_settings_failed"), now);
		return;
	}
	ui->streaming.settings_save_pending = true;
	ui->streaming.settings_save_navigation_epoch = ui->navigation_epoch;
	render_activate(ui, tr(ui, "stream_settings_saving"), now);
}

void stream_activate_selected(struct ui *ui, uint32_t now)
{
	const NsHost *host = stream_selected_host(ui);
	char validation_error[NS_ERROR_MAX] = { 0 };
	int error;

	if (host == NULL) {
		render_activate(ui, tr(ui, ui->streaming.loaded ?
				"stream_no_hosts" : "stream_load_failed"), now);
		(void)fprintf(stderr,
			      "UI_STREAM_LAUNCH REJECTED reason=%s\n",
			      ui->streaming.loaded ? "no-host" : "invalid-host-store");
		return;
	}
	if (ns_validate_host(host, validation_error,
			     sizeof(validation_error)) != 0) {
		render_activate(ui, tr(ui, "stream_invalid_host"), now);
		(void)fprintf(stderr,
			      "UI_STREAM_LAUNCH REJECTED reason=invalid-host detail=%s\n",
			      validation_error);
		return;
	}
	stream_refresh_launcher_gate(ui);
	if (!ui->streaming.moonlight_deployed) {
		render_activate(ui, tr(ui, "stream_not_deployed"), now);
		(void)fprintf(stderr,
			      "UI_STREAM_LAUNCH REJECTED reason=runner-unavailable error=%d\n",
			      ui->streaming.launcher_error);
		return;
	}
	if (!host->paired) {
		error = stream_backend_request_pair(ui->streaming.backend, host);
		if (error != 0) {
			render_activate(ui, tr(ui, error == EBUSY ?
				"stream_pair_busy" : "stream_pair_failed"), now);
			return;
		}
		ui->streaming.phase = STREAM_BACKEND_PAIRING;
		render_activate(ui, tr(ui, "stream_pair_starting"), now);
		(void)fprintf(stderr,
			      "UI_STREAM_PAIR REQUESTED host=%s\n", host->address);
		return;
	}
	if (host->codec != NS_CODEC_H264) {
		render_activate(ui, tr(ui, "stream_codec_unavailable"), now);
		(void)fprintf(stderr,
			      "UI_STREAM_LAUNCH REJECTED reason=unsupported-codec\n");
		return;
	}
	error = launch_queue_stream(ui, host, now);
	if (error != 0) {
		render_activate(ui, tr(ui, "stream_launch_failed"), now);
		(void)fprintf(stderr,
			      "UI_STREAM_LAUNCH REJECTED reason=request-invalid error=%d\n",
			      error);
	}
}

static void stream_draw_empty(struct ui *ui)
{
	bool scanning = ui->streaming.phase == STREAM_BACKEND_LOADING ||
		ui->streaming.phase == STREAM_BACKEND_DISCOVERING;
	const char *title = scanning ? tr(ui, "stream_scanning") :
		ui->streaming.loaded ? tr(ui, "stream_no_hosts") :
		tr(ui, "stream_load_failed");
	const char *hint = scanning ? tr(ui, "stream_scanning_hint") :
		ui->streaming.loaded ? tr(ui, "stream_add_host") :
		ui->streaming.load_error;

	text_draw(ui, 2, title,
		  UI_WIDTH / 2 - text_width(ui, 2, title, stream_primary) / 2,
		  214, stream_primary);
	stream_draw_clipped(ui, 0, hint, 110, 250, 420, stream_secondary);
}

void render_stream_page(struct ui *ui, uint32_t now)
{
	const NsHost *host = stream_selected_host(ui);
	char value[160];
	const char *status;

	/* Discovery starts only when this page is first presented. */
	if (ui->streaming.phase == STREAM_BACKEND_LOADING)
		(void)stream_reload(ui);

	render_glass_panel(ui->renderer, UI_LAYOUT_CONTENT_X, UI_LAYOUT_CONTENT_Y,
		UI_LAYOUT_CONTENT_WIDTH, UI_LAYOUT_CONTENT_HEIGHT,
		ui->focus_region == UI_FOCUS_CONTENT);
	text_draw(ui, 2, tr(ui, "stream_title"), 28, 91,
		ui->focus_region == UI_FOCUS_CONTENT ? stream_primary :
		stream_secondary);
	text_draw(ui, 0, tr(ui, "stream_safe_store"), 28, 118,
		  stream_secondary);
	if (host == NULL) {
		stream_draw_empty(ui);
		return;
	}

	render_fill_rect(ui->renderer, 28, 140, 584, 55,
			 (SDL_Color){ 13, 13, 13, 242 });
	render_outline_rect(ui->renderer, 28, 140, 584, 55, stream_line);
	text_draw(ui, 1, "<", 40, 153, stream_secondary);
	text_draw(ui, 1, ">", 592, 153, stream_secondary);
	stream_draw_clipped(ui, 2, host->name, 64, 143, 392,
		ui->streaming.setting_index < 0 ? stream_focus : stream_primary);
	stream_draw_clipped(ui, 0, host->address, 64, 169, 385,
			    stream_secondary);
	(void)snprintf(value, sizeof(value), "%zu / %zu",
		       ui->streaming.selected_index + 1,
		       ui->streaming.hosts.count);
	stream_draw_clipped(ui, 0, value, 478, 146, 62, stream_secondary);
	stream_draw_clipped(ui, 0,
			    tr(ui, host->paired ? "stream_paired" :
			       "stream_not_paired"),
			    548, 146, 54,
			    host->paired ? stream_primary : stream_secondary);

	render_fill_rect(ui->renderer, 28, 207, 282, 110,
			 (SDL_Color){ 13, 13, 13, 242 });
	render_outline_rect(ui->renderer, 28, 207, 282, 110, stream_line);
	text_draw(ui, 1, tr(ui, "stream_display"), 42, 214, stream_primary);
	(void)snprintf(value, sizeof(value), "%u x %u", 640U, 480U);
	stream_draw_row(ui, 42, 241, 118, tr(ui, "stream_resolution"), value,
			ui->streaming.setting_index == 0);
	(void)snprintf(value, sizeof(value), "%u FPS", host->fps);
	stream_draw_row(ui, 176, 241, 116, tr(ui, "stream_frame_rate"), value,
			ui->streaming.setting_index == 1);
	stream_draw_row(ui, 42, 282, 250, tr(ui, "stream_aspect"),
			stream_aspect_label(ui, host->aspect),
			ui->streaming.setting_index == 2);

	render_fill_rect(ui->renderer, 322, 207, 290, 110,
			 (SDL_Color){ 13, 13, 13, 242 });
	render_outline_rect(ui->renderer, 322, 207, 290, 110, stream_line);
	text_draw(ui, 1, tr(ui, "stream_transport"), 336, 214, stream_primary);
	stream_draw_row(ui, 336, 241, 118, tr(ui, "stream_codec"),
			stream_codec_label(host->codec),
			ui->streaming.setting_index == 3);
	(void)snprintf(value, sizeof(value), "%u kbps", host->bitrate_kbps);
	stream_draw_row(ui, 470, 241, 124, tr(ui, "stream_bitrate"), value,
			ui->streaming.setting_index == 4);
	(void)snprintf(value, sizeof(value), "%u bytes", host->packet_size);
	stream_draw_row(ui, 336, 282, 258, tr(ui, "stream_packet"), value,
			false);

	render_fill_rect(ui->renderer, 28, 329, 584, 84,
			 (SDL_Color){ 13, 13, 13, 242 });
	render_outline_rect(ui->renderer, 28, 329, 584, 84, stream_line);
	status = ui->action_until > now && ui->action_text != NULL ?
		ui->action_text : ui->streaming.phase == STREAM_BACKEND_PAIRING &&
		ui->streaming.pair_pin[0] != '\0' ? ui->streaming.pair_pin :
		ui->streaming.phase == STREAM_BACKEND_PAIRING ?
		tr(ui, "stream_pair_starting") :
		ui->streaming.phase == STREAM_BACKEND_PENDING ?
		tr(ui, "stream_discovery_pending") :
		!host->paired ? tr(ui, "stream_pair_required") :
		host->codec != NS_CODEC_H264 ? tr(ui, "stream_codec_unavailable") :
		ui->streaming.moonlight_deployed ? tr(ui, "stream_launch_ready") :
		tr(ui, "stream_not_deployed");
	stream_draw_clipped(ui, 1, status, 42, 341, 556, stream_focus);
	if (ui->streaming.phase == STREAM_BACKEND_PAIRING &&
	    ui->streaming.pair_pin[0] != '\0')
		(void)snprintf(value, sizeof(value), "%s %s",
			       tr(ui, "stream_pair_pin"), ui->streaming.pair_pin);
	else
		(void)snprintf(value, sizeof(value), "%s",
			       tr(ui, "stream_controls"));
	stream_draw_clipped(ui, 0, value,
			    42, 374, 556, stream_secondary);
}

void stream_destroy(struct ui *ui)
{
	stream_backend_stop(&ui->streaming.backend);
	memset(&ui->streaming, 0, sizeof(ui->streaming));
}
