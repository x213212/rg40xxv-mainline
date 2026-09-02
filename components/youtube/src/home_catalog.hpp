#ifndef RG40XXV_YOUTUBE_HOME_CATALOG_HPP
#define RG40XXV_YOUTUBE_HOME_CATALOG_HPP

#include <cstddef>
#include <sys/types.h>

namespace rg40xxv_youtube {

constexpr std::size_t kHomePageSize = 8;
// A source is fetched one page at a time.  Keep the in-memory/cache bound
// large enough for continuous scrolling without allowing an unbounded feed.
constexpr std::size_t kHomeCardLimit = 96;
constexpr std::size_t kHomeTitleBytes = 256;
constexpr std::size_t kHomeChannelBytes = 128;
constexpr std::size_t kHomePublishedBytes = 96;
constexpr std::size_t kHomeThumbnailBytes = 1024;
constexpr std::size_t kHomeThumbnailPathBytes = 512;
constexpr std::size_t kHomeChannelCount = 8;
constexpr std::size_t kHomeNoChannel = kHomeChannelCount;

struct HomeChannel {
	const char *name;
	const char *channel_id;
};

struct HomeCard {
	char video_id[12] {};
	char title[kHomeTitleBytes] {};
	char channel[kHomeChannelBytes] {};
	char published[kHomePublishedBytes] {};
	unsigned duration_seconds = 0;
	char thumbnail[kHomeThumbnailBytes] {};
	char watch_url[64] {};
	char thumbnail_path[kHomeThumbnailPathBytes] {};
};

enum class HomeFocus {
	search,
	card,
};

struct HomeSnapshot {
	HomeCard cards[kHomeCardLimit] {};
	std::size_t count = 0;
	std::size_t available_count = 0;
	std::size_t selected = 0;
	std::size_t query_preset = 0;
	HomeFocus focus = HomeFocus::card;
	bool network_ready = false;
	bool cache_hit = false;
	bool cache_stale = false;
	bool more_expected = false;
	bool valid = false;
};

struct HomeCatalog {
	HomeCard cards[kHomeCardLimit] {};
	HomeCard pending[kHomePageSize] {};
	// count is the number exposed to the view. available_count also includes
	// complete eight-card batches waiting behind the scroll threshold.
	std::size_t count = 0;
	std::size_t available_count = 0;
	std::size_t pending_count = 0;
	std::size_t response_count = 0;
	std::size_t selected = 0;
	std::size_t query_preset = 0;
	std::size_t active_channel = kHomeNoChannel;
	std::size_t channel_selector_selected = 0;
	HomeFocus focus = HomeFocus::card;
	bool channel_selector_open = false;
	bool loading = false;
	bool network_ready = false;
	bool cache_hit = false;
	bool cache_stale = false;
	bool response_started = false;
	bool more_expected = false;
	bool load_more_requested = false;
	bool thumbnails_loading = false;
	bool append_response = false;
	std::size_t response_base_index = 0;
	std::size_t next_offset = 0;
	unsigned long revision = 0;
	pid_t helper_pid = -1;
	int helper_stdout = -1;
	char input[8193] {};
	std::size_t input_used = 0;
	HomeSnapshot overview {};
	HomeSnapshot channel_snapshots[kHomeChannelCount] {};
	pid_t prewarm_pid = -1;
	bool prewarm_complete = false;
};

// Populate three immediately-visible real videos while the network-backed
// metadata feed is loading. A valid first BATCH replaces them; thumbnail
// paths can arrive later without blocking metadata display.
void home_catalog_init(HomeCatalog *catalog);

// Spawn `youtube_feed.py search QUERY` without a shell. The helper must
// implement the bounded TSV protocol documented in that file. This call never
// blocks on network I/O.
bool home_catalog_start(HomeCatalog *catalog, const char *helper_path,
			const char *query);

// Spawn the same bounded helper against one immutable YouTube channel ID.
// The lower video grid uses the existing progressive paging/cache protocol.
bool home_catalog_start_channel(HomeCatalog *catalog, const char *helper_path,
				const char *channel_id);

// Consume any currently available helper output. Returns true when visible
// state changed; it never waits for the child.
bool home_catalog_poll(HomeCatalog *catalog);

// Stop/reap the current helper and close its pipe. Safe to call repeatedly.
void home_catalog_cancel(HomeCatalog *catalog);

const char *home_catalog_query(const HomeCatalog *catalog);
const HomeChannel *home_catalog_channel(std::size_t index);
const char *home_catalog_source_name(const HomeCatalog *catalog);
bool home_catalog_channel_active(const HomeCatalog *catalog);
void home_catalog_open_channel_selector(HomeCatalog *catalog);
void home_catalog_close_channel_selector(HomeCatalog *catalog);
// Apply the modal choice: 0 restores the in-memory overview and refreshes its
// normal search feed; 1..8 switch the lower grid to that channel's videos.
bool home_catalog_apply_channel_selector(HomeCatalog *catalog,
					 const char *helper_path);
bool home_catalog_return_overview(HomeCatalog *catalog,
				  const char *helper_path);
// L1/R1 route directly between curated channels.  From overview, previous
// starts at the last channel and next starts at the first channel.
bool home_catalog_step_channel(HomeCatalog *catalog, const char *helper_path,
			       int direction);
// Start/reap one low-priority metadata-only coordinator for the curated
// channels.  Interactive channel changes retire it before starting their page.
bool home_catalog_maintain_prewarm(HomeCatalog *catalog,
				   const char *helper_path);
// If the selected row is within the final two visible items, start the next
// bounded page without clearing cards already on screen.
bool home_catalog_continue(HomeCatalog *catalog, const char *helper_path);
void home_catalog_left(HomeCatalog *catalog);
void home_catalog_right(HomeCatalog *catalog);
void home_catalog_up(HomeCatalog *catalog);
void home_catalog_down(HomeCatalog *catalog);

// A on SEARCH requests a refresh of the currently displayed query. A on a
// card returns its watch URL. The caller remains responsible for starting the
// endpoint broker/player so this module never owns DRM or libmpv state.
bool home_catalog_activate(const HomeCatalog *catalog, char *watch_url,
			   std::size_t watch_url_size);

} // namespace rg40xxv_youtube

#endif
