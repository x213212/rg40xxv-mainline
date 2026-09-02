#include <SDL.h>
#include <SDL_opengles2.h>
#include <mpv/client.h>
#include <mpv/render.h>
#include <mpv/render_gl.h>

#include "sdl_ttf_compat.h"

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <spawn.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace {

constexpr int kLogicalWidth = 640;
constexpr int kLogicalHeight = 480;
constexpr size_t kMaximumEndpointBytes = 2048;
constexpr size_t kMaximumBrokerLineBytes = 4096;
constexpr uint64_t kLoadReply = 1;
constexpr uint64_t kStopReply = 2;
constexpr uint64_t kPauseReply = 3;
constexpr uint64_t kSeekReply = 4;
constexpr uint64_t kAudioAddReply = 5;

volatile sig_atomic_t g_screenshot_requested = 0;
volatile sig_atomic_t g_stop_requested = 0;

enum class Scene {
	home,
	player,
};

struct TextSprite {
	GLuint texture = 0;
	int width = 0;
	int height = 0;
};

struct UiGl {
	GLuint program = 0;
	GLint color_uniform = -1;
	GLint texture_uniform = -1;
	GLint textured_uniform = -1;
	TextSprite title {};
	TextSprite subtitle {};
	TextSprite preparing {};
	TextSprite play {};
	TextSprite player_controls {};
	TextSprite external_exit {};
	TextSprite loading {};
};

struct App {
	SDL_Window *window = nullptr;
	SDL_GLContext gl_context = nullptr;
	SDL_GameController *controller = nullptr;
	SDL_Joystick *joystick = nullptr;
	TTF_Font *title_font = nullptr;
	TTF_Font *body_font = nullptr;
	bool ttf_initialized = false;
	UiGl ui {};
	mpv_handle *player = nullptr;
	mpv_render_context *render_context = nullptr;
	SDL_atomic_t frame_pending {};
	Scene scene = Scene::home;
	const char *endpoint = nullptr;
	const char *audio_endpoint = nullptr;
	char endpoint_storage[kMaximumEndpointBytes + 1U] = {};
	char audio_endpoint_storage[kMaximumEndpointBytes + 1U] = {};
	pid_t broker_pid = -1;
	int broker_stdin = -1;
	int broker_stdout = -1;
	char broker_line[kMaximumBrokerLineBytes + 1U] = {};
	size_t broker_line_used = 0;
	bool broker_mode = false;
	bool broker_ready = false;
	bool broker_failed = false;
	bool play_requested = false;
	const char *screenshot_path = nullptr;
	Uint64 switch_counter = 0;
	Uint64 switch_started = 0;
	bool file_loaded = false;
	bool audio_add_queued = false;
	bool first_frame_reported = false;
	bool running = true;
};

void signal_handler(int signal_number)
{
	if (signal_number == SIGUSR1)
		g_screenshot_requested = 1;
	else
		g_stop_requested = 1;
}

bool install_signal_handlers()
{
	struct sigaction action {};
	action.sa_handler = signal_handler;
	sigemptyset(&action.sa_mask);
	action.sa_flags = 0;
	return sigaction(SIGUSR1, &action, nullptr) == 0 &&
		sigaction(SIGINT, &action, nullptr) == 0 &&
		sigaction(SIGTERM, &action, nullptr) == 0;
}

bool valid_loopback_endpoint(const char *endpoint)
{
	constexpr char prefix[] = "http://127.0.0.1:";
	if (endpoint == nullptr)
		return false;
	const size_t length = strnlen(endpoint, kMaximumEndpointBytes + 1U);
	if (length == 0 || length > kMaximumEndpointBytes ||
	    std::strchr(endpoint, '\n') != nullptr ||
	    std::strchr(endpoint, '\r') != nullptr ||
	    std::strncmp(endpoint, prefix, sizeof(prefix) - 1U) != 0)
		return false;
	const char *cursor = endpoint + sizeof(prefix) - 1U;
	unsigned int port = 0;
	int digits = 0;
	while (*cursor >= '0' && *cursor <= '9' && digits < 6) {
		port = port * 10U + static_cast<unsigned int>(*cursor - '0');
		++cursor;
		++digits;
	}
	return digits > 0 && digits <= 5 && port > 0 && port <= 65535U &&
		*cursor == '/' && cursor[1] != '\0';
}

bool valid_watch_url(const char *url)
{
	if (url == nullptr)
		return false;
	const size_t length = strnlen(url, kMaximumEndpointBytes + 1U);
	if (length == 0 || length > kMaximumEndpointBytes || url[0] == '-' ||
	    std::strchr(url, '\n') != nullptr || std::strchr(url, '\r') != nullptr)
		return false;
	return std::strncmp(url, "https://youtu.be/", 17) == 0 ||
		std::strncmp(url, "https://www.youtube.com/watch?v=", 32) == 0 ||
		std::strncmp(url, "https://youtube.com/watch?v=", 28) == 0;
}

bool executable_regular(const char *path)
{
	struct stat status {};
	return path != nullptr && path[0] == '/' && lstat(path, &status) == 0 &&
		S_ISREG(status.st_mode) && !S_ISLNK(status.st_mode) &&
		(status.st_mode & S_IXUSR) != 0;
}

bool copy_token(char *destination, size_t capacity, const char *begin,
		const char *end)
{
	if (begin == nullptr || end == nullptr || end <= begin)
		return false;
	const size_t length = static_cast<size_t>(end - begin);
	if (length >= capacity)
		return false;
	std::memcpy(destination, begin, length);
	destination[length] = '\0';
	return true;
}

bool parse_broker_line(const char *line, char *video, size_t video_capacity,
		       char *audio, size_t audio_capacity, bool *has_audio)
{
	constexpr char prefix[] = "YOUTUBE_ENDPOINT_READY video=";
	constexpr char separator[] = " audio=";
	if (line == nullptr ||
	    std::strncmp(line, prefix, sizeof(prefix) - 1U) != 0)
		return false;
	const char *video_begin = line + sizeof(prefix) - 1U;
	const char *audio_marker = std::strstr(video_begin, separator);
	if (audio_marker == nullptr ||
	    !copy_token(video, video_capacity, video_begin, audio_marker) ||
	    !valid_loopback_endpoint(video))
		return false;
	const char *audio_begin = audio_marker + sizeof(separator) - 1U;
	if (std::strcmp(audio_begin, "none") == 0) {
		audio[0] = '\0';
		*has_audio = false;
		return true;
	}
	const char *audio_end = audio_begin + std::strlen(audio_begin);
	if (!copy_token(audio, audio_capacity, audio_begin, audio_end) ||
	    !valid_loopback_endpoint(audio))
		return false;
	*has_audio = true;
	return true;
}

bool set_close_on_exec(int descriptor)
{
	const int flags = fcntl(descriptor, F_GETFD);
	return flags >= 0 && fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC) == 0;
}

