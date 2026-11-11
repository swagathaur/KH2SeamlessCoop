#!/usr/bin/env python3
# doortime.py HOSTOFF CLIENTOFF : one round's timings from logs/live-host.log and logs/live-client.log
# (lines after the given line offsets), relative to the host's map jump.
import sys, re
def lines(name, off):
    return open(f'logs/live-{name}.log', errors='replace').read().splitlines()[int(off):]
def ev(ls):
    out = {}
    for l in ls:
        m = re.match(r'\[timing\] (\w+) ([0-9A-F]{2}-[0-9A-F]{2})(?: (-?\d+))? ms=(\d+)', l)
        if m:
            k = m.group(1) + (m.group(3) or '') if m.group(1) in ('avatar', 'proxyhidden') else m.group(1)
            out.setdefault(k, int(m.group(4)))
    return out
h, c = ev(lines('host', sys.argv[1])), ev(lines('client', sys.argv[2]))
t0 = h.get('jump')
if t0 is None:
    print('no host jump'); print('ALL_VISIBLE -'); sys.exit(0)
def d(x): return '-' if x is None else f'{(x - t0) / 1000:.2f}'
row = {'client_jump': c.get('jump'), 'host_control': h.get('control'), 'client_control': c.get('control'),
       'host_sees_client': h.get('avatar2'), 'client_sees_host': c.get('avatar1'),
       'host_proxy_hidden': h.get('proxyhidden1'), 'client_proxy_hidden': c.get('proxyhidden1')}
print(' '.join(f'{k}={d(v)}' for k, v in row.items()))
# How long each saw a party member in the other player's place: from control until the place holder
# was hidden (or, without that, until the avatar came).
def seen(ctrl, hid, av):
    end = hid if hid is not None else av
    return '-' if ctrl is None or end is None else f'{max(0, end - ctrl) / 1000:.2f}'
print('DONALD_SEEN host', seen(row['host_control'], row['host_proxy_hidden'], row['host_sees_client']),
      'client', seen(row['client_control'], row['client_proxy_hidden'], row['client_sees_host']))
both = [row['host_sees_client'], row['client_sees_host']]
print('ALL_VISIBLE', d(max(both)) if None not in both else '-')
