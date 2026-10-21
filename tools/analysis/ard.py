#!/usr/bin/env python3
"""Room data (.ard) reader: which enemy groups a room has, which battle
programs spawn them, and what a group or a script program contains.

Extract the rooms first (game data, git-ignored):
  python tools/extract_file.py "Src/<iso>.iso" --world tt ard/tt04.ard ...
  python tools/extract_file.py "Src/<iso>.iso" 00objentry.bin

Usage:
  ard.py enemies [ROOM...]          enemy groups per room (default: all in game/files/ard)
  ard.py script ROOM map|btl|evt [PROGRAM...]
  ard.py group ROOM NAME            a spawn group's entities and trigger boxes
ROOM is the file stem, e.g. tt04. Formats: OpenKH docs, kh2/file/type/areadata.md.
"""
import glob, os, struct, sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
FILES = os.path.join(ROOT, "game", "files")
SPAWN, SCRIPT = 0x0C, 0x0D
OPS = {0: "Spawn", 1: "MapVisibility", 2: "RandomSpawn", 3: "CasualSpawn", 4: "Capacity", 5: "AllocEnemy",
       6: "Unk06", 7: "Unk07", 9: "SpawnAlt", 0xA: "MapScript", 0xB: "BarrierFlag", 0xC: "AreaSettings",
       0xE: "Unk0e", 0xF: "Party", 0x10: "Bgm", 0x11: "MsgWall", 0x12: "AllocPacket", 0x13: "Camera",
       0x14: "StatusFlag3", 0x15: "Mission", 0x16: "Layout", 0x17: "StatusFlag5", 0x18: "AllocEffect",
       0x19: "Progress", 0x1A: "VisibilityOn", 0x1B: "VisibilityOff", 0x1C: "If", 0x1D: "Unk1d",
       0x1E: "BattleLevel", 0x1F: "Unk1f"}


def objentries():
    d = open(os.path.join(FILES, "00objentry.bin"), "rb").read()
    out = {}
    for i in range(struct.unpack_from("<I", d, 4)[0]):
        e = 8 + i * 0x60
        out[struct.unpack_from("<I", d, e)[0]] = (d[e + 4], d[e + 8:e + 0x28].split(b"\0")[0].decode("latin1"))
    return out


def bar(d):
    if d[:4] != b"BAR\x01":
        raise ValueError("not a BAR file")
    out = []
    for i in range(struct.unpack_from("<I", d, 4)[0]):
        t, _, name, off, size = struct.unpack_from("<HH4sII", d, 16 + i * 16)
        out.append((t, name.rstrip(b"\0").decode("latin1"), d[off:off + size]))
    return out


def spawn_groups(d):
    """[(header fields, entities, activators)] of a spawn point file."""
    n = struct.unpack_from("<i", d, 4)[0]
    pos, out = 8, []
    for _ in range(n):
        ty, fl, did, ec, ac, wc, rc, sc = struct.unpack_from("<BBhhhhhi", d, pos)
        pos += 0x2C
        ents = []
        for _ in range(ec):
            e = d[pos:pos + 0x40]
            mid, x, y, z, rx, ry, rz = struct.unpack_from("<I6f", e)
            ents.append({"id": mid, "pos": (x, y, z), "rot": ry, "spawn": e[0x1C], "arg": e[0x1D],
                         "serial": struct.unpack_from("<h", e, 0x1E)[0], "level": e[0x32]})
            pos += 0x40
        acts = []
        for _ in range(ac):
            a = d[pos:pos + 0x40]
            p = struct.unpack_from("<9f", a, 4)
            acts.append({"pos": p[:3], "scale": p[3:6], "rot": p[6:9]})
            pos += 0x40
        for _ in range(wc):
            pc = struct.unpack_from("<h", d, pos + 2)[0]
            pos += 8 + pc * 12
        pos += rc * 0x10 + sc * 8
        out.append(({"type": ty, "flag": fl, "id": did}, ents, acts))
    return out


def programs(d):
    pos, out = 0, []
    while pos + 4 <= len(d):
        pid, ln = struct.unpack_from("<hh", d, pos)
        if (pid == -1 and ln == 0) or ln < 4:
            break
        out.append((pid, d[pos + 4:pos + ln]))
        pos += ln
    return out


def decode(code):
    pc, out = 0, []
    while pc + 4 <= len(code):
        op, n = struct.unpack_from("<HH", code, pc)
        args = code[pc + 4:pc + 4 + n * 4]
        if op in (0, 9):
            txt = '"' + args.rstrip(b"\0").decode("latin1") + '"'
        elif op == 2:
            txt = " ".join('"' + args[i:i + 4].rstrip(b"\0").decode("latin1") + '"' for i in range(0, len(args), 4))
        else:
            txt = " ".join(str(struct.unpack_from("<i", args, 4 * i)[0]) for i in range(n))
        out.append(f"{OPS.get(op, hex(op))} {txt}")
        pc += 4 + n * 4
    return out


def load(room):
    return bar(open(os.path.join(FILES, "ard", room + ".ard"), "rb").read())


def cmd_enemies(rooms):
    ent = objentries()
    rooms = rooms or sorted(os.path.basename(p)[:-4] for p in glob.glob(os.path.join(FILES, "ard", "*.ard")))
    for room in rooms:
        entries = load(room)
        groups = {}
        for t, name, data in entries:
            if t == SPAWN:
                try:
                    foes = [ent.get(e["id"], (0, "?"))[1] for _, es, _ in spawn_groups(data) for e in es
                            if ent.get(e["id"], (None,))[0] in (3, 4)]
                except struct.error:
                    continue
                if foes:
                    groups[name] = foes
        if not groups:
            continue
        print(f"{room}: " + ", ".join(f"{k} ({len(v)}: {' '.join(sorted(set(v)))})" for k, v in groups.items()))
        for t, name, data in entries:
            if t == SCRIPT and name == "btl":
                for pid, code in programs(data):
                    used = [k for k in groups if k.encode() in code]
                    if used:
                        print(f"  btl program {pid}: {' '.join(used)}")


def cmd_script(room, kind, wanted):
    for t, name, data in load(room):
        if t == SCRIPT and name == kind:
            for pid, code in programs(data):
                if wanted and pid not in wanted:
                    continue
                print(f"Program {pid}")
                for line in decode(code):
                    print("  " + line)


def cmd_group(room, group):
    ent = objentries()
    for t, name, data in load(room):
        if t == SPAWN and name == group:
            for hdr, es, acts in spawn_groups(data):
                print(f"group type {hdr['type']} id {hdr['id']}")
                for e in es:
                    x, y, z = e["pos"]
                    print(f"  {ent.get(e['id'], (0, '?'))[1]:14s} {x:7.0f},{y:6.0f},{z:7.0f} spawn {e['spawn']}/{e['arg']}"
                          f" serial {e['serial']} level {e['level']}")
                for a in acts:
                    print("  trigger box at", [round(v) for v in a["pos"]], "half-size",
                          [round(v) for v in a["scale"]])


def main():
    a = sys.argv[1:]
    if not a:
        raise SystemExit(__doc__)
    if a[0] == "enemies":
        cmd_enemies(a[1:])
    elif a[0] == "script" and len(a) >= 3:
        cmd_script(a[1], a[2], [int(x) for x in a[3:]])
    elif a[0] == "group" and len(a) == 3:
        cmd_group(a[1], a[2])
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
