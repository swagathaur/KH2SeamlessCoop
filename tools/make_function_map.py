#!/usr/bin/env python3
"""Build the PS2Recomp function map (Ghidra CSV format) for SLPM_666.75.

Sources, in priority order for naming:
  1. SDK names found by ps2_analyzer (config/analyzer/analyzer.toml), so library
     calls bind to PS2Recomp runtime handlers by name
  2. Real names from the KH2 decomp's layout.tsv (kernel syscall wrappers etc.)
  3. Names recovered by the decomp in functions.tsv (mangled C++ where known)
  4. Ghidra's FUN_xxxxxxxx placeholder

Function boundaries come from the decomp's layout.tsv (a Ghidra export of this ELF).

Gap filling: Ghidra's map leaves some reachable code outside any function
(static constructors called only through .ctors, functions reached only via
pointers). Every .ctors target, and every `addiu $sp,$sp,-N` prologue lying in
a gap between known functions, becomes a function that runs to the next one.

Functions Ghidra merged across a `jr $ra` are split when no branch connects
the two parts (branchlib.split_after_returns); the later part is reached only
through a pointer.

Overlays call into the main executable too: every J/JAL target in an overlay
that lands in the main ELF must also be a function start there.

Usage: make_function_map.py <decomp_dir> <out_csv> [analyzer_toml] [elf] [overlay ...]
       (overlay names read game/overlays/ovl_<name>.x and config/functions_<name>.csv)
"""
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from branchlib import ends_flow, extend_into_gaps, split_after_returns  # noqa: E402

IDENT = re.compile(r"[^A-Za-z0-9_]")


def clean(name: str) -> str:
    """Turn a demangled or decorated name into a CSV- and C++-safe identifier."""
    name = name.split("(", 1)[0].strip()      # drop parameter lists
    name = name.replace("::", "__")
    name = IDENT.sub("_", name)
    if not name or name[0].isdigit():
        name = "_" + name
    return name


def read_layout(path: Path):
    funcs = {}
    for line in path.read_text().splitlines():
        if not line or line.startswith("#") or line.startswith("addr"):
            continue
        start, end, size, name = line.split("\t")[:4]
        funcs[int(start, 16)] = {"end": int(end, 16), "name": name}
    return funcs


def read_decomp_names(path: Path):
    names = {}
    for line in path.read_text().splitlines():
        if not line or line.startswith("#"):
            continue
        parts = line.rsplit("\t", 1)
        if len(parts) != 2:
            continue
        sym, addr = parts
        if sym.startswith("func_"):
            continue  # placeholder, no information
        names[int(addr, 16)] = sym
    return names


def read_analyzer_names(path: Path):
    """Collect name@0xADDR bindings from the analyzer's `stubs` list only.

    Its `entry_points` list is not used: it gives several unrelated functions
    the same name (strtol x5, _InitSys x4), so those labels are unreliable.
    """
    names = {}
    if not path or not path.exists():
        return names
    m = re.search(r"^stubs = \[(.*?)^\]", path.read_text(), re.S | re.M)
    for m in re.finditer(r'"([A-Za-z_][A-Za-z0-9_]*)@(0x[0-9A-Fa-f]+)"', m.group(1) if m else ""):
        name, addr = m.group(1), int(m.group(2), 16)
        if name in ("ret0", "ret1", "reta0"):
            continue
        names.setdefault(addr, name)
    return names


ELF_TEXT_VA, ELF_TEXT_OFF, ELF_TEXT_SIZE = 0x00100000, 0x1000, 0x27A4B4
# The decomp's map is dense up to the end of the SDK libraries; past that the
# section is data with ten scattered functions (crt/end-of-image code), and
# data there can look like code. Nothing is seeded past this address.
SEED_LIMIT = 0x00325BCC  # end of the last decomp function before the data area
CTORS_OFF, CTORS_SIZE = 0x27B4B4, 0x234


def read_analyzer_entry_points(path: Path):
    """name@addr pairs from the analyzer's entry_points list (signature matches)."""
    if not path or not path.exists():
        return {}
    m = re.search(r"^(?:entry_points|untracked_stubs) = \[(.*?)^\]", path.read_text(), re.S | re.M)
    out = {}
    for n, a in re.findall(r'"([A-Za-z_][A-Za-z0-9_]*)@(0x[0-9A-Fa-f]+)"', m.group(1) if m else ""):
        out.setdefault(int(a, 16), n)
    return out


