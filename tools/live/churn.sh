#!/bin/bash
# churn.sh kill GAME | start GAME : kill one game of a running session / start it again and load the
# Land of Dragons save (it rejoins the host)
cd "$HOME/mnt/KH2 Decomp project"
n=$(( $(ls .agent/jobs | grep -oE '^[0-9]+' | sort -n | tail -1) + 1 ))
case $1 in
kill)
  flag=$([ $2 = host ] && echo "--host=" || ([ $2 = p3 ] && echo "--name=Third" || echo "--name=Client"))
  printf 'Get-CimInstance Win32_Process -Filter "Name=%s" | Where-Object { $_.CommandLine -like "*%s*" } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force; "killed $($_.ProcessId)" }\n' "'kh2recomp.exe'" "$flag" > .agent/jobs/$n-kill$2.ps1
  for i in $(seq 30); do [ -f .agent/jobs/$n-kill$2.exit ] && break; sleep 1; done; cat .agent/jobs/$n-kill$2.out.log;;
start)
  if [ "${LIVE_SLOT:-1}" = 0 ]; then slot=("wait 30"); else slot=("press UP" "wait 30"); fi
  printf 'Set-Location "$PSScriptRoot\\..\\.."\n$env:KH2_AVATARS = "1"\npowershell -NoProfile -ExecutionPolicy Bypass -File tools\\live\\live.ps1 %s\n' $2 > .agent/jobs/$n-live$2.ps1
  for i in $(seq 60); do [ -f .agent/jobs/$n-live$2.exit ] && break; sleep 1; done
  tools/live/ctl.sh $2 --timeout 150 "wait 1700" "press DOWN" "wait 40" "press CIRCLE" "wait 160" "press CIRCLE" "wait 150" "${slot[@]}" "press CIRCLE" "wait 150" "press CIRCLE" "wait 600" >/dev/null
  tools/live/ctl.sh $2 --timeout 20 "state" | tail -1 | cut -c1-90;;
esac
