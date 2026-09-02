#!/bin/sh
set -eu

project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)

# Build first so the legacy diagnostic frontend and controller probe exercised
# below come from the same serialized source snapshot as the texture frontend.
"$project/build.sh"
"$project/tests/test-home-catalog.sh"
"$project/tests/test-player-controls.sh"
"$project/tests/test-resolver-cache.sh"
"$project/tests/test-youtube-feed.sh"
python3 "$project/tests/test-channel-cache-generation.py"
"$project/tests/test-endpoint-broker.sh"
python3 "$project/tests/test-yt-dlp-server.py"
python3 "$project/tests/test-transport-regressions.py"
"$project/tests/test-resolver-ensure.sh"
"$project/tests/test-texture-wrapper.sh"
"$project/tests/test-native-mvp.sh"
"$project/tests/test-prefetch-lifecycle.sh"
"$project/tests/test-sdl-texture-scene.sh"

printf '%s\n' 'YOUTUBE_H700_HOST_TEST PASS frontend=SDL_TEXTURE transport=STRICT cache=REPLAY resolver_daemon=PERSISTENT_BOUNDED wrapper=FORMAL_PATH legacy=DIAGNOSTIC device=PENDING'
