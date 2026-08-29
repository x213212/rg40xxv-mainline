#!/bin/sh
set -eu

NETSTREAM_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
NETSTREAM_BUILD_DIR=${NETSTREAM_BUILD_DIR:-"$NETSTREAM_ROOT/build"}
NETSTREAM_CC=${NETSTREAM_CC:-cc}
NETSTREAM_AR=${NETSTREAM_AR:-ar}
NETSTREAM_ACTION=${1:-build}

NETSTREAM_WARNINGS="-Wall -Wextra -Wpedantic -Werror"
NETSTREAM_BASE_FLAGS="-std=c11 -O2 -g -D_FORTIFY_SOURCE=2 $NETSTREAM_WARNINGS"
NETSTREAM_LINK_FLAGS="-Wl,-z,relro,-z,now"

if [ "${NETSTREAM_SANITIZE:-0}" = 1 ]; then
    NETSTREAM_BASE_FLAGS="$NETSTREAM_BASE_FLAGS -fno-omit-frame-pointer -fsanitize=address,undefined"
    NETSTREAM_LINK_FLAGS="$NETSTREAM_LINK_FLAGS -fsanitize=address,undefined"
fi

case "$NETSTREAM_ACTION" in
    build|test)
        ;;
    *)
        printf 'usage: %s [build|test]\n' "$0" >&2
        exit 2
        ;;
esac

mkdir -p "$NETSTREAM_BUILD_DIR"

# Intentional word splitting permits compiler flags above to remain portable.
# shellcheck disable=SC2086
"$NETSTREAM_CC" $NETSTREAM_BASE_FLAGS -I"$NETSTREAM_ROOT/include" \
    -c "$NETSTREAM_ROOT/src/netstream.c" \
    -o "$NETSTREAM_BUILD_DIR/netstream.o"
"$NETSTREAM_AR" rcs "$NETSTREAM_BUILD_DIR/libnetstream.a" \
    "$NETSTREAM_BUILD_DIR/netstream.o"

# shellcheck disable=SC2086
"$NETSTREAM_CC" $NETSTREAM_BASE_FLAGS -I"$NETSTREAM_ROOT/include" \
    -c "$NETSTREAM_ROOT/src/netstreamctl.c" \
    -o "$NETSTREAM_BUILD_DIR/netstreamctl.o"
# shellcheck disable=SC2086
"$NETSTREAM_CC" $NETSTREAM_LINK_FLAGS \
    "$NETSTREAM_BUILD_DIR/netstreamctl.o" \
    "$NETSTREAM_BUILD_DIR/libnetstream.a" \
    -o "$NETSTREAM_BUILD_DIR/netstreamctl"

if [ "$NETSTREAM_ACTION" = test ]; then
    # shellcheck disable=SC2086
    "$NETSTREAM_CC" $NETSTREAM_BASE_FLAGS -I"$NETSTREAM_ROOT/include" \
        "$NETSTREAM_ROOT/tests/test_netstream.c" \
        "$NETSTREAM_BUILD_DIR/libnetstream.a" $NETSTREAM_LINK_FLAGS \
        -o "$NETSTREAM_BUILD_DIR/test_netstream"
    "$NETSTREAM_BUILD_DIR/test_netstream"
    "$NETSTREAM_ROOT/tests/test_cli.sh" \
        "$NETSTREAM_BUILD_DIR/netstreamctl"
fi

printf 'built: %s\n' "$NETSTREAM_BUILD_DIR/netstreamctl"
