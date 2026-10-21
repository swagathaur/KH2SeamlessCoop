#!/usr/bin/env python3
"""Extract files from the PC release (KINGDOM HEARTS HD 1.5+2.5 ReMIX, Steam or
Epic): the kh2_*.hed / kh2_*.pkg archives in its Image\\<language> folder.

Usage:
  egs_extract.py IMAGE_DIR OUT_DIR NAME [NAME...]
  egs_extract.py IMAGE_DIR OUT_DIR --english     # the English text and fonts
IMAGE_DIR is e.g. "...\\KINGDOM HEARTS -HD 1.5+2.5 ReMIX-\\Image\\dt" (a backup\\
folder with original archives, as mod managers leave, is preferred).
--english writes msg/us/<file> as msg/jp/<file> (what this game asks for) under
OUT_DIR, for the file overrides (docs/playing.md, "English text"). The PC
release's button prompts (icon codes 0xDA and up, drawn by the PC version
itself) are turned back into the PS2 font's icons, or words where the PS2 has
no icon (sticks, L3/R3), following the Japanese layout this game uses (circle
confirms, cross jumps).

Format (from OpenKH's OpenKh.Egs): a .hed is 0x20-byte entries (MD5 of the
file name, u64 offset in the .pkg, u32 stored length, u32 real length). At
that offset: a 16-byte header (decompressed length, remastered asset count,
compressed length: -2 stored, -1 not compressed, else deflate size; date)
that is also the key seed, the remastered assets' 0x30-byte headers, then the
data, whose first 0x100 bytes are encrypted; compressed data is zlib (skip 2
bytes, raw deflate).
"""
import hashlib, os, struct, sys, zlib

MASTER = bytes.fromhex(
    "7E8897550B06F108EBBB141CD87AEC41" "34B2A346EF6BFEE1CF53A50512D28E52"
    "4A80E981B0F0B49CFF0F1513DA734E77" "BED730E5F65A113767BC836F2776D0CD"
    "690D2E514290B8B64CADCE5B1A1FF5AF" "01F85E3A6E688BE89FC9D9269229C833"
    "983254D4442566AC5F9921E48F1DC2D5" "A462F90261DE59E7079AFA2F953F86D3"
    "78A775EDD62D6487BDC7C1AAF28C17CB" "318AC3CC04EE6AAB5C2270CA9E716D85"
    "455DB9A9A61047FB827D847BC6E238FC" "2B0E209DC5F339A8A06558437CE33618"
    "724979AED17440C4914F2463BFBA2396" "50B357DF1E03487F354D3EE6A1DD093C"
    "3D3B568D932A9B4B0C28B1E0608919DB" "2CF76CB51B94C0DCEAB70AF416FDA200")
SCRAMBLE = bytes([1, 0, 0, 0, 2, 0, 0, 0, 4, 0, 0, 0, 8, 0, 0, 0, 0x10, 0, 0, 0,
                  0x20, 0, 0, 0, 0x40, 0, 0, 0, 0x80, 0, 0, 0, 0x1B, 0, 0, 0, 0x36, 0, 0, 0])
PASSES = 10
ARCHIVES = ["kh2_first", "kh2_second", "kh2_third", "kh2_fourth", "kh2_fifth", "kh2_sixth"]
ENGLISH = ["al", "bb", "ca", "dc", "di", "eh", "es", "gumi", "hb", "he", "jm", "lk", "lm", "mu", "nm",
           "po", "sys", "title", "tr", "tt", "wi", "wm", "fontimage", "fontinfo"]


def key_for(seed):
    k = bytearray(0xB0)
    for i, b in enumerate(seed):
        k[i] = b if b else i
    for i in range(PASSES * 4):
        f = k[0x0C + i * 4:0x10 + i * 4]
        if i % 4 == 0:
            f = bytes([MASTER[f[1]] ^ SCRAMBLE[i], MASTER[f[2]] ^ SCRAMBLE[i + 1],
                       MASTER[f[3]] ^ SCRAMBLE[i + 2], MASTER[f[0]] ^ SCRAMBLE[i + 3]])
        for j in range(4):
            k[0x10 + i * 4 + j] = k[i * 4 + j] ^ f[j]
    return k


def decrypt(data, key):
    data = bytearray(data)
    for c in range(0, min(len(data), 0x100), 0x10):
        n = min(len(data) - c, 0x10)
        for p in range(PASSES, -1, -1):
            for j in range(n):
                data[c + j] ^= key[0x10 * p + j]
    return bytes(data)


def read_hed(path):
    d = open(path, "rb").read()
    out = {}
    for i in range(0, len(d) - 0x1F, 0x20):
        md5 = d[i:i + 16]
        off, stored, real = struct.unpack_from("<QII", d, i + 16)
        out[md5] = (off, stored, real)
    return out


def read_asset(pkg, off):
    pkg.seek(off)
    seed = pkg.read(16)
    dlen, rcount, clen, _ = struct.unpack("<iiii", seed)
    pkg.seek(off + 16 + rcount * 0x30)
    raw = pkg.read(clen if clen >= 0 else dlen)
    if clen > -2:
        raw = decrypt(raw, key_for(seed))
    if clen >= 0:
        return zlib.decompress(raw[2:], -15)[:dlen]
    return raw[:dlen]


