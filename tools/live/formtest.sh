#!/bin/bash
# formtest.sh STEP... : drive forms and room changes between two games (a duo from doortime.sh up),
# checking both see each other after each room change and counting null calls.
#   h=N / c=N   host / client drives into form N (1 Valor, 2 Wisdom, 4 Master, 5 Final; 0 reverts)
#   r           a room change (08-01 <-> 08-02), then who sees whom
cd "$HOME/mnt/KH2 Decomp project"
model() { grep -a "our model is" logs/live-$1.log | tail -1 | grep -oE "is [0-9A-F]{4}" | cut -c4-; }
nc() { grep -ac "null-call" logs/live-$1.log; }
r=$(cat scratch/formtest.round 2>/dev/null || echo 0)
for step in "$@"; do
  case $step in
    h=*|c=*) g=$([ ${step%%=*} = h ] && echo host || echo client)
             tools/live/ctl.sh $g --timeout 40 "drive ${step#*=}" "wait 240" >/dev/null
             echo "$step: host $(model host) client $(model client) | null calls $(nc host) $(nc client)";;
    r) r=$((r+1)); echo $r > scratch/formtest.round
       line=$(bash tools/live/doortime.sh round $r 420 | head -1)
       echo "r$r: $(echo "$line" | grep -o 'to [0-9-]*\|host_sees_client=[0-9.-]*\|client_sees_host=[0-9.-]*' | tr '\n' ' ')| host $(model host) client $(model client) | null calls $(nc host) $(nc client)";;
  esac
done
