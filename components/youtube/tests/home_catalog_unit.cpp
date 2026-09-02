#include "home_catalog.hpp"

#include <cstdio>
#include <cstring>
#include <unistd.h>

using namespace rg40xxv_youtube;

int main(int argc, char **argv)
{
	if (argc != 2)
		return 64;
	HomeCatalog catalog {};
	home_catalog_init(&catalog);
	if (catalog.count != 3 || catalog.available_count != 3 ||
	    catalog.focus != HomeFocus::card || catalog.selected != 0 ||
	    std::strcmp(home_catalog_query(&catalog), "台灣 熱門") != 0)
		return 1;
	char initial_url[64] {};
	if (!home_catalog_activate(&catalog, initial_url, sizeof(initial_url)) ||
	    std::strcmp(initial_url,
		"https://www.youtube.com/watch?v=GwtNiL9eEYk") != 0)
		return 2;
	if (!home_catalog_start(&catalog, argv[1], "台灣 科技"))
		return 3;
	for (int attempt = 0; attempt < 200 && !catalog.network_ready; ++attempt) {
		(void)home_catalog_poll(&catalog);
		usleep(1000);
	}
	if (!catalog.network_ready || !catalog.loading || catalog.count != 8 ||
	    catalog.available_count != 8 || catalog.response_count != 8 ||
	    std::strcmp(catalog.cards[0].title, "Real card 1") != 0 ||
	    std::strcmp(catalog.cards[0].published, "1 days ago") != 0 ||
	    catalog.cards[0].duration_seconds != 61 ||
	    catalog.cards[0].thumbnail_path[0] != 0)
		return 4;
	for (int step = 0; step < 6; ++step)
		home_catalog_down(&catalog);
	if (catalog.focus != HomeFocus::card || catalog.selected != 6 ||
	    !catalog.load_more_requested)
		return 5;
	for (int attempt = 0; attempt < 1000 && catalog.available_count < 16; ++attempt) {
		(void)home_catalog_poll(&catalog);
		(void)home_catalog_continue(&catalog, argv[1]);
		usleep(1000);
	}
	if (!catalog.network_ready || !catalog.cache_hit ||
	    !catalog.cache_stale || catalog.count != 16 ||
	    catalog.available_count != 16 || catalog.response_count != 8 ||
	    catalog.selected != 6)
		return 6;
	for (int step = 0; step < 8; ++step)
		home_catalog_down(&catalog);
	for (int attempt = 0; attempt < 1000 && catalog.available_count < 24; ++attempt) {
		(void)home_catalog_poll(&catalog);
		(void)home_catalog_continue(&catalog, argv[1]);
		usleep(1000);
	}
	if (catalog.selected != 14 || catalog.count != 24 ||
	    catalog.available_count != 24 || !catalog.more_expected)
		return 7;
	char url[64] {};
	if (!home_catalog_activate(&catalog, url, sizeof(url)) ||
	    std::strcmp(url, "https://www.youtube.com/watch?v=HomeCard015") != 0)
		return 8;
	for (int attempt = 0; attempt < 500 && catalog.helper_stdout >= 0; ++attempt) {
		(void)home_catalog_poll(&catalog);
		usleep(1000);
	}
	if (catalog.helper_stdout >= 0)
		return 9;
	for (std::size_t outer = 0; outer < catalog.available_count; ++outer) {
		if (catalog.cards[outer].thumbnail_path[0] != '/')
			return 10;
		for (std::size_t inner = outer + 1; inner < catalog.available_count; ++inner) {
			if (std::strcmp(catalog.cards[outer].video_id,
					catalog.cards[inner].video_id) == 0)
				return 11;
		}
	}
	while (catalog.focus == HomeFocus::card)
		home_catalog_up(&catalog);
	if (catalog.focus != HomeFocus::search)
		return 12;
	home_catalog_right(&catalog);
	if (std::strcmp(home_catalog_query(&catalog), "遊戲 熱門") != 0)
		return 13;
	const char *expected_names[kHomeChannelCount] = {
		"總裁聊聊", "我是阿史", "你可敢信尼可拉斯楊", "攝徒日記 Fun TV",
		"壹電視 NEXT TV", "曉涵哥來了", "표은지", "游庭皓的財經皓角",
	};
	const char *expected_ids[kHomeChannelCount] = {
		"UC2j5Kw9qDWCZmU_emgqeguA", "UCcL163py441fTFfWy5tBjoQ",
		"UCfc5rX7XNwEvY0cgXygkDbQ", "UCvTe3Z7TZsjGzUERx4Ce6zA",
		"UC6hWBu1hfbNNk8Yeokd5aZQ", "UCvoBl4rnVsetDKA_Tdk-jeA",
		"UC9K0rLE1SMh86nVxzkCBpNA", "UC0lbAQVpenvfA2QqzsRtL_g",
	};
	for (std::size_t index = 0; index < kHomeChannelCount; ++index) {
		const HomeChannel *channel = home_catalog_channel(index);
		if (channel == nullptr || std::strcmp(channel->name, expected_names[index]) != 0 ||
		    std::strcmp(channel->channel_id, expected_ids[index]) != 0)
			return 14;
	}
	home_catalog_open_channel_selector(&catalog);
	if (!catalog.channel_selector_open || catalog.channel_selector_selected != 0)
		return 15;
	home_catalog_down(&catalog);
	if (catalog.channel_selector_selected != 1 ||
	    !home_catalog_apply_channel_selector(&catalog, argv[1]))
		return 16;
	for (int attempt = 0; attempt < 200 && !catalog.network_ready; ++attempt) {
		(void)home_catalog_poll(&catalog);
		usleep(1000);
	}
	if (!home_catalog_channel_active(&catalog) || catalog.active_channel != 0 ||
	    std::strcmp(home_catalog_source_name(&catalog), "總裁聊聊") != 0 ||
	    catalog.count != 8 || catalog.focus != HomeFocus::card)
		return 17;
	// A direct R1-equivalent switch changes the heading source immediately.
	if (!home_catalog_step_channel(&catalog, argv[1], 1) ||
	    catalog.active_channel != 1 ||
	    std::strcmp(home_catalog_source_name(&catalog), "我是阿史") != 0)
		return 19;
	for (int attempt = 0; attempt < 300 && !catalog.network_ready; ++attempt) {
		(void)home_catalog_poll(&catalog);
		usleep(1000);
	}
	if (!home_catalog_step_channel(&catalog, argv[1], -1) ||
	    catalog.active_channel != 0 || catalog.count != 8 ||
	    std::strcmp(catalog.cards[0].published, "1 days ago") != 0)
		return 20;
	// The previous channel snapshot is restored before its refresh helper can
	// produce a single byte.
	if (!catalog.loading || !catalog.network_ready || !catalog.cache_stale)
		return 21;
	const bool overview_started = home_catalog_return_overview(&catalog, argv[1]);
	if (!overview_started || home_catalog_channel_active(&catalog) ||
	    catalog.count != 24 || catalog.selected != 0 ||
	    catalog.focus != HomeFocus::search ||
	    std::strcmp(home_catalog_query(&catalog), "遊戲 熱門") != 0) {
		std::fprintf(stderr,
			     "overview_started=%d active=%d count=%zu selected=%zu query=%s\n",
			     overview_started ? 1 : 0,
			     home_catalog_channel_active(&catalog) ? 1 : 0,
			     catalog.count, catalog.selected, home_catalog_query(&catalog));
		return 18;
	}
	home_catalog_cancel(&catalog);
	std::puts("YOUTUBE_HOME_CATALOG_UNIT PASS cards=96-bound pages=8xCONTINUOUS threshold=LAST_TWO selection=PRESERVED metadata=TITLE+PUBLISHED+DURATION_BEFORE_THUMBNAILS thumbnails=PROGRESSIVE helper=NONBLOCKING channels=8xIMMUTABLE_ID snapshots=IMMEDIATE_REUSE L1_R1=DIRECT selector=MODAL overview=RESTORED");
	return 0;
}
