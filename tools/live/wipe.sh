#!/bin/bash
# wipe.sh HOSTFORM CLIENTFORM : both drive (0: none), lose their HP the game's way (hurt), and should
# go down then Game Over together; prints what each did, then Continue on both and the state after
cd "$HOME/mnt/KH2 Decomp project"
for g in host client; do tools/live/ctl.sh $g --timeout 10 "unfreeze 01C6C750" >/dev/null; done
[ "$1" != 0 ] && tools/live/ctl.sh host --timeout 20 "drive $1" "wait 220" >/dev/null
[ "$2" != 0 ] && tools/live/ctl.sh client --timeout 20 "drive $2" "wait 220" >/dev/null
c0=$(wc -l < logs/live-client.log); h0=$(wc -l < logs/live-host.log)
tools/live/ctl.sh client --timeout 20 "hurt 999" "wait 120" >/dev/null
tools/live/ctl.sh host --timeout 20 "hurt 999" "wait 600" >/dev/null
for g in host client; do o=$([ $g = host ] && echo $h0 || echo $c0); echo "$g: $(tail -n +$o logs/live-$g.log | grep -a 'downed\]' | sed 's/\[downed\] //' | cut -c1-40 | tr '\n' '|') $(tools/live/ctl.sh $g --timeout 10 state | tail -1 | grep -o 'hp=[^ ]*')"; done
for g in host client; do tools/live/ctl.sh $g --timeout 20 "press CIRCLE" "wait 20" >/dev/null; done; sleep 12
for g in host client; do tools/live/ctl.sh $g --timeout 10 "freeze 01C6C750 1E000000" >/dev/null; done
bash tools/live/health.sh
