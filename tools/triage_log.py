#!/usr/bin/env python3
"""Summarise a kh2recomp run log: what stopped it and what it complained about.

Usage: triage_log.py logs/run_<name>.log [logs/run_<name>.err.log]
"""
import re
import sys
from collections import Counter

PATTERNS = [
    ("missing-target", re.compile(r"\[guest-branch:missing-target\].*")),
    ("unimplemented-stub", re.compile(r"Unimplemented PS2 stub called\. name=(\S+)")),
    ("unknown-syscall", re.compile(r"Unknown syscallId=(0x[0-9a-fA-F]+)")),
    ("overlay", re.compile(r"\[kh2-overlay\].*")),
    ("pad2", re.compile(r"\[pad2\].*")),
    ("harness", re.compile(r"\[harness\].*")),
    ("exception", re.compile(r".*(exception|Exception|assert|Assert|SIGSEGV|access violation).*")),
]


def main():
    lines = []
    for path in sys.argv[1:]:
        try:
            lines += open(path, encoding="utf-8", errors="replace").read().splitlines()
        except FileNotFoundError:
            pass
    counts, first = Counter(), {}
    for line in lines:
        for kind, rx in PATTERNS:
            m = rx.search(line)
            if m:
                key = (kind, m.group(1) if m.groups() else "")
                counts[key] += 1
                first.setdefault(key, line.strip())
    last_status = next((l for l in reversed(lines) if l.startswith("[status]")), "(no status lines)")
    print(f"lines: {len(lines)}")
    print(f"last status: {last_status}")
    for (kind, detail), n in sorted(counts.items(), key=lambda kv: (kv[0][0], -kv[1])):
        print(f"{kind:20} x{n:<5} {first[(kind, detail)][:220]}")


if __name__ == "__main__":
    main()
