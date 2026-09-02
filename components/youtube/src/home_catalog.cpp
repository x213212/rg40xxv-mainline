#include "home_catalog.hpp"

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace rg40xxv_youtube {
namespace {

constexpr const char *kQueries[] = {
	"台灣 熱門",
	"遊戲 熱門",
	"音樂 熱門",
	"科技 新聞",
};

constexpr std::size_t kQueryCount = sizeof(kQueries) / sizeof(kQueries[0]);

// IDs were resolved from each channel's canonical YouTube page.  They are
// immutable channel identities; the runtime never performs a name search.
constexpr HomeChannel kChannels[] = {
	{"總裁聊聊", "UC2j5Kw9qDWCZmU_emgqeguA"},
	{"我是阿史", "UCcL163py441fTFfWy5tBjoQ"},
	{"你可敢信尼可拉斯楊", "UCfc5rX7XNwEvY0cgXygkDbQ"},
	{"攝徒日記 Fun TV", "UCvTe3Z7TZsjGzUERx4Ce6zA"},
	{"壹電視 NEXT TV", "UC6hWBu1hfbNNk8Yeokd5aZQ"},
	{"曉涵哥來了", "UCvoBl4rnVsetDKA_Tdk-jeA"},
	{"표은지", "UC9K0rLE1SMh86nVxzkCBpNA"},
	{"游庭皓的財經皓角", "UC0lbAQVpenvfA2QqzsRtL_g"},
};

static_assert(sizeof(kChannels) / sizeof(kChannels[0]) == kHomeChannelCount,
	      "the curated channel UI and catalog limit must stay aligned");

struct FallbackCard {
	const char *id;
	const char *title;
	const char *channel;
	const char *published;
	unsigned duration;
};

constexpr FallbackCard kFallback[] = {
	{"GwtNiL9eEYk", "Playback sample", "Known-good YouTube stream", "日期未知", 0},
	{"jNQXAC9IVRw", "Me at the zoo", "jawed", "2005-04-24", 19},
	{"M7lc1UVf-VE", "YouTube Player API demo", "Google for Developers", "日期未知", 0},
};

bool regular_executable(const char *path)
{
	struct stat status {};
	return path != nullptr && path[0] == '/' &&
		lstat(path, &status) == 0 && S_ISREG(status.st_mode) &&
		!S_ISLNK(status.st_mode) && (status.st_mode & S_IXUSR) != 0;
}

bool valid_id(const char *value)
{
	if (value == nullptr || std::strlen(value) != 11)
		return false;
	for (const unsigned char *p = reinterpret_cast<const unsigned char *>(value);
	     *p != 0; ++p) {
		if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
		      (*p >= '0' && *p <= '9') || *p == '_' || *p == '-'))
			return false;
	}
	return true;
}

bool valid_channel_id(const char *value)
{
	if (value == nullptr || std::strlen(value) != 24 || value[0] != 'U' ||
	    value[1] != 'C')
		return false;
	for (const unsigned char *p =
		     reinterpret_cast<const unsigned char *>(value + 2);
	     *p != 0; ++p) {
		if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
		      (*p >= '0' && *p <= '9') || *p == '_' || *p == '-'))
			return false;
	}
	return true;
}

bool https_url(const char *value)
{
	return value != nullptr && std::strncmp(value, "https://", 8) == 0 &&
		std::strchr(value, '\r') == nullptr &&
		std::strchr(value, '\n') == nullptr;
}

bool valid_thumbnail_file(const char *path)
{
	struct stat status {};
	return path != nullptr && path[0] == '/' &&
		std::strchr(path, '\r') == nullptr && std::strchr(path, '\n') == nullptr &&
		std::strchr(path, '\t') == nullptr && lstat(path, &status) == 0 &&
		S_ISREG(status.st_mode) && !S_ISLNK(status.st_mode) &&
		status.st_uid == geteuid() && status.st_nlink == 1 &&
		(status.st_mode & S_IROTH) == 0;
}

void copy_field(char *destination, std::size_t size, const char *source)
{
	if (size == 0)
		return;
	if (source == nullptr)
		source = "";
	std::snprintf(destination, size, "%s", source);
	// Avoid handing SDL_ttf an incomplete trailing UTF-8 codepoint after a
	// bounded copy. ASCII and all complete UTF-8 strings remain unchanged.
	std::size_t length = std::strlen(destination);
	while (length > 0 &&
	       (static_cast<unsigned char>(destination[length - 1]) & 0xc0U) == 0x80U)
		destination[--length] = 0;
}

