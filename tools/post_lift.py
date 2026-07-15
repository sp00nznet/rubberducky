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
    # memset with a watchpoint on the jsGcmFifo (src/rd_malloc.cpp)
    "00391E30": ("rd_hle_memset",         "memset"),
}


# ---------------------------------------------------------------------------
# Nonvolatile-register / caller-frame preservation around the PSGL device and
# context creators.
#
# The recompiler's frameless-cascade model lets a deeply-nested callee store
# above its own frame and clobber an ancestor's saved-register slot / locals.
# During PSGL bring-up this corrupts r31 (frame pointer) across
# _jsPlatformCreateDevice (func_0013DB20) and clobbers the caller's saved device
# pointer at [r31+0x14] across psglCreateContext (func_000359C4) — so
# psglCreateDeviceExtended returns a null device and psglMakeCurrent gets a null
# device, leaving LContext NULL and aborting the demo.
#
# These two wraps snapshot the affected nonvolatiles (and the one clobbered
# stack local) around those calls and restore them after — ABI-correct, since
# callees must preserve r14-r31. Targeted mitigations for the systemic bug; the
# real fix is frame isolation in the lifter. Each is a unique call site.
REG_PRESERVE_PATCHES = [
    # (find, replace, tag) — applied once each, idempotent via the tag check.
    (
        "        ctx->gpr[3] = ctx->gpr[0] | ctx->gpr[0];\n"
        "        func_0013DB20(ctx); DRAIN_TRAMPOLINE(ctx);\n"
        "        /* nop */;\n",
        "        ctx->gpr[3] = ctx->gpr[0] | ctx->gpr[0];\n"
        "        /* rd-regfix: preserve nonvolatiles across _jsPlatformCreateDevice */\n"
        "        { uint64_t _r28=ctx->gpr[28],_r29=ctx->gpr[29],_r31=ctx->gpr[31];\n"
        "          func_0013DB20(ctx); DRAIN_TRAMPOLINE(ctx);\n"
        "          ctx->gpr[28]=_r28; ctx->gpr[29]=_r29; ctx->gpr[31]=_r31; }\n"
        "        /* nop */;\n",
        "rd-regfix: preserve nonvolatiles across _jsPlatformCreateDevice",
    ),
    (
        "        vm_write32(ctx->gpr[31] + 0x14, ctx->gpr[3]);\n"
        "        func_000359C4(ctx); DRAIN_TRAMPOLINE(ctx);\n",
        "        vm_write32(ctx->gpr[31] + 0x14, ctx->gpr[3]);\n"
        "        /* rd-regfix: preserve nonvolatiles + device local across psglCreateContext */\n"
        "        { uint64_t _s31=ctx->gpr[31],_s30=ctx->gpr[30],_s29=ctx->gpr[29],_s28=ctx->gpr[28];\n"
        "          uint32_t _dev=vm_read32(ctx->gpr[31]+0x14);\n"
        "          func_000359C4(ctx); DRAIN_TRAMPOLINE(ctx);\n"
        "          ctx->gpr[31]=_s31; ctx->gpr[30]=_s30; ctx->gpr[29]=_s29; ctx->gpr[28]=_s28;\n"
        "          vm_write32(ctx->gpr[31]+0x14,_dev); }\n",
        "rd-regfix: preserve nonvolatiles + device local across psglCreateContext",
    ),
]


def apply_reg_preserve(path):
    with open(path, "r", encoding="utf-8", errors="surrogateescape") as f:
        txt = f.read()
    n = 0
    for find, repl, tag in REG_PRESERVE_PATCHES:
        if tag in txt:
            n += 1; continue                                 # already applied
        if find in txt:
            txt = txt.replace(find, repl, 1); n += 1
    with open(path, "w", encoding="utf-8", errors="surrogateescape") as f:
        f.write(txt)
    return n


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
    reg_applied = 0
    for path in glob.glob(os.path.join(RECOMP_DIR, "ppu_recomp_*.cpp")):
        patch_file(path)
        reg_applied += apply_reg_preserve(path)
        with open(path, "r", encoding="utf-8", errors="surrogateescape") as f:
            txt = f.read()
        for addr in ALLOC_PATCHES:
            if f"func_{addr}(ppu_context* ctx) {{\n    /* rd-patched" in txt:
                present += 1
    want = len(ALLOC_PATCHES)
    want_reg = len(REG_PRESERVE_PATCHES)
    print(f"post_lift: {present}/{want} target functions patched "
          f"(5 dlmalloc allocators + 2 raw-SPU MMIO), "
          f"{reg_applied}/{want_reg} reg-preserve wraps")
    if present != want or reg_applied != want_reg:
        print("  WARNING: a target wasn't found; re-lift may have changed addresses",
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
