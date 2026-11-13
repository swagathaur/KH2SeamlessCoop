#!/bin/bash
# ms302.sh : a duo on the Twilight Town save, both in the mission TT32_MS302 (02-20, event 154: three Dusks,
# reward Aerial Recovery), HP held
cd "$HOME/mnt/KH2 Decomp project"
LIVE_SLOT=0 bash tools/live/doortime.sh up "$@" 2>&1 | tail -1 | cut -c1-60
for g in host client; do tools/live/ctl.sh $g --timeout 10 "freeze 01C6C750 1E000000" >/dev/null; done
tools/live/ctl.sh host --timeout 60 "warp 2 32 154 154" "wait 700" >/dev/null
for g in host client; do echo "$g: $(grep -a 'mission\] started' logs/live-$g.log | tail -1)"; done
