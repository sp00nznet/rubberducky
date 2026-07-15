# Rubber Ducky Tech Demo — Static Recompilation

> A native PC port of Sony's *Bigduck* E3 2006 tech demo, rebuilt from the PS3 binary through static recompilation.

The **Rubber Ducky** (internally *Bigduck*) demo was one of the first pieces of PS3 tech Sony showed publicly — a bathtub full of rubber ducks and toy boats bobbing on GPU-simulated water, shown at E3 2006 to sell the PlayStation 3's RSX + Cell horsepower. It later shipped as a downloadable SDK sample (`NPEA00003`), built on Sony's early Cell graphics stack (`jsGcm`/PSGL, the `Gmm` GPU allocator, SPU-driven async copy). It never received a standalone PC release.

This project takes the demo's PS3 `EBOOT` binary, disassembles every PowerPC function, lifts them to C++, and links against [ps3recomp](https://github.com/sp00nznet/ps3recomp) — a set of HLE runtime libraries that replace the PS3 operating system with native host implementations. The result is a standalone Windows executable, no emulator required.

The demo ships as a **debug build with a full symbol table and DWARF** — a rare luxury for a PS3 port. Every one of the 8,530 recompiled functions carries its real name (`_jsGcmAllocateMemory`, `gmmAllocFixed`, `spu_memcpy_elf`, …), which makes the lifted code and every crash backtrace directly readable.

## Current Status

| Metric | Value |
|---|---|
| Title | Rubber Ducky / *Bigduck* Sample |
| Title ID | NPEA00003 |
| Engine | Sony Cell graphics sample (jsGcm / PSGL "debugrsx", Gmm allocator) |
| ELF type | ET_EXEC, PowerPC64 big-endian, self-contained (no .sprx, no relocations) |
| Build flavour | **Debug** — 8,582-symbol `.symtab` + 6 MB DWARF |
| OPD descriptors | 9,707 |
| Functions detected | 8,530 unique (99.0% recall vs. `.symtab`, 99.6% precision) |
| Functions lifted | 10,701 (+ 2,783 mid-function tail-entry wrappers) → 13,484 emitted |
| Imported libraries | 6 (cellGcmSys, cellResc, sys_io, sysPrxForUser, cellSysutil, cellSysmodule) |
| Imported functions | 90 |
| Generated C++ | ~105 MB across 3 chunks |
| Binary size | 16.4 MB (ELF) → 24 MB (exe) |
| ps3recomp version | research/proto-builds |
| Target | Windows x86-64 (clang-cl + Ninja) |

### Phase Progress

| Phase | Status | Notes |
|---|---|---|
| SELF → ELF | **Complete** | fake-SELF; plaintext ELF carved at `elf_offset` 2688 |
| ELF / loader analysis | **Complete** | entry OPD `0x4A51D8` → code `0x10230`, TOC `0x4C0120`; 2 PT_LOAD segments |
| Symbol extraction | **Complete** | 8,582 function symbols from `.symtab` → name map for the lifter |
| Function detection | **Complete** | 8,530 functions, scored 99.0% recall / 99.6% precision vs. ground truth |
| NID resolution | **Complete** | 90 imports across 6 libraries, all HLE-covered |
| PPU lifting | **Complete** | 13,484 functions → C++ with real symbol names as comments |
| Runtime build | **Complete** | ps3recomp_runtime built from research branch (clang-cl) |
| Link | **Complete** | 24 MB `rubberducky.exe` |
| CRT startup | **Working** | `sys_initialize_tls`, argv setup, entry dispatch |
| Module loading | **Working** | loads NET, SYSUTIL_NP, USBD, JPGDEC, RESC via cellSysmodule |
| SPU subsystem init | **Working** | `[SPU] initialize(nspu=6, nrawspu=1)` |
| D3D12 backend | **Working** | device (FL 11.0), D24S8 depth, triangle/line/point PSOs, 112 KB VB, window @1280×720 |
| Synthetic vblank / flip | **Running** | 60 Hz host timer thread drives the GCM FIFO drain + present |
| Guest heap | **Working** | dlmalloc's arena never backs (MORECORE unimplemented) → the 5 public allocators are overridden with a bump allocator (`src/rd_malloc.cpp`); the game's own debug malloc now returns real addresses |
| Reaches renderer | **Working** | past CRT/module init into `dmaCmd`, the demo's GPU command-submission path |
| Raw-SPU MFC proxy DMA | **Working** | `sys_raw_spu_mmio_write/_read` overridden (`src/rd_spu.cpp`): DMAs execute as memcpy in the flat VM, proxy tag-status reports complete |
| Raw-SPU code execution | **Blocked** | after the DMAs the demo sets NPC + RunCntl and runs an actual SPU program, polling SPU_Status for its stop code — needs an SPU execution core (current wall, below) |
| First frame | Not yet | gated by the raw-SPU execution wall below |

