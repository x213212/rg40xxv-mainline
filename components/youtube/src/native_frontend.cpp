#include <SDL.h>
#include "sdl_ttf_compat.h"

#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern char **environ;

namespace {

struct FeedItem {
	const char *title;
	const char *subtitle;
	const char *url;
};

constexpr std::array<FeedItem, 3> kFeed {{
	{"Playback sample", "Known-good H.264/AAC transport", "https://youtu.be/GwtNiL9eEYk"},
	{"Me at the zoo", "YouTube archive", "https://youtu.be/jNQXAC9IVRw"},
	{"Player API demo", "YouTube Developers", "https://youtu.be/M7lc1UVf-VE"},
}};

bool executable_regular(const char *path)
{
	struct stat status {};
	return path != nullptr && path[0] == '/' && lstat(path, &status) == 0 &&
		S_ISREG(status.st_mode) && (status.st_mode & S_IXUSR) != 0;
}

void draw_text(SDL_Renderer *renderer, TTF_Font *font, const char *text,
	      int x, int y, SDL_Color color)
{
	SDL_Surface *surface = TTF_RenderUTF8_Blended(font, text, color);
	if (surface == nullptr)
		return;
	SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);
	if (texture != nullptr) {
		SDL_Rect destination {x, y, surface->w, surface->h};
		(void)SDL_RenderCopy(renderer, texture, nullptr, &destination);
		SDL_DestroyTexture(texture);
	}
	SDL_FreeSurface(surface);
}

int play_selected(const char *session, const FeedItem &item)
{
	if (!executable_regular(session)) {
		std::fprintf(stderr, "YOUTUBE_NATIVE_UI playback=FAIL reason=session-missing path=%s\n",
			     session != nullptr ? session : "null");
		return 66;
	}
	char *const arguments[] = {
		const_cast<char *>(session),
		const_cast<char *>(item.url),
		nullptr,
	};
	pid_t child = -1;
	const int spawn_error = posix_spawn(&child, session, nullptr, nullptr,
					    arguments, environ);
	if (spawn_error != 0) {
		std::fprintf(stderr, "YOUTUBE_NATIVE_UI playback=FAIL reason=spawn error=%s\n",
			     std::strerror(spawn_error));
		return 66;
	}
	int status = 0;
	while (waitpid(child, &status, 0) < 0) {
		if (errno != EINTR)
			return 70;
	}
	if (WIFEXITED(status))
		return WEXITSTATUS(status);
	if (WIFSIGNALED(status))
		return 128 + WTERMSIG(status);
	return 70;
}

