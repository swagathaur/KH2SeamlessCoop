#!/bin/bash
# revert.sh GAME [again] : out of the form (Revert, the 4th command; "again": the cursor already there)
cd "$HOME/mnt/KH2 Decomp project"
if [ "$2" = again ]; then nav=(); else nav=("press DOWN" "wait 15" "press DOWN" "wait 15" "press DOWN" "wait 15"); fi
tools/live/ctl.sh $1 --timeout 60 "${nav[@]}" "press CIRCLE" "wait 300" "state" | tail -1 | cut -c1-100