### What Works Now

- **Full toolchain, end to end** — SELF carve → analysis → symbol-named lift → runtime build → link → run, all reproducible from the demo's own EBOOT.
- **Boots into real game code** — CRT initialises TLS, sets up `argv` (`/dev_bdvd/PS3_GAME/USRDIR/EBOOT.BIN`), and dispatches the entry OPD into recompiled PowerPC.
- **Module loader** — the game's `cellSysmoduleLoadModule` sequence runs: NET, SYSUTIL_NP, USBD, JPGDEC, RESC.
- **SPU + graphics bring-up** — SPU subsystem initialises; the demo prints its `debugrsx` banner and the D3D12 backend opens a 1280×720 window with depth buffer and all three primitive-class pipelines ready.
- **Working guest heap** — the demo's dlmalloc arena never gets backing memory (its MORECORE/sbrk isn't emulated), so all five public allocators (`malloc`/`free`/`calloc`/`realloc`/`memalign` at `0x00396D14`–`0x00396DE8`) are redirected to a bump allocator over `0x11000000`–`0x40000000` via `tools/post_lift.py`. The game's own debug malloc now reports real addresses, and the null-object vtable dispatch is gone.
- **Reaches its renderer** — boot now runs through CRT + module init into `dmaCmd` (`_Z6dmaCmdiiijyj`), the GPU command-submission path.
- **Raw-SPU MFC proxy DMA** — the demo drives Cell "AsyncCopy" by programming a raw SPU's MFC via problem-state MMIO. `src/rd_spu.cpp` overrides `sys_raw_spu_mmio_write/_read` to model the MFC command queue: DMAs execute synchronously as bounds-checked `memcpy` in the flat VM (SPU local store at `0xE0000000 + id*0x100000`), and the proxy tag-status reports complete so `waitForDmaTransfer` falls through. Verified with sane DMAs (GET, correct LS/EA/size).
- **VFS wired** — game data resolves under `/dev_hdd0/game/NPEA00003/USRDIR/` (junction into `input/USRDIR`).
- **Named everything** — because this is a debug build, every lifted function, every watchdog RVA, and every crash chain resolves to a real symbol. The current spin symbolised in seconds: `dmaCmd` → `sys_raw_spu_mmio_write`/`_read` + `_jsYieldThread`.

### Known Issues