void make_card(HomeCard *card, const char *id, const char *title,
	       const char *channel, const char *published, unsigned duration,
	       const char *thumbnail, const char *watch_url,
	       const char *thumbnail_path)
{
	*card = HomeCard {};
	copy_field(card->video_id, sizeof(card->video_id), id);
	copy_field(card->title, sizeof(card->title), title);
	copy_field(card->channel, sizeof(card->channel), channel);
	copy_field(card->published, sizeof(card->published), published);
	card->duration_seconds = duration;
	copy_field(card->thumbnail, sizeof(card->thumbnail), thumbnail);
	if (watch_url != nullptr && https_url(watch_url))
		copy_field(card->watch_url, sizeof(card->watch_url), watch_url);
	else
		std::snprintf(card->watch_url, sizeof(card->watch_url),
			      "https://www.youtube.com/watch?v=%s", id);
	if (thumbnail_path != nullptr)
		copy_field(card->thumbnail_path, sizeof(card->thumbnail_path),
			   thumbnail_path);
}

bool parse_count(const char *value, std::size_t maximum, std::size_t *result)
{
	if (value == nullptr || result == nullptr)
		return false;
	char *end = nullptr;
	errno = 0;
	const unsigned long parsed = std::strtoul(value, &end, 10);
	if (errno != 0 || end == value || *end != 0 || parsed > maximum)
		return false;
	*result = static_cast<std::size_t>(parsed);
	return true;
}

bool duplicate_response_id(const HomeCatalog *catalog, const char *video_id)
{
	for (std::size_t index = 0; index < catalog->pending_count; ++index) {
		if (std::strcmp(catalog->pending[index].video_id, video_id) == 0)
			return true;
	}
	if (!catalog->response_started && !catalog->append_response)
		return false;
	for (std::size_t index = 0; index < catalog->available_count; ++index) {
		if (std::strcmp(catalog->cards[index].video_id, video_id) == 0)
			return true;
	}
	return false;
}

void commit_pending(HomeCatalog *catalog)
{
	if (catalog->pending_count == 0)
		return;
	if (!catalog->response_started && !catalog->append_response) {
		char selected_id[sizeof(catalog->cards[0].video_id)] {};
		const std::size_t old_selected = catalog->selected;
		if (catalog->focus == HomeFocus::card &&
		    catalog->selected < catalog->count)
			copy_field(selected_id, sizeof(selected_id),
				   catalog->cards[catalog->selected].video_id);
		std::memcpy(catalog->cards, catalog->pending,
			    catalog->pending_count * sizeof(catalog->pending[0]));
		catalog->available_count = catalog->pending_count;
		catalog->count = catalog->available_count < kHomePageSize ?
			catalog->available_count : kHomePageSize;
		catalog->selected = catalog->count == 0 ? 0 :
			(old_selected < catalog->count ? old_selected : catalog->count - 1);
		if (selected_id[0] != 0) {
			for (std::size_t index = 0; index < catalog->count; ++index) {
				if (std::strcmp(catalog->cards[index].video_id,
						selected_id) == 0) {
					catalog->selected = index;
					break;
				}
			}
		}
		catalog->response_started = true;
		catalog->load_more_requested = false;
	} else {
		std::memcpy(catalog->cards + catalog->available_count, catalog->pending,
			    catalog->pending_count * sizeof(catalog->pending[0]));
		catalog->available_count += catalog->pending_count;
	}
	catalog->response_count += catalog->pending_count;
	catalog->pending_count = 0;
}

bool expose_requested_page(HomeCatalog *catalog)
{
	if (!catalog->load_more_requested || catalog->available_count <= catalog->count)
		return false;
	const std::size_t remaining = catalog->available_count - catalog->count;
	catalog->count += remaining < kHomePageSize ? remaining : kHomePageSize;
	catalog->load_more_requested = false;
	++catalog->revision;
	return true;
}

void request_page_near_end(HomeCatalog *catalog)
{
	if (catalog->focus != HomeFocus::card || catalog->count == 0 ||
	    catalog->selected + 2 < catalog->count)
		return;
	if (catalog->available_count > catalog->count || catalog->more_expected ||
	    catalog->loading)
		catalog->load_more_requested = true;
	(void)expose_requested_page(catalog);
}

