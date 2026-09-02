#!/bin/sh
set -eu

[ "$#" -eq 4 ]
[ "$1" = --timeout ]
[ "$2" = 90 ]
[ "$3" = pair ]
[ "$4" = AA:BB:CC:DD:EE:FF ]
printf '%s\t%s\t%s\t%s\n' "$1" "$2" "$3" "$4" >"$0.invocation"
: >"$0.marker"
