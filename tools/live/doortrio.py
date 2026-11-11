#!/usr/bin/env python3
# doortrio.py HOFF COFF POFF : one trio round, seconds from the host's map jump
import sys, re
names = ['host', 'client', 'p3']
def ev(name, off):
    out = {}
    for l in open(f'logs/live-{name}.log', errors='replace').read().splitlines()[int(off):]:
        m = re.match(r'\[timing\] (\w+) ([0-9A-F]{2}-[0-9A-F]{2})(?: (-?\d+))? ms=(\d+)', l)
        if m:
            k = m.group(1) + (m.group(3) or '') if m.group(1) in ('avatar', 'proxyhidden') else m.group(1)
            out.setdefault(k, int(m.group(4)))
    return out
E = {n: ev(n, o) for n, o in zip(names, sys.argv[1:4])}
t0 = E['host'].get('jump')
if t0 is None: print('no host jump ALL_VISIBLE -'); sys.exit(0)
f = lambda x: '-' if x is None else f'{(x - t0) / 1000:.2f}'
parts, last = [], []
for n in names:
    av = sorted(v for k, v in E[n].items() if k.startswith('avatar'))
    parts.append(f"{n}: jump={f(E[n].get('jump'))} ctl={f(E[n].get('control'))} avatars={len(av)} last={f(av[-1] if av else None)}")
    last.append(av[-1] if len(av) >= 2 else None)
print(' | '.join(parts), '| ALL_VISIBLE', f(max(last)) if None not in last else '-')