- **Raw-SPU code-execution wall (current blocker).** The demo drives GPU work through a **raw SPU**, not the plain GCM FIFO. The full problem-state protocol is now reverse-engineered (symbol-named debug build made this tractable):
  1. `dmaCmd` issues MFC proxy DMAs (regs `0x3004` LSA / `0x3008` EAH / `0x300C` EAL / `0x3010` size+tag / `0x3014` class+cmd) and `waitForDmaTransfer` polls tag-status `0x3104`. **This is emulated and working** (`src/rd_spu.cpp`).
  2. It then writes `SPU_NPC` (`0x4034`) and `SPU_RunCntl` (`0x401C = 1`) to **run an actual SPU program**, and polls `SPU_Status` (`0x4014`), extracting the STOP-and-signal code (bits 16–23) and reading the out-mailbox (`0x400C`). Our flat VM never executes the SPU, so the status/stop-code never appears and the poll spins.

  Getting past step 2 needs a **raw-SPU execution core**. The pieces are identified and mostly in place:
  - The running program is **`spu_0004`** (5.4 KB, entry LS `0xE0` = the NPC the game set), one of 5 SPU ELFs extracted to `spu/`. It **lifts cleanly** with `spu_lifter.py` — 33 functions, 95 % coverage, entry `spu_func_000000E0`, with a `spu_function_table[]` for indirect branches.
  - ps3recomp's SPU runtime already provides the execution machinery: `spu_run_with_halt(entry, ctx)` (runs a lifted image with a longjmp halt pad), `mfc_do_transfer` (SPU-side MFC DMA — copies between `ctx->ls` and `vm_base`, exactly our flat VM), `g_spu_out_mbox_hook` (SPU→PPU mailbox), and `spu_stop` (sets status + stop code).
  - Remaining glue (the next milestone): on RunCntl=1, mirror the raw-SPU LS (`vm_base + 0xE0000000 + id*0x100000`) into `ctx->ls`, register the lifted image, `spu_run_with_halt` it from NPC, route `g_spu_out_mbox_hook` to the problem-state out-mailbox (`0x4004`) + mailbox-status, and translate `ctx->status`/`stop_code` back into `SPU_Status` (`0x4014`) so the game's poll completes. Then repeat for the remaining SPU workloads (the per-frame renderer) and let the RSX commands they emit flow to the D3D12 backend.

  **What blocks it today (evidence, not estimate):** lifting `spu_0004` surfaced concrete gaps that make a quick wiring job impossible:
  - The SPU lifter emits **9 unresolved `brsl` targets** (`spu_link(...) /* TODO */`) for this image — the lifted code is functionally incomplete and would mis-execute. Fixing this is `spu_lifter.py` work (branch-target resolution), not project glue.
  - The lifter emits no initial **LS data image**, and the entry reads constants from LS (`0x1180`, `0x1110`) — the SPU ELF's data segments must be loaded into `ctx->ls` at runtime.
  - The entry consumes **`r3`–`r6` as arguments**; the raw-SPU convention for passing them (the game sets NPC + RunCntl but no GPRs via MMIO) is not yet established.

  So the milestone is: fix the SPU lifter's branch resolution → load the LS data image + establish the arg convention → wire run/mailbox/stop → repeat across the SPU workload chain → RSX output to D3D12. A stub that fakes the stop code was considered and rejected: the game consumes the SPU's mailbox output and DMA'd results, so faking would not produce a correct frame.
- **`sys_heap_malloc` returns a host pointer.** `libs/system/sysPrxForUser.c`'s `sys_heap_malloc` / `sys_heap_memalign` return a raw host `malloc()` pointer instead of a guest EA into `vm_base`. Not on this demo's critical path (it uses statically-linked dlmalloc, now overridden), but it will bite any title that does — flagged upstream for ps3recomp.
- **2 unresolved cellSysutil NIDs** — `0x887572D5`, `0xE558748D`. Imported but not yet in ps3recomp's cellSysutil; they return the default HLE stub (0). Impact TBD.
- **1 unimplemented lv2 syscall** — syscall `160`, hit once during graphics init; currently a no-op stub.
- **`JS_ASSERT … spu_memcpy_elf failed`** — the async-copy path expects a working SPU ELF copy; downstream of the heap wall.

### Research-branch fixes carried in this port

Bringing the demo up on a fresh clang-cl build of the **research/proto-builds** runtime surfaced a handful of pre-existing breakages, fixed in the ps3recomp checkout this project builds against:

1. `spurs_taskset.h` declared `vm_read32`/`vm_read64`/`vm_write32`/`vm_write64` with 64-bit EAs, conflicting with `ppu_memory.h`'s canonical 32-bit signatures — corrected to match.
2. `cellGcmSys.c` and `spu_workload.c` carried stale local `extern` re-declarations (`vm_read32`, `spu_run_lifted_job_abi`) that conflicted with the now-included headers — removed.
3. `rsx_commands.c` and `sys_ppu_thread.c` called `getenv` without `<stdlib.h>` — under clang-cl the implicit declaration truncates the returned pointer, a real 64-bit bug. Added the include.
4. `spu_workload.c` hard-referenced `tsp_spu_func_00000A00`, a **game-specific** lifted SPU symbol from the ydkj port, breaking the link for every other title. Gave it a `__attribute__((weak))` default so a game that ships that SPU image still overrides it.

