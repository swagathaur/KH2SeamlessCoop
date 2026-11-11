#!/bin/bash
# doortrio.sh up | warm | round N [waitframes] : doortime.sh for three games
cd "$HOME/mnt/KH2 Decomp project"
case $1 in
up)
  n=$(ls .agent/jobs | grep -oE '^[0-9]+' | sort -n | tail -1); n=$((n+1))
  printf 'Get-Process kh2recomp -ErrorAction SilentlyContinue | Stop-Process -Force\n' > .agent/jobs/$n-kill.ps1
  for i in $(seq 30); do [ -f .agent/jobs/$n-kill.exit ] && break; sleep 1; done
  n=$((n+1))
  printf 'Set-Location "$PSScriptRoot\\..\\.."\n$env:KH2_AVATARS = "1"\n$env:KH2_FAST_ROOMS = "%s"\npowershell -NoProfile -ExecutionPolicy Bypass -File tools\\live\\live.ps1 trio\n' "${2:-1}" > .agent/jobs/$n-live.ps1
  for i in $(seq 90); do [ -f .agent/jobs/$n-live.exit ] && break; sleep 1; done
  load=("wait 1700" "press DOWN" "wait 40" "press CIRCLE" "wait 160" "press CIRCLE" "wait 150" "press UP" "wait 30" "press CIRCLE" "wait 150" "press CIRCLE" "wait 400")
  for g in host client p3; do (tools/live/ctl.sh $g --timeout 160 "${load[@]}" >/dev/null) & done; wait
  for g in host client p3; do tools/live/ctl.sh $g --timeout 20 "wait 100" "state" | tail -1 | cut -c1-90; done
  ;;
warm)
  for g in host client p3; do tools/live/ctl.sh $g --timeout 20 "freeze 01C6C750 1E000000" >/dev/null; done
  tools/live/ctl.sh host --timeout 60 "warp 8 1" "wait 1500" >/dev/null; echo warm
  ;;
round)
  r=$(( $2 % 2 == 1 ? 2 : 1 ))
  ho=$(wc -l < logs/live-host.log); co=$(wc -l < logs/live-client.log); po=$(wc -l < logs/live-p3.log)
  tools/live/ctl.sh host --timeout 60 "warp 8 $r" "wait ${3:-1000}" >/dev/null
  echo "round $2 to 08-0$r: $(python3 tools/live/doortrio.py $ho $co $po)"
  grep -a "null-call\|job dropped" logs/live-host.log logs/live-client.log logs/live-p3.log | tail -2
  ;;
esac
