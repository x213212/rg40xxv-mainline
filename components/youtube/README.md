# RG40XX V native YouTube client

This candidate keeps browsing and playback native: a controller-first C++ UI,
Google device authorization for account metadata, and AArch64 libmpv/FFmpeg for
media. `yt-dlp` lives in an owner-private persistent resolver service; it is
never the UI or decoder.

## Controller-first MVP

The formal route is the SDL texture scene; the earlier frontend remains a
separately named diagnostic:

- `src/sdl_texture_scene.cpp`: the controller-first HOME/feed/search and
  libmpv software-rendered PLAYER scene. The packaged canonical launcher
  invokes this binary and passes direct `--broker`/`--stream` arguments
  unchanged. X opens the channel selector; A applies a channel to the lower
  video grid; B restores the in-memory overview before it ever exits HOME.
- `src/native_frontend.cpp`: the legacy 640x480 SDL2/SDL_ttf diagnostic with fixed
  real YouTube entries. D-pad moves, A plays, and B returns. It never grabs
  evdev, so MENU+START remains owned by the existing outer exit supervisor.
- `bin/rg40xxv-youtube-native-session`: validates one watch URL, creates an
  owner-only runtime directory, runs the resolver, starts the loopback range
  bridge, passes only loopback endpoints to the player, and removes the signed
  configuration on every exit path.
- `src/player_probe.cpp`: keeps the headless QEMU modes and adds a real
  libmpv DRM/ALSA mode. A or START toggles pause, B returns, D-pad left/right
  seeks 10 seconds, and L1/R1 seeks 30 seconds. The controller fd is not
  grabbed. DRM defaults to `/dev/dri/card1` and `DSI-1`, with environment
  overrides for a device-specific smoke test. Decode remains the known-safe
  FFmpeg software H.264 path until Cedrus is separately proven.

Build and run the bounded host/package gates:

```sh
./build.sh
./tests/test-host.sh
./tests/test-package.sh
```

After a reviewed p7 package installs this component under
`/opt/rg40xxv/youtube`, the exact live smoke entry is:

```sh
/opt/rg40xxv/bin/rg40xxv-exit-chord --grace-ms 1500 -- \
  /opt/rg40xxv/youtube/bin/rg40xxv-youtube-native
```

The formal candidate admission uses `evidence_scope=COMPONENT_GATE`, so the
tile is launchable and visibly marked `VERIFY` without promoting the overall
component to user visual acceptance. The exact-binary receipt in
`evidence/DEVICE_YOUTUBE_NATIVE_20260831.md` records observed playback, ALSA
progress, timeline advancement, and a non-uniform rendered frame. Its scene,
daemon, broker, and device-p8 identities are bound by the formal release
verifier, but the observation used a non-target p8 and does not reconstruct the
full hot-bundle closure. It therefore cannot promote formal device acceptance:
playback, audio, timeline, video, and memory remain `PENDING_DEVICE`, while
user visual/color acceptance remains `PENDING_DEVICE_USER`. Host/QEMU output
alone is never device visual or audio acceptance.

## Resolver cache and prefetch contract

`tools/resolver_cache.py` keeps signed configurations under its caller-selected
private root. The formal wrapper uses
`/mnt/data/rg40xxv/state/home/.cache/rg40xxv-youtube/resolver`; the tool-only
fallback remains `/run/rg40xxv-youtube-native/cache`. Directories are 0700; entries,
single-use claims, stable per-video locks, and two global resolver-slot locks
are 0600. Only canonical 11-character YouTube IDs become filenames.

The persistent HOME/PLAYER frontend presents HOME before doing resolver work.
After focus remains stable for 500 ms it starts at most one cache-only
coordinator for the selected card and its existing previous/next neighbours:

```sh
/opt/rg40xxv/youtube/tools/resolver_cache.py prefetch-set SELECTED_URL PREVIOUS_URL NEXT_URL
```

