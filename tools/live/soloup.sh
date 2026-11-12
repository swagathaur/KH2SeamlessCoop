#!/bin/bash
# soloup.sh : a fresh solo game (avatars env as doortime.sh), Land of Dragons save loaded
cd "$HOME/mnt/KH2 Decomp project"
n=$(ls .agent/jobs | grep -oE '^[0-9]+' | sort -n | tail -1); n=$((n+1))
printf 'Get-Process kh2recomp -ErrorAction SilentlyContinue | Stop-Process -Force\n' > .agent/jobs/$n-kill.ps1
for i in $(seq 30); do [ -f .agent/jobs/$n-kill.exit ] && break; sleep 1; done
n=$((n+1))
extra=""; for kv in $LIVE_ENV; do extra+="\$env:${kv%%=*} = \"${kv#*=}\"\n"; done
[ -n "$LIVE_EXTRA" ] && extra+="\$env:LIVE_EXTRA = \"$LIVE_EXTRA\"\n" # (more game arguments)
printf 'Set-Location "$PSScriptRoot\\..\\.."\n$env:KH2_AVATARS = "1"\n%bpowershell -NoProfile -ExecutionPolicy Bypass -File tools\\live\\live.ps1 solo\n' "$extra" > .agent/jobs/$n-live.ps1
for i in $(seq 90); do [ -f .agent/jobs/$n-live.exit ] && break; sleep 1; done
tools/live/ctl.sh solo --timeout 150 "wait 1700" "press DOWN" "wait 40" "press CIRCLE" "wait 160" "press CIRCLE" "wait 150" "press UP" "wait 30" "press CIRCLE" "wait 150" "press CIRCLE" "wait 400" >/dev/null
tools/live/ctl.sh solo --timeout 20 "wait 100" "state" | tail -1 | cut -c1-90
