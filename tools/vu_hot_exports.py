"""Write a .def exporting VU1Interpreter's member functions from kh2recomp.exe.

VU1 programs compiled while the game runs (port/runtime/vu_hot.cpp) are a DLL
linked against the game's import library; their code calls members of the
runtime's VU1Interpreter. Marking that class for export in its header would
change a header every translated function includes (a full rebuild), so the
members are found in the runtime's object files instead (link /DUMP /SYMBOLS)
and listed here, run just before the game is linked.

    python tools/vu_hot_exports.py <link.exe> <ps2_runtime object dir> <out.def>
"""
import re
import subprocess
import sys
from pathlib import Path


def main():
    linker, objdir, out = sys.argv[1], Path(sys.argv[2]), Path(sys.argv[3])
    objs = sorted(str(p) for p in objdir.rglob("*.obj") if "vu" in p.parent.name.lower() or "vu" in p.name.lower())
    names = set()
    for i in range(0, len(objs), 40):
        result = subprocess.run([linker, "/DUMP", "/SYMBOLS", *objs[i:i + 40]], capture_output=True, text=True,
                                errors="replace")
        for line in result.stdout.splitlines():
            # "... SECT3  notype ()    External     | ?name@VU1Interpreter@@..."
            if "External" not in line or "UNDEF" in line or "()" not in line:
                continue
            m = re.search(r"\|\s+(\?\S*@VU1Interpreter@@\S*)", line)
            if m:
                names.add(m.group(1))
    text = "EXPORTS\n" + "".join(f"    {n}\n" for n in sorted(names))
    if not out.exists() or out.read_text() != text:
        out.write_text(text)
    print(f"vu_hot_exports: {len(names)} VU1Interpreter members exported")


if __name__ == "__main__":
    main()
