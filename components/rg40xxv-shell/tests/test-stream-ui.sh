#!/bin/sh
set -eu

project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
workspace=$(CDPATH= cd -- "$project/../.." && pwd -P)
temporary=$(mktemp -d)
trap 'status=$?; trap - EXIT; rm -rf -- "$temporary"; exit "$status"' \
	EXIT HUP INT TERM

state_dir="$temporary/state"
rom_root="$temporary/roms"
screenshot="$temporary/stream.bmp"
stdout="$temporary/stdout"
stderr="$temporary/stderr"
input_stdout="$temporary/input-stdout"
input_stderr="$temporary/input-stderr"
launch_capture="$temporary/launch-capture"
fake_launcher="$temporary/fake-launcher.sh"
fake_stream_runner="$temporary/fake-stream-runner.sh"
stream_capture="$temporary/stream-arguments"
stream_events="$temporary/stream-events.log"
loader="$workspace/firmware/mnt/rootfs/usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1"
library_path="$workspace/firmware/mnt/rootfs/usr/lib/aarch64-linux-gnu:$workspace/firmware/mnt/rootfs/usr/lib"

mkdir -m 0700 "$state_dir"
mkdir -p "$rom_root"
cp "$project/tests/netstream-state.fixture/hosts.v1" "$state_dir/hosts.v1"
cp "$project/tests/netstream-state.fixture/wifi.v1" "$state_dir/wifi.v1"
chmod 0600 "$state_dir/hosts.v1" "$state_dir/wifi.v1"
cp "$project/tests/fake-launcher.fixture" "$fake_launcher"
chmod 0755 "$fake_launcher"
cp "$project/tests/fake-stream-runner.fixture" "$fake_stream_runner"
chmod 0755 "$fake_stream_runner"

SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
	qemu-aarch64-static "$loader" --library-path "$library_path" \
	"$project/build/rg40xxv-shell" \
	--windowed --font "$project/assets/RG40XXV-UI-Sans.otf" \
	--rom-root "$rom_root" --state-dir "$state_dir" \
	--stream-preview --screenshot "$screenshot" \
	>"$stdout" 2>"$stderr"

grep -Fq 'UI_RESULT PASS' "$stdout"
grep -Fq 'stream_hosts=2 stream_selected=2 stream_store=ready' "$stdout"
grep -Fq 'moonlight=not-deployed' "$stdout"
grep -Fq 'STREAM_RESULT PASS hosts=2 selected=2 paired=yes width=640 height=480 fps=60 bitrate=12000 codec=H265 aspect=fill runner=not-deployed' "$stdout"
test -s "$screenshot"
test "$(stat -c '%a' "$state_dir")" = 700
test "$(stat -c '%a' "$state_dir/hosts.v1")" = 600
test "$(stat -c '%a' "$state_dir/wifi.v1")" = 600
if grep -Fq 'fixture-super-secret' "$stdout" "$stderr"; then
	printf '%s\n' 'Wi-Fi secret leaked into UI output' >&2
	exit 1
fi

SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
	LAUNCH_CAPTURE="$launch_capture" \
	qemu-aarch64-static "$loader" --library-path "$library_path" \
	"$project/build/rg40xxv-shell" \
	--windowed --font "$project/assets/RG40XXV-UI-Sans.otf" \
	--rom-root "$rom_root" --state-dir "$state_dir" \
	--launcher "$fake_launcher" --stream-preview --benchmark \
	--launch-once --demo-ms 420 \
	>"$input_stdout" 2>"$input_stderr"

grep -Fq 'stream_hosts=2 stream_selected=1 stream_store=ready' \
	"$input_stdout"
grep -Fq 'STREAM_RESULT PASS hosts=2 selected=1 paired=yes width=640 height=480 fps=60 bitrate=8000 codec=H264 aspect=fit runner=not-deployed' \
	"$input_stdout"
test ! -e "$launch_capture"
if grep -Fq 'fixture-super-secret' "$input_stdout" "$input_stderr"; then
	printf '%s\n' 'Wi-Fi secret leaked during streaming input test' >&2
	exit 1
fi