# PC button icon -> PS2 icon (font "icon": 0x14 SELECT, 0x15 START, 0x16
# d-pad, 0x1A/0x1C up/down arrows, 0x1E R1, 0x1F R2, 0x20 L1, 0x21 L2,
# 0x22 triangle, 0x23 cross, 0x24 square, 0x25 circle) or text. Paired from
# messages that have icons in both languages; the PC's confirm/attack (0xDA)
# is circle and its jump/back (0xDB) cross, as in the Japanese game.
PC_ICONS = {0xDA: 0x25, 0xDB: 0x23, 0xDE: 0x24, 0xDF: 0x22, 0xE0: 0x20, 0xE1: 0x1E, 0xE2: 0x21,
            0xE3: 0x1F, 0xE6: 0x14, 0xE7: 0x15, 0xEB: 0x16, 0xEC: 0x16, 0xED: 0x16, 0xEF: 0x16,
            0xF0: 0x16, 0xF4: 0x16, 0xF5: 0x16, 0xF6: 0x1A, 0xF7: 0x1C}
PC_WORDS = {0xE4: "L3", 0xE5: "R3", 0xE8: "Left Stick", 0xE9: "Right Stick"}
# Bytes of data after each message command (OpenKh InternationalSystemDecode).
CMD_DATA = {0x04: 1, 0x05: 6, 0x06: 1, 0x07: 4, 0x08: 3, 0x09: 1, 0x0A: 1, 0x0B: 1, 0x0C: 1, 0x0E: 1,
            0x0F: 5, 0x11: 4, 0x12: 2, 0x13: 4, 0x14: 2, 0x15: 2, 0x16: 1, 0x17: 2, 0x18: 2,
            0x19: 1, 0x1A: 1, 0x1B: 1, 0x1C: 1, 0x1D: 1, 0x1E: 1, 0x1F: 1}


def encode_us(text):
    out = bytearray()
    for ch in text:
        if ch == " ":
            out.append(0x01)
        elif "A" <= ch <= "Z":
            out.append(0x2E + ord(ch) - ord("A"))
        elif "a" <= ch <= "z":
            out.append(0x9A + ord(ch) - ord("a"))
        elif "0" <= ch <= "9":
            out.append(0x90 + ord(ch) - ord("0"))
        else:
            raise ValueError(ch)
    return bytes(out)


def fix_message(data, start):
    """One message from start to its 0 terminator, with PC icons replaced."""
    out = bytearray()
    i = start
    while i < len(data) and data[i] != 0:
        c = data[i]
        n = CMD_DATA.get(c, 0)
        if c == 0x09 and i + 1 < len(data) and data[i + 1] in PC_WORDS:
            out += encode_us(PC_WORDS[data[i + 1]])
        elif c == 0x09 and i + 1 < len(data) and data[i + 1] in PC_ICONS:
            out += bytes([0x09, PC_ICONS[data[i + 1]]])
        else:
            out += data[i:i + 1 + n]
        i += 1 + n
    return bytes(out) + b"\0"


def fix_msg(m):
    count = struct.unpack_from("<I", m, 4)[0]
    entries = [struct.unpack_from("<II", m, 8 + i * 8) for i in range(count)]
    body = bytearray()
    placed = {}
    table = bytearray(struct.pack("<II", 1, count))
    base = 8 + count * 8
    for mid, off in entries:
        if off not in placed:
            placed[off] = base + len(body)
            body += fix_message(m, off)
        table += struct.pack("<II", mid, placed[off])
    return bytes(table + body)


def fix_bar(d):
    """Rewrite the message tables (type 2) of a .bar; other entries as they are."""
    count = struct.unpack_from("<I", d, 4)[0]
    entries = [list(struct.unpack_from("<HH4sII", d, 16 + i * 16)) for i in range(count)]
    datas = []
    for t, _, _, off, size in entries:
        datas.append(fix_msg(d[off:off + size]) if t == 2 else d[off:off + size])
    out = bytearray(d[:16])
    pos = 16 + count * 16
    blobs = bytearray()
    for e, blob in zip(entries, datas):
        e[3] = pos + len(blobs)
        e[4] = len(blob)
        blobs += blob
        blobs += b"\0" * (-len(blobs) % 16)
    for e in entries:
        out += struct.pack("<HH4sII", *e)
    return bytes(out + blobs)


def main():
    a = sys.argv[1:]
    if len(a) < 3:
        raise SystemExit(__doc__)
    image, out, names = a[0], a[1], a[2:]
    rename = {}
    if names == ["--english"]:
        names = []
        for n in ENGLISH:
            for ext in (".bar",):
                src = f"msg/us/{n}{ext}"
                names.append(src)
                rename[src] = f"msg/jp/{n}{ext}"
    archives = []
    for arc in ARCHIVES:
        base = os.path.join(image, "backup", arc)
        if not os.path.exists(base + ".pkg"):
            base = os.path.join(image, arc)
        if os.path.exists(base + ".hed") and os.path.exists(base + ".pkg"):
            archives.append((base, read_hed(base + ".hed")))
    if not archives:
        raise SystemExit(f"no kh2_*.hed/.pkg in {image}")
    found = 0
    for name in names:
        md5 = hashlib.md5(name.encode()).digest()
        for base, hed in archives:
            if md5 in hed:
                with open(base + ".pkg", "rb") as pkg:
                    data = read_asset(pkg, hed[md5][0])
                if name in rename and name.endswith(".bar") and "font" not in name:
                    data = fix_bar(data)
                dst = os.path.join(out, rename.get(name, name))
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                open(dst, "wb").write(data)
                print(f"{name}: {len(data)} bytes from {os.path.basename(base)} -> {dst}")
                found += 1
                break
        else:
            print(f"{name}: not found")
    print(f"{found} of {len(names)} files")


if __name__ == "__main__":
    main()
