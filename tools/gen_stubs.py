#!/usr/bin/env python3
"""Turn ps2_analyzer's stub proposals into a vetted [general].stubs list.

Only names the runtime actually implements as stubs are emitted; everything
else is reported so it can be handled deliberately. DO_NOT_STUB records names
the runtime implements but which KH2 must keep as guest code.

Adapted from DQ8Recomp's tools/oracle/gen_stubs.py (GPL-3.0).

Usage: gen_stubs.py <analyzer.toml> <ps2_call_list.h>  > stubs.toml.fragment
"""
import re
import sys

DO_NOT_STUB = {
    # The C allocator must have exactly one owner; keep the guest's newlib heap.
    "malloc": "guest owns the C heap",
    "_malloc_r": "guest owns the C heap",
    "calloc": "guest owns the C heap",
    "_calloc_r": "guest owns the C heap",
    "memalign": "same arena as guest malloc",
    "free": "same arena as guest malloc",
    "_free_r": "same arena as guest malloc",
    "realloc": "same arena as guest malloc",
    "malloc_extend_top": "newlib arena growth; the runtime's version always fails",
    "_malloc_trim_r": "newlib arena internals",
    "__malloc_lock": "guest semaphore",
    "__malloc_unlock": "pairs with __malloc_lock",
}


# Public SDK wrappers ps2_analyzer does not recognise (it matched only the
# internal entries, whose ABI differs). Found from their callers in KH2:
#   0x00167358 builds "cdrom0:\\<NAME>" and retries sceSifLoadModule until it
#              succeeds, so an unbound wrapper loops forever on SIF RPC.
#   0x001672C8 DMAs an IRX image to the IOP, then sceSifLoadModuleBuffer.
#   sceSifCallRpc is in the runtime's syscall list, so it is reported, not
#   accepted; DQ8Recomp binds it by hand too, so RPCs go through the runtime's
#   IOP model instead of the guest library's raw SIF DMA.
MANUAL_STUBS = [
    ("sceSifLoadModule", "0x0030E770"),        # wraps _sceSifLoadModule (0x0030E548)
    ("sceSifLoadModuleBuffer", "0x0030E508"),  # wraps _sceSifLoadModuleBuffer (0x0030DF38)
    ("sceSifCallRpc", "0x003094B8"),
    # Public wrapper of sceCdLayerSearchFile (0x002F6CA0, layer 0), called on the
    # boot path by FUN_001AE590. Unbound, the guest runs the real async CD
    # command and waits forever (DQ8Recomp hit the same).
    ("sceCdSearchFile", "0x002F6FA0"),
]


def macro_names(path, macro):
    out, active = set(), False
    for line in open(path, encoding="utf-8"):
        if f"#define {macro}" in line:
            active = True
        if active:
            out.update(re.findall(r"X\(([A-Za-z_][A-Za-z0-9_]*)\)", line))
            if not line.rstrip().endswith("\\"):
                break
    return out


def proposed_stubs(path):
    out, in_list = [], False
    for line in open(path, encoding="utf-8"):
        if line.startswith("stubs = ["):
            in_list = True
            continue
        if in_list:
            if line.startswith("]"):
                break
            m = re.search(r'"([^"@]+)@(0x[0-9A-Fa-f]+)"', line)
            if m:
                out.append((m.group(1), m.group(2)))
    return out


def main():
    analyzer_toml, call_list = sys.argv[1], sys.argv[2]
    stubs = macro_names(call_list, "PS2_STUB_LIST")
    syscalls = macro_names(call_list, "PS2_SYSCALL_LIST")
    accepted, as_syscall, unknown, kept = [], [], [], []
    for name, addr in proposed_stubs(analyzer_toml):
        if name in DO_NOT_STUB:
            kept.append((name, addr))
        elif name in stubs:
            accepted.append((name, addr))
        elif name in syscalls:
            as_syscall.append((name, addr))
        else:
            unknown.append((name, addr))

    log = sys.stderr.write
    log(f"accepted={len(accepted)} kept-guest={len(kept)} syscall-list={len(as_syscall)} unknown={len(unknown)}\n")
    for n, a in kept:
        log(f"  kept guest  : {n}@{a}  ({DO_NOT_STUB[n]})\n")
    for n, a in as_syscall:
        log(f"  syscall-list: {n}@{a}\n")
    for n, a in unknown:
        log(f"  unknown     : {n}@{a}\n")

    for n, a in MANUAL_STUBS:
        if n not in stubs and n not in syscalls:
            raise SystemExit(f"manual stub {n} has no runtime handler")
        accepted.append((n, a))
        log(f"  manual      : {n}@{a}\n")
    print("stubs = [")
    for n, a in accepted:
        print(f'    "{n}@{a}",')
    print("]")


if __name__ == "__main__":
    main()
