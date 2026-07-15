#!/usr/bin/env python3
"""Post-lift patches for the Rubber Ducky recompilation.

Run once after ppu_lifter.py, before building. Idempotent: re-running on an
already-patched tree is a no-op. Regenerate-friendly (the lifted src/recomp is
gitignored and rebuilt from the ELF).

Currently patches: the five dlmalloc public wrappers -> rd_hle_* bump allocator
(src/rd_malloc.cpp). See that file for why. The lifter emits each function as

    void func_XXXXXXXX(ppu_context* ctx) {
        ...body...
    }                                        <- a lone "}" at column 0

so we replace the whole span [signature line .. next lone "}"] with a one-line
redirect. This is robust to the body's own nested braces (which are indented).
"""
import glob, os, re, sys

RECOMP_DIR = os.path.join(os.path.dirname(__file__), "..", "src", "recomp")

# guest addr -> (override symbol, human name). Redirect the lifted wrapper body
# to a host HLE implementation. See src/rd_malloc.cpp and src/rd_spu.cpp.
ALLOC_PATCHES = {
    # dlmalloc public allocators -> bump allocator (src/rd_malloc.cpp)
    "00396DE8": ("rd_hle_malloc",         "malloc"),
    "00396DBC": ("rd_hle_free",           "free"),
    "00396D84": ("rd_hle_calloc",         "calloc"),
    "00396D4C": ("rd_hle_realloc",        "realloc"),
    "00396D14": ("rd_hle_memalign",       "memalign"),
    # raw-SPU MFC proxy DMA -> synchronous memcpy + tag-complete (src/rd_spu.cpp)
    "003A7CAC": ("rd_hle_spu_mmio_write", "sys_raw_spu_mmio_write"),
    "003A7D00": ("rd_hle_spu_mmio_read",  "sys_raw_spu_mmio_read"),
}


def patch_file(path):
    with open(path, "r", encoding="utf-8", errors="surrogateescape") as f:
        lines = f.readlines()
    out, i, n_patched = [], 0, 0
    while i < len(lines):
        line = lines[i]
        m = re.match(r"void func_([0-9A-F]{8})\(ppu_context\* ctx\) \{\s*$", line)
        if m and m.group(1) in ALLOC_PATCHES:
            addr = m.group(1)
            sym, name = ALLOC_PATCHES[addr]
            if "/* rd-patched" in (lines[i + 1] if i + 1 < len(lines) else ""):
                out.append(line); i += 1; continue          # already patched
            # find the closing lone "}" that ends this function
            j = i + 1
            while j < len(lines) and lines[j].rstrip("\n") != "}":
                j += 1
            out.append(f"void func_{addr}(ppu_context* ctx) {{\n")
            out.append(f"    /* rd-patched: {name} -> {sym} (host HLE) */\n")
            out.append(f"    extern void {sym}(ppu_context* ctx); {sym}(ctx);\n")
            out.append("}\n")
            n_patched += 1
            i = j + 1
            continue
        out.append(line); i += 1
    if n_patched:
        with open(path, "w", encoding="utf-8", errors="surrogateescape") as f:
            f.writelines(out)
    return n_patched


def main():
    # Count functions that end up patched (idempotent: already-patched count too).
    present = 0
    for path in glob.glob(os.path.join(RECOMP_DIR, "ppu_recomp_*.cpp")):
        patch_file(path)
        with open(path, "r", encoding="utf-8", errors="surrogateescape") as f:
            txt = f.read()
        for addr in ALLOC_PATCHES:
            if f"func_{addr}(ppu_context* ctx) {{\n    /* rd-patched" in txt:
                present += 1
    want = len(ALLOC_PATCHES)
    print(f"post_lift: {present}/{want} target functions patched "
          f"(5 dlmalloc allocators + 2 raw-SPU MMIO)")
    if present != want:
        print("  WARNING: a target wasn't found; re-lift may have changed addresses",
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