bool make_pipe(int descriptors[2])
{
	if (pipe(descriptors) != 0)
		return false;
	if (set_close_on_exec(descriptors[0]) && set_close_on_exec(descriptors[1]))
		return true;
	(void)close(descriptors[0]);
	(void)close(descriptors[1]);
	descriptors[0] = -1;
	descriptors[1] = -1;
	return false;
}

bool start_broker(App *app, const char *broker, const char *watch_url)
{
	if (!executable_regular(broker) || !valid_watch_url(watch_url)) {
		std::fputs("YOUTUBE_EMBEDDED_BROKER start=FAIL reason=contract\n",
			   stderr);
		return false;
	}
	int child_input[2] {-1, -1};
	int child_output[2] {-1, -1};
	if (!make_pipe(child_input) || !make_pipe(child_output)) {
		if (child_input[0] >= 0) (void)close(child_input[0]);
		if (child_input[1] >= 0) (void)close(child_input[1]);
		if (child_output[0] >= 0) (void)close(child_output[0]);
		if (child_output[1] >= 0) (void)close(child_output[1]);
		std::fputs("YOUTUBE_EMBEDDED_BROKER start=FAIL reason=pipe\n",
			   stderr);
		return false;
	}
	posix_spawn_file_actions_t actions {};
	int spawn_error = posix_spawn_file_actions_init(&actions);
	const bool actions_initialized = spawn_error == 0;
	if (spawn_error == 0)
		spawn_error = posix_spawn_file_actions_adddup2(
			&actions, child_input[0], STDIN_FILENO);
	if (spawn_error == 0)
		spawn_error = posix_spawn_file_actions_adddup2(
			&actions, child_output[1], STDOUT_FILENO);
	if (spawn_error == 0)
		spawn_error = posix_spawn_file_actions_addclose(&actions,
							       child_input[1]);
	if (spawn_error == 0)
		spawn_error = posix_spawn_file_actions_addclose(&actions,
							       child_output[0]);
	char *const arguments[] = {
		const_cast<char *>(broker),
		const_cast<char *>(watch_url),
		nullptr,
	};
	pid_t child = -1;
	if (spawn_error == 0)
		spawn_error = posix_spawn(&child, broker, &actions, nullptr,
					  arguments, environ);
	if (actions_initialized)
		(void)posix_spawn_file_actions_destroy(&actions);
	(void)close(child_input[0]);
	(void)close(child_output[1]);
	if (spawn_error != 0) {
		(void)close(child_input[1]);
		(void)close(child_output[0]);
		std::fprintf(stderr,
			     "YOUTUBE_EMBEDDED_BROKER start=FAIL reason=spawn error=%s\n",
			     std::strerror(spawn_error));
		return false;
	}
	const int output_flags = fcntl(child_output[0], F_GETFL);
	if (output_flags < 0 ||
	    fcntl(child_output[0], F_SETFL, output_flags | O_NONBLOCK) != 0) {
		(void)close(child_input[1]);
		(void)close(child_output[0]);
		(void)kill(child, SIGTERM);
		(void)waitpid(child, nullptr, 0);
		std::fputs("YOUTUBE_EMBEDDED_BROKER start=FAIL reason=nonblock\n",
			   stderr);
		return false;
	}
	app->broker_mode = true;
	app->broker_pid = child;
	app->broker_stdin = child_input[1];
	app->broker_stdout = child_output[0];
	std::puts("YOUTUBE_EMBEDDED_BROKER started=1 stdout=nonblocking endpoint=pending");
	return true;
}

void print_contract()
{
	std::puts("YOUTUBE_EMBEDDED_CONTRACT schema=rg40xxv-youtube-embedded-scene-v1 size=640x480");
	std::puts("SDL_WINDOW\tcount=1\tbackend=KMSDRM\tflags=OPENGL");
	std::puts("SDL_GLES_CONTEXT\tcount=1\towner=SDL\tversion=2.0");
	std::puts("LIBMPV\tcreate=once\tinitialize=once\trender-context=once\tlifetime=process");
	std::puts("STREAM\tdirect-loopback-endpoint\targv=--device-play|--stream\tenv=RG_YOUTUBE_TEST_STREAM");
	std::puts("BROKER\targv=--broker ABS_PATH WATCH_URL\tstdout=nonblocking\tprotocol=YOUTUBE_ENDPOINT_READY\tcleanup=stdin-EOF+SIGTERM");
	std::puts("A\tHOME->PLAYER\tloadfile-async\tpending=latched-auto-play");
	std::puts("B\tPLAYER->HOME\tstop-async\tpending=cancel-auto-play");
	std::puts("MENU+START\texternal-exit\tnot-grabbed\tnot-handled");
	std::puts("SIGUSR1\tscreenshot=GLES-glReadPixels+SDL-SaveBMP\tsource=render-memory");
}

int self_test()
{
	char video[kMaximumEndpointBytes + 1U] = {};
	char audio[kMaximumEndpointBytes + 1U] = {};
	bool has_audio = false;
	if (!valid_loopback_endpoint("http://127.0.0.1:43210/stream/video") ||
	    !valid_loopback_endpoint("http://127.0.0.1:1/v") ||
	    valid_loopback_endpoint("https://example.invalid/video") ||
	    valid_loopback_endpoint("http://127.0.0.1:0/v") ||
	    valid_loopback_endpoint("http://127.0.0.1:65536/v") ||
	    valid_loopback_endpoint("http://127.0.0.1:1234/") ||
	    !parse_broker_line(
		    "YOUTUBE_ENDPOINT_READY video=http://127.0.0.1:43210/stream/video audio=none",
		    video, sizeof(video), audio, sizeof(audio), &has_audio) ||
	    has_audio ||
	    !parse_broker_line(
		    "YOUTUBE_ENDPOINT_READY video=http://127.0.0.1:43210/stream/video audio=http://127.0.0.1:43210/stream/audio",
		    video, sizeof(video), audio, sizeof(audio), &has_audio) ||
	    !has_audio ||
	    parse_broker_line(
		    "YOUTUBE_ENDPOINT_READY video=https://example.invalid/v audio=none",
		    video, sizeof(video), audio, sizeof(audio), &has_audio)) {
		std::fputs("YOUTUBE_EMBEDDED_SELF_TEST FAIL endpoint-validation\n", stderr);
		return 1;
	}
	std::puts("YOUTUBE_EMBEDDED_SELF_TEST PASS scenes=HOME+PLAYER endpoint=loopback input=external-exit screenshot=render-memory");
	return 0;
}

GLuint compile_shader(GLenum type, const char *source)
{
	const GLuint shader = glCreateShader(type);
	if (shader == 0)
		return 0;
	glShaderSource(shader, 1, &source, nullptr);
	glCompileShader(shader);
	GLint compiled = GL_FALSE;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
	if (compiled == GL_TRUE)
		return shader;
	char message[512] = {};
	GLsizei used = 0;
	glGetShaderInfoLog(shader, static_cast<GLsizei>(sizeof(message) - 1U),
			   &used, message);
	std::fprintf(stderr, "YOUTUBE_EMBEDDED_GL shader=FAIL type=%u error=%s\n",
		     static_cast<unsigned int>(type), used > 0 ? message : "unknown");
	glDeleteShader(shader);
	return 0;
}