def read_runtime_stub_names(path: Path):
    """Names in the runtime's PS2_STUB_LIST (ps2_recomp stubs these by name)."""
    if not path.exists():
        return set()
    m = re.search(r"#define PS2_STUB_LIST\(X\)(.*?)(?:\n\s*\n|#define)", path.read_text(encoding="utf-8"), re.S)
    return set(re.findall(r"X\((\w+)\)", m.group(1))) if m else set()


def is_runtime_stub_name(name: str, stub_names) -> bool:
    # ps2_recomp also matches with one leading underscore stripped
    return name in stub_names or (name.startswith("_") and name[1:] in stub_names)


def is_branch(w: int) -> bool:
    """MIPS instructions with a delay slot: J/JAL, JR/JALR, branches (incl. likely)."""
    op = w >> 26
    if op in (2, 3) or 4 <= op <= 7 or 0x14 <= op <= 0x17:
        return True
    if op == 0 and (w & 0x3F) in (0x08, 0x09):          # jr, jalr
        return True
    if op == 1 and ((w >> 16) & 0x1F) in (0, 1, 2, 3, 0x10, 0x11, 0x12, 0x13):  # REGIMM branches
        return True
    if op == 0x11 and ((w >> 21) & 0x1F) == 8:          # bc1*
        return True
    return False


