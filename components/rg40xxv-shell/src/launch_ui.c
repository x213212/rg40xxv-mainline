#define _POSIX_C_SOURCE 200809L

#include "ui.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static const char *selected_route(const struct game_entry *game)
{
	/* Unlocked titles use the launcher's verified runtime fallback chain. */
	return game != NULL && game->route_lock && strchr(game->core, ':') != NULL ?
		game->core : "";
}

static const char *stream_codec_argument(NsCodec codec)
{
	return codec == NS_CODEC_H264 ? "h264" : NULL;
}

static void launch_result_message(struct ui *ui, uint32_t now)
{
	struct launch_state *launch = &ui->launch;
	const char *failed = launch->kind == LAUNCH_KIND_STREAM ?
		tr(ui, "stream_launch_failed") : tr(ui, "launch_failed");
	const char *abnormal = launch->kind == LAUNCH_KIND_STREAM ?
		tr(ui, "stream_abnormal_exit") : tr(ui, "launch_abnormal_exit");

	launch->transition_detail[0] = '\0';
	launch->diagnostics[0] = '\0';
	launch->diagnostics_expanded = false;
	launch->transition_presented = false;
	if (launch->last_error != 0) {
		(void)snprintf(launch->transition_detail,
			       sizeof(launch->transition_detail), "%s", failed);
		(void)snprintf(launch->diagnostics,
			       sizeof(launch->diagnostics), "%s %d · %s",
			       tr(ui, "launch_error_code"), launch->last_error,
			       strerror(launch->last_error));
		launch->transition = LAUNCH_TRANSITION_ERROR;
		launch->transition_until = now + 2600U;
	} else if (launch->process.term_signal != 0) {
		(void)snprintf(launch->transition_detail,
			       sizeof(launch->transition_detail), "%s", abnormal);
		(void)snprintf(launch->diagnostics,
			       sizeof(launch->diagnostics), "%s %d",
			       tr(ui, "launch_exit_signal"),
			       launch->process.term_signal);
		launch->transition = LAUNCH_TRANSITION_ERROR;
		launch->transition_until = now + 2600U;
	} else if (launch->process.exit_code != 0) {
		(void)snprintf(launch->transition_detail,
			       sizeof(launch->transition_detail), "%s", abnormal);
		(void)snprintf(launch->diagnostics,
			       sizeof(launch->diagnostics), "%s %d",
			       tr(ui, "launch_exit_code"),
			       launch->process.exit_code);
		launch->transition = LAUNCH_TRANSITION_ERROR;
		launch->transition_until = now + 2600U;
	} else {
		launch->transition = LAUNCH_TRANSITION_RETURNED;
		launch->transition_until = now + 1100U;
	}
}

void launch_configure(struct ui *ui, const char *executable,
		      const char *log_path, const char *font_path, bool windowed)
{
	struct launch_state *launch = &ui->launch;

	memset(launch, 0, sizeof(*launch));
	launch->pending_game_id = SIZE_MAX;
	launch->process.exit_code = -1;
	launch->windowed = windowed;
	(void)snprintf(launch->executable, sizeof(launch->executable), "%s",
		       executable);
	(void)snprintf(launch->log_path, sizeof(launch->log_path), "%s",
		       log_path);
	(void)snprintf(launch->font_path, sizeof(launch->font_path), "%s",
		       font_path);
}

