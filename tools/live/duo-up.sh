#!/bin/bash
# duo-up.sh: start a live host and client (a runner job), load both, wait until joined.
cd "$HOME/mnt/KH2 Decomp project" || exit 1
n=$(ls .agent/jobs | grep -oE '^[0-9]+' | sort -n | tail -1); n=$((n+1))
printf 'Set-Location "$PSScriptRoot\\..\\.."\n$env:KH2_AVATARS = "1"\npowershell -NoProfile -ExecutionPolicy Bypass -File tools\\live\\live.ps1 duo\n' > .agent/jobs/$n-live.ps1
for i in $(seq 90); do [ -f .agent/jobs/$n-live.exit ] && break; sleep 1; done
cat .agent/jobs/$n-live.out.log
(tools/live/live-load.sh host > /tmp/lh.txt) & (tools/live/live-load.sh client > /tmp/lc.txt); wait
tools/live/ctl.sh client "wait 300" "state" | tail -1
