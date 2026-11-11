#!/bin/bash
# revert.sh GAME : that game reverts from its form (Revert, the 4th command; each count of DOWN presses tried
# until our model is Sora's own, 0054)
cd "$HOME/mnt/KH2 Decomp project"
log=logs/live-$1.log
model() { grep -a "our model is" $log | tail -1 | grep -oE "is [0-9A-F]{4}" | cut -c4-; }
for downs in 3 2 1 0; do
  [ "$(model)" = 0054 ] && break
  nav=(); for i in $(seq $downs); do nav+=("press DOWN" "wait 15"); done
  tools/live/ctl.sh $1 --timeout 60 "press TRIANGLE" "wait 15" "${nav[@]}" "press CIRCLE" "wait 300" >/dev/null
done
echo "$1 model $(model)"
