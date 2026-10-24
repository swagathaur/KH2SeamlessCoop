#!/usr/bin/env python3
"""Make a portable copy of the HD texture index.

The index (game/files/hd/manifest.txt, made by egs_hd_extract.py) points into
the PC release's .pkg archives where they are installed ("#archive N <path>",
entries "pkg:<archive>:<entry offset>:<data offset>:<stored>:<real>:<type>").
This copies just the parts the index uses (each texture's 16-byte seed at
the entry offset and its encrypted data at the data offset; about 5 GB of
the release's 16) into pack files next to a rewritten manifest, whose
#archive lines name them relatively, so the copy works on a PC without the
PC release (hd_textures.cpp resolves relative archive paths next to the
manifest). Packs are split before 3.5 GB (FAT32 cannot hold 4 GB files).

    python tools/hd_portable.py game/files/hd E:/KH2/game/files/hd
"""
import os
import sys

PACK_LIMIT = 3_500_000_000
CHUNK = 1 << 20


def parse_ref(token):
    parts = token.split(':')
    archive, entry, data, stored, real = (int(x) for x in parts[1:6])
    rest = parts[6:]
    return archive, entry, data, stored, real, rest


def data_size(stored, real):
    size = stored if stored >= 0 else real
    return size + (16 - size % 16) % 16


def main(src_dir, dst_dir):
    src_manifest = os.path.join(src_dir, 'manifest.txt')
    lines = open(src_manifest, encoding='utf-8').read().splitlines()
    archives = {}
    for line in lines:
        if line.startswith('#archive '):
            index, path = line[9:].split(' ', 1)
            archives[int(index)] = path
    # Every texture the manifest uses: (archive, entry offset, data offset) ->
    # data size. Each is copied as its 16-byte seed followed by its data.
    pieces = {}
    for line in lines:
        if line.startswith('#'):
            continue
        for token in line.split():
            if token.startswith('pkg:'):
                a, e, d, st, re_, _ = parse_ref(token)
                key = (a, e, d)
                pieces[key] = max(pieces.get(key, 0), data_size(st, re_))
    missing = sorted({k[0] for k in pieces if not os.path.exists(archives.get(k[0], ''))})
    if missing:
        sys.exit('missing archives: ' + ', '.join(archives.get(a, str(a)) for a in missing))
    os.makedirs(dst_dir, exist_ok=True)
    where = {}
    pack_index, pack, pack_size, packs = -1, None, PACK_LIMIT, []
    total = sum(16 + v for v in pieces.values())
    done = 0
    open_src = {}

    def copy(src, off, size):
        src.seek(off)
        left = size
        while left:
            buf = src.read(min(CHUNK, left))
            if not buf:
                sys.exit('short read at %d' % off)
            pack.write(buf)
            left -= len(buf)

    for key in sorted(pieces, key=lambda k: (k[0], k[2])):
        a, e, d = key
        size = pieces[key]
        if pack_size + 16 + size > PACK_LIMIT:
            if pack:
                pack.close()
            pack_index += 1
            name = 'hdpack_%d.bin' % pack_index
            packs.append(name)
            pack = open(os.path.join(dst_dir, name), 'wb')
            pack_size = 0
        if a not in open_src:
            open_src[a] = open(archives[a], 'rb')
        copy(open_src[a], e, 16)
        copy(open_src[a], d, size)
        where[key] = (pack_index, pack_size, pack_size + 16)
        pack_size += 16 + size
        before = done
        done += 16 + size
        if done * 20 // total != before * 20 // total:
            print('%d%%' % (done * 100 // total), flush=True)
    if pack:
        pack.close()
    for f in open_src.values():
        f.close()
    # The manifest, pointing at the packs.
    out = []
    for line in lines:
        if line.startswith('#archive '):
            continue
        if line.startswith('#format'):
            out.append(line)
            for i, name in enumerate(packs):
                out.append('#archive %d %s' % (i, name))
            continue
        if line.startswith('#') or 'pkg:' not in line:
            out.append(line)
            continue
        tokens = line.split(' ')
        for i, token in enumerate(tokens):
            if token.startswith('pkg:'):
                a, e, d, st, re_, rest = parse_ref(token)
                p, oe, od = where[(a, e, d)]
                tokens[i] = ':'.join(['pkg', str(p), str(oe), str(od), str(st), str(re_)] + rest)
        out.append(' '.join(tokens))
    with open(os.path.join(dst_dir, 'manifest.txt'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(out) + '\n')
    print('%d pieces, %.2f GB in %d pack(s) -> %s' % (len(pieces), total / 1e9, len(packs), dst_dir))


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
