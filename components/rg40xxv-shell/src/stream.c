#include "ui.h"

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
			    const char *label, const char *value)
{
	stream_draw_clipped(ui, 0, label, x, y, width, stream_secondary);
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
	NsStore store = { .dir_fd = -1, .lock_fd = -1 };
	NsHostDb loaded;
	char selected_name[NS_HOST_NAME_MAX_BYTES + 1] = { 0 };
	const NsHost *selected = stream_selected_host(ui);
	int index = -1;

	if (selected != NULL)
		(void)snprintf(selected_name, sizeof(selected_name), "%s",
			       selected->name);
	ui->streaming.loaded = false;
	ui->streaming.load_error[0] = '\0';
	if (ns_store_open(&store, ui->streaming.state_dir,
			  ui->streaming.load_error,
			  sizeof(ui->streaming.load_error)) != 0) {
		memset(&ui->streaming.hosts, 0, sizeof(ui->streaming.hosts));
		ui->streaming.selected_index = 0;
		return -1;
	}
	if (ns_hosts_load(&store, &loaded, ui->streaming.load_error,
			  sizeof(ui->streaming.load_error)) != 0) {
		ns_store_close(&store);
		memset(&ui->streaming.hosts, 0, sizeof(ui->streaming.hosts));
		ui->streaming.selected_index = 0;
		return -1;
	}
	ns_store_close(&store);
	ui->streaming.hosts = loaded;
	if (selected_name[0] != '\0')
		index = ns_host_find(&loaded, selected_name);
	if (index < 0 && loaded.default_name[0] != '\0')
		index = ns_host_find(&loaded, loaded.default_name);
	ui->streaming.selected_index = index >= 0 ? (size_t)index : 0;
	ui->streaming.loaded = true;
	return 0;
}

void stream_init(struct ui *ui, const char *state_dir,
		 const char *launcher_path)
{
	int length;

	memset(&ui->streaming, 0, sizeof(ui->streaming));
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
	(void)stream_reload(ui);
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
	if (!host->paired) {
		render_activate(ui, tr(ui, "stream_pair_required"), now);
		(void)fprintf(stderr,
			      "UI_STREAM_LAUNCH REJECTED reason=unpaired\n");
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
	if (host->codec != NS_CODEC_H264) {
		render_activate(ui, tr(ui, "stream_codec_unavailable"), now);
		(void)fprintf(stderr,
			      "UI_STREAM_LAUNCH REJECTED reason=unsupported-codec\n");
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
	const char *title = ui->streaming.loaded ? tr(ui, "stream_no_hosts") :
		tr(ui, "stream_load_failed");
	const char *hint = ui->streaming.loaded ? tr(ui, "stream_add_host") :
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
	stream_draw_clipped(ui, 2, host->name, 64, 143, 392, stream_primary);
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
	(void)snprintf(value, sizeof(value), "%u x %u",
		       host->resolution.width, host->resolution.height);
	stream_draw_row(ui, 42, 241, 118, tr(ui, "stream_resolution"), value);
	(void)snprintf(value, sizeof(value), "%u FPS", host->fps);
	stream_draw_row(ui, 176, 241, 116, tr(ui, "stream_frame_rate"), value);
	stream_draw_row(ui, 42, 282, 250, tr(ui, "stream_aspect"),
			stream_aspect_label(ui, host->aspect));

	render_fill_rect(ui->renderer, 322, 207, 290, 110,
			 (SDL_Color){ 13, 13, 13, 242 });
	render_outline_rect(ui->renderer, 322, 207, 290, 110, stream_line);
	text_draw(ui, 1, tr(ui, "stream_transport"), 336, 214, stream_primary);
	stream_draw_row(ui, 336, 241, 118, tr(ui, "stream_codec"),
			stream_codec_label(host->codec));
	(void)snprintf(value, sizeof(value), "%u kbps", host->bitrate_kbps);
	stream_draw_row(ui, 470, 241, 124, tr(ui, "stream_bitrate"), value);
	(void)snprintf(value, sizeof(value), "%u bytes", host->packet_size);
	stream_draw_row(ui, 336, 282, 258, tr(ui, "stream_packet"), value);

	render_fill_rect(ui->renderer, 28, 329, 584, 84,
			 (SDL_Color){ 13, 13, 13, 242 });
	render_outline_rect(ui->renderer, 28, 329, 584, 84, stream_line);
	status = ui->action_until > now && ui->action_text != NULL ?
		ui->action_text : !host->paired ? tr(ui, "stream_pair_required") :
		host->codec != NS_CODEC_H264 ? tr(ui, "stream_codec_unavailable") :
		ui->streaming.moonlight_deployed ? tr(ui, "stream_launch_ready") :
		tr(ui, "stream_not_deployed");
	stream_draw_clipped(ui, 1, status, 42, 341, 556, stream_focus);
	stream_draw_clipped(ui, 0, tr(ui, "stream_fixed_argv"),
			    42, 374, 556, stream_secondary);
}

void stream_destroy(struct ui *ui)
{
	memset(&ui->streaming, 0, sizeof(ui->streaming));
}
