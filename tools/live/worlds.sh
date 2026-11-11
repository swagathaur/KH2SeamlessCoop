#!/bin/bash
# worlds.sh party | go W R : for a duo (doortime.sh up) on the Land of Dragons save.
#   party  both games get Donald and Goofy in Halloween Town, Atlantica and the Pride Lands (the save has not
#          reached them: no party there), and their HP held
#   go     the host warps to world W room R (decimal); both games' states after
cd "$HOME/mnt/KH2 Decomp project"
case $1 in
party) for g in host client; do tools/live/ctl.sh $g --timeout 20 "poke 0032F08C 00010212" "poke 0032F090 00010212" \
         "poke 0032F09C 00010212" "freeze 01C6C750 1E000000" >/dev/null; done; echo party;;
go) tools/live/ctl.sh host --timeout 90 "warp $2 $3" "wait 1000" >/dev/null
    for g in host client; do tools/live/ctl.sh $g --timeout 20 state | tail -1 | cut -c27-90; done;;
esac
