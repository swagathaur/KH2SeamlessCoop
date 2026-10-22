#!/usr/bin/env python3
"""Extract the PC release's high-resolution textures (the "remastered assets"
packed after each file in kh2_*.pkg) for the runtime's texture replacement
(port/runtime/hd_textures.h).

Usage:
  egs_hd_extract.py IMAGE_DIR OUT_DIR [--list] [--extract] [PREFIX ...]
IMAGE_DIR as for egs_extract.py. PREFIXes pick files by name (default: the
2D ones: menu/ field2d/ file/ msg/ event/layout/ msn/ gumibattle/ gumimenu/
itempic/ ...); names come from OpenKH's kh2idx.txt (tools/data/kh2_names.txt).

Each remastered asset names the place in its original file the texture came
from (OriginalAssetOffset, bit 29 set). The runtime recognises the texture
when the game uploads it to the GS by a checksum of its first bytes, so the
manifest (OUT_DIR/manifest.txt) lists, per image:
  <CRC-32 and Adler-32 of 256 bytes, hex> <bytes hashed>@<skip> <image path> <source file> <checks>
(the 256 bytes start after any blank lead-in: <skip> bytes all alike), and
for a model's or map's texture "v:<TBP0>:<CBP>:<CSA>:<TBW>:<x>:<y>:<w>:<h>"
(its texture environment: palette variants share their data and differ
there; maps keep several textures in one 4-bit atlas, one CSA and CLAMP
region each, and the PC's image covers that region x,y wxh) or "-". A
model's or map's texture data is found through the section's own upload
list (texture_data), not the PC's offset, which drifts in big maps.
where <checks> is len:hash,... for longer prefixes (1 KiB to 256 KiB, as far as
the file goes): many textures start with the same transparent bytes, so the
runtime confirms a hit with the longest prefix the upload covers.
By default the images stay in the game's .pkg files and the manifest points
at them ("pkg:<archive>:<entry>:<data>:<stored>:<real>:<type>"; "#archive"
lines name the .pkg files): nothing is copied. --extract writes them out as
files under OUT_DIR instead.
Files under a language folder are written for the English text: msg/us/x is
looked up as msg/jp/x would be (the runtime sees the bytes only, so this only
matters for the names in the manifest).
"""
import hashlib, os, re, struct, sys, zlib
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import egs_extract as E

DEFAULT_PREFIXES = ["menu/", "field2d/", "file/", "msg/", "event/layout/", "msn/", "gumibattle/", "gumimenu/",
                    "itempic/", "libretto", "minigame/", "remastered/", "dbg/", "effect/", "limit/", "title",
                    "map/", "obj/"]



# Remastered images laid out differently from the texture they replace (the
# PC draws them its own way): left out.
EXCLUDE = {
    ("msg/us/fontimage.bar", "US_fontimage_bar2.png"),  # button icons: the PC's own atlas
}


def key64(data):
    """CRC-32 (zlib) in the high half, Adler-32 in the low half."""
    return (zlib.crc32(data) & 0xFFFFFFFF) << 32 | (zlib.adler32(data) & 0xFFFFFFFF)


def texture_envs(original):
    """(TBP0, CBP, CSA, TBW, x, y, w, h) of every texture environment in a
    model's or map's texture sections (BAR entries of type 7, in order): the
    "-<n>" images count through them, which tells palette variants apart.
    x, y, w, h is the part of the texture the environment's CLAMP lets it
    read (maps keep several textures in one atlas, each with its own CSA and
    region); the PC's image for the environment covers just that part."""
    out = []
    if original[:4] != b"BAR\x01":
        return out
    count = struct.unpack_from("<I", original, 4)[0]
    for i in range(count):
        t, _, _, off, size = struct.unpack_from("<HH4sII", original, 16 + i * 16)
        if t != 7 or size < 36:
            continue
        gs_count, env_off = struct.unpack_from("<i", original, off + 12)[0], struct.unpack_from("<i", original, off + 24)[0]
        for g in range(max(gs_count, 0)):
            at = off + env_off + g * 0xA0 + 0x70
            if at + 0x18 > off + size:
                break
            tex0 = struct.unpack_from("<Q", original, at)[0]
            clamp = struct.unpack_from("<Q", original, at + 0x10)[0]
            reg = struct.unpack_from("<Q", original, at + 0x18)[0] & 0xFF
            tw, th = 1 << ((tex0 >> 26) & 15), 1 << ((tex0 >> 30) & 15)
            x, y, w, h = 0, 0, tw, th
            if reg == 0x08:  # CLAMP_1
                def axis(mode, lo, hi, size):
                    if mode == 2:  # region clamp
                        return (lo, hi - lo + 1) if hi >= lo else (0, size)
                    if mode == 3 and (lo + 1) & lo == 0:  # region repeat: (u & mask) | fix
                        return hi, lo + 1
                    return 0, size
                x, w = axis(clamp & 3, (clamp >> 4) & 0x3FF, (clamp >> 14) & 0x3FF, tw)
                y, h = axis((clamp >> 2) & 3, (clamp >> 24) & 0x3FF, (clamp >> 34) & 0x3FF, th)
                if x + w > tw or y + h > th:
                    x, y, w, h = 0, 0, tw, th
            out.append((tex0 & 0x3FFF, (tex0 >> 37) & 0x3FFF, (tex0 >> 56) & 0x1F, (tex0 >> 14) & 0x3F,
                        x, y, w, h))
    return out