int show_home(int selected, const char *message, int demo_ms)
{
	const bool no_controller = std::getenv("RG_YOUTUBE_NO_CONTROLLER") != nullptr;
	const char *font_path = std::getenv("RG_YOUTUBE_FONT");
	if (font_path == nullptr || font_path[0] == '\0')
		font_path = "/opt/rg40xxv/share/RG40XXV-UI-Sans.otf";
	const Uint32 sdl_flags = SDL_INIT_VIDEO | SDL_INIT_EVENTS |
		(no_controller ? 0U : SDL_INIT_GAMECONTROLLER);
	if (SDL_Init(sdl_flags) != 0) {
		std::fprintf(stderr, "YOUTUBE_NATIVE_UI init=FAIL subsystem=SDL error=%s\n",
			     SDL_GetError());
		return -66;
	}
	if (TTF_Init() != 0) {
		std::fprintf(stderr, "YOUTUBE_NATIVE_UI init=FAIL subsystem=TTF error=%s\n",
			     TTF_GetError());
		SDL_Quit();
		return -66;
	}
	TTF_Font *title_font = TTF_OpenFont(font_path, 30);
	TTF_Font *body_font = TTF_OpenFont(font_path, 20);
	TTF_Font *small_font = TTF_OpenFont(font_path, 15);
	if (title_font == nullptr || body_font == nullptr || small_font == nullptr) {
		std::fprintf(stderr, "YOUTUBE_NATIVE_UI init=FAIL subsystem=font path=%s error=%s\n",
			     font_path, TTF_GetError());
		if (title_font != nullptr) TTF_CloseFont(title_font);
		if (body_font != nullptr) TTF_CloseFont(body_font);
		if (small_font != nullptr) TTF_CloseFont(small_font);
		TTF_Quit();
		SDL_Quit();
		return -66;
	}
	const bool windowed = std::getenv("RG_YOUTUBE_WINDOWED") != nullptr;
	SDL_Window *window = SDL_CreateWindow("RG40XXV YouTube Native",
		SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 640, 480,
		windowed ? SDL_WINDOW_SHOWN : SDL_WINDOW_FULLSCREEN);
	SDL_Renderer *renderer = window != nullptr ? SDL_CreateRenderer(
		window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC) : nullptr;
	if (window != nullptr && renderer == nullptr)
		renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
	if (window == nullptr || renderer == nullptr) {
		std::fprintf(stderr, "YOUTUBE_NATIVE_UI init=FAIL subsystem=video error=%s\n",
			     SDL_GetError());
		if (renderer != nullptr) SDL_DestroyRenderer(renderer);
		if (window != nullptr) SDL_DestroyWindow(window);
		TTF_CloseFont(small_font); TTF_CloseFont(body_font); TTF_CloseFont(title_font);
		TTF_Quit(); SDL_Quit();
		return -66;
	}
	SDL_GameController *controller = nullptr;
	for (int index = 0; !no_controller && index < SDL_NumJoysticks(); ++index) {
		if (SDL_IsGameController(index)) {
			controller = SDL_GameControllerOpen(index);
			if (controller != nullptr) break;
		}
	}
	const Uint32 started = SDL_GetTicks();
	bool running = true;
	int result = -1;
	while (running) {
		SDL_Event event {};
		while (SDL_PollEvent(&event) != 0) {
			if (event.type == SDL_QUIT) { result = -2; running = false; }
			if (event.type == SDL_KEYDOWN) {
				if (event.key.keysym.sym == SDLK_UP) selected = (selected + 2) % 3;
				if (event.key.keysym.sym == SDLK_DOWN) selected = (selected + 1) % 3;
				if (event.key.keysym.sym == SDLK_RETURN || event.key.keysym.sym == SDLK_SPACE) { result = selected; running = false; }
				if (event.key.keysym.sym == SDLK_ESCAPE) { result = -2; running = false; }
			}
			if (event.type == SDL_CONTROLLERBUTTONDOWN) {
				switch (event.cbutton.button) {
				case SDL_CONTROLLER_BUTTON_DPAD_UP: selected = (selected + 2) % 3; break;
				case SDL_CONTROLLER_BUTTON_DPAD_DOWN: selected = (selected + 1) % 3; break;
				case SDL_CONTROLLER_BUTTON_A: result = selected; running = false; break;
				case SDL_CONTROLLER_BUTTON_B: result = -2; running = false; break;
				default: break; // MENU and START remain visible to the outer supervisor.
				}
			}
			if (event.type == SDL_JOYHATMOTION && controller == nullptr) {
				if ((event.jhat.value & SDL_HAT_UP) != 0) selected = (selected + 2) % 3;
				if ((event.jhat.value & SDL_HAT_DOWN) != 0) selected = (selected + 1) % 3;
			}
			if (event.type == SDL_JOYBUTTONDOWN && controller == nullptr) {
				if (event.jbutton.button == 0 || event.jbutton.button == 7) { result = selected; running = false; }
				if (event.jbutton.button == 1) { result = -2; running = false; }
			}
		}
		if (demo_ms > 0 && SDL_GetTicks() - started >= static_cast<Uint32>(demo_ms)) {
			result = -2;
			running = false;
		}
		SDL_SetRenderDrawColor(renderer, 10, 10, 12, 255);
		SDL_RenderClear(renderer);
		draw_text(renderer, title_font, "YouTube  Native", 28, 24, {245, 245, 245, 255});
		draw_text(renderer, small_font, "D-PAD  Navigate    A  Play    B  Back", 30, 67, {150, 150, 150, 255});
		for (int index = 0; index < 3; ++index) {
			SDL_Rect card {28, 106 + index * 92, 584, 76};
			if (index == selected) {
				SDL_SetRenderDrawColor(renderer, 238, 238, 238, 255);
				SDL_RenderFillRect(renderer, &card);
				draw_text(renderer, body_font, kFeed[index].title, 48, card.y + 10, {15, 15, 15, 255});
				draw_text(renderer, small_font, kFeed[index].subtitle, 48, card.y + 42, {75, 75, 75, 255});
			} else {
				SDL_SetRenderDrawColor(renderer, 65, 65, 68, 255);
				SDL_RenderDrawRect(renderer, &card);
				draw_text(renderer, body_font, kFeed[index].title, 48, card.y + 10, {210, 210, 210, 255});
				draw_text(renderer, small_font, kFeed[index].subtitle, 48, card.y + 42, {125, 125, 125, 255});
			}
		}
		if (message != nullptr && message[0] != '\0')
			draw_text(renderer, small_font, message, 30, 405, {220, 155, 80, 255});
		draw_text(renderer, small_font, "MENU + START  Exit", 455, 448, {125, 125, 125, 255});
		SDL_RenderPresent(renderer);
		SDL_Delay(8);
	}
	if (controller != nullptr) SDL_GameControllerClose(controller);
	SDL_DestroyRenderer(renderer); SDL_DestroyWindow(window);
	TTF_CloseFont(small_font); TTF_CloseFont(body_font); TTF_CloseFont(title_font);
	TTF_Quit(); SDL_Quit();
	return result;
}

} // namespace

int main(int argc, char **argv)
{
	if (argc == 2 && std::strcmp(argv[1], "--contract") == 0) {
		std::puts("YOUTUBE_NATIVE_UI_CONTRACT schema=rg40xxv-youtube-native-mvp-v1 size=640x480");
		std::puts("UP/DOWN\tnavigate");
		std::puts("A\tplay");
		std::puts("B\treturn");
		std::puts("MENU+START\texternal-exit\tnot-grabbed");
		for (const auto &item : kFeed)
			std::printf("FEED\t%s\t%s\n", item.title, item.url);
		return 0;
	}
	int demo_ms = 0;
	if (argc == 3 && std::strcmp(argv[1], "--demo-ms") == 0)
		demo_ms = std::atoi(argv[2]);
	else if (argc != 1) {
		std::fprintf(stderr, "usage: %s [--contract|--demo-ms MILLISECONDS]\n", argv[0]);
		return 64;
	}
	const char *session = std::getenv("RG_YOUTUBE_SESSION");
	if (session == nullptr || session[0] == '\0')
		session = "/opt/rg40xxv/youtube/bin/rg40xxv-youtube-native-session";
	int selected = 0;
	char message[96] = {0};
	for (;;) {
		const int choice = show_home(selected, message, demo_ms);
		if (choice == -2) return 0;
		if (choice < 0) return -choice;
		selected = choice;
		const int status = play_selected(session, kFeed[choice]);
		if (status == 0 || status == 130 || status == 143)
			message[0] = '\0';
		else
			(void)std::snprintf(message, sizeof(message), "Playback returned status %d", status);
		if (demo_ms > 0) return status;
	}
}