Changing focus does not start an endpoint broker or range bridge. A is the only
HOME action that may call the endpoint broker. Edge cards naturally omit the
unavailable neighbour. Per-video `flock` deduplicates work and the two global
slots cap all resolver executions at two, even though the bounded coordinator
may queue three distinct IDs. A cache identity is the canonical video ID plus
pinned format 18 and the earliest signed expiry. A config is fresh only
if the earliest strict signed-URL `expire` covers media duration plus 600
seconds, cache age is at most 900 seconds, and wall/monotonic drift is at most
30 seconds. The on-disk resolver cache retains at most 96 entries, uses a fixed
64-slot lock pool, permits at most 64 live claims, and removes claims left by a
crash after one hour.

On A, obtain a single-use claim with `resolver_cache.py acquire WATCH_URL`.
Stdout contains only its private claim path. Under the video lock, acquire
strictly revalidates the entry and atomically publishes an independent 0600
claim snapshot. The bounded entry remains reusable for 15 minutes, so returning
HOME or replaying the same video does not start yt-dlp again. Call
`resolver_cache.py release CLAIM` immediately after the range bridge publishes
readiness; it has already loaded the URLs in memory. Release removes only that
claim. `rg40xxv-youtube-native-session` implements this lifecycle for the
bounded launcher smoke path. The persistent frontend can call the same CLI
contract without tearing down HOME while resolution is pending.

## Persistent resolver service

The formal wrapper asks `bin/rg40xxv-youtube-resolver-ensure` to create one
system-manager-owned `rg40xxv-youtube-resolver.service` with two bounded
workers.  The daemon is not a child of the scene or game-session cgroup, so
leaving HOME, MENU+START, or a player failure cannot remove the shared socket
during normal production use.  A hot-test teardown explicitly
stops this unit before replacing its temporary `/usr/local` code and then returns
to the production UI, so a daemon cannot keep executing an older hot-test file.
The AArch64 scene itself owns a nonblocking process lock at
`/run/rg40xxv-youtube/scene.lock`; direct and wrapper launches therefore cannot
overlap video or audio.  The lock descriptor is close-on-exec, so feed,
prefetch and broker children cannot keep a dead scene locked.

The daemon imports yt-dlp once, coalesces duplicate video requests,
uses an owner-private mode-0600 AF_UNIX socket under `/run/rg40xxv-youtube`,
and stores only yt-dlp's reusable support cache under the private persistent
HOME cache. Interactive jobs are dequeued ahead of background jobs. Background
extraction runs on disposable nice-15 threads so a later interactive request
still runs at nice 0 even when an unprivileged process cannot undo nice 15.
Resolver clients verify socket ownership and peer credentials. Background
prefetch requires the daemon and never falls back to a one-shot yt-dlp process;
interactive acquisition retains the bounded one-shot fallback.

## Native HOME and search feed

`tools/youtube_feed.py home` and `tools/youtube_feed.py search QUERY` return
up to 24 cards in progressive eight-card batches without starting a browser.
The fast path parses
YouTube's anonymous `videoRenderer` search response for `hl=zh-TW&gl=TW`;
bundled `yt-dlp --flat-playlist` is only the bounded fallback.  HOME uses the
deterministic query `台灣 熱門` because anonymous `/` can contain no videos.

`tools/youtube_feed.py channel UC...` uses an immutable channel ID and the
official `/channel/UC.../videos` page; it never searches by display name.  The
selector is modal, so the established overview and Taiwan/technology search
remain unchanged.  Selecting a channel only switches the lower video grid;
B restores the prior overview cards immediately and refreshes them in the
background.  The pinned official channel pages are:

