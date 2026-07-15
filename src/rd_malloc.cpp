/*
 * Rubber Ducky — guest heap override.
 *
 * The demo statically links dlmalloc (mspace variant). Its arena is created
 * lazily via dlmalloc's MORECORE/sbrk, which recomp doesn't back — so the
 * mspace pointer at TOC+0x1108 stays null and every malloc returns 0, landing
 * the game's objects at address 0 (null-vtable dispatch).
 *
 * Rather than replicate dlmalloc's OS-heap init, we replace the five public
 * allocators (malloc/free/calloc/realloc/memalign) with a bump allocator over a
 * committed guest-VM region above the image. This is the flОw "guest malloc
 * bypass" pattern. The five lifted wrapper bodies are redirected here by
 * tools/post_lift.py; see that script for the exact call sites.
 *
 * Heap window: 0x11000000 (just above the image top, seg1 end 0x1091F08) up to
 * the sys_memory_allocate window at 0x40000000 — ~752 MB, never freed. Ample
 * for a demo; if a long run OOMs, promote to a real free-list allocator.
 * ponytail: bump-only, no free; real allocator only if a run actually OOMs.
 */
#include "ppu_recomp.h"
#include <cstdio>
#include <cstring>

extern "C" uint8_t* vm_base;

static uint32_t       g_heap     = 0x11000000u;
static const uint32_t g_heap_end = 0x40000000u;
static uint32_t       g_allocs   = 0;

static uint32_t rd_bump(uint32_t size, uint32_t align)
{
    if (align < 16) align = 16;
    g_heap = (g_heap + (align - 1)) & ~(align - 1);
    size = (size + 15u) & ~15u;
    if (size == 0) size = 16;
    if (g_heap + size > g_heap_end) {
        fprintf(stderr, "[rd_malloc] OOM: %u bytes, used %u MB\n",
                size, (g_heap - 0x11000000u) / (1024 * 1024));
        return 0;
    }
    uint32_t ptr = g_heap;
    g_heap += size;
    if (vm_base) memset(vm_base + ptr, 0, size);
    if (++g_allocs <= 20)
        fprintf(stderr, "[rd_malloc] %u -> 0x%08X (#%u)\n", size, ptr, g_allocs);
    return ptr;
}

void rd_hle_malloc(ppu_context* ctx)      /* malloc(size) */
{
    ctx->gpr[3] = rd_bump((uint32_t)ctx->gpr[3], 16);
}

void rd_hle_memalign(ppu_context* ctx)    /* memalign(align, size) */
{
    ctx->gpr[3] = rd_bump((uint32_t)ctx->gpr[4], (uint32_t)ctx->gpr[3]);
}

void rd_hle_calloc(ppu_context* ctx)      /* calloc(n, size) */
{
    uint64_t n = (uint32_t)ctx->gpr[3], s = (uint32_t)ctx->gpr[4];
    ctx->gpr[3] = rd_bump((uint32_t)(n * s), 16);    /* rd_bump already zeroes */
}

void rd_hle_realloc(ppu_context* ctx)     /* realloc(ptr, size) */
{
    uint32_t old = (uint32_t)ctx->gpr[3];
    uint32_t size = (uint32_t)ctx->gpr[4];
    uint32_t neu = rd_bump(size, 16);
    if (neu && old && vm_base) {
        uint32_t cp = size < 0x100000 ? size : 0x100000;   /* bounded copy */
        memcpy(vm_base + neu, vm_base + old, cp);
    }
    ctx->gpr[3] = neu;
}

void rd_hle_free(ppu_context* ctx) { (void)ctx; }   /* bump: no-op */
