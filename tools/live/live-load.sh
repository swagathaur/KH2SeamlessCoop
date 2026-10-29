#!/bin/bash
# live-load.sh NAME: from boot, load memory card slot 1 (waits for the title screen first).
d="$(dirname "$0")"
"$d/ctl.sh" "$1" --timeout 150 "wait 1700" "press DOWN" "wait 40" "press CIRCLE" "wait 160" "press CIRCLE" "wait 150" "press CIRCLE" "wait 150" "press CIRCLE" "wait 400" "state"