bool parse_item(HomeCatalog *catalog, char *line)
{
	char *fields[9] {};
	std::size_t count = 0;
	char *cursor = line;
	while (count < sizeof(fields) / sizeof(fields[0])) {
		fields[count++] = cursor;
		char *tab = std::strchr(cursor, '\t');
		if (tab == nullptr)
			break;
		*tab = 0;
		cursor = tab + 1;
	}
	if (count != 9 || std::strcmp(fields[0], "ITEM") != 0 ||
	    !valid_id(fields[1]) || fields[2][0] == 0 ||
	    fields[4][0] == 0 || !https_url(fields[6]) || !https_url(fields[7]) ||
	    (fields[8][0] != 0 && !valid_thumbnail_file(fields[8])) ||
	    duplicate_response_id(catalog, fields[1]) ||
	    catalog->pending_count >= kHomePageSize ||
	    catalog->response_base_index + catalog->response_count +
		catalog->pending_count >= kHomeCardLimit)
		return false;
	char *end = nullptr;
	errno = 0;
	const unsigned long duration = std::strtoul(fields[5], &end, 10);
	if (errno != 0 || end == fields[5] || *end != 0 || duration > UINT_MAX)
		return false;
	make_card(&catalog->pending[catalog->pending_count++], fields[1], fields[2],
		  fields[3], fields[4], static_cast<unsigned>(duration), fields[6],
		  fields[7], fields[8]);
	return true;
}

bool parse_thumbnail(HomeCatalog *catalog, char *line)
{
	char *fields[4] {};
	std::size_t count = 0;
	char *cursor = line;
	while (count < sizeof(fields) / sizeof(fields[0])) {
		fields[count++] = cursor;
		char *tab = std::strchr(cursor, '\t');
		if (tab == nullptr)
			break;
		*tab = 0;
		cursor = tab + 1;
	}
	if (count != 3 || std::strcmp(fields[0], "THUMB") != 0 ||
	    !valid_id(fields[1]) || !valid_thumbnail_file(fields[2]) ||
	    !catalog->response_started)
		return false;
	for (std::size_t index = 0; index < catalog->available_count; ++index) {
		if (std::strcmp(catalog->cards[index].video_id, fields[1]) != 0)
			continue;
		if (std::strcmp(catalog->cards[index].thumbnail_path, fields[2]) == 0)
			return false;
		copy_field(catalog->cards[index].thumbnail_path,
			   sizeof(catalog->cards[index].thumbnail_path), fields[2]);
		++catalog->revision;
		return true;
	}
	return false;
}

bool parse_thumbnails_done(HomeCatalog *catalog, char *line)
{
	char *fields[3] {};
	std::size_t count = 0;
	char *cursor = line;
	while (count < sizeof(fields) / sizeof(fields[0])) {
		fields[count++] = cursor;
		char *tab = std::strchr(cursor, '\t');
		if (tab == nullptr)
			break;
		*tab = 0;
		cursor = tab + 1;
	}
	std::size_t reported = 0;
	if (count != 2 || std::strcmp(fields[0], "THUMBS") != 0 ||
	    !parse_count(fields[1], kHomeCardLimit, &reported) ||
	    reported > catalog->response_count)
		return false;
	std::size_t actual = 0;
	for (std::size_t index = catalog->response_base_index;
	     index < catalog->available_count; ++index) {
		if (catalog->cards[index].thumbnail_path[0] != 0)
			++actual;
	}
	if (reported != actual)
		return false;
	catalog->thumbnails_loading = false;
	++catalog->revision;
	return true;
}

bool parse_batch(HomeCatalog *catalog, char *line)
{
	char *fields[5] {};
	std::size_t count = 0;
	char *cursor = line;
	while (count < sizeof(fields) / sizeof(fields[0])) {
		fields[count++] = cursor;
		char *tab = std::strchr(cursor, '\t');
		if (tab == nullptr)
			break;
		*tab = 0;
		cursor = tab + 1;
	}
	std::size_t batch_count = 0;
	std::size_t cumulative_count = 0;
	if (count != 4 || std::strcmp(fields[0], "BATCH") != 0 ||
	    !parse_count(fields[1], kHomePageSize, &batch_count) ||
	    !parse_count(fields[2], kHomeCardLimit, &cumulative_count) ||
	    batch_count == 0 || batch_count != catalog->pending_count ||
	    cumulative_count != catalog->response_count + catalog->pending_count)
		return false;
	const bool more = std::strcmp(fields[3], "more=YES") == 0;
	if (!more && std::strcmp(fields[3], "more=NO") != 0)
		return false;
	if (more && cumulative_count >= kHomeCardLimit)
		return false;
	commit_pending(catalog);
	catalog->network_ready = true;
	catalog->more_expected = more;
	++catalog->revision;
	request_page_near_end(catalog);
	return true;
}

