#ifndef RG40XXV_YOUTUBE_PLAYER_CONTROLS_H
#define RG40XXV_YOUTUBE_PLAYER_CONTROLS_H

#include <SDL.h>
#include <mpv/client.h>

#include "../include/sdl_ttf_compat.h"

namespace rg40xxv_youtube {

enum class PlayerControlResult {
	none,
	return_home,
};

struct PlayerControls;

/*
 * All communication with mpv is asynchronous.  The caller must pass every
 * event returned by mpv_wait_event(..., 0.0) to consume_mpv_event().
 */
PlayerControls *player_controls_create(mpv_handle *mpv,
					       SDL_Renderer *renderer,
					       TTF_Font *font,
					       Uint32 now);
void player_controls_destroy(PlayerControls *controls);

/* Reset the per-file timeline when a new loadfile command is queued. */
void player_controls_reset(PlayerControls *controls, Uint32 now);

/* Stop held seek-repeat state when leaving PLAYER for HOME. */
void player_controls_leave_player(PlayerControls *controls);

PlayerControlResult player_controls_handle_event(PlayerControls *controls,
						 const SDL_Event &event,
						 bool controller_active,
						 Uint32 now);
void player_controls_tick(PlayerControls *controls, Uint32 now);
void player_controls_consume_mpv_event(PlayerControls *controls,
					       const mpv_event *event,
					       Uint32 now);
bool player_controls_commands_pending(const PlayerControls *controls);

/* Draw after the video texture and before SDL_RenderPresent(). */
void player_controls_draw(PlayerControls *controls, Uint32 now);
bool player_controls_overlay_visible(const PlayerControls *controls,
					     Uint32 now);

void player_controls_print_contract(void);
int player_controls_self_test(void);

} // namespace rg40xxv_youtube

#endif
