#!/bin/bash
# mem.sh : the games' working sets (MB)
cd "$HOME/mnt/KH2 Decomp project"
n=$(( $(ls .agent/jobs | grep -oE '^[0-9]+' | sort -n | tail -1) + 1 ))
cp tools/live/mem.ps1 .agent/jobs/$n-mem.ps1
for i in $(seq 30); do [ -f .agent/jobs/$n-mem.exit ] && break; sleep 1; done; cat .agent/jobs/$n-mem.out.log | tr '\n' ' '; echo