def fill_gaps(layout, elf_path: Path, entry_points=None):
    """Add functions for .ctors targets, analyzer entry points and prologues in unmapped gaps.

    Entry points matter for kernel-mode helpers such as kFindAddress/kCopy:
    the SDK installs them with SetSyscall, and the runtime fails the syscall
    if no translated function exists at the handler address.
    """
    elf = elf_path.read_bytes()
    text = elf[ELF_TEXT_OFF:ELF_TEXT_OFF + ELF_TEXT_SIZE]
    word = lambda va: struct.unpack_from("<I", text, va - ELF_TEXT_VA)[0]
    starts = sorted(layout)
    covered_end = {s: layout[s]["end"] for s in starts}

    def in_function(va):
        import bisect
        i = bisect.bisect_right(starts, va) - 1
        return i >= 0 and starts[i] <= va < covered_end[starts[i]]

    seeds = set()
    ctors = struct.unpack_from(f"<{CTORS_SIZE // 4}I", elf, CTORS_OFF)
    for t in ctors:
        if ELF_TEXT_VA <= t < ELF_TEXT_VA + ELF_TEXT_SIZE and t not in layout and not in_function(t):
            seeds.add(t)
    # Direct J/JAL targets from code inside known functions that land outside
    # every function: leaf functions without a prologue (e.g. an empty
    # `jr $ra` reached only by a tail jump at 0x002F1284).
    for a in starts:
        for va in range(a, covered_end[a], 4):
            w = word(va)
            if w >> 26 in (2, 3):
                t = (va & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
                if ELF_TEXT_VA <= t < SEED_LIMIT and t not in layout and not in_function(t):
                    seeds.add(t)
    named = {}
    for t, n in (entry_points or {}).items():
        if ELF_TEXT_VA <= t < SEED_LIMIT and t not in layout and not in_function(t):
            seeds.add(t)
            named[t] = f"{n}_{t:08x}"
    code_end = min(max(covered_end.values()), SEED_LIMIT)
    for a, b in zip(starts, starts[1:]):
        va = covered_end[a]
        while va < b and va < code_end:
            w = word(va)
            if w >> 16 == 0x27BD and (w & 0xFFFF) >= 0x8000:
                seeds.add(va)
            va += 4

    added = 0
    for seed in sorted(seeds):
        nxt = min([s for s in starts if s > seed] + [s for s in seeds if s > seed] + [code_end])
        end = nxt
        while end > seed + 4 and word(end - 4) == 0:   # trim alignment padding
            end -= 4
        if is_branch(word(end - 4)) and end < nxt:     # but keep the delay slot (often a nop)
            end += 4
        layout[seed] = {"end": end, "name": named.get(seed, f"FUN_{seed:08x}")}
        added += 1
    return added


OVERLAY_BASE = 0x01F18000


def overlay_targets(names):
    """J/JAL targets from overlay function bodies that land in the main ELF."""
    root = Path(__file__).resolve().parent.parent
    out = set()
    for name in names:
        image = (root / "game" / "overlays" / f"ovl_{name}.x").read_bytes()
        for line in (root / "config" / f"functions_{name}.csv").read_text().splitlines()[1:]:
            _, start, end, _ = line.split(",")
            for va in range(int(start, 16), int(end, 16), 4):
                w = struct.unpack_from("<I", image, va - OVERLAY_BASE)[0]
                if w >> 26 in (2, 3):
                    t = (va & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
                    if ELF_TEXT_VA <= t < ELF_TEXT_VA + ELF_TEXT_SIZE:
                        out.add(t)
    return out


def extend_main_gaps(layout, elf_path: Path) -> int:
    text = elf_path.read_bytes()[ELF_TEXT_OFF:ELF_TEXT_OFF + ELF_TEXT_SIZE]
    word = lambda va: struct.unpack_from("<I", text, va - ELF_TEXT_VA)[0]
    funcs = {s: [v["name"], v["end"]] for s, v in layout.items()}
    n = extend_into_gaps(funcs, word, ELF_TEXT_VA, SEED_LIMIT)
    for s, (_, e) in funcs.items():
        layout[s]["end"] = e
    return n


def split_main_returns(layout, elf_path: Path) -> int:
    text = elf_path.read_bytes()[ELF_TEXT_OFF:ELF_TEXT_OFF + ELF_TEXT_SIZE]
    word = lambda va: struct.unpack_from("<I", text, va - ELF_TEXT_VA)[0]
    funcs = {s: [v["name"], v["end"]] for s, v in layout.items()}
    n = split_after_returns(funcs, word)
    for s, (name, e) in funcs.items():
        if s in layout:
            layout[s]["end"] = e
        else:
            layout[s] = {"end": e, "name": name}
    return n


def split_at_calls(layout, elf_path: Path, external=frozenset()):
    """JAL targets (and any branch from another binary) must be function starts:
    split a function such a branch lands inside, or seed one in a gap."""
    elf = elf_path.read_bytes()
    text = elf[ELF_TEXT_OFF:ELF_TEXT_OFF + ELF_TEXT_SIZE]
    import bisect
    starts = sorted(layout)
    targets = set(external)
    for a in starts:
        for va in range(a, layout[a]["end"], 4):
            w = struct.unpack_from("<I", text, va - ELF_TEXT_VA)[0]
            if w >> 26 == 3:
                targets.add((va & 0xF0000000) | ((w & 0x03FFFFFF) << 2))
    split = 0
    for t in sorted(targets):
        if t in layout or not (ELF_TEXT_VA <= t < SEED_LIMIT):
            continue
        i = bisect.bisect_right(starts, t) - 1
        if i < 0:
            continue
        f = starts[i]
        if f < t < layout[f]["end"]:
            layout[t] = {"end": layout[f]["end"], "name": f"FUN_{t:08x}"}
            layout[f]["end"] = t
            starts.insert(i + 1, t)
            split += 1
        elif t >= layout[f]["end"] and t in external:
            nxt = starts[i + 1] if i + 1 < len(starts) else ELF_TEXT_VA + ELF_TEXT_SIZE
            end = nxt
            while end > t + 4 and struct.unpack_from("<I", text, end - 4 - ELF_TEXT_VA)[0] == 0:
                end -= 4
            layout[t] = {"end": end, "name": f"FUN_{t:08x}"}
            starts.insert(i + 1, t)
            split += 1
    return split


def pointer_targets(layout, elf_path: Path):
    """Code addresses stored in data (vtables, callback tables, function pointers).

    Scans every word of the loaded image that lies outside known function
    bodies. A target in an unmapped gap is taken; a target inside a function is
    taken only if it starts a stack frame, because switch jump tables also live
    in data and point into the middle of functions.
    """
    import bisect
    elf = elf_path.read_bytes()
    text = elf[ELF_TEXT_OFF:ELF_TEXT_OFF + ELF_TEXT_SIZE]
    starts = sorted(layout)
    ends = [layout[s]["end"] for s in starts]
    code_end = min(max(ends), SEED_LIMIT)

    def owner(a):
        i = bisect.bisect_right(starts, a) - 1
        return i if i >= 0 and a < ends[i] else None

    candidates = set()
    from_code = set()  # built with lui + addiu/ori: never a switch-table case label
    for off in range(0, ELF_TEXT_SIZE - 3, 4):
        va = ELF_TEXT_VA + off
        if va < code_end and owner(va) is not None:
            continue  # inside code
        candidates.add(struct.unpack_from("<I", text, off)[0])
    # Addresses built in code with lui + addiu/ori (callbacks passed as
    # arguments, e.g. qsort comparators at 0x00320448).
    for s0 in starts:
        e0 = layout[s0]["end"]
        for va in range(s0, e0, 4):
            w = struct.unpack_from("<I", text, va - ELF_TEXT_VA)[0]
            if w >> 26 != 0x0F:
                continue
            rt, hi = (w >> 16) & 31, w & 0xFFFF
            for k in range(1, 9):
                if va + 4 * k >= e0:
                    break
                w2 = struct.unpack_from("<I", text, va + 4 * k - ELF_TEXT_VA)[0]
                op2 = w2 >> 26
                if op2 in (0x09, 0x0D) and (w2 >> 21) & 31 == rt:
                    lo = w2 & 0xFFFF
                    if op2 == 0x09 and lo & 0x8000:
                        lo -= 0x10000
                    candidates.add(((hi << 16) + lo) & 0xFFFFFFFF)
                    from_code.add(((hi << 16) + lo) & 0xFFFFFFFF)
                    break
    found = set()
    for t in candidates:
        if not (ELF_TEXT_VA <= t < code_end) or t & 3 or t in layout:
            continue
        w = struct.unpack_from("<I", text, t - ELF_TEXT_VA)[0]
        if w == 0:
            continue
        i = owner(t)
        after_return = t - 8 >= ELF_TEXT_VA and \
            struct.unpack_from("<I", text, t - 8 - ELF_TEXT_VA)[0] == 0x03E00008  # jr $ra; delay
        if w >> 16 == 0x27BD and (w & 0xFFFF) >= 0x8000:
            found.add(t)            # stack-frame prologue: a real function start
        elif after_return:
            found.add(t)            # nothing falls through into it
        elif t in from_code and t - 8 >= ELF_TEXT_VA and \
                ends_flow(struct.unpack_from("<I", text, t - 8 - ELF_TEXT_VA)[0]):
            found.add(t)            # callback address after a j/b (0x001AFE18)
        elif i is None:
            # In a gap: only if it directly follows a known function (after zero
            # padding), as compiled functions are packed; data blobs that merely
            # contain code-like values are not.
            p = t - 4
            while p >= ELF_TEXT_VA and owner(p) is None and \
                    struct.unpack_from("<I", text, p - ELF_TEXT_VA)[0] == 0:
                p -= 4
            if owner(p) is not None:
                found.add(t)
    return found


def main():
    decomp = Path(sys.argv[1])
    out = Path(sys.argv[2])
    analyzer = Path(sys.argv[3]) if len(sys.argv) > 3 else None
    elf = Path(sys.argv[4]) if len(sys.argv) > 4 else None
    external = overlay_targets(sys.argv[5:])

    layout = read_layout(decomp / "layout.tsv")
    if elf:
        eps = read_analyzer_entry_points(analyzer)
        total, rounds = 0, 0
        while True:   # seeded functions can reveal further targets
            # Re-scanned each round: code that gap filling turns into a
            # function can build further callback addresses (0x001AFE18).
            ptrs = pointer_targets(layout, elf)
            external |= ptrs
            added = fill_gaps(layout, elf, eps) + split_at_calls(layout, elf, external)
            added += extend_main_gaps(layout, elf)
            added += split_main_returns(layout, elf)
            total, rounds = total + added, rounds + 1
            if added == 0 or rounds > 20:
                break
        print(f"gap filling added {total} functions in {rounds} rounds ({len(external)} external/pointer targets)")
    decomp_names = read_decomp_names(decomp / "functions.tsv")
    stub_names = read_runtime_stub_names(
        Path(__file__).resolve().parent.parent / "third_party/PS2Recomp/ps2xRuntime/include/ps2_call_list.h")
    sdk_names = read_analyzer_names(analyzer)

    used = {}
    rows = []
    counts = {"sdk": 0, "layout": 0, "decomp": 0, "auto": 0}
    for start in sorted(layout):
        f = layout[start]
        if start in sdk_names:
            name, src = sdk_names[start], "sdk"
        elif not f["name"].startswith(("FUN_", "thunk_FUN_")):
            name, src = clean(f["name"]), "layout"
        elif start in decomp_names:
            name, src = clean(decomp_names[start]), "decomp"
            if is_runtime_stub_name(name, stub_names):
                # ps2_recomp binds any function with a runtime stub's name to
                # that stub. The decomp's recovered names are guesses: it calls
                # the two-instruction task-retire helper at 0x001024A8
                # "printf", which made KH2 run AREA::start twice.
                name = f"{name}_{start:08x}"
        else:
            name, src = f"FUN_{start:08x}", "auto"
        if name in used and src != "auto":
            name = f"{name}_{start:08x}"   # keep names unique
        used[name] = start
        counts[src] += 1
        rows.append((name, start, f["end"]))

    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open("w", newline="\n") as fh:
        fh.write("Name,Start,End,Size\n")
        for name, start, end in rows:
            fh.write(f"{name},0x{start:08X},0x{end:08X},{end - start}\n")

    print(f"wrote {len(rows)} functions to {out}")
    for k, v in counts.items():
        print(f"  {k:7} {v}")


if __name__ == "__main__":
    main()
