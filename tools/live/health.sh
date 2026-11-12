#!/bin/bash
# health.sh [GAMES...] : per game: alive (answers ctl), room, player, null calls, thread deaths, jobs dropped, hangs
cd "$HOME/mnt/KH2 Decomp project"
for g in ${@:-host client}; do
  st=$(tools/live/ctl.sh $g --timeout 8 state 2>/dev/null | tail -1)
  [ -z "$st" ] && st="NO ANSWER"
  printf '%-7s %s | nc %s dead %s dropped %s hang %s\n' $g "$(echo "$st" | grep -o 'state [^|]*' | cut -c7-60)" \
    "$(grep -ac null-call logs/live-$g.log)" "$(grep -ac 'returned to 0' logs/live-$g.log)" \
    "$(grep -ac 'job dropped' logs/live-$g.err.log logs/live-$g.log | awk -F: '{s+=$2} END{print s}')" \
    "$(grep -ac '\[watchdog\]\|crashed' logs/live-$g.log logs/live-$g.err.log | awk -F: '{s+=$2} END{print s}')"
done
