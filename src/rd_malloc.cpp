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

/* memset(dst, c, n) with a watchpoint on the jsGcmFifo @ 0x68C474 (68 bytes).
 * Something zeroes that fifo between _jsGcmInitFromRM and SetRenderTarget; log
 * the call whose range covers it so we can find the culprit. */
static const uint32_t RD_FIFO = 0x68C474u;   /* jsGcmFifo @ device+0x14 */
static const uint32_t RD_FIFO_LEN = 68u;

void rd_hle_memset(ppu_context* ctx)
{
    uint32_t dst = (uint32_t)ctx->gpr[3];
    int      c   = (int)(uint32_t)ctx->gpr[4];
    uint32_t n   = (uint32_t)ctx->gpr[5];

    /* The device struct @ 0x68C460 embeds the jsGcmFifo at +0x14. The game
     * clears the whole 512-byte device with a memset that runs *after* the
     * fifo was already initialized (order the lift preserves but that leaves
     * the fifo zeroed by the time SetRenderTarget reads it). If a memset would
     * clobber an already-initialized (non-zero) fifo, preserve those 68 bytes.
     * ponytail: targeted guard on a known aliasing clear; real fix is the lift
     * ordering, but this confirms/unblocks the render path. */
    bool covers = dst <= RD_FIFO && RD_FIFO < (uint64_t)dst + n;
    uint8_t saved[RD_FIFO_LEN];
    bool restore = false;
    if (covers && vm_base && c == 0) {
        memcpy(saved, vm_base + RD_FIFO, RD_FIFO_LEN);
        for (uint32_t i = 0; i < RD_FIFO_LEN; ++i)
            if (saved[i]) { restore = true; break; }
    }
    if (vm_base && n) memset(vm_base + dst, c, n);
    if (restore) {
        memcpy(vm_base + RD_FIFO, saved, RD_FIFO_LEN);
        fprintf(stderr, "[rd_dbg] memset 0x%08X n=%u would clobber live fifo -> preserved\n",
                dst, n);
    }
    /* ctx->gpr[3] already = dst (memset returns dst) */
}

/* _Unwind_Resume neutralized. The lifter reaches a cleanup landing-pad's
 * _Unwind_Resume on a NORMAL execution path (verified: NONE of __cxa_throw /
 * _Unwind_RaiseException / __cxa_allocate_exception / terminate ever run, and
 * the exception-object arg is 0xFFFFFFFF = no exception in flight). So the real
 * _Unwind_Resume "resumes" a nonexistent exception -> terminate() -> abort() ->
 * sys_process_exit(1), killing DuckApp::onInit before the frame loop. Making it
 * a no-op lets the (already-run) cleanup fall through, so onInit completes and
 * the demo proceeds into its SPU render pipeline.
 * ponytail: neutralizes a spurious lift-EH unwind; the real fix is landing-pad
 * control flow in spu_lifter/ppu_lifter. Safe here because the title never
 * raises a real C++ exception (confirmed by the EH-entrypoint taps). */
void rd_hle_unwind_resume(ppu_context* ctx) { (void)ctx; }
