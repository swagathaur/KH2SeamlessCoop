#!/bin/bash
# formstress.sh FROM TO [wait] : rounds FROM..TO; per round the client's form (its player's entry) and null-calls so far
cd "$HOME/mnt/KH2 Decomp project"
for r in $(seq $1 $2); do
  line=$(bash tools/live/doortime.sh round $r ${3:-450} | head -1 | cut -c1-60)
  nc=$(grep -a -c "null-call" logs/live-client.log)
  model=$(grep -a "our model is" logs/live-client.log | tail -1 | grep -oE "is [0-9A-F]{4}")
  echo "$line | client $model | null-calls $nc"
  [ "$nc" != 0 ] && break
done
