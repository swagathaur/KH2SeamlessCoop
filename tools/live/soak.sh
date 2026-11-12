#!/bin/bash
# soak.sh SECONDS [GAMES] : random co-op play until SECONDS pass (fits a tool call: ~160): the host goes to a
# random room (SOAK_KINDS=9: and kills and restarts of the other games) (LoD, HB, TT, BC, OC, AG, PR, HT, Atl, PL, TWTNW, SP), the others drive, revert, take hits,
# attack, open and close the pause menu at random; health after each step. Stops at the first problem.
cd "$HOME/mnt/KH2 Decomp project"
end=$((SECONDS + ${1:-150})); games=(${2:-host client})
rooms=("8 1" "8 2" "4 1" "4 2" "2 1" "5 1" "6 1" "7 1" "16 1" "14 1" "14 2" "11 2" "10 1" "10 2" "18 1" "17 1")
log=scratch/soak.log; step=$(cat scratch/soak.step 2>/dev/null || echo 0)
while [ $SECONDS -lt $end ]; do
  step=$((step+1)); echo $step > scratch/soak.step
  case $((RANDOM % ${SOAK_KINDS:-8})) in
    0|1) r=${rooms[$((RANDOM % ${#rooms[@]}))]}; act="host to $r"
         tools/live/ctl.sh host --timeout 40 "warp $r" "wait $((${SOAK_MINWAIT:-200} + RANDOM % 500))" >/dev/null;;
    2) g=${games[$((RANDOM % ${#games[@]}))]}; f=$((RANDOM % 7)); act="$g drive $f"
       tools/live/ctl.sh $g --timeout 20 "drive $f" "wait $((60 + RANDOM % 200))" >/dev/null;;
    3) g=${games[$((RANDOM % ${#games[@]}))]}; act="$g attacks"
       tools/live/ctl.sh $g --timeout 20 "mash CIRCLE 20" "input - 60 $((RANDOM%256)) $((RANDOM%256))" >/dev/null;;
    4) g=${games[$((RANDOM % ${#games[@]}))]}; act="$g pause"
       tools/live/ctl.sh $g --timeout 20 "press START" "wait $((20 + RANDOM % 80))" "press START" "wait 30" >/dev/null;;
    5) g=${games[$((RANDOM % ${#games[@]}))]}; act="$g hurt"
       tools/live/ctl.sh $g --timeout 20 "unfreeze 01C6C750" "hurt $((RANDOM % 20))" "wait 60" "freeze 01C6C750 1E000000" >/dev/null;;
    6|7) r=${rooms[$((RANDOM % ${#rooms[@]}))]}; g=${games[$((RANDOM % ${#games[@]}))]}; f=$((RANDOM % 7)); d=$((RANDOM % 90))
       act="host to $r while $g drives $f after $d frames"
       (tools/live/ctl.sh $g --timeout 20 "wait $d" "drive $f" >/dev/null &)
       tools/live/ctl.sh host --timeout 40 "warp $r" "wait $((250 + RANDOM % 400))" >/dev/null;;
    8) g=${games[$(( 1 + RANDOM % (${#games[@]} - 1) ))]}; act="$g killed and started again"
       bash tools/live/churn.sh kill $g >/dev/null; sleep $((RANDOM % 5))
       bash tools/live/churn.sh start $g >/dev/null
       tools/live/ctl.sh $g --timeout 20 "unlock" "freeze 01C6C750 1E000000" >/dev/null;;
  esac
  h=$(bash tools/live/health.sh ${games[@]})
  echo "$step $act" >> $log; echo "$h" >> $log
  if echo "$h" | grep -q "NO ANSWER\|nc [1-9]\|dead [1-9]\|dropped [1-9]\|hang [1-9]"; then echo "PROBLEM at step $step ($act)"; echo "$h"; exit 1; fi
  # (someone not in the host's room for 4 steps running: stuck)
  rooms_now=$(echo "$h" | grep -oE ' [0-9a-f]{2}-[0-9a-f]{2} ' | sort -u | wc -l)
  [ "$rooms_now" -gt 1 ] && apart=$((apart+1)) || apart=0
  if [ "${apart:-0}" -ge 4 ]; then echo "PROBLEM: apart from the host for 4 steps (step $step)"; echo "$h"; exit 1; fi
done
echo "ok through step $step"; echo "$h"
