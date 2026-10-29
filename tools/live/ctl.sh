#!/bin/bash
# ctl.sh NAME [--timeout S] CMD...   append commands to a live game's control file and wait until
# they have run; prints the [control] lines (and any other new log lines with --all).
cd "$HOME/mnt/KH2 Decomp project" || exit 1
name=$1; shift
timeout=120; all=0
while [[ $1 == --* ]]; do case $1 in --timeout) timeout=$2; shift 2;; --all) all=1; shift;; *) shift;; esac; done
log=logs/live-$name.log; ctl=scratch/ctl-$name.txt
tag=t$RANDOM$RANDOM
start=$(wc -l < "$log" 2>/dev/null || echo 0)
for c in "$@"; do printf '%s\n' "$c" >> "$ctl"; done
printf 'echo %s\n' "$tag" >> "$ctl"
for ((i=0;i<timeout*2;i++)); do
  if tail -n +$((start+1)) "$log" | grep -q "\[control\].* echo $tag"; then break; fi
  sleep 0.5
done
if ((all)); then tail -n +$((start+1)) "$log" | grep -v "^INFO\|echo $tag\|saved shots"
else tail -n +$((start+1)) "$log" | grep "\[control\]\|\[harness\] quit\|exit" | grep -v "echo $tag"; fi
tail -n +$((start+1)) "$log" | grep -q "echo $tag" || echo "(timed out after ${timeout}s; game still running?)"