## Build Pipeline (reproducible)

```bash
# 0. Carve the plaintext ELF out of the fake-SELF EBOOT.BIN (elf_offset = 2688)
#    -> input/EBOOT.elf, and stage input/USRDIR/ with the demo's assets.

R=../ps3recomp   # ps3recomp checkout on the research/proto-builds branch

# 1. Loader + image + OPD + import metadata  -> meta/*.json
python $R/tools/ppu_loader.py  input/EBOOT.elf -o meta/

# 2. Symbols (debug build!) -> name map for the lifter
python $R/tools/elf_symbols.py input/EBOOT.elf -o meta/EBOOT.symbols.json
#    (converted to meta/EBOOT.names.json: {"0xADDR": {"label": name}})

# 3. HLE NID table for the 6 imported libraries (+ input libs)
python $R/tools/gen_hle_nids.py cellGcmSys cellResc sysPrxForUser \
       cellSysutil cellSysmodule cellPad cellKb cellMouse \
       --out src/gen/ppu_hle_nids.cpp

# 4. Lift every function to C++ (real names as comments, HLE-stub dispatch,
#    code-end at the top of .sceStub.text so .eh_frame/.rodata stay data)
python $R/tools/ppu_lifter.py input/EBOOT.elf \
       --functions meta/EBOOT.functions.json \
       --names     meta/EBOOT.names.json \
       --hle-stubs meta/EBOOT.imports.json \
       --code-end  0x40c080 \
       --output    src/recomp

# 4b. Post-lift patches (idempotent): redirect the 5 dlmalloc public allocators
#     to the bump allocator in src/rd_malloc.cpp
python tools/post_lift.py

# 5. Build the runtime library (once) and the game (clang-cl + Ninja)
cmake -S $R -B $R/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
cmake --build $R/build
cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
cmake --build build

# 6. Run
PS3_VFS_ROOT=input ./build/rubberducky input/EBOOT.elf
```

## How It Works

```
+-----------------------------------------------------------+
|                 Bigduck EBOOT.BIN (fake-SELF)             |
|          PowerPC 64-bit, big-endian, ET_EXEC             |
+---------------------------+-------------------------------+
                            | carve @2688
                  +---------v----------+
                  | ppu_loader / _symbols |  8,530 funcs, 8,582 names
                  +---------+----------+
                            |
                  +---------v---------+
                  | ppu_lifter (C++)  |  ~105 MB, symbol-annotated
                  +---------+---------+
                            |
             +--------------v--------------+
             |  clang-cl + Ninja link      |
             |                             |
             |  ppu_recomp_00N.cpp         |
             |  + boot_main.cpp (harness)  |
             |  + ppu_loader/hle/sysprx/fs |---> rubberducky.exe
             |  + ppu_hle_nids.cpp (90 NID)|
             |  + ps3recomp_runtime.lib    |
             |    (incl. D3D12 backend)    |
             +--------------+--------------+
```

### Project Structure

```
rubberducky/
├── README.md
├── CMakeLists.txt          # links the runtime + the generic boot harness
├── config.toml             # analysis-driven pipeline config
├── input/                  # your own EBOOT.elf + USRDIR assets (gitignored)
├── meta/                   # analysis outputs (committed): loader/imports/
│                           #   image/functions/symbols/names JSON
├── tools/
│   └── post_lift.py        # idempotent post-lift patches (allocator override)
└── src/
    ├── boot_main.cpp       # ps3recomp boot harness, rebranded for this title
    ├── rd_malloc.cpp       # guest heap override (bump allocator)
    ├── compat/             # <dirent.h>/<unistd.h> Win32 shims
    ├── gen/                # generated HLE NID table (committed)
    └── recomp/             # 105 MB lifted C++ (gitignored; regenerate)
```

## Building

### Prerequisites

