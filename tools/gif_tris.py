#!/usr/bin/env python3
"""Triangles from a GIF capture (--gif-capture), and where the mesh has cracks.

    gif_tris.py CAPTURE.bin [--rect x0,y0,x1,y1] [--png out.png --size WxH]

CAPTURE.bin is [u32 size][size bytes of GIF packets]...  Prints, per texture,
the triangles drawn, then vertex pairs that almost (but not exactly) coincide
and T-junctions: the places a rasterizer can leave pixel gaps between
triangles that are meant to share an edge. Coordinates are GS window pixels.
"""
import argparse
import struct
from collections import defaultdict


def parse(path):
    data = open(path, 'rb').read()
    pos = 0
    prim = 0
    prmode = 0
    prmodecont = 1
    tex0 = [0, 0]
    frame = [0, 0]
    ofsset = [False, False]
    ofs = [(0, 0), (0, 0)]
    queue = []
    tris = []
    draw = 0

    def ctxt():
        p = prim if prmodecont else ((prim & 7) | (prmode & ~7))
        return (p >> 9) & 1

    def kick(x, y, z, adc):
        nonlocal draw
        queue.append((x, y, z))
        t = prim & 7
        if adc:
            if t in (3,) and len(queue) >= 3:
                del queue[:3]
            elif t in (4, 5) and len(queue) > 3:
                del queue[0 if t == 4 else 1]
            return
        if t == 3:
            if len(queue) == 3:
                emit(queue[0], queue[1], queue[2]); queue.clear()
        elif t == 4:
            if len(queue) >= 3:
                emit(queue[-3], queue[-2], queue[-1])
                del queue[0]
        elif t == 5:
            if len(queue) >= 3:
                emit(queue[0], queue[-2], queue[-1])
                del queue[1]
        else:
            queue.clear()

    def emit(a, b, c):
        cx = ctxt()
        ox, oy = ofs[cx]
        tris.append({'v': [((p[0] - ox) / 16.0, (p[1] - oy) / 16.0, p[2]) for p in (a, b, c)],
                     'raw': [(p[0], p[1]) for p in (a, b, c)],
                     'tbp': tex0[cx] & 0x3FFF, 'prim': prim, 'draw': draw,
                     'fbp': frame[cx] & 0x1FF, 'ofs': ofs[cx], 'ofsset': ofsset[cx]})

    def adreg(addr, val):
        nonlocal prim, prmode, prmodecont, draw
        if addr == 0x00:
            prim = val & 0x7FF; queue.clear(); draw += 1
        elif addr == 0x1B:
            prmode = val & 0x7FF
        elif addr == 0x1A:
            prmodecont = val & 1
        elif addr in (0x06, 0x07):
            tex0[addr - 6] = val
        elif addr in (0x18, 0x19):
            ofs[addr - 0x18] = (val & 0xFFFF, (val >> 32) & 0xFFFF)
            ofsset[addr - 0x18] = True
        elif addr in (0x4C, 0x4D):
            frame[addr - 0x4C] = val
        elif addr in (0x04, 0x05, 0x0C, 0x0D):
            x, y = val & 0xFFFF, (val >> 16) & 0xFFFF
            z = (val >> 32) & (0xFFFFFF if addr in (0x04, 0x0C) else 0xFFFFFFFF)
            kick(x, y, z, addr in (0x0C, 0x0D))

    while pos + 4 <= len(data):
        size, = struct.unpack_from('<I', data, pos)
        pkt = data[pos + 4:pos + 4 + size]
        pos += 4 + size
        p = 0
        while p + 16 <= len(pkt):
            lo, hi = struct.unpack_from('<QQ', pkt, p)
            p += 16
            nloop = lo & 0x7FFF
            pre = (lo >> 46) & 1
            flg = (lo >> 58) & 3
            nreg = (lo >> 60) & 15 or 16
            regs = [(hi >> (4 * i)) & 15 for i in range(nreg)]
            if pre:
                adreg(0x00, (lo >> 47) & 0x7FF)
            if flg == 0:
                for _ in range(nloop):
                    for r in regs:
                        if p + 16 > len(pkt):
                            break
                        a, b = struct.unpack_from('<QQ', pkt, p)
                        p += 16
                        if r == 0x0:
                            adreg(0x00, a)
                        elif r in (0x4, 0xC):
                            x, y = a & 0xFFFF, (a >> 32) & 0xFFFF
                            z = (b >> 4) & 0xFFFFFF
                            adc = (b >> 47) & 1 or r == 0xC
                            kick(x, y, z, adc)
                        elif r in (0x5, 0xD):
                            x, y = a & 0xFFFF, (a >> 32) & 0xFFFF
                            z = b & 0xFFFFFFFF
                            adc = (b >> 47) & 1 or r == 0xD
                            kick(x, y, z, adc)
                        elif r in (0x6, 0x7):
                            tex0[r - 6] = a
                        elif r == 0xE:
                            adreg(b & 0xFF, a)
            elif flg == 1:
                words = nloop * nreg
                for i in range(words):
                    if p + 8 > len(pkt):
                        break
                    v, = struct.unpack_from('<Q', pkt, p)
                    p += 8
                    r = regs[i % nreg]
                    if r == 0x0:
                        adreg(0x00, v)
                    elif r in (0x4, 0x5, 0xC, 0xD):
                        adreg({0x4: 0x04, 0x5: 0x05, 0xC: 0x0C, 0xD: 0x0D}[r], v)
                    elif r in (0x6, 0x7):
                        tex0[r - 6] = v
                if words & 1:
                    p += 8
            else:
                p += nloop * 16
            if (lo >> 15) & 1 and False:
                pass
    return tris


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('capture')
    ap.add_argument('--rect', help='x0,y0,x1,y1 in GS window pixels')
    ap.add_argument('--tbp', type=lambda s: int(s, 0), action='append')
    ap.add_argument('--png')
    ap.add_argument('--size', default='512x448')
    ap.add_argument('--scale', type=int, default=4)
    a = ap.parse_args()
    tris = parse(a.capture)
    print(f'{len(tris)} triangles')
    rect = tuple(float(v) for v in a.rect.split(',')) if a.rect else None

    def inside(t):
        if a.tbp and t['tbp'] not in a.tbp:
            return False
        if not rect:
            return True
        return any(rect[0] <= v[0] <= rect[2] and rect[1] <= v[1] <= rect[3] for v in t['v'])

    sel = [t for t in tris if inside(t)]
    by_tex = defaultdict(int)
    for t in sel:
        by_tex[t['tbp']] += 1
    print(f'{len(sel)} in range; by texture base:', ', '.join(f'0x{k:x}:{n}' for k, n in sorted(by_tex.items())))

    # Near-coincident vertices (0 < d <= 4 units of 1/16 px).
    verts = defaultdict(set)
    for i, t in enumerate(sel):
        for r in t['raw']:
            verts[r].add(i)
    keys = list(verts)
    grid = defaultdict(list)
    for k in keys:
        grid[(k[0] >> 3, k[1] >> 3)].append(k)
    near = []
    for k in keys:
        gx, gy = k[0] >> 3, k[1] >> 3
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                for o in grid[(gx + dx, gy + dy)]:
                    if o <= k:
                        continue
                    d = max(abs(o[0] - k[0]), abs(o[1] - k[1]))
                    if 0 < d <= 4:
                        near.append((k, o, d))
    print(f'{len(near)} near-coincident vertex pairs (1..4 sixteenths of a pixel apart)')
    for k, o, d in near[:40]:
        ti = sorted(verts[k])[:3]
        to = sorted(verts[o])[:3]
        print(f'  ({k[0]/16:.4f},{k[1]/16:.4f}) vs ({o[0]/16:.4f},{o[1]/16:.4f}) d={d}  tex 0x{sel[ti[0]]["tbp"]:x}/0x{sel[to[0]]["tbp"]:x} draws {sel[ti[0]]["draw"]}/{sel[to[0]]["draw"]}')

    # T-junctions: a vertex strictly inside another triangle's edge.
    edges = defaultdict(list)
    for i, t in enumerate(sel):
        r = t['raw']
        for j in range(3):
            p, q = r[j], r[(j + 1) % 3]
            edges[(min(p, q), max(p, q))].append(i)
    tj = 0
    samples = []
    for (p, q), owners in edges.items():
        if len(owners) != 1:
            continue  # shared or boundary-with-partner edges are fine
        x0, y0 = p
        x1, y1 = q
        L2 = (x1 - x0) ** 2 + (y1 - y0) ** 2
        if L2 == 0:
            continue
        minx, maxx = min(x0, x1), max(x0, x1)
        miny, maxy = min(y0, y1), max(y0, y1)
        for gx in range((minx >> 3) - 1, (maxx >> 3) + 2):
            for gy in range((miny >> 3) - 1, (maxy >> 3) + 2):
                for k in grid.get((gx, gy), ()):
                    if k in (p, q):
                        continue
                    t_ = ((k[0] - x0) * (x1 - x0) + (k[1] - y0) * (y1 - y0)) / L2
                    if not 0.02 < t_ < 0.98:
                        continue
                    cross = (k[0] - x0) * (y1 - y0) - (k[1] - y0) * (x1 - x0)
                    dist = abs(cross) / L2 ** 0.5
                    if dist < 1.0:
                        tj += 1
                        if len(samples) < 20:
                            samples.append((k, p, q, dist))
        if (maxx - minx) > 4000 or (maxy - miny) > 4000:
            continue
    print(f'{tj} T-junction vertices (within 1/16 px of another triangle edge)')
    for k, p, q, dist in samples:
        print(f'  vertex ({k[0]/16:.3f},{k[1]/16:.3f}) on edge ({p[0]/16:.3f},{p[1]/16:.3f})-({q[0]/16:.3f},{q[1]/16:.3f}) d={dist:.2f}/16')

    if a.png:
        from PIL import Image, ImageDraw
        w, h = (int(v) for v in a.size.split('x'))
        s = a.scale
        img = Image.new('RGB', (w * s, h * s), (0, 0, 0))
        dr = ImageDraw.Draw(img)
        for t in sel:
            pts = [(v[0] * s, v[1] * s) for v in t['v']]
            dr.polygon(pts, outline=(90, 90, 90))
        for k, o, d in near:
            dr.ellipse((k[0] / 16 * s - 3, k[1] / 16 * s - 3, k[0] / 16 * s + 3, k[1] / 16 * s + 3), outline=(255, 60, 60))
        for k, p, q, dist in samples:
            dr.ellipse((k[0] / 16 * s - 3, k[1] / 16 * s - 3, k[0] / 16 * s + 3, k[1] / 16 * s + 3), outline=(60, 255, 60))
        if rect:
            img = img.crop((int(rect[0] * s), int(rect[1] * s), int(rect[2] * s), int(rect[3] * s)))
        img.save(a.png)


if __name__ == '__main__':
    main()
