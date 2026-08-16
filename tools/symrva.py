#!/usr/bin/env python3
"""symrva.py -- resolve host RVAs (as printed by the [WATCHDOG]/[exit-chain]
diagnostics) to symbols, using the linker .map. Usage:

    grep -a 'ret rva=' run.log | python tools/symrva.py build/rubberducky.map
    python tools/symrva.py build/rubberducky.map 0x1574C10 0x229DB70
"""
import bisect, re, sys

def load(mapfile):
    rows, seen = [], False
    for line in open(mapfile, encoding="utf-8", errors="replace"):
        if "Publics by Value" in line:
            seen = True; continue
        if not seen: continue
        m = re.match(r"\s+[0-9a-fA-F]{4}:[0-9a-fA-F]+\s+(\S+)\s+([0-9a-fA-F]{16})\s", line)
        if m:
            rows.append((int(m.group(2), 16), m.group(1)))
    rows.sort()
    return rows

def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    rows = load(sys.argv[1])
    if not rows:
        sys.exit("no symbols parsed from the map")
    base = rows[0][0] & ~0xFFFF          # image base from the lowest Rva+Base
    base = 0x140000000                    # /BASE for a Windows x64 exe
    addrs = [r[0] for r in rows]
    def who(rva):
        va = base + rva
        i = bisect.bisect_right(addrs, va) - 1
        if i < 0: return "??"
        a, n = rows[i]
        return f"{n}+0x{va-a:X}" if va != a else n
    args = sys.argv[2:]
    vals = [int(a, 16) for a in args] if args else \
           [int(m, 16) for m in re.findall(r"rva=0x([0-9A-Fa-f]+)", sys.stdin.read())]
    for v in vals:
        print(f"0x{v:<10X} {who(v)}")

main()