- Python 3.9+
- CMake 3.20+, Ninja
- LLVM/clang-cl 14+ (the loader uses `__builtin_bswap` / weak symbols MSVC lacks)
- A Visual Studio 2022 dev environment (MSVC headers + Windows SDK for clang-cl)
- [ps3recomp](https://github.com/sp00nznet/ps3recomp) on the `research/proto-builds` branch, at `../ps3recomp`

> **Note:** You must supply your own copy of the demo (NPEA00003). This repo contains only the recompilation toolchain, harness, and generated NID table — no copyrighted game binary or assets.

## Related Projects

- **[ps3recomp](https://github.com/sp00nznet/ps3recomp)** — the PS3 HLE runtime this builds against
- **[flow](https://github.com/sp00nznet/flow)** / **[youdontknowjack](https://github.com/sp00nznet/youdontknowjack)** — sister ports further along the same pipeline
- **[N64Recomp](https://github.com/N64Recomp/N64Recomp)** — the project that proved static recompilation works at scale

## Legal

This project contains no proprietary Sony code, game binaries, encryption keys, or copyrighted assets. It is a clean-room reimplementation of PS3 system libraries paired with automated binary-translation tools. Users must supply their own legally obtained copy of the demo.

## Changelog

### v0.3.0 — Raw-SPU MFC proxy DMA; SPU-execution wall mapped (2026-07-15)
- **MFC proxy DMA emulated.** Reverse-engineered the demo's raw-SPU problem-state protocol from the symbol-named `dmaCmd` / `doDma` / `waitForDmaTransfer` and overrode `sys_raw_spu_mmio_write/_read` (`src/rd_spu.cpp`, patched in by `tools/post_lift.py`). MFC DMA commands now execute synchronously as bounds-checked `memcpy` in the flat VM (LS at `0xE0000000 + id*0x100000`); the proxy tag-status reports complete so the DMA-wait poll falls through. Verified: the first async-copy DMAs run with correct direction/LS/EA/size.
- **Next wall precisely mapped.** Past the DMAs, the demo sets `SPU_NPC`/`SPU_RunCntl` and executes an actual SPU program, polling `SPU_Status` for a STOP-and-signal code + out-mailbox output. This needs a full raw-SPU execution core (the 5 embedded `.spu_image` ELFs are extracted to `spu/`). Documented in Known Issues as the next milestone; a fake-completion stub was rejected as it can't produce a correct frame.

### v0.2.0 — Past the heap wall, into the renderer (2026-07-15)
- **Guest heap fixed.** Root-caused the `malloc → 0` wall: the demo statically links dlmalloc (mspace variant) whose arena is created lazily via a MORECORE/sbrk that recomp doesn't back, so the arena pointer at `TOC+0x1108` stays null. Rather than replicate the OS heap init, the five public allocators are overridden with a bump allocator (`src/rd_malloc.cpp`) at guest `0x11000000`, injected into the lifted code by a reproducible `tools/post_lift.py`.
- **Null-object dispatch gone.** With real allocations, the `unresolved indirect call -> 0x004A41F8` (null-vtable) is resolved and the earlier `spu_memcpy_elf` assert clears.
- **Reaches the GPU submission path.** Boot now runs into `dmaCmd`, where the true rendering wall lives: the demo drives a **raw SPU via MMIO** (`0xE0040000 + id*0x100000 + off`) and spins polling for SPU completion. Precisely diagnosed (see Known Issues); next milestone is raw-SPU emulation.
- **VFS wired** to `/dev_hdd0/game/NPEA00003/USRDIR/`.

### v0.1.0 — First Boot (2026-07-15)
- Carved the plaintext ELF from the fake-SELF EBOOT; full loader/import/symbol analysis.
- Lifted all 8,530 functions (13,484 emitted) to symbol-named C++ via ps3recomp's research-branch tools.
- Built the runtime + game with clang-cl; fixed four pre-existing research-branch build breakages along the way (see above).
- **First boot reaches real game init:** CRT/TLS, the cellSysmodule load sequence, SPU init, and full D3D12 backend bring-up (1280×720 window, depth + PSOs).
- Identified the current wall: the demo's internal static-pool `malloc` returns `0` (heap-init has not landed), which cascades into a null-object virtual dispatch. Next milestone: get the heap initialised so the first frame can build.

---

*"Wait and press &lt;START&gt; to start the demo."*
