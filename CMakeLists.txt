# Rubber Ducky Tech Demo (NPEA00003) — minimal PPU boot harness.
#
# Wires the lifter output to ps3recomp's generic first-boot harness (copied to
# src/boot_main.cpp and given this demo's window title): load EBOOT -> register
# lifted functions + HLE NID handlers -> dispatch the entry OPD -> run real boot
# code until it hits an unlifted function / unimplemented import / lv2 syscall,
# each logged so we know exactly what to implement next. A synthetic RSX vblank
# thread drives the D3D12 backend so the demo's frame loop advances.
#
# Toolchain: clang-cl (the loader uses __builtin_bswap / weak symbols MSVC
# lacks; clang-cl keeps the MSVC ABI so it links the prebuilt runtime .lib).
#   cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
#   cmake --build build
#   ./build/rubberducky "input/EBOOT.elf"
cmake_minimum_required(VERSION 3.20)
project(rubberducky CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_C_STANDARD 17)

# ps3recomp checkout (research/proto-builds branch) + its prebuilt runtime lib.
set(PS3RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../ps3recomp" CACHE PATH "ps3recomp repo root")
set(PS3RECOMP_RUNTIME_LIB "${PS3RECOMP_DIR}/build/ps3recomp_runtime.lib"
    CACHE FILEPATH "prebuilt runtime library")

# Lifted game code (chunked .cpp) + the generated HLE NID table + recomp stubs.
file(GLOB RECOMP_SOURCES "src/recomp/*.cpp")
file(GLOB GEN_SOURCES    "src/gen/*.cpp")

add_executable(rubberducky
    "${CMAKE_CURRENT_SOURCE_DIR}/src/boot_main.cpp"      # boot harness (rebranded)
    "${CMAKE_CURRENT_SOURCE_DIR}/src/rd_malloc.cpp"      # guest heap override (bump)
    "${CMAKE_CURRENT_SOURCE_DIR}/src/rd_spu.cpp"         # raw-SPU MFC proxy DMA
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_loader.cpp"        # vm + ELF loader + run
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_hle.cpp"           # NID -> handler dispatch
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_sysprx.cpp"        # boot-critical CRT
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_fs.cpp"            # cellFs VFS
    ${GEN_SOURCES}
    ${RECOMP_SOURCES}
)

# Every libs/* subdir (the generated NID table's handlers live there).
file(GLOB LIB_INC_DIRS LIST_DIRECTORIES true "${PS3RECOMP_DIR}/libs/*")

target_include_directories(rubberducky PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/compat"            # <dirent.h>/<unistd.h> Win shims
    "${CMAKE_CURRENT_SOURCE_DIR}/src/recomp"            # ppu_recomp.h
    "${PS3RECOMP_DIR}/runtime/ppu"
    "${PS3RECOMP_DIR}/runtime/spu"
    "${PS3RECOMP_DIR}/include"
    ${LIB_INC_DIRS}
)

target_link_libraries(rubberducky PRIVATE "${PS3RECOMP_RUNTIME_LIB}")
if(WIN32)
    target_link_libraries(rubberducky PRIVATE
        ws2_32 bcrypt xinput ole32 d3d12 dxgi dxguid d3dcompiler)
    # Deep recompiled call chains nest on the host stack (each guest call is a
    # host call); reserve 256 MB for the main thread so it doesn't overflow the
    # 1 MB default. Spawned threads get their reserve in sys_ppu_thread_create.
    target_link_options(rubberducky PRIVATE /STACK:268435456 /MAP:rubberducky.map)
endif()