bool parse_done(HomeCatalog *catalog, char *line)
{
	char *fields[6] {};
	std::size_t count = 0;
	char *cursor = line;
	while (count < sizeof(fields) / sizeof(fields[0])) {
		fields[count++] = cursor;
		char *tab = std::strchr(cursor, '\t');
		if (tab == nullptr)
			break;
		*tab = 0;
		cursor = tab + 1;
	}
	if (count != 5 || std::strcmp(fields[0], "DONE") != 0)
		return false;
	std::size_t reported = 0;
	if (!parse_count(fields[1], kHomePageSize, &reported) || reported == 0 ||
	    reported != catalog->response_count + catalog->pending_count)
		return false;
	const bool hit = std::strcmp(fields[3], "cache=HIT") == 0;
	const bool stale = std::strcmp(fields[3], "cache=STALE") == 0;
	if (!hit && !stale && std::strcmp(fields[3], "cache=MISS") != 0)
		return false;
	std::size_t next_offset = 0;
	const bool at_end = std::strcmp(fields[4], "next=END") == 0;
	if (!at_end) {
		const char prefix[] = "next=";
		if (std::strncmp(fields[4], prefix, sizeof(prefix) - 1) != 0 ||
		    !parse_count(fields[4] + sizeof(prefix) - 1, kHomeCardLimit,
				 &next_offset) ||
		    next_offset <= catalog->response_base_index)
			return false;
	}
	// Accept the original ITEM...DONE shape for an eight-card helper while
	// using BATCH markers for progressive 24-card helpers.
	commit_pending(catalog);
	catalog->network_ready = true;
	catalog->cache_hit = hit || stale;
	catalog->cache_stale = stale;
	catalog->loading = false;
	catalog->thumbnails_loading = true;
	catalog->next_offset = at_end ? catalog->available_count : next_offset;
	catalog->more_expected = !at_end;
	catalog->append_response = false;
	++catalog->revision;
	request_page_near_end(catalog);
	return true;
}

bool consume_line(HomeCatalog *catalog, char *line)
{
	if (std::strncmp(line, "ITEM\t", 5) == 0)
		return parse_item(catalog, line);
	if (std::strncmp(line, "BATCH\t", 6) == 0)
		return parse_batch(catalog, line);
	if (std::strncmp(line, "DONE\t", 5) == 0)
		return parse_done(catalog, line);
	if (std::strncmp(line, "THUMB\t", 6) == 0)
		return parse_thumbnail(catalog, line);
	if (std::strncmp(line, "THUMBS\t", 7) == 0)
		return parse_thumbnails_done(catalog, line);
	return false;
}

} // namespace

void home_catalog_init(HomeCatalog *catalog)
{
	if (catalog == nullptr)
		return;
	*catalog = HomeCatalog {};
	for (const auto &item : kFallback) {
		HomeCard *card = &catalog->cards[catalog->count++];
		char thumbnail[kHomeThumbnailBytes] {};
		std::snprintf(thumbnail, sizeof(thumbnail),
			      "https://i.ytimg.com/vi/%s/mqdefault.jpg", item.id);
		make_card(card, item.id, item.title, item.channel, item.published,
			  item.duration, thumbnail, nullptr, nullptr);
	}
	catalog->available_count = catalog->count;
	catalog->focus = HomeFocus::card;
	catalog->selected = 0;
	catalog->revision = 1;
}

static void terminate_and_reap(pid_t *pid)
{
	if (pid == nullptr || *pid <= 0)
		return;
	(void)kill(*pid, SIGTERM);
	for (int attempt = 0; attempt < 25; ++attempt) {
		const pid_t result = waitpid(*pid, nullptr, WNOHANG);
		if (result == *pid || (result < 0 && errno == ECHILD)) {
			*pid = -1;
			return;
		}
		if (result < 0 && errno != EINTR)
			break;
		usleep(2000);
	}
	(void)kill(*pid, SIGKILL);
	while (waitpid(*pid, nullptr, 0) < 0 && errno == EINTR) {}
	*pid = -1;
}