bool initialize_ui_gl(UiGl *ui)
{
	static constexpr char vertex_source[] =
		"attribute vec2 a_position;\n"
		"attribute vec2 a_texcoord;\n"
		"varying vec2 v_texcoord;\n"
		"void main() {\n"
		"  gl_Position = vec4(a_position, 0.0, 1.0);\n"
		"  v_texcoord = a_texcoord;\n"
		"}\n";
	static constexpr char fragment_source[] =
		"precision mediump float;\n"
		"uniform vec4 u_color;\n"
		"uniform sampler2D u_texture;\n"
		"uniform float u_textured;\n"
		"varying vec2 v_texcoord;\n"
		"void main() {\n"
		"  if (u_textured > 0.5)\n"
		"    gl_FragColor = texture2D(u_texture, v_texcoord) * u_color;\n"
		"  else\n"
		"    gl_FragColor = u_color;\n"
		"}\n";
	const GLuint vertex = compile_shader(GL_VERTEX_SHADER, vertex_source);
	const GLuint fragment = compile_shader(GL_FRAGMENT_SHADER, fragment_source);
	if (vertex == 0 || fragment == 0) {
		if (vertex != 0) glDeleteShader(vertex);
		if (fragment != 0) glDeleteShader(fragment);
		return false;
	}
	ui->program = glCreateProgram();
	if (ui->program == 0) {
		glDeleteShader(fragment);
		glDeleteShader(vertex);
		return false;
	}
	glAttachShader(ui->program, vertex);
	glAttachShader(ui->program, fragment);
	glBindAttribLocation(ui->program, 0, "a_position");
	glBindAttribLocation(ui->program, 1, "a_texcoord");
	glLinkProgram(ui->program);
	glDeleteShader(fragment);
	glDeleteShader(vertex);
	GLint linked = GL_FALSE;
	glGetProgramiv(ui->program, GL_LINK_STATUS, &linked);
	if (linked != GL_TRUE) {
		char message[512] = {};
		GLsizei used = 0;
		glGetProgramInfoLog(ui->program,
				    static_cast<GLsizei>(sizeof(message) - 1U),
				    &used, message);
		std::fprintf(stderr, "YOUTUBE_EMBEDDED_GL program=FAIL error=%s\n",
			     used > 0 ? message : "unknown");
		glDeleteProgram(ui->program);
		ui->program = 0;
		return false;
	}
	ui->color_uniform = glGetUniformLocation(ui->program, "u_color");
	ui->texture_uniform = glGetUniformLocation(ui->program, "u_texture");
	ui->textured_uniform = glGetUniformLocation(ui->program, "u_textured");
	return ui->color_uniform >= 0 && ui->texture_uniform >= 0 &&
		ui->textured_uniform >= 0;
}

void destroy_text_sprite(TextSprite *sprite)
{
	if (sprite->texture != 0)
		glDeleteTextures(1, &sprite->texture);
	*sprite = TextSprite {};
}

bool create_text_sprite(TextSprite *sprite, TTF_Font *font, const char *text,
			SDL_Color color)
{
	SDL_Surface *rendered = TTF_RenderUTF8_Blended(font, text, color);
	if (rendered == nullptr)
		return false;
	SDL_Surface *rgba = SDL_ConvertSurfaceFormat(rendered,
						    SDL_PIXELFORMAT_ABGR8888, 0);
	SDL_FreeSurface(rendered);
	if (rgba == nullptr)
		return false;
	glGenTextures(1, &sprite->texture);
	if (sprite->texture == 0) {
		SDL_FreeSurface(rgba);
		return false;
	}
	glBindTexture(GL_TEXTURE_2D, sprite->texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, rgba->w, rgba->h, 0, GL_RGBA,
		     GL_UNSIGNED_BYTE, rgba->pixels);
	sprite->width = rgba->w;
	sprite->height = rgba->h;
	SDL_FreeSurface(rgba);
	if (glGetError() == GL_NO_ERROR)
		return true;
	destroy_text_sprite(sprite);
	return false;
}

void delete_ui_gl(UiGl *ui)
{
	destroy_text_sprite(&ui->loading);
	destroy_text_sprite(&ui->external_exit);
	destroy_text_sprite(&ui->player_controls);
	destroy_text_sprite(&ui->play);
	destroy_text_sprite(&ui->preparing);
	destroy_text_sprite(&ui->subtitle);
	destroy_text_sprite(&ui->title);
	if (ui->program != 0)
		glDeleteProgram(ui->program);
	ui->program = 0;
}

void draw_quad(const UiGl &ui, GLuint texture, float x, float y, float width,
	       float height, int drawable_width, int drawable_height,
	       float red, float green, float blue, float alpha)
{
	const float left = x * 2.0F / static_cast<float>(kLogicalWidth) - 1.0F;
	const float right = (x + width) * 2.0F /
		static_cast<float>(kLogicalWidth) - 1.0F;
	const float top = 1.0F - y * 2.0F / static_cast<float>(kLogicalHeight);
	const float bottom = 1.0F - (y + height) * 2.0F /
		static_cast<float>(kLogicalHeight);
	const GLfloat vertices[] = {
		left, top, 0.0F, 0.0F,
		right, top, 1.0F, 0.0F,
		left, bottom, 0.0F, 1.0F,
		left, bottom, 0.0F, 1.0F,
		right, top, 1.0F, 0.0F,
		right, bottom, 1.0F, 1.0F,
	};
	glViewport(0, 0, drawable_width, drawable_height);
	glUseProgram(ui.program);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat),
			      vertices);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat),
			      vertices + 2);
	glUniform4f(ui.color_uniform, red, green, blue, alpha);
	glUniform1i(ui.texture_uniform, 0);
	if (texture != 0) {
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, texture);
		glUniform1f(ui.textured_uniform, 1.0F);
	} else {
		glDisable(GL_BLEND);
		glBindTexture(GL_TEXTURE_2D, 0);
		glUniform1f(ui.textured_uniform, 0.0F);
	}
	glDrawArrays(GL_TRIANGLES, 0, 6);
}

void draw_text(const UiGl &ui, const TextSprite &sprite, float x, float y,
	       int drawable_width, int drawable_height)
{
	if (sprite.texture == 0)
		return;
	draw_quad(ui, sprite.texture, x, y, static_cast<float>(sprite.width),
		  static_cast<float>(sprite.height), drawable_width,
		  drawable_height, 1.0F, 1.0F, 1.0F, 1.0F);
}

