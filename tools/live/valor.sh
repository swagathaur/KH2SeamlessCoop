#!/bin/bash
# valor.sh GAME [again] : that game drives into Valor (gauge set to 3 first). The command menu's cursor on
# Attack (a fresh game); "again": on Drive (after a revert, the cursor stays there)
cd "$HOME/mnt/KH2 Decomp project"
if [ "$2" = again ]; then nav=(); else nav=("press DOWN" "wait 15" "press DOWN" "wait 15" "press DOWN" "wait 15"); fi
tools/live/ctl.sh $1 --timeout 60 "poke 01C6C901 03" "wait 10" "${nav[@]}" "press CIRCLE" "wait 30" "press CIRCLE" "wait 300" "state" | tail -1 | cut -c1-100
