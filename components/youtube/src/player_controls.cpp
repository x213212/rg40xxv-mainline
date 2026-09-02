#include "player_controls.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <vector>

namespace rg40xxv_youtube {
namespace {

constexpr Uint32 kOverlayVisibleMs = 3000;
constexpr Uint32 kSeekInitialRepeatMs = 350;
constexpr Uint32 kSeekRepeatMs = 180;
constexpr double kSeekSeconds = 10.0;
constexpr uint64_t kObservePosition = UINT64_C(0x595443010001);
constexpr uint64_t kObserveDuration = UINT64_C(0x595443010002);
constexpr uint64_t kObservePause = UINT64_C(0x595443010003);
constexpr uint64_t kPlayerCommandPrefix = UINT64_C(0x5954432000000000);
constexpr uint64_t kPlayerCommandSerialMask = UINT64_C(0x00000000ffffffff);

using CommandSink = bool (*)(void *, uint64_t, const char **);

bool deadline_reached(Uint32 now, Uint32 deadline)
{
	return static_cast<Sint32>(now - deadline) >= 0;
}

bool before_deadline(Uint32 now, Uint32 deadline)
{
	return deadline != 0 && static_cast<Sint32>(deadline - now) > 0;
}

struct FakeCommandLog {
	std::vector<std::string> commands;
	std::vector<uint64_t> replies;
};

bool fake_command(void *opaque, uint64_t reply, const char **args)
{
	auto *log = static_cast<FakeCommandLog *>(opaque);
	log->replies.push_back(reply);
	std::string command;
	for (size_t i = 0; args && args[i]; ++i) {
		if (!command.empty())
			command += '|';
		command += args[i];
	}
	log->commands.push_back(command);
	return true;
}

bool mpv_command_sink(void *opaque, uint64_t reply, const char **args)
{
	auto *mpv = static_cast<mpv_handle *>(opaque);
	const int result = mpv_command_async(mpv, reply, args);
	if (result >= 0)
		return true;
	std::fprintf(stderr,
		     "YOUTUBE_PLAYER_CONTROL command=FAIL name=%s error=%s\n",
		     args && args[0] ? args[0] : "unknown",
		     mpv_error_string(result));
	return false;
}

void format_clock(double seconds, char *out, size_t size)
{
	if (!std::isfinite(seconds) || seconds < 0.0)
		seconds = 0.0;
	const unsigned total = static_cast<unsigned>(seconds);
	const unsigned hours = total / 3600U;
	const unsigned minutes = (total / 60U) % 60U;
	const unsigned secs = total % 60U;
	if (hours)
		std::snprintf(out, size, "%u:%02u:%02u", hours, minutes, secs);
	else
		std::snprintf(out, size, "%02u:%02u", minutes, secs);
}

} // namespace

struct PlayerControls {
	mpv_handle *mpv = nullptr;
	SDL_Renderer *renderer = nullptr;
	TTF_Font *font = nullptr;
	SDL_Texture *status_texture = nullptr;
	int status_width = 0;
	int status_height = 0;
	char status_text[160] = {};
	CommandSink command_sink = nullptr;
	void *command_opaque = nullptr;
	double position = 0.0;
	double duration = 0.0;
	bool position_known = false;
	bool duration_known = false;
	bool paused = false;
	bool pause_known = false;
	bool last_logged_pause = false;
	bool pause_logged = false;
	int last_logged_second = -1;
	bool keyboard_left = false;
	bool keyboard_right = false;
	bool controller_left = false;
	bool controller_right = false;
	bool joystick_left = false;
	bool joystick_right = false;
	int held_seek_direction = 0;
	Uint32 next_seek_repeat = 0;
	Uint32 overlay_until = 0;
	uint64_t next_command_serial = 1;
	std::vector<uint64_t> pending_commands;
};

namespace {

void show_overlay(PlayerControls *controls, Uint32 now)
{
	controls->overlay_until = now + kOverlayVisibleMs;
}

bool queue(PlayerControls *controls, const char **args)
{
	if (!controls || !controls->command_sink)
		return false;
	uint64_t serial = controls->next_command_serial++ &
			  kPlayerCommandSerialMask;
	if (serial == 0)
		serial = controls->next_command_serial++ &
			 kPlayerCommandSerialMask;
	const uint64_t reply = kPlayerCommandPrefix | serial;
	if (!controls->command_sink(controls->command_opaque, reply, args))
		return false;
	controls->pending_commands.push_back(reply);
	return true;
}

void seek_once(PlayerControls *controls, int direction, Uint32 now,
	       const char *source)
{
	const char *seconds = direction < 0 ? "-10" : "+10";
	const char *args[] = {"seek", seconds, "relative+keyframes", nullptr};
	if (!queue(controls, args))
		return;
	if (controls->position_known) {
		controls->position += direction * kSeekSeconds;
		controls->position = std::max(0.0, controls->position);
		if (controls->duration_known)
			controls->position = std::min(controls->position,
						      controls->duration);
	}
	show_overlay(controls, now);
	std::printf("YOUTUBE_PLAYER_CONTROL action=SEEK seconds=%s source=%s async=1\n",
		    seconds, source);
}

int requested_seek_direction(const PlayerControls *controls)
{
	const bool right = controls->keyboard_right || controls->controller_right ||
			   controls->joystick_right;
	const bool left = controls->keyboard_left || controls->controller_left ||
			  controls->joystick_left;
	if (right == left)
		return 0;
	return right ? 1 : -1;
}

void refresh_seek_hold(PlayerControls *controls, Uint32 now,
		       const char *source)
{
	const int requested = requested_seek_direction(controls);
	if (requested == controls->held_seek_direction)
		return;
	controls->held_seek_direction = requested;
	if (!requested) {
		controls->next_seek_repeat = 0;
		return;
	}
	seek_once(controls, requested, now, source);
	controls->next_seek_repeat = now + kSeekInitialRepeatMs;
}

void toggle_pause(PlayerControls *controls, Uint32 now, const char *source)
{
	const char *args[] = {"cycle", "pause", nullptr};
	if (!queue(controls, args))
		return;
	/* Property observation is authoritative; this optimistic state makes the
	 * overlay respond in the same frame as the button press. */
	controls->paused = !controls->paused;
	controls->pause_known = true;
	show_overlay(controls, now);
	std::printf("YOUTUBE_PLAYER_CONTROL action=%s source=%s async=1\n",
		    controls->paused ? "PAUSE" : "RESUME", source);
}

void clear_holds(PlayerControls *controls)
{
	controls->keyboard_left = false;
	controls->keyboard_right = false;
	controls->controller_left = false;
	controls->controller_right = false;
	controls->joystick_left = false;
	controls->joystick_right = false;
	controls->held_seek_direction = 0;
	controls->next_seek_repeat = 0;
}

void update_status_texture(PlayerControls *controls)
{
	if (!controls->renderer || !controls->font)
		return;
	char position[24];
	char duration[24];
	format_clock(controls->position_known ? controls->position : 0.0,
		     position, sizeof(position));
	format_clock(controls->duration_known ? controls->duration : 0.0,
		     duration, sizeof(duration));
	char next[sizeof(controls->status_text)];
	std::snprintf(next, sizeof(next), "%s  %s / %s   A %s   LEFT/RIGHT 10s   B BACK",
		      controls->paused ? "PAUSED" : "PLAYING", position,
		      controls->duration_known ? duration : "--:--",
		      controls->paused ? "RESUME" : "PAUSE");
	if (std::strcmp(next, controls->status_text) == 0)
		return;
	const SDL_Color white {245, 245, 247, 255};
	SDL_Surface *surface = TTF_RenderUTF8_Blended(controls->font, next, white);
	if (!surface) {
		std::fprintf(stderr,
			     "YOUTUBE_PLAYER_CONTROL overlay-text=FAIL error=%s\n",
			     TTF_GetError());
		return;
	}
	SDL_Texture *texture = SDL_CreateTextureFromSurface(controls->renderer,
							 surface);
	if (!texture) {
		SDL_FreeSurface(surface);
		return;
	}
	(void)SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
	if (controls->status_texture)
		SDL_DestroyTexture(controls->status_texture);
	controls->status_texture = texture;
	controls->status_width = surface->w;
	controls->status_height = surface->h;
	std::strncpy(controls->status_text, next,
		     sizeof(controls->status_text) - 1);
	controls->status_text[sizeof(controls->status_text) - 1] = 0;
	SDL_FreeSurface(surface);
}

} // namespace

PlayerControls *player_controls_create(mpv_handle *mpv,
					       SDL_Renderer *renderer,
					       TTF_Font *font,
					       Uint32 now)
{
	if (!mpv || !renderer || !font)
		return nullptr;
	auto *controls = new (std::nothrow) PlayerControls;
	if (!controls)
		return nullptr;
	controls->mpv = mpv;
	controls->renderer = renderer;
	controls->font = font;
	controls->command_sink = mpv_command_sink;
	controls->command_opaque = mpv;
	const int position = mpv_observe_property(mpv, kObservePosition,
						  "time-pos", MPV_FORMAT_DOUBLE);
	const int duration = mpv_observe_property(mpv, kObserveDuration,
						  "duration", MPV_FORMAT_DOUBLE);
	const int pause = mpv_observe_property(mpv, kObservePause, "pause",
					       MPV_FORMAT_FLAG);
	if (position < 0 || duration < 0 || pause < 0) {
		(void)mpv_unobserve_property(mpv, kObservePosition);
		(void)mpv_unobserve_property(mpv, kObserveDuration);
		(void)mpv_unobserve_property(mpv, kObservePause);
		delete controls;
		return nullptr;
	}
	show_overlay(controls, now);
	std::puts("YOUTUBE_PLAYER_CONTROL READY properties=time-pos+duration+pause commands=async");
	return controls;
}

void player_controls_destroy(PlayerControls *controls)
{
	if (!controls)
		return;
	if (controls->mpv) {
		(void)mpv_unobserve_property(controls->mpv, kObservePosition);
		(void)mpv_unobserve_property(controls->mpv, kObserveDuration);
		(void)mpv_unobserve_property(controls->mpv, kObservePause);
	}
	if (controls->status_texture)
		SDL_DestroyTexture(controls->status_texture);
	delete controls;
}

void player_controls_reset(PlayerControls *controls, Uint32 now)
{
	if (!controls)
		return;
	controls->position = 0.0;
	controls->duration = 0.0;
	controls->position_known = false;
	controls->duration_known = false;
	controls->paused = false;
	controls->pause_known = false;
	controls->pause_logged = false;
	controls->last_logged_second = -1;
	controls->status_text[0] = 0;
	clear_holds(controls);
	show_overlay(controls, now);
}

void player_controls_leave_player(PlayerControls *controls)
{
	if (!controls)
		return;
	clear_holds(controls);
	controls->overlay_until = 0;
}

PlayerControlResult player_controls_handle_event(PlayerControls *controls,
						 const SDL_Event &event,
						 bool controller_active,
						 Uint32 now)
{
	if (!controls)
		return PlayerControlResult::none;
	if (event.type == SDL_KEYDOWN && !event.key.repeat &&
	    !controller_active) {
		switch (event.key.keysym.sym) {
		case SDLK_RETURN:
		case SDLK_SPACE:
		case SDLK_a:
			toggle_pause(controls, now, "keyboard");
			break;
		case SDLK_ESCAPE:
		case SDLK_b:
			player_controls_leave_player(controls);
			return PlayerControlResult::return_home;
		case SDLK_LEFT:
			controls->keyboard_left = true;
			refresh_seek_hold(controls, now, "keyboard");
			break;
		case SDLK_RIGHT:
			controls->keyboard_right = true;
			refresh_seek_hold(controls, now, "keyboard");
			break;
		default:
			break;
		}
	} else if (event.type == SDL_KEYUP && !event.key.repeat &&
		   !controller_active) {
		if (event.key.keysym.sym == SDLK_LEFT)
			controls->keyboard_left = false;
		else if (event.key.keysym.sym == SDLK_RIGHT)
			controls->keyboard_right = false;
		refresh_seek_hold(controls, now, "keyboard");
	} else if (event.type == SDL_CONTROLLERBUTTONDOWN) {
		if (event.cbutton.button == SDL_CONTROLLER_BUTTON_A)
			toggle_pause(controls, now, "controller");
		else if (event.cbutton.button == SDL_CONTROLLER_BUTTON_B) {
			player_controls_leave_player(controls);
			return PlayerControlResult::return_home;
		} else if (event.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_LEFT) {
			controls->controller_left = true;
			refresh_seek_hold(controls, now, "controller");
		} else if (event.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) {
			controls->controller_right = true;
			refresh_seek_hold(controls, now, "controller");
		}
	} else if (event.type == SDL_CONTROLLERBUTTONUP) {
		if (event.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_LEFT)
			controls->controller_left = false;
		else if (event.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT)
			controls->controller_right = false;
		refresh_seek_hold(controls, now, "controller");
	} else if (event.type == SDL_JOYBUTTONDOWN && !controller_active) {
		if (event.jbutton.button == 1)
			toggle_pause(controls, now, "joystick");
		else if (event.jbutton.button == 0) {
			player_controls_leave_player(controls);
			return PlayerControlResult::return_home;
		}
	} else if (event.type == SDL_JOYHATMOTION && !controller_active) {
		controls->joystick_left = (event.jhat.value & SDL_HAT_LEFT) != 0;
		controls->joystick_right = (event.jhat.value & SDL_HAT_RIGHT) != 0;
		refresh_seek_hold(controls, now, "joystick-hat");
	}
	return PlayerControlResult::none;
}

void player_controls_tick(PlayerControls *controls, Uint32 now)
{
	if (!controls || !controls->held_seek_direction ||
	    !controls->next_seek_repeat ||
	    !deadline_reached(now, controls->next_seek_repeat))
		return;
	seek_once(controls, controls->held_seek_direction, now, "repeat");
	controls->next_seek_repeat = now + kSeekRepeatMs;
}

void player_controls_consume_mpv_event(PlayerControls *controls,
					       const mpv_event *event,
					       Uint32 now)
{
	if (!controls || !event)
		return;
	if (event->event_id == MPV_EVENT_COMMAND_REPLY) {
		const auto pending = std::find(controls->pending_commands.begin(),
					       controls->pending_commands.end(),
					       event->reply_userdata);
		if (pending != controls->pending_commands.end()) {
			controls->pending_commands.erase(pending);
			if (event->error < 0)
				std::fprintf(stderr,
					     "YOUTUBE_PLAYER_CONTROL reply=FAIL id=%llu error=%s\n",
					     static_cast<unsigned long long>(event->reply_userdata),
					     mpv_error_string(event->error));
		}
		return;
	}
	if (event->event_id != MPV_EVENT_PROPERTY_CHANGE || !event->data)
		return;
	const auto *property = static_cast<const mpv_event_property *>(event->data);
	if (!property->name)
		return;
	if (std::strcmp(property->name, "time-pos") == 0) {
		controls->position_known = property->format == MPV_FORMAT_DOUBLE &&
					   property->data;
		if (controls->position_known)
			controls->position =
				std::max(0.0, *static_cast<double *>(property->data));
		if (controls->position_known) {
			const int second = static_cast<int>(controls->position);
			if (controls->last_logged_second < 0 ||
			    second < controls->last_logged_second ||
			    second >= controls->last_logged_second + 2) {
				controls->last_logged_second = second;
				std::printf("YOUTUBE_PLAYER_TIMELINE position=%d advancing=1\n",
					    second);
			}
		}
	} else if (std::strcmp(property->name, "duration") == 0) {
		controls->duration_known = property->format == MPV_FORMAT_DOUBLE &&
					   property->data;
		if (controls->duration_known)
			controls->duration =
				std::max(0.0, *static_cast<double *>(property->data));
	} else if (std::strcmp(property->name, "pause") == 0) {
		controls->pause_known = property->format == MPV_FORMAT_FLAG &&
					property->data;
		if (controls->pause_known)
			controls->paused =
				*static_cast<int *>(property->data) != 0;
		if (controls->pause_known &&
		    (!controls->pause_logged ||
		     controls->last_logged_pause != controls->paused)) {
			controls->last_logged_pause = controls->paused;
			controls->pause_logged = true;
			std::printf("YOUTUBE_PLAYER_STATE paused=%d observed=1\n",
				    controls->paused ? 1 : 0);
		}
		if (controls->paused)
			show_overlay(controls, now);
	}
}

bool player_controls_commands_pending(const PlayerControls *controls)
{
	return controls && !controls->pending_commands.empty();
}

bool player_controls_overlay_visible(const PlayerControls *controls,
					     Uint32 now)
{
	return controls && (controls->paused ||
			    before_deadline(now, controls->overlay_until));
}

void player_controls_draw(PlayerControls *controls, Uint32 now)
{
	if (!player_controls_overlay_visible(controls, now) ||
	    !controls->renderer)
		return;
	SDL_BlendMode previous_blend = SDL_BLENDMODE_NONE;
	(void)SDL_GetRenderDrawBlendMode(controls->renderer, &previous_blend);
	(void)SDL_SetRenderDrawBlendMode(controls->renderer, SDL_BLENDMODE_BLEND);
	SDL_Rect panel {0, 390, 640, 90};
	SDL_SetRenderDrawColor(controls->renderer, 7, 8, 12, 220);
	SDL_RenderFillRect(controls->renderer, &panel);
	SDL_Rect track {28, 407, 584, 8};
	SDL_SetRenderDrawColor(controls->renderer, 105, 108, 117, 255);
	SDL_RenderFillRect(controls->renderer, &track);
	double fraction = 0.0;
	if (controls->duration_known && controls->duration > 0.0 &&
	    controls->position_known)
		fraction = std::clamp(controls->position / controls->duration,
				      0.0, 1.0);
	SDL_Rect elapsed {track.x, track.y,
			  static_cast<int>(track.w * fraction), track.h};
	SDL_SetRenderDrawColor(controls->renderer, 230, 31, 48, 255);
	if (elapsed.w > 0)
		SDL_RenderFillRect(controls->renderer, &elapsed);
	SDL_Rect knob {track.x + std::max(0, elapsed.w - 4), track.y - 4, 8, 16};
	SDL_RenderFillRect(controls->renderer, &knob);
	update_status_texture(controls);
	if (controls->status_texture) {
		const float scale = controls->status_width > 600
					? 600.0f / controls->status_width : 1.0f;
		SDL_Rect text {(640 - static_cast<int>(controls->status_width * scale)) / 2,
			       432, static_cast<int>(controls->status_width * scale),
			       static_cast<int>(controls->status_height * scale)};
		SDL_RenderCopy(controls->renderer, controls->status_texture,
			       nullptr, &text);
	}
	(void)SDL_SetRenderDrawBlendMode(controls->renderer, previous_blend);
}

void player_controls_print_contract(void)
{
	std::puts("YOUTUBE_PLAYER_CONTROLS_CONTRACT schema=rg40xxv-youtube-player-controls-v1");
	std::puts("A\tPLAYER=pause-toggle\tmpv=command-async");
	std::puts("LEFT_RIGHT\tseek=-10/+10\trepeat=350ms-then-180ms\tmpv=command-async");
	std::puts("B\tPLAYER->HOME\tresult=return-home");
	std::puts("TIMELINE\tproperties=time-pos+duration+pause\ttransport=mpv-observe-property");
	std::puts("OVERLAY\tprogress=current/duration\tvisibility=3s-or-paused\trender=before-present");
}

int player_controls_self_test(void)
{
	PlayerControls controls {};
	FakeCommandLog log;
	controls.command_sink = fake_command;
	controls.command_opaque = &log;
	show_overlay(&controls, 1000);

	SDL_Event event {};
	event.type = SDL_CONTROLLERBUTTONDOWN;
	event.cbutton.button = SDL_CONTROLLER_BUTTON_A;
	if (player_controls_handle_event(&controls, event, true, 1000) !=
		PlayerControlResult::none || log.commands.size() != 1 ||
	    log.commands.back() != "cycle|pause" || !controls.paused)
		return 1;

	event = {};
	event.type = SDL_CONTROLLERBUTTONDOWN;
	event.cbutton.button = SDL_CONTROLLER_BUTTON_DPAD_LEFT;
	(void)player_controls_handle_event(&controls, event, true, 1100);
	if (log.commands.size() != 2 ||
	    log.commands.back() != "seek|-10|relative+keyframes")
		return 2;
	player_controls_tick(&controls, 1449);
	if (log.commands.size() != 2)
		return 3;
	player_controls_tick(&controls, 1450);
	if (log.commands.size() != 3 ||
	    log.commands.back() != "seek|-10|relative+keyframes")
		return 4;
	event.type = SDL_CONTROLLERBUTTONUP;
	(void)player_controls_handle_event(&controls, event, true, 1460);
	player_controls_tick(&controls, 2000);
	if (log.commands.size() != 3)
		return 5;
	if (log.replies.size() != 3 || log.replies[0] == log.replies[1] ||
	    log.replies[0] == log.replies[2] || log.replies[1] == log.replies[2] ||
	    !player_controls_commands_pending(&controls))
		return 5;
	for (const uint64_t reply : log.replies) {
		mpv_event command_reply {};
		command_reply.event_id = MPV_EVENT_COMMAND_REPLY;
		command_reply.reply_userdata = reply;
		player_controls_consume_mpv_event(&controls, &command_reply, 2000);
	}
	if (player_controls_commands_pending(&controls))
		return 5;

	double position = 65.9;
	double duration = 125.0;
	int paused = 0;
	mpv_event_property position_property {
		"time-pos", MPV_FORMAT_DOUBLE, &position
	};
	mpv_event position_event {};
	position_event.event_id = MPV_EVENT_PROPERTY_CHANGE;
	position_event.data = &position_property;
	player_controls_consume_mpv_event(&controls, &position_event, 2100);
	mpv_event_property duration_property {
		"duration", MPV_FORMAT_DOUBLE, &duration
	};
	mpv_event duration_event {};
	duration_event.event_id = MPV_EVENT_PROPERTY_CHANGE;
	duration_event.data = &duration_property;
	player_controls_consume_mpv_event(&controls, &duration_event, 2100);
	mpv_event_property pause_property {"pause", MPV_FORMAT_FLAG, &paused};
	mpv_event pause_event {};
	pause_event.event_id = MPV_EVENT_PROPERTY_CHANGE;
	pause_event.data = &pause_property;
	player_controls_consume_mpv_event(&controls, &pause_event, 2100);
	if (!controls.position_known || controls.position != position ||
	    !controls.duration_known || controls.duration != duration ||
	    controls.paused)
		return 6;
	if (!player_controls_overlay_visible(&controls, 4449) ||
	    player_controls_overlay_visible(&controls, 4450))
		return 7;

	event = {};
	event.type = SDL_CONTROLLERBUTTONDOWN;
	event.cbutton.button = SDL_CONTROLLER_BUTTON_B;
	if (player_controls_handle_event(&controls, event, true, 4500) !=
	    PlayerControlResult::return_home)
		return 8;

	char clock[24];
	format_clock(3661.0, clock, sizeof(clock));
	if (std::strcmp(clock, "1:01:01") != 0)
		return 9;
	std::puts("YOUTUBE_PLAYER_CONTROLS_SELF_TEST PASS pause=async seek=repeat properties=observed overlay=timed back=home");
	return 0;
}

} // namespace rg40xxv_youtube
