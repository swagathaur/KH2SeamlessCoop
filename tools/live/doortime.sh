#!/bin/bash
# doortime.sh up | warm | round N : timing of room changes with avatars (each call fits a 3-minute tool call).
#   up [F] [B]  a fresh duo (avatars; KH2_FAST_ROOMS=F, default 1; KH2_AVATAR_BASE_IN_FORM=B, default 1),
#               both on the Land of Dragons save (file 01 of the client's card)
#   warm        HP held for both (the room's Heartless), the host to 08-01
#   round N  the host goes to 08-02 (N odd) or 08-01 (N even); prints seconds from the host's map jump to:
#            the client's jump, each game in control, each seeing the other's avatar; and ALL_VISIBLE
cd "$HOME/mnt/KH2 Decomp project"
case $1 in
up)
  n=$(ls .agent/jobs | grep -oE '^[0-9]+' | sort -n | tail -1); n=$((n+1))
  printf 'Get-Process kh2recomp -ErrorAction SilentlyContinue | Stop-Process -Force\n' > .agent/jobs/$n-kill.ps1
  for i in $(seq 30); do [ -f .agent/jobs/$n-kill.exit ] && break; sleep 1; done
  # (the Land of Dragons save, file 01: a world with its party)
  n=$((n+1))
  extra=""; for kv in $LIVE_ENV; do extra+="\$env:${kv%%=*} = \"${kv#*=}\"\n"; done # (LIVE_ENV="NAME=VALUE ...")
  printf 'Set-Location "$PSScriptRoot\\..\\.."\n$env:KH2_AVATARS = "1"\n$env:KH2_FAST_ROOMS = "%s"\n$env:KH2_AVATAR_BASE_IN_FORM = "%s"\n%bpowershell -NoProfile -ExecutionPolicy Bypass -File tools\\live\\live.ps1 duo\n' "${2:-1}" "${3:-1}" "$extra" > .agent/jobs/$n-live.ps1
  for i in $(seq 90); do [ -f .agent/jobs/$n-live.exit ] && break; sleep 1; done
  # (LIVE_SLOT=0: the save in slot 00, the Twilight Town (Roxas) one; default the Land of Dragons one)
  if [ "${LIVE_SLOT:-1}" = 0 ]; then slot=("wait 30"); else slot=("press UP" "wait 30"); fi
  load=("wait 1700" "press DOWN" "wait 40" "press CIRCLE" "wait 160" "press CIRCLE" "wait 150" "${slot[@]}" "press CIRCLE" "wait 150" "press CIRCLE" "wait 400")
  (tools/live/ctl.sh host --timeout 150 "${load[@]}" >/dev/null) & (tools/live/ctl.sh client --timeout 150 "${load[@]}" >/dev/null); wait
  tools/live/ctl.sh client --timeout 20 "wait 300" "state" | tail -1 | cut -c1-140
  ;;
warm)
  # (HP held, so the room's Heartless do not end it)
  for g in host client; do tools/live/ctl.sh $g --timeout 20 "freeze 01C6C750 1E000000" >/dev/null; done
  tools/live/ctl.sh host --timeout 60 "warp 8 1" "wait 1500" >/dev/null; echo warm
  ;;
round)
  # (DOOR_ROOMS="WORLD ODD EVEN", decimal: the rooms the host goes between; default 8 2 1, Land of Dragons)
  read dw dodd deven <<< "${DOOR_ROOMS:-8 2 1}"
  r=$(( $2 % 2 == 1 ? dodd : deven ))
  ho=$(wc -l < logs/live-host.log); co=$(wc -l < logs/live-client.log)
  tools/live/ctl.sh host --timeout 60 "warp $dw $r" "wait ${3:-1200}" >/dev/null
  echo "round $2 to $dw-$r: $(python3 tools/live/doortime.py $ho $co | tr '\n' ' ')"
  grep -a "null-call\|job dropped" logs/live-host.log logs/live-client.log | tail -2
  ;;
esac
