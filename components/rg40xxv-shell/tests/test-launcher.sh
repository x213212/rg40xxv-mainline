#!/bin/sh
set -eu

project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
temporary=$(mktemp -d)
trap 'rm -rf -- "$temporary"' EXIT HUP INT TERM

launcher="$temporary/fake-launcher.sh"
content="$temporary/game ; no-shell-injection.gba"
capture="$temporary/arguments"
log="$temporary/launcher.log"
binary="$temporary/launcher-test"

touch "$content"
cp "$project/tests/fake-launcher.fixture" "$launcher"
chmod 0755 "$launcher"

cc -std=c11 -O2 -Wall -Wextra -Werror \
	-I"$project/include" \
	"$project/tests/launcher_test.c" "$project/src/launcher.c" \
	-o "$binary"

LAUNCH_CAPTURE="$capture" "$binary" "$launcher" "$content" "$log" \
	"$temporary/grandchild.pid"

test "$(sed -n '1p' "$capture")" = '<--route>'
test "$(sed -n '2p' "$capture")" = '<aarch64:mgba>'
test "$(sed -n '3p' "$capture")" = '<-->'
test "$(sed -n '4p' "$capture")" = '<GBA>'
test "$(sed -n '5p' "$capture")" = "<$content>"
test ! -e "$temporary/no-shell-injection.gba"
test -s "$temporary/grandchild.pid"
if kill -0 "$(sed -n '1p' "$temporary/grandchild.pid")" 2>/dev/null; then
	printf '%s\n' 'grandchild process survived process-group cleanup' >&2
	exit 1
fi
printf '%s\n' 'launcher argv/process-group tests: PASS'