void launch_queue_selected(struct ui *ui, uint32_t now)
{
	struct launcher_request request;
	const struct game_entry *game;
	size_t game_id;
	int error;

	if (ui->launch.pending || ui->launch.process.active)
		return;
	game_id = catalog_visible_id(ui, ui->game_index);
	game = game_id == SIZE_MAX ? NULL : &ui->catalog.games[game_id];
	if (game == NULL || !game->playable) {
		render_activate(ui, tr(ui, "not_playable"), now);
		return;
	}
	request = (struct launcher_request) {
		.executable = ui->launch.executable,
		.route = selected_route(game),
		.platform = game->system,
		.content = game->path,
		.log_path = ui->launch.log_path,
	};
	error = launcher_request_validate(&request);
	if (error != 0) {
		ui->launch.last_error = error;
		render_activate(ui, tr(ui, error == ENOENT ?
			"launcher_missing" : "launch_failed"), now);
		return;
	}
	ui->launch.pending_game_id = game_id;
	ui->launch.last_error = 0;
	ui->launch.transition_detail[0] = '\0';
	ui->launch.diagnostics[0] = '\0';
	(void)snprintf(ui->launch.game_title, sizeof(ui->launch.game_title), "%s",
		       game->title);
	(void)snprintf(ui->launch.game_platform,
		sizeof(ui->launch.game_platform), "%s", game->system_label);
	(void)snprintf(ui->launch.game_core, sizeof(ui->launch.game_core), "%s",
		game->runtime);
	ui->launch.transition = LAUNCH_TRANSITION_STARTING;
	ui->launch.kind = LAUNCH_KIND_GAME;
	ui->launch.transition_until = 0U;
	ui->launch.transition_presented = false;
	ui->launch.pending = true;
	audio_play_chime(ui, 1840.0);
}

int launch_queue_stream(struct ui *ui, const NsHost *host, uint32_t now)
{
	struct stream_launcher_request request;
	const char *codec;
	const char *aspect;
	int error;

	if (ui->launch.pending || ui->launch.process.active)
		return EBUSY;
	if (host == NULL)
		return EINVAL;
	codec = stream_codec_argument(host->codec);
	aspect = ns_aspect_name(host->aspect);
	request = (struct stream_launcher_request) {
		.executable = ui->streaming.launcher_path,
		.host = host->address,
		.width = host->resolution.width,
		.height = host->resolution.height,
		.fps = host->fps,
		.bitrate_kbps = host->bitrate_kbps,
		.codec = codec,
		.aspect = aspect,
		.log_path = ui->launch.log_path,
	};
	error = stream_launcher_request_validate(&request);
	if (error != 0)
		return error;
	ui->launch.pending_stream_host = *host;
	ui->launch.pending_game_id = SIZE_MAX;
	ui->launch.last_error = 0;
	ui->launch.transition_detail[0] = '\0';
	ui->launch.diagnostics[0] = '\0';
	(void)snprintf(ui->launch.game_title, sizeof(ui->launch.game_title), "%s",
		       host->name);
	(void)snprintf(ui->launch.game_platform,
		sizeof(ui->launch.game_platform), "%s", tr(ui, "nav_streaming"));
	(void)snprintf(ui->launch.game_core, sizeof(ui->launch.game_core), "%s",
		stream_codec_argument(host->codec));
	ui->launch.transition = LAUNCH_TRANSITION_STARTING;
	ui->launch.kind = LAUNCH_KIND_STREAM;
	ui->launch.transition_until = 0U;
	ui->launch.transition_presented = false;
	ui->launch.pending = true;
	audio_play_chime(ui, 1840.0);
	(void)now;
	return 0;
}

