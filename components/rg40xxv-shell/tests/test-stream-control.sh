#!/bin/sh
set -eu

project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
temporary=$(mktemp -d)
trap 'status=$?; trap - EXIT; rm -rf -- "$temporary"; exit "$status"' \
	EXIT HUP INT TERM

release="$temporary/release"
state="$temporary/state"
capture="$temporary/moonlight.args"
wrapper_capture="$temporary/wrapper.args"
exit_capture="$temporary/exit-chord.args"
mkdir -p "$release/moonlight/bin" "$release/bin"
cp "$project/tests/fake-moonlight-control.fixture" \
	"$release/moonlight/bin/moonlight"
cp "$project/tests/fake-moonlight-h700-control.fixture" \
	"$release/moonlight/bin/moonlight-h700"
chmod 0755 "$release/moonlight/bin/moonlight" \
	"$release/moonlight/bin/moonlight-h700"
cat >"$release/bin/rg40xxv-exit-chord" <<'EOF'
#!/bin/sh
set -eu
printf '<%s>\n' "$@" >"${FAKE_EXIT_CHORD_CAPTURE:?}"
test "${1:-}" = --grace-ms
test "${2:-}" = 1500
test "${3:-}" = --
shift 3
exec "$@"
EOF
chmod 0755 "$release/bin/rg40xxv-exit-chord"

RG_RELEASE_ROOT="$release" RG_STATE_ROOT="$state" \
	RG_STREAM_KEY_DIR="$state/moonlight/keys" \
	FAKE_MOONLIGHT_CAPTURE="$capture" \
	FAKE_MOONLIGHT_H700_CAPTURE="$wrapper_capture" \
	"$project/../payload/rg40xxv-stream" pair sunshine.local 4826 \
	>"$temporary/pair.out"

test "$(sed -n '1p' "$capture")" = '<pair>'
test "$(sed -n '2p' "$capture")" = '<-keydir>'
test "$(sed -n '3p' "$capture")" = "<$state/moonlight/keys>"
test "$(sed -n '4p' "$capture")" = '<-pin>'
test "$(sed -n '5p' "$capture")" = '<4826>'
test "$(sed -n '6p' "$capture")" = '<sunshine.local>'
grep -Fq 'PIN on the target PC: 4826' "$temporary/pair.out"
grep -Fq 'Succesfully paired' "$temporary/pair.out"
test "$(stat -c '%a' "$state/moonlight/keys")" = 700
test "$(stat -c '%a' "$state/moonlight/keys/client.pem")" = 600
grep -Fq 'fixture-certificate' "$state/moonlight/keys/client.pem"

# Pairing is a control-plane operation and must not depend on the stream image
# runner being present.
chmod 0644 "$release/moonlight/bin/moonlight-h700"
RG_RELEASE_ROOT="$release" RG_STATE_ROOT="$state" \
	RG_STREAM_KEY_DIR="$state/moonlight/keys" \
	FAKE_MOONLIGHT_CAPTURE="$capture" \
	FAKE_MOONLIGHT_H700_CAPTURE="$wrapper_capture" \
	"$project/../payload/rg40xxv-stream" pair sunshine.local 4826 \
	>"$temporary/pair-without-runner.out"
grep -Fq 'Succesfully paired' "$temporary/pair-without-runner.out"
chmod 0755 "$release/moonlight/bin/moonlight-h700"

if RG_RELEASE_ROOT="$release" RG_STATE_ROOT="$state" \
	RG_STREAM_KEY_DIR="$state/moonlight/keys" \
	FAKE_MOONLIGHT_CAPTURE="$capture" \
	FAKE_MOONLIGHT_H700_CAPTURE="$wrapper_capture" \
	"$project/../payload/rg40xxv-stream" pair sunshine.local 0000 \
	>"$temporary/invalid.out" 2>"$temporary/invalid.err"; then
	printf '%s\n' '0000 PIN was unexpectedly accepted' >&2
	exit 1
fi
grep -Fq 'field=pin' "$temporary/invalid.err"

RG_RELEASE_ROOT="$release" RG_STATE_ROOT="$state" \
	RG_STREAM_KEY_DIR="$state/moonlight/keys" \
	FAKE_MOONLIGHT_CAPTURE="$capture" \
	FAKE_MOONLIGHT_H700_CAPTURE="$wrapper_capture" \
	"$project/../payload/rg40xxv-stream" list sunshine.local \
	>"$temporary/list.out"