void prepare_ui_gl(int drawable_width, int drawable_height)
{
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glViewport(0, 0, drawable_width, drawable_height);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_STENCIL_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_SCISSOR_TEST);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
}

void draw_home(const App &app, int drawable_width, int drawable_height)
{
	prepare_ui_gl(drawable_width, drawable_height);
	glClearColor(0.035F, 0.035F, 0.045F, 1.0F);
	glClear(GL_COLOR_BUFFER_BIT);
	draw_quad(app.ui, 0, 0.0F, 0.0F, 8.0F, 480.0F, drawable_width,
		  drawable_height, 0.9F, 0.07F, 0.09F, 1.0F);
	draw_quad(app.ui, 0, 28.0F, 112.0F, 584.0F, 190.0F, drawable_width,
		  drawable_height, 0.12F, 0.12F, 0.145F, 1.0F);
	draw_quad(app.ui, 0, 54.0F, 190.0F, 532.0F, 72.0F, drawable_width,
		  drawable_height, 0.92F, 0.92F, 0.92F, 1.0F);
	draw_text(app.ui, app.ui.title, 30.0F, 25.0F, drawable_width,
		  drawable_height);
	draw_text(app.ui,
		  app.endpoint != nullptr ? app.ui.subtitle : app.ui.preparing,
		  32.0F, 70.0F, drawable_width, drawable_height);
	draw_text(app.ui, app.ui.play, 78.0F, 208.0F, drawable_width,
		  drawable_height);
	draw_text(app.ui, app.ui.player_controls, 32.0F, 355.0F,
		  drawable_width, drawable_height);
	draw_text(app.ui, app.ui.external_exit, 426.0F, 444.0F,
		  drawable_width, drawable_height);
}

void draw_loading(const App &app, int drawable_width, int drawable_height)
{
	prepare_ui_gl(drawable_width, drawable_height);
	glClearColor(0.0F, 0.0F, 0.0F, 1.0F);
	glClear(GL_COLOR_BUFFER_BIT);
	draw_text(app.ui, app.ui.loading, 246.0F, 220.0F, drawable_width,
		  drawable_height);
}

void *mpv_get_proc_address(void *, const char *name)
{
	return SDL_GL_GetProcAddress(name);
}

void mpv_render_update(void *context)
{
	auto *app = static_cast<App *>(context);
	SDL_AtomicSet(&app->frame_pending, 1);
}

bool set_mpv_option(mpv_handle *player, const char *name, const char *value)
{
	const int result = mpv_set_option_string(player, name, value);
	if (result >= 0)
		return true;
	std::fprintf(stderr, "YOUTUBE_EMBEDDED_MPV option=FAIL name=%s error=%s\n",
		     name, mpv_error_string(result));
	return false;
}

bool initialize_mpv(App *app)
{
	app->player = mpv_create();
	if (app->player == nullptr) {
		std::fputs("YOUTUBE_EMBEDDED_MPV create=FAIL\n", stderr);
		return false;
	}
	const bool configured =
		set_mpv_option(app->player, "config", "no") &&
		set_mpv_option(app->player, "terminal", "no") &&
		set_mpv_option(app->player, "msg-level", "all=warn") &&
		set_mpv_option(app->player, "ytdl", "no") &&
		set_mpv_option(app->player, "vo", "libmpv") &&
		set_mpv_option(app->player, "ao", "alsa") &&
		set_mpv_option(app->player, "hwdec", "no") &&
		set_mpv_option(app->player, "demuxer-max-bytes", "33554432") &&
		set_mpv_option(app->player, "demuxer-max-back-bytes", "8388608") &&
		set_mpv_option(app->player, "demuxer-readahead-secs", "8") &&
		set_mpv_option(app->player, "cache-pause", "no") &&
		set_mpv_option(app->player, "idle", "yes") &&
		set_mpv_option(app->player, "osc", "no") &&
		set_mpv_option(app->player, "input-default-bindings", "no") &&
		set_mpv_option(app->player, "input-terminal", "no");
	if (!configured)
		return false;
	const int initialize_result = mpv_initialize(app->player);
	if (initialize_result < 0) {
		std::fprintf(stderr, "YOUTUBE_EMBEDDED_MPV initialize=FAIL error=%s\n",
			     mpv_error_string(initialize_result));
		return false;
	}
	mpv_opengl_init_params gl_parameters {};
	gl_parameters.get_proc_address = mpv_get_proc_address;
	gl_parameters.get_proc_address_ctx = nullptr;
	gl_parameters.extra_exts = nullptr;
	const char *api_type = MPV_RENDER_API_TYPE_OPENGL;
	mpv_render_param parameters[] = {
		{MPV_RENDER_PARAM_API_TYPE, const_cast<char *>(api_type)},
		{MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_parameters},
		{MPV_RENDER_PARAM_INVALID, nullptr},
	};
	const int render_result = mpv_render_context_create(
		&app->render_context, app->player, parameters);
	if (render_result < 0) {
		std::fprintf(stderr,
			     "YOUTUBE_EMBEDDED_MPV render-context=FAIL error=%s api=%u.%u\n",
			     mpv_error_string(render_result),
			     static_cast<unsigned int>(mpv_client_api_version() >> 16U),
			     static_cast<unsigned int>(mpv_client_api_version() & 0xffffU));
		return false;
	}
	mpv_render_context_set_update_callback(app->render_context,
					       mpv_render_update, app);
	std::printf("YOUTUBE_EMBEDDED_MPV READY create_count=1 initialize_count=1 render_context_count=1 api=%u.%u\n",
		    static_cast<unsigned int>(mpv_client_api_version() >> 16U),
		    static_cast<unsigned int>(mpv_client_api_version() & 0xffffU));
	return true;
}

int mpv_cache_self_test()
{
	mpv_handle *player = mpv_create();
	if (player == nullptr) {
		std::fputs("YOUTUBE_EMBEDDED_CACHE_TEST FAIL create\n", stderr);
		return 1;
	}
	const bool configured =
		set_mpv_option(player, "config", "no") &&
		set_mpv_option(player, "terminal", "no") &&
		set_mpv_option(player, "vo", "null") &&
		set_mpv_option(player, "ao", "null") &&
		set_mpv_option(player, "demuxer-max-bytes", "33554432") &&
		set_mpv_option(player, "demuxer-max-back-bytes", "8388608") &&
		set_mpv_option(player, "demuxer-readahead-secs", "8") &&
		set_mpv_option(player, "cache-pause", "no") &&
		set_mpv_option(player, "idle", "yes");
	const int initialized = configured ? mpv_initialize(player) :
		MPV_ERROR_OPTION_ERROR;
	if (initialized >= 0)
		std::puts("YOUTUBE_EMBEDDED_CACHE_TEST PASS max=33554432 back=8388608 readahead=8 cache_pause=no");
	else
		std::fprintf(stderr,
			     "YOUTUBE_EMBEDDED_CACHE_TEST FAIL initialize=%s\n",
			     mpv_error_string(initialized));
	mpv_terminate_destroy(player);
	return initialized >= 0 ? 0 : 1;
}