static void begin_pending_launch(struct ui *ui)
{
	int error;

	if (ui->launch.kind == LAUNCH_KIND_GAME &&
	    ui->launch.pending_game_id >= ui->catalog.game_count) {
		ui->launch.pending = false;
		ui->launch.transition = LAUNCH_TRANSITION_NONE;
		return;
	}
	if (ui->launch.kind == LAUNCH_KIND_NONE) {
		ui->launch.pending = false;
		ui->launch.transition = LAUNCH_TRANSITION_NONE;
		return;
	}
	ui->launch.pending = false;
	ui->launch.transition = LAUNCH_TRANSITION_NONE;
	ui->launch.transition_presented = false;
	lifecycle_session_suspend(ui);
	if (ui->launch.kind == LAUNCH_KIND_STREAM) {
		const NsHost *host = &ui->launch.pending_stream_host;
		struct stream_launcher_request request = {
			.executable = ui->streaming.launcher_path,
			.host = host->address,
			.width = host->resolution.width,
			.height = host->resolution.height,
			.fps = host->fps,
			.bitrate_kbps = host->bitrate_kbps,
			.codec = stream_codec_argument(host->codec),
			.aspect = ns_aspect_name(host->aspect),
			.log_path = ui->launch.log_path,
		};

		error = stream_launcher_process_start(&ui->launch.process, &request);
	} else {
		struct game_entry *game =
			&ui->catalog.games[ui->launch.pending_game_id];
		struct launcher_request request = {
			.executable = ui->launch.executable,
			.route = selected_route(game),
			.platform = game->system,
			.content = game->path,
			.log_path = ui->launch.log_path,
		};

		error = launcher_process_start(&ui->launch.process, &request);
	}
	if (error != 0) {
		ui->launch.last_error = error;
		if (lifecycle_session_resume(ui) != 0) {
			fprintf(stderr, "UI resume after launch failure: %s\n",
				SDL_GetError());
			ui->running = false;
			return;
		}
		launch_result_message(ui, SDL_GetTicks());
		return;
	}
	if (ui->launch.kind == LAUNCH_KIND_GAME) {
		history_mark_launched(ui, ui->launch.pending_game_id);
		ui->launch.history_needs_write = true;
		/* Persist launch history while the child owns the screen. */
		if (persistence_start(ui) == 0) {
			persistence_request_history(ui);
			persistence_stop(ui);
		}
	}
}

void launch_update(struct ui *ui, uint32_t now)
{
	bool finished = false;
	int error;

	if (ui->launch.process.active) {
		error = launcher_process_poll(&ui->launch.process, &finished);
		if (error != 0) {
			ui->launch.last_error = error;
			launcher_process_terminate(&ui->launch.process, 1000U);
			finished = true;
		}
		if (!finished)
			return;
		launcher_process_terminate(&ui->launch.process, 400U);
		if (lifecycle_session_resume(ui) != 0) {
			fprintf(stderr, "UI resume after game exit: %s\n", SDL_GetError());
			ui->running = false;
			return;
		}
		if (ui->launch.history_needs_write) {
			persistence_request_history(ui);
			ui->launch.history_needs_write = false;
		}
		launch_result_message(ui, SDL_GetTicks());
		return;
	}
	if (ui->launch.transition == LAUNCH_TRANSITION_RETURNED &&
	    ui->launch.transition_presented &&
	    SDL_TICKS_PASSED(now, ui->launch.transition_until)) {
		ui->launch.transition = LAUNCH_TRANSITION_NONE;
		ui->launch.transition_presented = false;
		ui->launch.kind = LAUNCH_KIND_NONE;
	}
	if (ui->launch.pending &&
	    ui->launch.transition == LAUNCH_TRANSITION_STARTING &&
	    ui->launch.transition_presented)
		begin_pending_launch(ui);
}

void launch_transition_presented(struct ui *ui)
{
	const char *phase;

	if (ui->launch.transition == LAUNCH_TRANSITION_NONE ||
	    ui->launch.transition_presented)
		return;
	ui->launch.transition_presented = true;
	phase = ui->launch.transition == LAUNCH_TRANSITION_STARTING ? "starting" :
		ui->launch.transition == LAUNCH_TRANSITION_RETURNED ? "returned" :
		"error";
	(void)fprintf(stderr,
		      "UI_LAUNCH_TRANSITION PRESENTED phase=%s kind=%s id=%zu\n",
		      phase, ui->launch.kind == LAUNCH_KIND_STREAM ? "stream" :
		      ui->launch.kind == LAUNCH_KIND_GAME ? "game" : "none",
		      ui->launch.pending_game_id);
	(void)fflush(stderr);
}

void launch_shutdown(struct ui *ui)
{
	ui->launch.pending = false;
	ui->launch.transition = LAUNCH_TRANSITION_NONE;
	ui->launch.kind = LAUNCH_KIND_NONE;
	if (ui->launch.process.active)
		launcher_process_terminate(&ui->launch.process, 1500U);
}
