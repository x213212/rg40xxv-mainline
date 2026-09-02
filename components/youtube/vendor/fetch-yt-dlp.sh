#!/bin/sh
# Download the pinned yt-dlp release and verify it against SHA256SUMS.
#
# The binary itself is not committed: it is a third-party runtime, and this
# repository keeps those in their own projects (see NOTICE.md). build.sh needs
# it present, so fetch it once per clone.
set -eu

version=$(sed -n 's/^- Version: `\(.*\)`$/\1/p' "$(dirname "$0")/README.md")
[ -n "$version" ] || { echo "cannot read the pinned version from README.md" >&2; exit 1; }

cd "$(dirname "$0")"
url="https://github.com/yt-dlp/yt-dlp/releases/download/$version/yt-dlp"
echo "fetching yt-dlp $version"
curl -fL --proto '=https' --tlsv1.2 -o yt-dlp.part "$url"
mv yt-dlp.part yt-dlp
sha256sum -c SHA256SUMS
chmod 0755 yt-dlp
echo "yt-dlp $version verified"