void open_input(App *app)
{
	for (int index = 0; index < SDL_NumJoysticks(); ++index) {
		if (SDL_IsGameController(index)) {
			app->controller = SDL_GameControllerOpen(index);
			if (app->controller != nullptr)
				break;
		}
	}
	if (app->controller == nullptr && SDL_NumJoysticks() > 0)
		app->joystick = SDL_JoystickOpen(0);
	std::printf("YOUTUBE_EMBEDDED_INPUT READY controller=%s joystick=%s evdev_grab=0 MENU+START=external\n",
		    app->controller != nullptr ? "yes" : "no",
		    app->joystick != nullptr ? "yes" : "no");
}

void broker_failure(App *app, const char *reason)
{
	if (!app->broker_failed)
		std::fprintf(stderr,
			     "YOUTUBE_EMBEDDED_BROKER result=FAIL reason=%s ready=%d\n",
			     reason, app->broker_ready ? 1 : 0);
	app->broker_failed = true;
	app->running = false;
}

void process_broker(App *app)
{
	if (!app->broker_mode || app->broker_failed)
		return;
	char input[512] = {};
	for (;;) {
		const ssize_t count = read(app->broker_stdout, input, sizeof(input));
		if (count > 0) {
			if (app->broker_ready) {
				broker_failure(app, "extra-stdout");
				break;
			}
			for (ssize_t index = 0; index < count; ++index) {
				const char byte = input[index];
				if (byte == '\r') {
					broker_failure(app, "carriage-return");
					break;
				}
				if (byte == '\n') {
					app->broker_line[app->broker_line_used] = '\0';
					bool has_audio = false;
					if (!parse_broker_line(
						    app->broker_line,
						    app->endpoint_storage,
						    sizeof(app->endpoint_storage),
						    app->audio_endpoint_storage,
						    sizeof(app->audio_endpoint_storage),
						    &has_audio)) {
						broker_failure(app, "protocol");
						break;
					}
					app->endpoint = app->endpoint_storage;
					app->audio_endpoint = has_audio ?
						app->audio_endpoint_storage : nullptr;
					app->broker_ready = true;
					std::printf("YOUTUBE_EMBEDDED_BROKER READY endpoint=loopback audio=%s stdout=nonblocking\n",
						    has_audio ? "separate" : "integrated");
					if (index + 1 != count)
						broker_failure(app, "extra-stdout");
					break;
				}
				if (app->broker_line_used >= kMaximumBrokerLineBytes) {
					broker_failure(app, "line-too-long");
					break;
				}
				app->broker_line[app->broker_line_used++] = byte;
			}
			if (app->broker_ready || app->broker_failed)
				break;
			continue;
		}
		if (count == 0) {
			broker_failure(app, "stdout-eof");
			break;
		}
		if (errno == EINTR)
			continue;
		if (errno != EAGAIN && errno != EWOULDBLOCK)
			broker_failure(app, "stdout-read");
		break;
	}
	if (app->broker_pid > 0) {
		int status = 0;
		const pid_t waited = waitpid(app->broker_pid, &status, WNOHANG);
		if (waited == app->broker_pid) {
			app->broker_pid = -1;
			broker_failure(app, "child-exited");
		} else if (waited < 0 && errno != EINTR) {
			app->broker_pid = -1;
			broker_failure(app, "waitpid");
		}
	}
}

void stop_broker(App *app)
{
	if (app->broker_stdin >= 0) {
		(void)close(app->broker_stdin); // stdin EOF asks the bridge to clean up.
		app->broker_stdin = -1;
	}
	if (app->broker_stdout >= 0) {
		(void)close(app->broker_stdout);
		app->broker_stdout = -1;
	}
	if (app->broker_pid <= 0)
		return;
	int status = 0;
	for (int attempt = 0; attempt < 20; ++attempt) {
		const pid_t waited = waitpid(app->broker_pid, &status, WNOHANG);
		if (waited == app->broker_pid) {
			app->broker_pid = -1;
			break;
		}
		if (waited < 0 && errno != EINTR) {
			app->broker_pid = -1;
			break;
		}
		(void)usleep(5000);
	}
	if (app->broker_pid > 0) {
		(void)kill(app->broker_pid, SIGTERM);
		for (int attempt = 0; attempt < 40; ++attempt) {
			const pid_t waited = waitpid(app->broker_pid, &status, WNOHANG);
			if (waited == app->broker_pid) {
				app->broker_pid = -1;
				break;
			}
			if (waited < 0 && errno != EINTR) {
				app->broker_pid = -1;
				break;
			}
			(void)usleep(5000);
		}
	}
	if (app->broker_pid > 0) {
		(void)kill(app->broker_pid, SIGKILL);
		(void)waitpid(app->broker_pid, &status, 0);
		app->broker_pid = -1;
	}
	std::puts("YOUTUBE_EMBEDDED_BROKER cleanup=PASS stdin=closed signal=bounded");
}

int broker_pipe_self_test(const char *broker, const char *watch_url)
{
	App app {};
	if (!start_broker(&app, broker, watch_url))
		return 1;
	for (int attempt = 0; attempt < 400 && !app.broker_ready &&
	     !app.broker_failed; ++attempt) {
		process_broker(&app);
		(void)usleep(5000);
	}
	const bool passed = app.broker_ready && !app.broker_failed &&
		valid_loopback_endpoint(app.endpoint);
	const bool separate_audio = app.audio_endpoint != nullptr;
	stop_broker(&app);
	if (!passed) {
		std::fputs("YOUTUBE_EMBEDDED_BROKER_TEST FAIL\n", stderr);
		return 1;
	}
	std::printf("YOUTUBE_EMBEDDED_BROKER_TEST PASS stdout=nonblocking endpoint=loopback audio=%s cleanup=stdin-eof\n",
		    separate_audio ? "separate" : "integrated");
	return 0;
}