void home_catalog_cancel(HomeCatalog *catalog)
{
	if (catalog == nullptr)
		return;
	if (catalog->helper_stdout >= 0) {
		(void)close(catalog->helper_stdout);
		catalog->helper_stdout = -1;
	}
	if (catalog->helper_pid > 0) {
		terminate_and_reap(&catalog->helper_pid);
	}
	terminate_and_reap(&catalog->prewarm_pid);
	catalog->prewarm_complete = false;
	catalog->loading = false;
	catalog->more_expected = false;
	catalog->load_more_requested = false;
	catalog->thumbnails_loading = false;
	catalog->input_used = 0;
}

bool home_catalog_maintain_prewarm(HomeCatalog *catalog,
				   const char *helper_path)
{
	if (catalog == nullptr || catalog->prewarm_complete)
		return false;
	if (catalog->prewarm_pid > 0) {
		int status = 0;
		const pid_t result = waitpid(catalog->prewarm_pid, &status, WNOHANG);
		if (result == 0 || (result < 0 && errno == EINTR))
			return true;
		catalog->prewarm_pid = -1;
		// Prewarm is best-effort.  Do not create a hot respawn loop when the
		// network is offline; the next interactive source change permits one
		// later retry.
		catalog->prewarm_complete = true;
		return result > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
	}
	if (catalog->helper_stdout >= 0 || catalog->loading ||
	    !catalog->network_ready || !regular_executable(helper_path))
		return false;
	posix_spawn_file_actions_t actions {};
	int error = posix_spawn_file_actions_init(&actions);
	if (error == 0)
		error = posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO,
			"/dev/null", O_WRONLY, 0);
	char *arguments[kHomeChannelCount + 3] {};
	arguments[0] = const_cast<char *>(helper_path);
	arguments[1] = const_cast<char *>("prewarm");
	for (std::size_t index = 0; index < kHomeChannelCount; ++index)
		arguments[index + 2] = const_cast<char *>(kChannels[index].channel_id);
	pid_t child = -1;
	if (error == 0)
		error = posix_spawn(&child, helper_path, &actions, nullptr, arguments,
				    environ);
	(void)posix_spawn_file_actions_destroy(&actions);
	if (error != 0)
		return false;
	catalog->prewarm_pid = child;
	return true;
}

static bool home_catalog_start_mode(HomeCatalog *catalog, const char *helper_path,
				    const char *mode, const char *value,
				    std::size_t offset, bool append)
{
	if (catalog == nullptr || !regular_executable(helper_path) || mode == nullptr ||
	    value == nullptr || value[0] == 0 ||
	    (std::strcmp(mode, "search") != 0 && std::strcmp(mode, "channel") != 0) ||
	    std::strchr(value, '\n') != nullptr || std::strchr(value, '\r') != nullptr ||
	    std::strchr(value, '\t') != nullptr)
		return false;
	if (offset > kHomeCardLimit || (append && offset != catalog->available_count))
		return false;
	home_catalog_cancel(catalog);
	int output[2] {-1, -1};
	if (pipe(output) != 0)
		return false;
	posix_spawn_file_actions_t actions {};
	int error = posix_spawn_file_actions_init(&actions);
	if (error == 0)
		error = posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO);
	if (error == 0)
		error = posix_spawn_file_actions_addclose(&actions, output[0]);
	char offset_text[24] {};
	std::snprintf(offset_text, sizeof(offset_text), "%zu", offset);
	char *const arguments[] = {
		const_cast<char *>(helper_path), const_cast<char *>(mode),
		const_cast<char *>(value), offset_text, nullptr,
	};
	pid_t child = -1;
	if (error == 0)
		error = posix_spawn(&child, helper_path, &actions, nullptr, arguments, environ);
	(void)posix_spawn_file_actions_destroy(&actions);
	(void)close(output[1]);
	if (error != 0) {
		(void)close(output[0]);
		return false;
	}
	const int flags = fcntl(output[0], F_GETFL);
	if (flags < 0 || fcntl(output[0], F_SETFL, flags | O_NONBLOCK) != 0) {
		(void)close(output[0]);
		(void)kill(child, SIGTERM);
		while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {}
		return false;
	}
	catalog->helper_pid = child;
	catalog->helper_stdout = output[0];
	catalog->pending_count = 0;
	catalog->response_count = 0;
	catalog->response_started = append;
	catalog->append_response = append;
	catalog->response_base_index = append ? catalog->available_count : 0;
	if (!append && catalog->count == 0) {
		catalog->network_ready = false;
		catalog->cache_hit = false;
		catalog->cache_stale = false;
	}
	catalog->loading = true;
	catalog->more_expected = true;
	catalog->load_more_requested = false;
	catalog->thumbnails_loading = false;
	catalog->input_used = 0;
	return true;
}