def texture_data(original):
    """Where each texture environment's picture is, as the game uploads it:
    for every BAR entry of type 7, environment n (GsInfo n) samples upload
    (TextureInfo) number Offset1[n] + 1 (upload 0 is the palette), whose
    DMA reference tag (+0x70) points at its data in the section. The PC's
    OriginalAssetOffset only approximates this (it drifts in big maps).
    Returns [(file offset, bytes)] in the order of texture_envs, or []."""
    out = []
    if original[:4] != b"BAR\x01":
        return out
    count = struct.unpack_from("<I", original, 4)[0]
    for i in range(count):
        t, _, _, off, size = struct.unpack_from("<HH4sII", original, 16 + i * 16)
        if t != 7 or size < 36:
            continue
        _, _, tcount, gcount, off1, tinf = struct.unpack_from("<6i", original, off)
        uploads = []
        for u in range(max(tcount, 0)):
            at = off + tinf + u * 0x90 + 0x70
            if at + 8 > off + size:
                break
            tag = struct.unpack_from("<Q", original, at)[0]
            uploads.append((off + ((tag >> 32) & 0x7FFFFFFF), (tag & 0xFFFF) * 16))
        for g in range(max(gcount, 0)):
            if off + off1 + g >= off + size:
                out.append(None)
                continue
            u = original[off + off1 + g] + 1
            out.append(uploads[u] if u < len(uploads) else None)
    return out


def leading_blank(data, at, limit=65536):
    """Bytes from at that repeat data[at], in whole 16-byte steps."""
    first = data[at]
    n = 0
    end = min(len(data), at + limit)
    while at + n + 16 <= end and data[at + n:at + n + 16] == bytes([first]) * 16:
        n += 16
    return n


def read_full(pkg, off, want_data):
    """The original file and its remastered assets: (name, original offset,
    data offset in the .pkg, stored length, real length, data or None)."""
    pkg.seek(off)
    seed = pkg.read(16)
    dlen, rcount, clen, _ = struct.unpack("<iiii", seed)
    heads = [pkg.read(0x30) for _ in range(rcount)]
    raw = pkg.read(clen if clen >= 0 else dlen)
    key = E.key_for(seed)
    if clen > -2:
        raw = E.decrypt(raw, key)
    original = zlib.decompress(raw[2:], -15)[:dlen] if clen >= 0 else raw[:dlen]
    assets = []
    pos = pkg.tell()
    for h in heads:
        name = h[:0x20].split(b"\0")[0].decode(errors="replace")
        roff, orig_off, adlen, aclen = struct.unpack("<iiii", h[0x20:0x30])
        size = aclen if aclen >= 0 else adlen
        size += -size % 16
        pos += -(pos - off) % 16  # each asset is 16-aligned after the previous
        data = None
        if want_data:
            pkg.seek(pos)
            data = pkg.read(size)
            if aclen > -2:
                data = E.decrypt(data, key)
            if aclen >= 0:
                data = zlib.decompress(data[2:], -15)[:adlen]
            else:
                data = data[:adlen]
        assets.append((name, orig_off, pos, aclen, adlen, data))
        pos += size
    return original, assets