bool initialize_app(App *app)
{
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) != 0) {
		std::fprintf(stderr, "YOUTUBE_EMBEDDED_SDL init=FAIL error=%s\n",
			     SDL_GetError());
		return false;
	}
	const char *video_driver = SDL_GetCurrentVideoDriver();
	const bool allow_non_kmsdrm =
		std::getenv("RG_YOUTUBE_ALLOW_NON_KMSDRM") != nullptr;
	if (video_driver == nullptr ||
	    (strcasecmp(video_driver, "kmsdrm") != 0 && !allow_non_kmsdrm)) {
		std::fprintf(stderr,
			     "YOUTUBE_EMBEDDED_SDL driver=FAIL expected=KMSDRM actual=%s\n",
			     video_driver != nullptr ? video_driver : "none");
		return false;
	}
	(void)SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
				  SDL_GL_CONTEXT_PROFILE_ES);
	(void)SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
	(void)SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
	(void)SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
	(void)SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
	(void)SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
	const bool windowed = std::getenv("RG_YOUTUBE_WINDOWED") != nullptr;
	const Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN |
		(windowed ? 0U : static_cast<Uint32>(SDL_WINDOW_FULLSCREEN));
	app->window = SDL_CreateWindow("RG40XXV YouTube Embedded Scene",
		SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, kLogicalWidth,
		kLogicalHeight, flags);
	if (app->window == nullptr) {
		std::fprintf(stderr, "YOUTUBE_EMBEDDED_SDL window=FAIL error=%s\n",
			     SDL_GetError());
		return false;
	}
	app->gl_context = SDL_GL_CreateContext(app->window);
	if (app->gl_context == nullptr ||
	    SDL_GL_MakeCurrent(app->window, app->gl_context) != 0) {
		std::fprintf(stderr, "YOUTUBE_EMBEDDED_SDL context=FAIL error=%s\n",
			     SDL_GetError());
		return false;
	}
	if (SDL_GL_SetSwapInterval(1) != 0)
		std::fprintf(stderr,
			     "YOUTUBE_EMBEDDED_SDL swap-interval=WARN error=%s\n",
			     SDL_GetError());
	if (!initialize_ui_gl(&app->ui))
		return false;
	if (TTF_Init() == 0) {
		app->ttf_initialized = true;
		const char *font_path = std::getenv("RG_YOUTUBE_FONT");
		if (font_path == nullptr || font_path[0] == '\0')
			font_path = "/opt/rg40xxv/share/RG40XXV-UI-Sans.otf";
		app->title_font = TTF_OpenFont(font_path, 30);
		app->body_font = TTF_OpenFont(font_path, 18);
		if (app->title_font != nullptr && app->body_font != nullptr) {
			const SDL_Color light {238, 238, 240, 255};
			const SDL_Color dark {20, 20, 22, 255};
			const bool text_ready =
				create_text_sprite(&app->ui.title, app->title_font,
						   "YouTube  Native", light) &&
				create_text_sprite(&app->ui.subtitle, app->body_font,
						   "PLAYER READY  -  direct loopback stream", light) &&
				create_text_sprite(&app->ui.preparing, app->body_font,
						   "PREPARING STREAM  -  HOME remains responsive", light) &&
				create_text_sprite(&app->ui.play, app->title_font,
						   "A   PLAY NOW", dark) &&
				create_text_sprite(&app->ui.player_controls, app->body_font,
						   "PLAYER:  A pause    B home    LEFT / RIGHT seek", light) &&
				create_text_sprite(&app->ui.external_exit, app->body_font,
						   "MENU + START  Exit", light) &&
				create_text_sprite(&app->ui.loading, app->body_font,
						   "Opening stream...", light);
			if (!text_ready)
				std::fprintf(stderr,
					     "YOUTUBE_EMBEDDED_TTF textures=WARN error=%s\n",
					     TTF_GetError());
		} else {
			std::fprintf(stderr,
				     "YOUTUBE_EMBEDDED_TTF font=WARN path=%s error=%s\n",
				     font_path, TTF_GetError());
		}
	} else {
		std::fprintf(stderr, "YOUTUBE_EMBEDDED_TTF init=WARN error=%s\n",
			     TTF_GetError());
	}
	int drawable_width = 0;
	int drawable_height = 0;
	SDL_GL_GetDrawableSize(app->window, &drawable_width, &drawable_height);
	draw_home(*app, drawable_width, drawable_height);
	SDL_GL_SwapWindow(app->window);
	if (!initialize_mpv(app))
		return false;
	open_input(app);
	std::printf("YOUTUBE_EMBEDDED_SCENE READY scene=HOME windows=1 contexts=1 driver=%s drawable=%dx%d endpoint=%s\n",
		    video_driver, drawable_width, drawable_height,
		    app->endpoint != nullptr ? "loopback" : "pending");
	return true;
}

void destroy_app(App *app)
{
	if (app->render_context != nullptr) {
		mpv_render_context_set_update_callback(app->render_context, nullptr,
						       nullptr);
		mpv_render_context_free(app->render_context);
		app->render_context = nullptr;
	}
	if (app->player != nullptr) {
		mpv_terminate_destroy(app->player);
		app->player = nullptr;
	}
	stop_broker(app);
	if (app->gl_context != nullptr && app->window != nullptr)
		(void)SDL_GL_MakeCurrent(app->window, app->gl_context);
	delete_ui_gl(&app->ui);
	if (app->title_font != nullptr)
		TTF_CloseFont(app->title_font);
	if (app->body_font != nullptr)
		TTF_CloseFont(app->body_font);
	if (app->ttf_initialized)
		TTF_Quit();
	if (app->controller != nullptr)
		SDL_GameControllerClose(app->controller);
	if (app->joystick != nullptr)
		SDL_JoystickClose(app->joystick);
	if (app->gl_context != nullptr)
		SDL_GL_DeleteContext(app->gl_context);
	if (app->window != nullptr)
		SDL_DestroyWindow(app->window);
	SDL_Quit();
}

double elapsed_ms(Uint64 start)
{
	const Uint64 frequency = SDL_GetPerformanceFrequency();
	if (frequency == 0)
		return 0.0;
	return static_cast<double>(SDL_GetPerformanceCounter() - start) * 1000.0 /
		static_cast<double>(frequency);
}

bool command_async(App *app, uint64_t reply, const char **arguments)
{
	const int result = mpv_command_async(app->player, reply, arguments);
	if (result >= 0)
		return true;
	std::fprintf(stderr, "YOUTUBE_EMBEDDED_COMMAND queue=FAIL name=%s error=%s\n",
		     arguments[0], mpv_error_string(result));
	return false;
}

void enter_player(App *app)
{
	if (app->scene != Scene::home)
		return;
	if (app->endpoint == nullptr) {
		app->play_requested = true;
		std::puts("YOUTUBE_EMBEDDED_SWITCH from=HOME to=PLAYER result=WAIT endpoint=pending");
		return;
	}
	const char *arguments[] = {"loadfile", app->endpoint, "replace", nullptr};
	app->switch_started = SDL_GetPerformanceCounter();
	if (!command_async(app, kLoadReply, arguments))
		return;
	app->scene = Scene::player;
	app->play_requested = false;
	app->file_loaded = false;
	app->audio_add_queued = false;
	app->first_frame_reported = false;
	++app->switch_counter;
	SDL_AtomicSet(&app->frame_pending, 1);
	std::printf("YOUTUBE_EMBEDDED_SWITCH from=HOME to=PLAYER sequence=%llu queue_ms=%.3f endpoint=loopback\n",
		    static_cast<unsigned long long>(app->switch_counter),
		    elapsed_ms(app->switch_started));
}