bool home_catalog_start(HomeCatalog *catalog, const char *helper_path,
			const char *query)
{
	return home_catalog_start_mode(catalog, helper_path, "search", query, 0,
				       false);
}

bool home_catalog_start_channel(HomeCatalog *catalog, const char *helper_path,
				const char *channel_id)
{
	if (!valid_channel_id(channel_id))
		return false;
	return home_catalog_start_mode(catalog, helper_path, "channel", channel_id,
				       0, false);
}

bool home_catalog_continue(HomeCatalog *catalog, const char *helper_path)
{
	if (catalog == nullptr || !catalog->load_more_requested || catalog->loading ||
	    catalog->helper_stdout >= 0 || !catalog->more_expected ||
	    catalog->available_count >= kHomeCardLimit ||
	    catalog->next_offset != catalog->available_count)
		return false;
	const char *mode = home_catalog_channel_active(catalog) ? "channel" : "search";
	const char *value = home_catalog_channel_active(catalog) ?
		kChannels[catalog->active_channel].channel_id : home_catalog_query(catalog);
	return home_catalog_start_mode(catalog, helper_path, mode, value,
				       catalog->next_offset, true);
}

bool home_catalog_poll(HomeCatalog *catalog)
{
	if (catalog == nullptr || catalog->helper_stdout < 0)
		return false;
	bool changed = false;
	for (;;) {
		const std::size_t room = sizeof(catalog->input) - 1 - catalog->input_used;
		if (room == 0) {
			home_catalog_cancel(catalog);
			return changed;
		}
		const ssize_t length = read(catalog->helper_stdout,
			catalog->input + catalog->input_used, room);
		if (length < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			home_catalog_cancel(catalog);
			break;
		}
		if (length == 0) {
			(void)close(catalog->helper_stdout);
			catalog->helper_stdout = -1;
			if (catalog->helper_pid > 0) {
				(void)waitpid(catalog->helper_pid, nullptr, 0);
				catalog->helper_pid = -1;
			}
			if (catalog->loading || catalog->thumbnails_loading) {
				catalog->loading = false;
				catalog->thumbnails_loading = false;
					catalog->more_expected = false;
					catalog->pending_count = 0;
					catalog->append_response = false;
				++catalog->revision;
				changed = true;
			}
			break;
		}
		catalog->input_used += static_cast<std::size_t>(length);
		catalog->input[catalog->input_used] = 0;
		char *line = catalog->input;
		for (;;) {
			char *newline = std::strchr(line, '\n');
			if (newline == nullptr)
				break;
			*newline = 0;
			if (newline > line && newline[-1] == '\r')
				newline[-1] = 0;
			changed = consume_line(catalog, line) || changed;
			line = newline + 1;
		}
		const std::size_t remaining = catalog->input + catalog->input_used - line;
		if (remaining != 0)
			std::memmove(catalog->input, line, remaining);
		catalog->input_used = remaining;
		catalog->input[remaining] = 0;
	}
	return changed;
}

const char *home_catalog_query(const HomeCatalog *catalog)
{
	return catalog != nullptr ? kQueries[catalog->query_preset % kQueryCount] : "";
}

const HomeChannel *home_catalog_channel(std::size_t index)
{
	return index < kHomeChannelCount ? &kChannels[index] : nullptr;
}

bool home_catalog_channel_active(const HomeCatalog *catalog)
{
	return catalog != nullptr && catalog->active_channel < kHomeChannelCount;
}

const char *home_catalog_source_name(const HomeCatalog *catalog)
{
	if (!home_catalog_channel_active(catalog))
		return "總覽推薦";
	return kChannels[catalog->active_channel].name;
}

static void capture_overview(HomeCatalog *catalog)
{
	std::memcpy(catalog->overview.cards, catalog->cards,
		    sizeof(catalog->overview.cards));
	catalog->overview.count = catalog->count;
	catalog->overview.available_count = catalog->available_count;
	catalog->overview.selected = catalog->selected;
	catalog->overview.query_preset = catalog->query_preset;
	catalog->overview.focus = catalog->focus;
	catalog->overview.network_ready = catalog->network_ready;
	catalog->overview.cache_hit = catalog->cache_hit;
	catalog->overview.cache_stale = catalog->cache_stale;
	catalog->overview.more_expected = catalog->more_expected;
	catalog->overview.valid = true;
}

