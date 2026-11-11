#!/bin/bash
# valor.sh GAME : that game drives into Valor Form (its drive gauge set to 3 first, in battle mode for it). The command menu's
# cursor is where it was last left: each count of DOWN presses is tried until our model is Valor's (0055).
cd "$HOME/mnt/KH2 Decomp project"
log=logs/live-$1.log
model() { grep -a "our model is" $log | tail -1 | grep -oE "is [0-9A-F]{4}" | cut -c4-; }
for downs in 3 2 1 0; do
  [ "$(model)" = 0055 ] && break
  nav=(); for i in $(seq $downs); do nav+=("press DOWN" "wait 15"); done
  tools/live/ctl.sh $1 --timeout 60 "press TRIANGLE" "wait 15" "press TRIANGLE" "wait 15" "freeze 01C61958 01" "poke 01C6C901 03" "wait 30" \
    "${nav[@]}" "press CIRCLE" "wait 30" "press CIRCLE" "wait 300" "unfreeze 01C61958" >/dev/null
done
echo "$1 model $(model)"