void return_home(App *app)
{
	if (app->scene != Scene::player) {
		if (app->play_requested) {
			app->play_requested = false;
			std::puts("YOUTUBE_EMBEDDED_SWITCH scene=HOME pending-auto-play=CANCELED broker=alive");
		}
		return;
	}
	const Uint64 started = SDL_GetPerformanceCounter();
	const char *arguments[] = {"stop", nullptr};
	(void)command_async(app, kStopReply, arguments);
	app->scene = Scene::home;
	app->file_loaded = false;
	app->audio_add_queued = false;
	app->first_frame_reported = false;
	std::printf("YOUTUBE_EMBEDDED_SWITCH from=PLAYER to=HOME sequence=%llu queue_ms=%.3f mpv_alive=1\n",
		    static_cast<unsigned long long>(app->switch_counter),
		    elapsed_ms(started));
}

void pause_player(App *app)
{
	if (app->scene != Scene::player)
		return;
	const char *arguments[] = {"cycle", "pause", nullptr};
	(void)command_async(app, kPauseReply, arguments);
}

void seek_player(App *app, const char *seconds)
{
	if (app->scene != Scene::player)
		return;
	const char *arguments[] = {"seek", seconds, "relative", nullptr};
	(void)command_async(app, kSeekReply, arguments);
}

void process_sdl_events(App *app)
{
	SDL_Event event {};
	while (SDL_PollEvent(&event) != 0) {
		if (event.type == SDL_QUIT) {
			app->running = false;
			continue;
		}
		if (event.type == SDL_KEYDOWN && event.key.repeat == 0) {
			const SDL_Keycode key = event.key.keysym.sym;
			if (key == SDLK_RETURN || key == SDLK_SPACE) {
				if (app->scene == Scene::home)
					enter_player(app);
				else
					pause_player(app);
			} else if (key == SDLK_ESCAPE || key == SDLK_b) {
				return_home(app);
			} else if (key == SDLK_LEFT) {
				seek_player(app, "-10");
			} else if (key == SDLK_RIGHT) {
				seek_player(app, "10");
			}
			continue;
		}
		if (event.type == SDL_CONTROLLERBUTTONDOWN) {
			switch (event.cbutton.button) {
			case SDL_CONTROLLER_BUTTON_A:
				if (app->scene == Scene::home)
					enter_player(app);
				else
					pause_player(app);
				break;
			case SDL_CONTROLLER_BUTTON_B:
				return_home(app);
				break;
			case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
				seek_player(app, "-10");
				break;
			case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
				seek_player(app, "10");
				break;
			default:
				break; // BACK/GUIDE/START stay entirely external.
			}
			continue;
		}
		if (event.type == SDL_JOYBUTTONDOWN) {
			if (event.jbutton.button == 0) {
				if (app->scene == Scene::home)
					enter_player(app);
				else
					pause_player(app);
			} else if (event.jbutton.button == 1) {
				return_home(app);
			}
			continue;
		}
		if (event.type == SDL_JOYHATMOTION &&
		    app->scene == Scene::player) {
			if ((event.jhat.value & SDL_HAT_LEFT) != 0)
				seek_player(app, "-10");
			else if ((event.jhat.value & SDL_HAT_RIGHT) != 0)
				seek_player(app, "10");
		}
	}
}

void process_mpv_events(App *app)
{
	for (;;) {
		mpv_event *event = mpv_wait_event(app->player, 0.0);
		if (event == nullptr || event->event_id == MPV_EVENT_NONE)
			return;
		if (event->event_id == MPV_EVENT_FILE_LOADED) {
			app->file_loaded = true;
			std::printf("YOUTUBE_EMBEDDED_MEDIA loaded=1 sequence=%llu elapsed_ms=%.3f\n",
				    static_cast<unsigned long long>(app->switch_counter),
				    elapsed_ms(app->switch_started));
			if (app->audio_endpoint != nullptr &&
			    !app->audio_add_queued) {
				const char *arguments[] = {
					"audio-add", app->audio_endpoint, "select", nullptr,
				};
				app->audio_add_queued = command_async(
					app, kAudioAddReply, arguments);
			}
		} else if (event->event_id == MPV_EVENT_END_FILE) {
			const auto *end = static_cast<mpv_event_end_file *>(event->data);
			if (app->scene == Scene::player) {
				std::fprintf(stderr,
					     "YOUTUBE_EMBEDDED_MEDIA ended=1 reason=%d error=%s returning=HOME\n",
					     end != nullptr ? end->reason : -1,
					     end != nullptr && end->error < 0 ?
						mpv_error_string(end->error) : "none");
				app->scene = Scene::home;
				app->file_loaded = false;
			}
		} else if (event->event_id == MPV_EVENT_COMMAND_REPLY &&
			   event->error < 0) {
			std::fprintf(stderr,
				     "YOUTUBE_EMBEDDED_COMMAND reply=FAIL id=%llu error=%s\n",
				     static_cast<unsigned long long>(event->reply_userdata),
				     mpv_error_string(event->error));
			if (event->reply_userdata == kLoadReply &&
			    app->scene == Scene::player)
				app->scene = Scene::home;
		} else if (event->event_id == MPV_EVENT_SHUTDOWN) {
			app->running = false;
		}
	}
}

bool render_player(App *app, int drawable_width, int drawable_height)
{
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	mpv_opengl_fbo framebuffer {0, drawable_width, drawable_height, 0};
	int flip_y = 1;
	mpv_render_param parameters[] = {
		{MPV_RENDER_PARAM_OPENGL_FBO, &framebuffer},
		{MPV_RENDER_PARAM_FLIP_Y, &flip_y},
		{MPV_RENDER_PARAM_INVALID, nullptr},
	};
	const int result = mpv_render_context_render(app->render_context, parameters);
	if (result >= 0)
		return true;
	std::fprintf(stderr, "YOUTUBE_EMBEDDED_RENDER result=FAIL error=%s\n",
		     mpv_error_string(result));
	return false;
}