static void capture_channel(HomeCatalog *catalog)
{
	if (!home_catalog_channel_active(catalog))
		return;
	HomeSnapshot *snapshot = &catalog->channel_snapshots[catalog->active_channel];
	std::memcpy(snapshot->cards, catalog->cards, sizeof(snapshot->cards));
	snapshot->count = catalog->count;
	snapshot->available_count = catalog->available_count;
	snapshot->selected = catalog->selected;
	snapshot->query_preset = catalog->query_preset;
	snapshot->focus = catalog->focus;
	snapshot->network_ready = catalog->network_ready;
	snapshot->cache_hit = catalog->cache_hit;
	snapshot->cache_stale = catalog->cache_stale;
	snapshot->more_expected = catalog->more_expected;
	snapshot->valid = snapshot->count != 0;
}

static void restore_snapshot(HomeCatalog *catalog, const HomeSnapshot *snapshot)
{
	std::memcpy(catalog->cards, snapshot->cards,
		    sizeof(catalog->cards));
	catalog->count = snapshot->count;
	catalog->available_count = snapshot->available_count;
	catalog->selected = snapshot->selected;
	catalog->query_preset = snapshot->query_preset;
	catalog->focus = snapshot->focus;
	catalog->network_ready = snapshot->network_ready;
	catalog->cache_hit = true;
	catalog->cache_stale = true;
	catalog->pending_count = 0;
	catalog->response_count = 0;
	catalog->response_started = false;
	catalog->more_expected = snapshot->more_expected;
	catalog->load_more_requested = false;
	catalog->thumbnails_loading = false;
	catalog->append_response = false;
	catalog->response_base_index = 0;
	catalog->next_offset = catalog->available_count;
}

static void restore_overview(HomeCatalog *catalog)
{
	if (!catalog->overview.valid) {
		home_catalog_init(catalog);
		return;
	}
	restore_snapshot(catalog, &catalog->overview);
}

static void clear_channel_feed(HomeCatalog *catalog)
{
	for (auto &card : catalog->cards)
		card = HomeCard {};
	for (auto &card : catalog->pending)
		card = HomeCard {};
	catalog->count = 0;
	catalog->available_count = 0;
	catalog->pending_count = 0;
	catalog->response_count = 0;
	catalog->selected = 0;
	catalog->focus = HomeFocus::card;
	catalog->network_ready = false;
	catalog->cache_hit = false;
	catalog->cache_stale = false;
	catalog->response_started = false;
	catalog->more_expected = true;
	catalog->load_more_requested = false;
	catalog->thumbnails_loading = false;
	catalog->append_response = false;
	catalog->response_base_index = 0;
	catalog->next_offset = 0;
}

void home_catalog_open_channel_selector(HomeCatalog *catalog)
{
	if (catalog == nullptr)
		return;
	catalog->channel_selector_selected = home_catalog_channel_active(catalog) ?
		catalog->active_channel + 1 : 0;
	catalog->channel_selector_open = true;
	++catalog->revision;
}

void home_catalog_close_channel_selector(HomeCatalog *catalog)
{
	if (catalog == nullptr || !catalog->channel_selector_open)
		return;
	catalog->channel_selector_open = false;
	++catalog->revision;
}

bool home_catalog_return_overview(HomeCatalog *catalog,
				  const char *helper_path)
{
	if (!home_catalog_channel_active(catalog))
		return false;
	capture_channel(catalog);
	home_catalog_cancel(catalog);
	catalog->active_channel = kHomeNoChannel;
	catalog->channel_selector_open = false;
	catalog->channel_selector_selected = 0;
	restore_overview(catalog);
	++catalog->revision;
	const bool refreshing = home_catalog_start(catalog, helper_path,
						 home_catalog_query(catalog));
	return refreshing || catalog->count != 0;
}

bool home_catalog_apply_channel_selector(HomeCatalog *catalog,
					 const char *helper_path)
{
	if (catalog == nullptr || !catalog->channel_selector_open ||
	    catalog->channel_selector_selected > kHomeChannelCount)
		return false;
	const std::size_t choice = catalog->channel_selector_selected;
	catalog->channel_selector_open = false;
	if (choice == 0) {
		if (!home_catalog_channel_active(catalog)) {
			++catalog->revision;
			return true;
		}
		return home_catalog_return_overview(catalog, helper_path);
	}
	const std::size_t channel = choice - 1;
	if (catalog->active_channel == channel) {
		++catalog->revision;
		return true;
	}
	if (!home_catalog_channel_active(catalog))
		capture_overview(catalog);
	else
		capture_channel(catalog);
	home_catalog_cancel(catalog);
	catalog->active_channel = channel;
	const HomeSnapshot *snapshot = &catalog->channel_snapshots[channel];
	if (snapshot->valid)
		restore_snapshot(catalog, snapshot);
	else
		clear_channel_feed(catalog);
	++catalog->revision;
	const bool refreshing = home_catalog_start_channel(
		catalog, helper_path, kChannels[channel].channel_id);
	return refreshing || snapshot->valid;
}

