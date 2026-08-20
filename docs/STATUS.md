# Detailed Status

Per-phase detail and known issues. See [the README](../README.md) for the summary.

## Metrics

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
| Raw-SPU code execution | **Working** | the lifted SPU (`spu_0004`) runs as an async host-thread worker with live mailbox exchange; the AsyncCopy ready-handshake completes and the game leaves init |
| Natural cellGcmInit | **Working** | the game's own `_cellGcmInitBody` fires (cmdbuf, ioAddr `0x11100000`); `cellGcmSys` Init + SetGraphicsHandler run |
| FIFO sync (cellGcmFinish) | **Working** | SET_REFERENCE + control-register get/ref now advance as the RSX drains, so `cellGcmFinish` completes |
| RSX FIFO init | **Working** | `process_fifo` now walks the FIFO ring following JUMPs (drain to `control->put`), so the FIFO-init reference-sync completes |
| Video-out config | **Working** | `cellVideoOutGetState`/`GetResolution`/`Configure` registered → 1280×720; render-target dimensions valid |
| PSGL device init | **Working** | flip handlers set, `SetFlipMode`, tiles/zcull — device init passes |
| Fifo survives to render | **Working** | fixed a `memset` that zeroed the initialized `jsGcmFifo` (it sits at device+0x14 inside a 512-byte struct the game clears); guarded via the memset override |
| First RSX draw command | **Working** | the demo emits a `CLEAR_SURFACE` the RSX parser processes (`mask=0x10 color=0x0`) — first game-issued RSX command |
| FIFO reference-sync | **Working** | drain FIFO every ~1ms (not only 60Hz) so `_jsGcmFifoFinish`'s reference-wait passes; `JSGcmFifo.cpp:142` now transient |
| PSGL device creation completes | **Working** | fixed frameless-cascade `r31`/stack-slot corruption around `_jsPlatformCreateDevice` + `psglCreateContext`; device+context+`MakeCurrent` succeed, `LContext` non-NULL |
| Graphics init without aborting | **Working** | full PSGL bring-up runs with **zero asserts / no `exit(1)`** |
| `onInit` completes (no exit) | **Working** | neutralized a spurious `_Unwind_Resume` that was quitting the app; onInit runs into its SPU setup |
| cellGcmFinish reference-sync | **Working** | the "flag=30 spin" was **misdiagnosed** — it is `cellGcmFinish`'s ref-sync loop, whose `usleep(30)` yield is misrouted to `event_flag_wait` (a syscall-141 collision). `process_fifo` drains the `SET_REFERENCE` and sets `ctrl->ref`, so the loop exits and the demo proceeds |
| First RSX draw + flip setup | **Working** | `SetFlipHandler`/`SetFlipMode`, then a real `[RSX] CLEAR_SURFACE mask=0x10` |
| Duck assets load into GL | **Working** | loads **all** duck meshes (`duckRflct`, `newLOD1`, `newLOD3`, `duck.smesh`/`.reflect.smesh`/`.many.smesh`) + `duck.tga`, and runs `createGLIndexedArrays` + `cellPad`/`cellKb`/`cellMouse` init |
| GL buffer name-space | **Working** | the PSGL buffer name-space allocator returns valid IDs; VBOs create normally |
| Scene geometry | **Working** | the demo's meshes draw — tub, walls, towel, faucet, duck |
| Textures | **Working** | correct after the TEXTURE_CONTROL1 crossbar fix; cube maps decode all six faces |
| Vertex programs | **Working** | `SET_TRANSFORM_PROGRAM_START` honoured, so each draw runs its own program |
| SPU simulation | **Working** | the sim SPUs receive their per-frame work via `sys_spu_thread_receive_event` and DMA real data |
| Fluid / water surface | **Blocked** | the guest's particle buffers are never populated, so the isosurface is degenerate — see Known Issues |

### What Works Now

- **Full toolchain, end to end** — SELF carve -> analysis -> symbol-named lift -> runtime build -> link -> run, all reproducible from the demo's own EBOOT.
- **Boots into real game code** — CRT initialises TLS, sets up `argv`, and dispatches the entry OPD into recompiled PowerPC.
- **Module loader** — the game's `cellSysmoduleLoadModule` sequence runs: NET, SYSUTIL_NP, USBD, JPGDEC, RESC.
- **Renders its own scene** — the bathroom draws correctly: tiled walls, blue mosaic tub, checkered towel, chrome faucet, and the rubber duck.
- **Working guest heap** — the demo's dlmalloc arena never gets backing memory, so the five public allocators are redirected to a bump allocator via `tools/post_lift.py`.
- **Raw-SPU MFC proxy DMA** — `sys_raw_spu_mmio_write/_read` model the MFC command queue; DMAs execute as bounds-checked `memcpy` in the flat VM.
- **SPU simulation runs** — the sim SPUs (`hfluid`, `isosurf1`, `rbodycoll`) are persistent workers that request each frame's work descriptor with `stop 0x110`
  (`sys_spu_thread_receive_event`); servicing that took them from 2 DMA reads and zero writes to ~11,400 reads and ~8,600 writes per run.
- **VFS wired** — game data resolves under `/dev_hdd0/game/NPEA00003/USRDIR/`.
- **Named everything** — because this is a debug build, every lifted function and every crash chain resolves to a real symbol.

### Known Issues

- **The water does not render (current blocker).** The fluid's isosurface vertex buffer is written every frame but holds every vertex at a single
  point — the degenerate output an isosurface extractor emits when there is no surface to extract. Traced end to end: the sim SPUs write mostly
  zeros because they *read* zeros; their bulk input buffers are never written by anything. The particle state the guest is supposed to seed stays
  empty, so everything downstream is a faithful consequence. The RSX side is not implicated — those draws are submitted correctly and simply have
  no geometry to rasterize.
- **Frame rate.** With the SPU simulation running the demo sits at roughly 4-5 fps: about 16 M interpreted SPU instructions per frame, on one host
  thread. The interpreter itself runs at 72-75 M instructions/s, so the cost is real work rather than overhead. Dropping `RD_SPU_INTERP=1` gives
  roughly 12 fps with no simulation. The lifted SPU images exist under `src/spu_gen/` but only one is compiled and the per-frame dispatch runs
  pure-interpreted, so it never rejoins them — wiring that up is the speed fix.
- **Per-frame PSGL assert.** `JSGCM/PlatformDevice.cpp:985` fires about 1.5x per frame. It is the guest's own vblank handler posting a semaphore
  (created `init=1 max=2`) faster than the guest consumes it, which returns `CELL_EBUSY` — a consequence of running below 60 fps, not a defect.
- **Vertex budget.** Heavy frames request more vertices than the per-frame buffer holds and some draws truncate. The fat part is the 256-byte
  per-vertex VP stride rather than the buffer size.
- **2 unresolved cellSysutil NIDs** — `0x887572D5`, `0xE558748D`; they return the default HLE stub.
- **1 unimplemented lv2 syscall** — syscall `160`, hit once during graphics init.