launch_state="$temporary/launch-state"
mkdir -m 0700 "$launch_state"
cp "$project/tests/netstream-state-launch.fixture/hosts.v1" \
	"$launch_state/hosts.v1"
chmod 0600 "$launch_state/hosts.v1"
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
	FAKE_STREAM_CAPTURE="$stream_capture" FAKE_STREAM_EVENT_MARKER=1 \
	qemu-aarch64-static "$loader" --library-path "$library_path" \
	"$project/build/rg40xxv-shell" \
	--windowed --font "$project/assets/RG40XXV-UI-Sans.otf" \
	--rom-root "$rom_root" --state-dir "$launch_state" \
	--stream-launcher "$fake_stream_runner" \
	--launch-log "$stream_events" \
	--stream-preview --launch-once --demo-ms 900 \
	>"$temporary/stream-launch-stdout" 2>>"$stream_events"

test "$(sed -n '1p' "$stream_capture")" = '<stream>'
test "$(sed -n '2p' "$stream_capture")" = '<sunshine-living.local>'
test "$(sed -n '3p' "$stream_capture")" = '<640>'
test "$(sed -n '4p' "$stream_capture")" = '<480>'
test "$(sed -n '5p' "$stream_capture")" = '<60>'
test "$(sed -n '6p' "$stream_capture")" = '<8000>'
test "$(sed -n '7p' "$stream_capture")" = '<h264>'
test "$(sed -n '8p' "$stream_capture")" = '<stretch>'
test -z "$(sed -n '9p' "$stream_capture")"
grep -Fq 'runner=deployed' "$temporary/stream-launch-stdout"
grep -Fq 'UI_RESULT PASS' "$temporary/stream-launch-stdout"

starting_line=$(sed -n \
	'/UI_LAUNCH_TRANSITION PRESENTED phase=starting kind=stream/=' \
	"$stream_events" | sed -n '1p')
runner_line=$(sed -n '/FAKE_STREAM_RUNNER STARTED/=' "$stream_events" |
	sed -n '1p')
returned_line=$(sed -n \
	'/UI_LAUNCH_TRANSITION PRESENTED phase=returned kind=stream/=' \
	"$stream_events" | sed -n '1p')
test -n "$starting_line"
test -n "$runner_line"
test -n "$returned_line"
test "$starting_line" -lt "$runner_line"
test "$runner_line" -lt "$returned_line"

unpaired_state="$temporary/unpaired-state"
mkdir -m 0700 "$unpaired_state"
cp "$project/tests/netstream-state-unpaired.fixture/hosts.v1" \
	"$unpaired_state/hosts.v1"
chmod 0600 "$unpaired_state/hosts.v1"
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
	FAKE_STREAM_CAPTURE="$temporary/unpaired-capture" \
	qemu-aarch64-static "$loader" --library-path "$library_path" \
	"$project/build/rg40xxv-shell" \
	--windowed --font "$project/assets/RG40XXV-UI-Sans.otf" \
	--rom-root "$rom_root" --state-dir "$unpaired_state" \
	--stream-launcher "$fake_stream_runner" \
	--launch-log "$temporary/unpaired-child.log" \
	--stream-preview --launch-once --demo-ms 460 \
	>"$temporary/unpaired-stdout" 2>"$temporary/unpaired-stderr"
grep -Fq 'paired=no' "$temporary/unpaired-stdout"
grep -Fq 'runner=deployed' "$temporary/unpaired-stdout"
grep -Fq 'UI_STREAM_LAUNCH REJECTED reason=unpaired' \
	"$temporary/unpaired-stderr"
test ! -e "$temporary/unpaired-capture"
if grep -Fq 'UI_LAUNCH_TRANSITION PRESENTED phase=starting' \
	"$temporary/unpaired-stderr"; then
	printf '%s\n' 'unpaired stream released the UI session' >&2
	exit 1
fi