| Display name | YouTube channel |
|---|---|
| 總裁聊聊 | [`UC2j5Kw9qDWCZmU_emgqeguA`](https://www.youtube.com/channel/UC2j5Kw9qDWCZmU_emgqeguA) |
| 我是阿史 | [`UCcL163py441fTFfWy5tBjoQ`](https://www.youtube.com/channel/UCcL163py441fTFfWy5tBjoQ) |
| 你可敢信尼可拉斯楊 | [`UCfc5rX7XNwEvY0cgXygkDbQ`](https://www.youtube.com/channel/UCfc5rX7XNwEvY0cgXygkDbQ) |
| 攝徒日記 Fun TV | [`UCvTe3Z7TZsjGzUERx4Ce6zA`](https://www.youtube.com/channel/UCvTe3Z7TZsjGzUERx4Ce6zA) |
| 壹電視 NEXT TV | [`UC6hWBu1hfbNNk8Yeokd5aZQ`](https://www.youtube.com/channel/UC6hWBu1hfbNNk8Yeokd5aZQ) |
| 曉涵哥來了 | [`UCvoBl4rnVsetDKA_Tdk-jeA`](https://www.youtube.com/channel/UCvoBl4rnVsetDKA_Tdk-jeA) |
| 표은지 | [`UC9K0rLE1SMh86nVxzkCBpNA`](https://www.youtube.com/channel/UC9K0rLE1SMh86nVxzkCBpNA) |
| 游庭皓的財經皓角 | [`UC0lbAQVpenvfA2QqzsRtL_g`](https://www.youtube.com/channel/UC0lbAQVpenvfA2QqzsRtL_g) |

Stdout contains only this tab-separated protocol (all text is one-line,
sanitized and bounded):

```text
ITEM<TAB>ID<TAB>TITLE<TAB>CHANNEL<TAB>DURATION_SECONDS<TAB>THUMBNAIL_URL<TAB>WATCH_URL<TAB>LOCAL_THUMBNAIL_PATH
DONE<TAB>COUNT<TAB>QUERY<TAB>cache=HIT|STALE|MISS
```

The formal wrapper stores the metadata cache at
`/mnt/data/rg40xxv/state/home/.cache/rg40xxv-youtube/feed`; the standalone tool
has a `/run` fallback. Freshness expires in 180 seconds and the owner-only cache
remains eligible for stale-while-refresh for six hours by default. A valid stale
snapshot is flushed immediately; one nonblocking-lock winner refreshes it while
concurrent helpers return without waiting. Refresh failure preserves the stale
bytes. Up to four thumbnail requests run in parallel;
only bounded `image/jpeg` responses from `*.ytimg.com` are atomically published
as private local files.  Signed media URLs are neither accepted nor emitted.
Metadata is capped at 32 source/query entries, thumbnails at 64 JPEGs, and
metadata refreshes use a fixed 32-slot lock pool. Thus repeated searches or
program restarts cannot grow the p7 cache without bound.

## Transport fix

The stock FFmpeg 4.4 HTTP client opens Google Video URLs with an open-ended GET.
The current CDN rejects that request with HTTP 403 even though a bounded Range
request succeeds. `tools/bounded_range_bridge.py` exposes a loopback-only,
seekable HTTP resource and converts each FFmpeg request into bounded upstream
ranges. It supports seeking, adapts its range size after a 403, and never logs a
signed CDN URL.

The safe default uses Android format 18: 640x360 H.264 plus AAC in one seekable
MP4. It fits the 640x480 panel without cropping. The tested 720p60 format 298 is
available but its current Android-VR URL is denied at high offsets without a
YouTube GVS PO token; it is therefore not the production default.
HOME shows the truthful `360P FORMAT 18` profile. Battery and AC currently keep
that same proven transport. A 480p battery profile or 720p AC control must not
be exposed until an integrated H.264/AAC URL or a Cedrus-backed split-stream
path passes the same full-range and device playback gates.

Runtime requirements are Python 3.10+ and optionally Node 22+ for YouTube's
JavaScript challenge solver. The official architecture-independent yt-dlp
2026.07.04 executable is pinned in `vendor/yt-dlp`; its digest is recorded in
`vendor/SHA256SUMS`. Resolver output is mode 0600 and must be deleted when
playback ends.

## Tests

Build and local AArch64 decode smoke test:

```sh
./build.sh
./qemu-smoke.sh
./tests/test-youtube-feed.sh
```

Real YouTube AArch64 H.264/AAC playback through the range bridge:

```sh
./qemu-youtube-real-smoke.sh 'https://youtu.be/GwtNiL9eEYk' 6
```

The real test must report `YOUTUBE_QEMU_PLAYBACK PASS`,
`YOUTUBE_REAL_SMOKE PASS`, and bridge `failures:0`. QEMU user mode does not
emulate Panfrost, Cedrus, DRM/KMS, or ALSA; those remain device-only gates.

See `evidence/REAL_URL_5S_20260826.md` for the exact verified output.