def main():
    a = sys.argv[1:]
    if len(a) < 2:
        raise SystemExit(__doc__)
    image, out = a[0], a[1]
    listing = "--list" in a
    extract = "--extract" in a
    prefixes = [x for x in a[2:] if not x.startswith("--")] or DEFAULT_PREFIXES
    names_file = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data", "kh2_names.txt")
    names = [l.strip() for l in open(names_file, encoding="utf-8") if l.strip() and not l.startswith("#")]
    names = [n for n in names if any(n.startswith(p) for p in prefixes)]
    # Only the English (us) language folders, and files without one.
    other = ("/jp/", "/uk/", "/fr/", "/gr/", "/it/", "/sp/", "/fm/")
    names = [n for n in names if not any(o in n for o in other)]
    archives = []
    for arc in E.ARCHIVES:
        base = os.path.join(image, "backup", arc)
        if not os.path.exists(base + ".pkg"):
            base = os.path.join(image, arc)
        if os.path.exists(base + ".hed"):
            archives.append((base, E.read_hed(base + ".hed")))
    os.makedirs(out, exist_ok=True)
    manifest = ["#archive %d %s" % (i, os.path.abspath(base + ".pkg")) for i, (base, _) in enumerate(archives)]
    files = images = skipped = dupes = 0
    seen = set()
    for name in names:
        md5 = hashlib.md5(name.encode()).digest()
        for ai, (base, hed) in enumerate(archives):
            if md5 not in hed:
                continue
            with open(base + ".pkg", "rb") as pkg:
                pkg.seek(hed[md5][0])
                if struct.unpack("<iiii", pkg.read(16))[1] == 0:
                    break
                original, assets = read_full(pkg, hed[md5][0], extract)
            files += 1
            envs = texture_envs(original)
            datas = texture_data(original)
            for aname, orig_off, dpos, aclen, adlen, data in assets:
                ext = os.path.splitext(aname)[1].lower()
                if (name, aname) in EXCLUDE:
                    skipped += 1
                    continue
                if not (orig_off & 0x20000000) or ext not in (".png", ".dds"):
                    skipped += 1
                    continue
                at = orig_off & 0x0FFFFFFF
                # Model and map textures ("-<n>.dds"): the offset is the
                # texture data's plus 16 * n (palette variants of one texture
                # share the data, so the PC tells them apart that way).
                m = re.match(r"-(\d+)\.", aname)
                variant = "-"
                if m:
                    n = int(m.group(1))
                    if n < len(datas) and datas[n] is not None:
                        at = datas[n][0]
                    else:
                        at -= 16 * n
                    if n < len(envs):
                        variant = "v:%d:%d:%d:%d:%d:%d:%d:%d" % envs[n]
                    if at < 0:
                        skipped += 1
                        continue
                chunk = original[at:at + 256]
                if len(chunk) < 64:
                    skipped += 1
                    continue
                # Keyed past a blank start (leading bytes all alike, in 16-byte
                # steps), as the runtime does: blank starts are everywhere.
                skip = leading_blank(original, at)
                # A blank start (one byte value over 64 KiB) would also match
                # the game's own empty buffers: leave those out.
                head = original[at:at + 65536]
                if head.count(head[0]) == len(head):
                    skipped += 1
                    continue
                rel = os.path.join(name.replace("/", os.sep) + ".hd", aname.replace("/", "_"))
                if listing:
                    print(name, aname, hex(at), adlen)
                elif extract:
                    dst = os.path.join(out, rel)
                    os.makedirs(os.path.dirname(dst), exist_ok=True)
                    with open(dst, "wb") as f:
                        f.write(data)
                else:
                    # Read from the .pkg by the runtime: archive, file entry,
                    # data offset, stored and real length, type.
                    rel = "pkg:%d:%d:%d:%d:%d:%s" % (ai, hed[md5][0], dpos, aclen, adlen, ext[1:])
                checks = []
                for n in (1024, 4096, 16384, 65536, 262144):
                    if at + n <= len(original):
                        checks.append("%d:%016x" % (n, key64(original[at:at + n])))
                keyed = original[at + skip:at + skip + 256]
                if len(keyed) < 256 and skip:
                    skipped += 1
                    continue
                ident = (key64(keyed), skip, tuple(checks), variant)
                if ident in seen:
                    dupes += 1  # the same original bytes again (shared art): one image
                    continue
                seen.add(ident)
                manifest.append("%016x %d@%d %s %s %s %s" % (key64(keyed), len(keyed), skip, rel.replace(os.sep, "/"),
                                                          name, ",".join(checks) or "-", variant))
                images += 1
            break
    if not listing:
        with open(os.path.join(out, "manifest.txt"), "w") as f:
            f.write("\n".join(manifest) + "\n")
    print(f"{files} files, {images} images, {skipped} skipped, {dupes} duplicates")


if __name__ == "__main__":
    main()