non_executable_runner="$temporary/non-executable-stream-runner"
cp "$project/tests/fake-stream-runner.fixture" "$non_executable_runner"
chmod 0644 "$non_executable_runner"
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
	FAKE_STREAM_CAPTURE="$temporary/nonexec-capture" \
	qemu-aarch64-static "$loader" --library-path "$library_path" \
	"$project/build/rg40xxv-shell" \
	--windowed --font "$project/assets/RG40XXV-UI-Sans.otf" \
	--rom-root "$rom_root" --state-dir "$launch_state" \
	--stream-launcher "$non_executable_runner" \
	--launch-log "$temporary/nonexec-child.log" \
	--stream-preview --launch-once --demo-ms 460 \
	>"$temporary/nonexec-stdout" 2>"$temporary/nonexec-stderr"
grep -Fq 'runner=not-deployed' "$temporary/nonexec-stdout"
grep -Fq 'UI_STREAM_LAUNCH REJECTED reason=runner-unavailable' \
	"$temporary/nonexec-stderr"
test ! -e "$temporary/nonexec-capture"
if grep -Fq 'UI_LAUNCH_TRANSITION PRESENTED phase=starting' \
	"$temporary/nonexec-stderr"; then
	printf '%s\n' 'non-executable stream runner released the UI session' >&2
	exit 1
fi

SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
	FAKE_STREAM_CAPTURE="$temporary/codec-capture" \
	qemu-aarch64-static "$loader" --library-path "$library_path" \
	"$project/build/rg40xxv-shell" \
	--windowed --font "$project/assets/RG40XXV-UI-Sans.otf" \
	--rom-root "$rom_root" --state-dir "$state_dir" \
	--stream-launcher "$fake_stream_runner" \
	--launch-log "$temporary/codec-child.log" \
	--stream-preview --launch-once --demo-ms 460 \
	>"$temporary/codec-stdout" 2>"$temporary/codec-stderr"
grep -Fq 'codec=H265' "$temporary/codec-stdout"
grep -Fq 'runner=deployed' "$temporary/codec-stdout"
grep -Fq 'UI_STREAM_LAUNCH REJECTED reason=unsupported-codec' \
	"$temporary/codec-stderr"
test ! -e "$temporary/codec-capture"
if grep -Fq 'UI_LAUNCH_TRANSITION PRESENTED phase=starting' \
	"$temporary/codec-stderr"; then
	printf '%s\n' 'unsupported stream codec released the UI session' >&2
	exit 1
fi

invalid_state="$temporary/invalid-state"
mkdir -m 0700 "$invalid_state"
cp "$project/tests/netstream-state-invalid.fixture/hosts.v1" \
	"$invalid_state/hosts.v1"
chmod 0600 "$invalid_state/hosts.v1"
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
	FAKE_STREAM_CAPTURE="$temporary/invalid-capture" \
	qemu-aarch64-static "$loader" --library-path "$library_path" \
	"$project/build/rg40xxv-shell" \
	--windowed --font "$project/assets/RG40XXV-UI-Sans.otf" \
	--rom-root "$rom_root" --state-dir "$invalid_state" \
	--stream-launcher "$fake_stream_runner" \
	--launch-log "$temporary/invalid-child.log" \
	--stream-preview --launch-once --demo-ms 460 \
	>"$temporary/invalid-stdout" 2>"$temporary/invalid-stderr"
grep -Fq 'stream_store=unavailable' "$temporary/invalid-stdout"
grep -Fq 'UI_STREAM_LAUNCH REJECTED reason=invalid-host-store' \
	"$temporary/invalid-stderr"
test ! -e "$temporary/invalid-capture"
if grep -Fq 'UI_LAUNCH_TRANSITION PRESENTED phase=starting' \
	"$temporary/invalid-stderr"; then
	printf '%s\n' 'invalid stream host released the UI session' >&2
	exit 1
fi

grep -aFq '請先完成這台 Moonlight 主機的配對' \
	"$project/build/rg40xxv-shell"
grep -aFq '串流主機設定無效' "$project/build/rg40xxv-shell"
grep -aFq 'Moonlight 正式執行器尚未部署' \
	"$project/build/rg40xxv-shell"
grep -aFq '/opt/rg40xxv/bin/rg40xxv-stream' \
	"$project/build/rg40xxv-shell"

printf '%s\n' 'UI netstream host-store integration: PASS'