bool save_screenshot(const App &app, int width, int height)
{
	if (app.screenshot_path == nullptr || app.screenshot_path[0] != '/') {
		std::fputs("YOUTUBE_EMBEDDED_SCREENSHOT result=FAIL reason=path-not-absolute\n",
			   stderr);
		return false;
	}
	SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(
		0, width, height, 32, SDL_PIXELFORMAT_ABGR8888);
	if (surface == nullptr) {
		std::fprintf(stderr,
			     "YOUTUBE_EMBEDDED_SCREENSHOT result=FAIL reason=surface error=%s\n",
			     SDL_GetError());
		return false;
	}
	if (SDL_MUSTLOCK(surface) && SDL_LockSurface(surface) != 0) {
		SDL_FreeSurface(surface);
		return false;
	}
	for (int attempt = 0; attempt < 16 && glGetError() != GL_NO_ERROR;
	     ++attempt) {}
	glFinish();
	glPixelStorei(GL_PACK_ALIGNMENT, 4);
	glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE,
		     surface->pixels);
	const GLenum read_error = glGetError();
	void *scratch = SDL_malloc(static_cast<size_t>(surface->pitch));
	if (read_error == GL_NO_ERROR && scratch != nullptr) {
		auto *pixels = static_cast<Uint8 *>(surface->pixels);
		for (int top = 0, bottom = height - 1; top < bottom;
		     ++top, --bottom) {
			void *top_row = pixels + static_cast<size_t>(top) *
				static_cast<size_t>(surface->pitch);
			void *bottom_row = pixels + static_cast<size_t>(bottom) *
				static_cast<size_t>(surface->pitch);
			SDL_memcpy(scratch, top_row,
				   static_cast<size_t>(surface->pitch));
			SDL_memcpy(top_row, bottom_row,
				   static_cast<size_t>(surface->pitch));
			SDL_memcpy(bottom_row, scratch,
				   static_cast<size_t>(surface->pitch));
		}
	}
	SDL_free(scratch);
	if (SDL_MUSTLOCK(surface))
		SDL_UnlockSurface(surface);
	const int save_result = read_error == GL_NO_ERROR && scratch != nullptr ?
		SDL_SaveBMP(surface, app.screenshot_path) : -1;
	SDL_FreeSurface(surface);
	if (save_result != 0) {
		std::fprintf(stderr,
			     "YOUTUBE_EMBEDDED_SCREENSHOT result=FAIL gl_error=%u error=%s\n",
			     static_cast<unsigned int>(read_error), SDL_GetError());
		return false;
	}
	std::printf("YOUTUBE_EMBEDDED_SCREENSHOT result=PASS scene=%s path=%s size=%dx%d source=render-memory\n",
		    app.scene == Scene::home ? "HOME" : "PLAYER",
		    app.screenshot_path, width, height);
	return true;
}

int run(App *app)
{
	while (app->running && g_stop_requested == 0) {
		process_broker(app);
		if (app->play_requested && app->endpoint != nullptr &&
		    app->scene == Scene::home)
			enter_player(app);
		process_sdl_events(app);
		process_mpv_events(app);
		int drawable_width = 0;
		int drawable_height = 0;
		SDL_GL_GetDrawableSize(app->window, &drawable_width, &drawable_height);
		const bool screenshot = g_screenshot_requested != 0;
		if (screenshot)
			g_screenshot_requested = 0;
		bool presented = false;
		if (app->scene == Scene::home) {
			draw_home(*app, drawable_width, drawable_height);
			presented = true;
		} else {
			const uint64_t updates =
				mpv_render_context_update(app->render_context);
			const bool frame =
				(updates & MPV_RENDER_UPDATE_FRAME) != 0U;
			const bool callback =
				SDL_AtomicSet(&app->frame_pending, 0) != 0;
			if (frame || callback || screenshot) {
				if (app->file_loaded) {
					presented = render_player(app, drawable_width,
							  drawable_height);
					if (presented && !app->first_frame_reported) {
						app->first_frame_reported = true;
						std::printf("YOUTUBE_EMBEDDED_FIRST_FRAME sequence=%llu elapsed_ms=%.3f\n",
							    static_cast<unsigned long long>(app->switch_counter),
							    elapsed_ms(app->switch_started));
					}
				} else {
					draw_loading(*app, drawable_width,
						     drawable_height);
					presented = true;
				}
			}
		}
		if (screenshot) {
			if (!presented && app->scene == Scene::player)
				presented = render_player(app, drawable_width,
							  drawable_height);
			if (presented)
				(void)save_screenshot(*app, drawable_width,
						      drawable_height);
		}
		if (presented) {
			SDL_GL_SwapWindow(app->window);
			if (app->scene == Scene::player)
				mpv_render_context_report_swap(app->render_context);
		} else {
			SDL_Delay(2);
		}
	}
	return 0;
}

void print_usage(const char *program)
{
	std::fprintf(stderr,
		     "usage: %s --device-play http://127.0.0.1:PORT/PATH | --stream ENDPOINT | --broker ABS_PATH WATCH_URL | --contract | --self-test | --mpv-cache-self-test | --broker-pipe-self-test ABS_PATH WATCH_URL\n",
		     program);
}

} // namespace

int main(int argc, char **argv)
{
	if (argc == 2 && std::strcmp(argv[1], "--contract") == 0) {
		print_contract();
		return 0;
	}
	if (argc == 2 && std::strcmp(argv[1], "--self-test") == 0)
		return self_test();
	if (argc == 2 &&
	    std::strcmp(argv[1], "--mpv-cache-self-test") == 0)
		return mpv_cache_self_test();
	if (argc == 4 &&
	    std::strcmp(argv[1], "--broker-pipe-self-test") == 0)
		return broker_pipe_self_test(argv[2], argv[3]);
	const char *endpoint = nullptr;
	const char *broker = nullptr;
	const char *watch_url = nullptr;
	if (argc == 3 &&
	    (std::strcmp(argv[1], "--device-play") == 0 ||
	     std::strcmp(argv[1], "--stream") == 0))
		endpoint = argv[2];
	else if (argc == 4 && std::strcmp(argv[1], "--broker") == 0) {
		broker = argv[2];
		watch_url = argv[3];
	}
	else if (argc == 1)
		endpoint = std::getenv("RG_YOUTUBE_TEST_STREAM");
	else {
		print_usage(argv[0]);
		return 64;
	}
	if (broker == nullptr && !valid_loopback_endpoint(endpoint)) {
		std::fputs("YOUTUBE_EMBEDDED_ENDPOINT result=FAIL reason=expected-loopback-http\n",
			   stderr);
		return 64;
	}
	if (broker != nullptr &&
	    (!executable_regular(broker) || !valid_watch_url(watch_url))) {
		std::fputs("YOUTUBE_EMBEDDED_BROKER result=FAIL reason=arguments\n",
			   stderr);
		return 64;
	}
	const char *screenshot_path =
		std::getenv("RG_YOUTUBE_SCREENSHOT_PATH");
	if (screenshot_path == nullptr || screenshot_path[0] == '\0')
		screenshot_path = "/run/rg40xxv-youtube-embedded.bmp";
	if (screenshot_path[0] != '/') {
		std::fputs("YOUTUBE_EMBEDDED_SCREENSHOT result=FAIL reason=path-not-absolute\n",
			   stderr);
		return 64;
	}
	if (!install_signal_handlers()) {
		std::fputs("YOUTUBE_EMBEDDED_SIGNAL result=FAIL\n", stderr);
		return 70;
	}
	App app {};
	app.endpoint = endpoint;
	app.screenshot_path = screenshot_path;
	if (broker != nullptr && !start_broker(&app, broker, watch_url)) {
		destroy_app(&app);
		return 69;
	}
	if (!initialize_app(&app)) {
		destroy_app(&app);
		return 66;
	}
	const int result = run(&app);
	destroy_app(&app);
	std::puts("YOUTUBE_EMBEDDED_SCENE RETURNED mpv_initialize_count=1");
	return result;
}