grep -Fq '1. Steam' "$temporary/list.out"
grep -Fq '2. pc' "$temporary/list.out"
test "$(sed -n '1p' "$capture")" = '<list>'
test "$(sed -n '4p' "$capture")" = '<sunshine.local>'

RG_RELEASE_ROOT="$release" RG_STATE_ROOT="$state" \
	RG_STREAM_KEY_DIR="$state/moonlight/keys" \
	FAKE_MOONLIGHT_CAPTURE="$capture" \
	FAKE_MOONLIGHT_H700_CAPTURE="$wrapper_capture" \
	FAKE_EXIT_CHORD_CAPTURE="$exit_capture" \
	"$project/../payload/rg40xxv-stream" stream sunshine.local \
	640 480 60 5000 h264 fit
grep -Fxq '<-app>' "$wrapper_capture"
test "$(tail -n 1 "$wrapper_capture")" = '<pc>'
test "$(sed -n '1p' "$exit_capture")" = '<--grace-ms>'
test "$(sed -n '2p' "$exit_capture")" = '<1500>'
test "$(sed -n '3p' "$exit_capture")" = '<-->'
test "$(sed -n '4p' "$exit_capture")" = \
	"<$release/moonlight/bin/moonlight-h700>"

RG_RELEASE_ROOT="$release" RG_STATE_ROOT="$state" \
	RG_STREAM_KEY_DIR="$state/moonlight/keys" \
	FAKE_MOONLIGHT_CAPTURE="$capture" \
	FAKE_MOONLIGHT_H700_CAPTURE="$wrapper_capture" \
	FAKE_EXIT_CHORD_CAPTURE="$exit_capture" \
	"$project/../payload/rg40xxv-stream" stream sunshine.local \
	640 480 60 5000 h264 fit Steam
test "$(tail -n 1 "$wrapper_capture")" = '<Steam>'

# The H700 client always decodes at the native 640x480 panel size.  Old host
# profiles and direct callers cannot silently request a larger stream.
for rejected_resolution in '1280 720' '640 360' '320 240'; do
	set -- $rejected_resolution
	if RG_RELEASE_ROOT="$release" RG_STATE_ROOT="$state" \
		RG_STREAM_KEY_DIR="$state/moonlight/keys" \
		FAKE_MOONLIGHT_CAPTURE="$capture" \
		FAKE_MOONLIGHT_H700_CAPTURE="$wrapper_capture" \
		FAKE_EXIT_CHORD_CAPTURE="$exit_capture" \
		"$project/../payload/rg40xxv-stream" stream sunshine.local \
		"$1" "$2" 60 5000 h264 fit Steam \
		>"$temporary/resolution.out" 2>"$temporary/resolution.err"; then
		printf 'non-panel stream resolution was accepted: %s x %s\n' \
			"$1" "$2" >&2
		exit 1
	fi
	grep -Fq 'field=resolution expected=640x480' \
		"$temporary/resolution.err"
done

# Control-plane commands stay outside the chord supervisor.  A MENU+START
# press may end a live Moonlight stream, but must never interfere with pairing
# or host/app discovery.
rm -f -- "$exit_capture"
RG_RELEASE_ROOT="$release" RG_STATE_ROOT="$state" \
	RG_STREAM_KEY_DIR="$state/moonlight/keys" \
	FAKE_MOONLIGHT_CAPTURE="$capture" \
	FAKE_MOONLIGHT_H700_CAPTURE="$wrapper_capture" \
	FAKE_EXIT_CHORD_CAPTURE="$exit_capture" \
	"$project/../payload/rg40xxv-stream" list sunshine.local \
	>"$temporary/list-unwrapped.out"
test ! -e "$exit_capture"

chmod 0644 "$release/bin/rg40xxv-exit-chord"
if RG_RELEASE_ROOT="$release" RG_STATE_ROOT="$state" \
	RG_STREAM_KEY_DIR="$state/moonlight/keys" \
	FAKE_MOONLIGHT_CAPTURE="$capture" \
	FAKE_MOONLIGHT_H700_CAPTURE="$wrapper_capture" \
	FAKE_EXIT_CHORD_CAPTURE="$exit_capture" \
	"$project/../payload/rg40xxv-stream" stream sunshine.local \
	640 480 60 5000 h264 fit Steam \
	>"$temporary/missing-chord.out" 2>"$temporary/missing-chord.err"; then
	printf '%s\n' 'non-executable exit-chord supervisor was accepted' >&2
	exit 1
fi
grep -Fq 'exit-chord=' "$temporary/missing-chord.err"

printf '%s\n' 'stream 640x480 + fixed-PIN/keydir/list + stream-only MENU+START child-group contract: PASS'
