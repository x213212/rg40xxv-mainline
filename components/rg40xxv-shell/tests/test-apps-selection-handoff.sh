#!/bin/sh
set -eu

project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
workspace=${RG40XXV_WORKSPACE:-$(CDPATH= cd -- "$project/../../../.." && pwd -P)}
temporary=$(mktemp -d)
trap 'status=$?; trap - EXIT; rm -rf -- "$temporary"; exit "$status"' \
	EXIT HUP INT TERM

launcher=$temporary/fake-launcher.sh
handoff=$temporary/launch-request.v1
portmaster=$temporary/PortMaster.sh
terminal=$temporary/Terminal.sh

printf '%s\n' '#!/bin/sh' 'exit 0' >"$launcher"
printf '%s\n' '#!/bin/sh' 'exit 0' >"$portmaster"
printf '%s\n' '#!/bin/sh' 'exit 0' >"$terminal"
chmod 0700 "$launcher"
chmod 0600 "$portmaster" "$terminal"

cc -std=c11 -O2 -Wall -Wextra -Werror \
	-I"$project/include" \
	-I"$workspace/services/netstream/include" \
	-I"$workspace/firmware/mnt/rootfs/usr/include/SDL2" \
	"$project/tests/apps_selection_handoff_test.c" \
	"$project/src/catalog.c" "$project/src/launch_ui.c" \
	"$project/src/launcher.c" \
	-o "$temporary/apps-selection-handoff-test"

"$temporary/apps-selection-handoff-test" \
	"$launcher" "$handoff" "$portmaster" "$terminal"
