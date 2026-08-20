# Build Pipeline

How the demo's PS3 `EBOOT` becomes a native Windows executable.
See [the README](../README.md) for what the port currently does.

## Reproducing the build

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