bool home_catalog_step_channel(HomeCatalog *catalog, const char *helper_path,
			       int direction)
{
	if (catalog == nullptr || (direction != -1 && direction != 1))
		return false;
	if (!home_catalog_channel_active(catalog)) {
		home_catalog_open_channel_selector(catalog);
		catalog->channel_selector_selected = direction > 0 ? 1 : kHomeChannelCount;
	} else {
		catalog->channel_selector_open = true;
		const std::size_t current = catalog->active_channel;
		const std::size_t next = direction > 0 ?
			(current + 1) % kHomeChannelCount :
			(current + kHomeChannelCount - 1) % kHomeChannelCount;
		catalog->channel_selector_selected = next + 1;
	}
	return home_catalog_apply_channel_selector(catalog, helper_path);
}

void home_catalog_left(HomeCatalog *catalog)
{
	if (catalog == nullptr)
		return;
	if (catalog->channel_selector_open) {
		catalog->channel_selector_selected =
			(catalog->channel_selector_selected + kHomeChannelCount) %
			(kHomeChannelCount + 1);
		++catalog->revision;
		return;
	}
	bool changed = false;
	if (catalog->focus == HomeFocus::search) {
		catalog->query_preset = (catalog->query_preset + kQueryCount - 1) % kQueryCount;
		changed = true;
	} else if (catalog->selected > 0) {
		--catalog->selected;
		changed = true;
	}
	if (changed)
		++catalog->revision;
}

void home_catalog_right(HomeCatalog *catalog)
{
	if (catalog == nullptr)
		return;
	if (catalog->channel_selector_open) {
		catalog->channel_selector_selected =
			(catalog->channel_selector_selected + 1) %
			(kHomeChannelCount + 1);
		++catalog->revision;
		return;
	}
	bool changed = false;
	if (catalog->focus == HomeFocus::search) {
		catalog->query_preset = (catalog->query_preset + 1) % kQueryCount;
		changed = true;
	} else if (catalog->selected + 1 < catalog->count) {
		++catalog->selected;
		changed = true;
		request_page_near_end(catalog);
	}
	if (changed)
		++catalog->revision;
}

void home_catalog_up(HomeCatalog *catalog)
{
	if (catalog == nullptr)
		return;
	if (catalog->channel_selector_open) {
		catalog->channel_selector_selected =
			(catalog->channel_selector_selected + kHomeChannelCount) %
			(kHomeChannelCount + 1);
		++catalog->revision;
		return;
	}
	if (catalog->focus == HomeFocus::search)
		return;
	bool changed = false;
	if (catalog->selected == 0 && !home_catalog_channel_active(catalog)) {
		catalog->focus = HomeFocus::search;
		changed = true;
	} else if (catalog->selected > 0) {
		--catalog->selected;
		changed = true;
	}
	if (changed)
		++catalog->revision;
}

void home_catalog_down(HomeCatalog *catalog)
{
	if (catalog == nullptr)
		return;
	if (catalog->channel_selector_open) {
		catalog->channel_selector_selected =
			(catalog->channel_selector_selected + 1) %
			(kHomeChannelCount + 1);
		++catalog->revision;
		return;
	}
	if (catalog->count == 0)
		return;
	if (catalog->focus == HomeFocus::search) {
		catalog->focus = HomeFocus::card;
		catalog->selected = 0;
		++catalog->revision;
	} else if (catalog->selected + 1 < catalog->count) {
		++catalog->selected;
		++catalog->revision;
	}
	request_page_near_end(catalog);
}

bool home_catalog_activate(const HomeCatalog *catalog, char *watch_url,
			   std::size_t watch_url_size)
{
	if (catalog == nullptr || watch_url == nullptr || watch_url_size == 0 ||
	    catalog->channel_selector_open ||
	    catalog->focus != HomeFocus::card || catalog->selected >= catalog->count)
		return false;
	copy_field(watch_url, watch_url_size, catalog->cards[catalog->selected].watch_url);
	return watch_url[0] != 0;
}

} // namespace rg40xxv_youtube
