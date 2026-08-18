/*
 * Rubber Ducky — raw-SPU MFC proxy-DMA emulation.
 *
 * The demo drives GPU data movement ("Cell/AsyncCopy") through a raw SPU's MFC
 * used as a *proxy* DMA engine: the PPU pokes the SPU problem-state registers to
 * issue DMAs and polls the proxy tag-status for completion. No SPU code runs —
 * it's pure DMA. Concretely (see dmaCmd / waitForDmaTransfer, both symbol-named
 * in this debug build):
 *
 *   sys_raw_spu_mmio_write(id, off, val)  ->  guest [0xE0040000 + id*0x100000 + off]
 *   sys_raw_spu_mmio_read (id, off)       <-  same
 *
 *   0x3004 MFC_LSA          local-store address (offset into the SPU's LS)
 *   0x3008 MFC_EAH          effective address, high 32
 *   0x300C MFC_EAL          effective address, low 32
 *   0x3010 MFC_Size_Tag     (tag << 16) | size
 *   0x3014 MFC_Class_CMD    write = issue DMA (low byte = opcode); read = CMDStatus
 *   0x3104 MFC_Prxy_TagStat read = which tag groups have completed
 *   0x321C MFC_Prxy_QueryMask / 0x3204 QueryType   (tag-group query setup)
 *
 * On real hardware the MFC copies asynchronously and sets the tag bit when done.
 * We complete every DMA synchronously on the Class_CMD write (a memcpy in the
 * flat VM: raw SPU LS lives at guest 0xE0000000 + id*0x100000), then report all
 * tag groups complete so the game's poll falls through. GET = main->LS,
 * PUT = LS->main; the demo bounces main->LS->main to do async copies.
 *
 * These three handlers replace the lifted sys_raw_spu_mmio_write/_read wrappers
 * (patched in by tools/post_lift.py).
 */
#include "ppu_recomp.h"
#include "spu_context.h"     /* spu_context, SPU_LS_SIZE, spu_context_init, SPU_STATUS_* */
#include <cstdio>
#include <cstring>
#include <cstdlib>   /* getenv */
#include <windows.h> /* CreateThread, interlocked, barriers */

extern "C" uint8_t* vm_base;
extern "C" {
    int  spu_run_with_halt(void (*entry)(spu_context*), spu_context* ctx);
    uint32_t spu_interp_run(spu_context* ctx, uint32_t start_lsa);
    /* SPU->PPU outbound-mailbox delivery hook (runtime/spu/spu_channels.c). */
    extern void (*g_spu_out_mbox_hook)(uint32_t group_id, uint32_t spu_id,
                                       uint32_t which, uint32_t value);
    /* Lifted spu_0004 (src/spu_gen/spu_0004) — the lifter's header declares these
     * outside its extern "C", so declare them here with C linkage ourselves. */
    void spu_recomp_register(void);
    void spu_func_000000E0(spu_context* ctx);
}

#define RAW_SPU_LS_BASE   0xE0000000u   /* per-SPU local store  */
#define RAW_SPU_PS_BASE   0xE0040000u   /* per-SPU problem state */
#define RAW_SPU_STRIDE    0x00100000u

/* The demo's raw SPU runs spu_0004 (entry LS 0xE0 = the NPC). Its ELF is already
 * in guest RAM as part of the .spu_image section (extracted copy = spu/). */
#define SPU0004_GUEST_ELF 0x497500u

static int rd_spu_trace(void);

#define SPU_MAX 8
static spu_context g_spu_ctx[SPU_MAX];
static int         g_spu_ready[SPU_MAX];
static volatile long g_spu_running[SPU_MAX];             /* 1 while the SPU thread runs */
/* SPU->PPU outbound mailbox ring (filled by the SPU thread, drained by the PPU). */
static volatile uint32_t g_out_mbox[SPU_MAX][16];
static volatile long     g_out_mbox_wr[SPU_MAX];         /* producer index (SPU thread) */
static volatile long     g_out_mbox_rd[SPU_MAX];         /* consumer index (PPU) */

/* The runtime's own mailbox hook (installed at init: routes an SPU thread's
 * WrOutMbox/WrOutIntrMbox to its connected event queue). We overwrite the single
 * global g_spu_out_mbox_hook to catch the RAW SPU's mailbox, so save the prior
 * hook and chain to it for everything that isn't our raw SPU -- otherwise the
 * interpreted sim SPUs' completion mailboxes get dropped and the PPU waits on
 * their event queue forever. */
static void (*g_prev_out_mbox_hook)(uint32_t, uint32_t, uint32_t, uint32_t) = 0;

static void rd_spu_mbox_hook(uint32_t grp, uint32_t sid, uint32_t which, uint32_t val)
{
    /* Not our raw SPU (raw ids are < SPU_MAX; sim-thread tids are >= 0x2000):
     * only the INTERRUPT mailbox (which==1) generates a connected-queue event
     * (the SPU's completion signal the PPU waits on). The regular WrOutMbox
     * (which==0) is a plain PPU-readable mailbox, NOT an event -- forwarding it
     * pushed spurious events onto q3/q5/q7 and made the game abort. Drop those. */
    if (sid >= SPU_MAX) {
        /* RD_MBOX_EVENTS=1 also forwards the plain WrOutMbox. The sim SPUs that
         * complete via WrOutMbox alone (rbodycoll/isosurf1/hfluid write 0x3F and
         * never touch the interrupt mailbox) otherwise never wake DuckApp::onUpdate,
         * which blocks in sys_event_queue_receive on their connected queue. */
        static int fwd0 = -1;
        if (fwd0 < 0) fwd0 = getenv("RD_MBOX_EVENTS") ? 1 : 0;
        if ((which == 1 || fwd0) && g_prev_out_mbox_hook)
            g_prev_out_mbox_hook(grp, sid, which, val);
        return;
    }
    if (which != 0) return;                              /* which 0 = WrOutMbox */
    long w = g_out_mbox_wr[sid];
    g_out_mbox[sid][w & 15] = val;
    _WriteBarrier();
    g_out_mbox_wr[sid] = w + 1;                          /* publish */
}

/* Load a guest-resident SPU ELF's PT_LOAD segments into ctx->ls (big-endian). */
static void rd_spu_load_image(spu_context* ctx, uint32_t guest_elf)
{
    const uint8_t* e = vm_base + guest_elf;
    auto be32 = [](const uint8_t* p) {
        return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
    };
    auto be16 = [](const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); };
    uint32_t phoff = be32(e + 28);
    uint16_t phentsize = be16(e + 42), phnum = be16(e + 44);
    for (uint16_t i = 0; i < phnum; i++) {
        const uint8_t* ph = e + phoff + i * phentsize;
        if (be32(ph) != 1) continue;                     /* PT_LOAD */
        uint32_t off = be32(ph + 4), vaddr = be32(ph + 8), filesz = be32(ph + 16);
        if ((uint64_t)vaddr + filesz <= SPU_LS_SIZE)
            memcpy(ctx->ls + vaddr, e + off, filesz);
    }
}

/* Raw SPU is a persistent, asynchronous worker: on RunCntl the SPU inits, sends
 * its "ready" out-mailbox, then loops waiting for commands while the PPU runs on.
 * A synchronous run would wait for a STOP that never comes, so run it on a host
 * thread. The SPU thread and PPU share ctx->ls + vm_base; the MFC DMA + mailbox
 * paths (runtime/spu) already operate on those. */
static DWORD WINAPI rd_spu_thread(LPVOID p)
{
    uint32_t id = (uint32_t)(uintptr_t)p;
    spu_context* ctx = &g_spu_ctx[id];
    spu_run_with_halt(spu_func_000000E0, ctx);           /* spu_0004 entry = 0xE0 */
    uint32_t ps  = RAW_SPU_PS_BASE + id * RAW_SPU_STRIDE;
    uint32_t lsb = RAW_SPU_LS_BASE + id * RAW_SPU_STRIDE;
    memcpy(vm_base + lsb, ctx->ls, SPU_LS_SIZE);          /* mirror final LS back */
    vm_write32(ps + 0x4024, (ctx->stop_code << 16) | ctx->status);   /* SPU_Status */
    g_spu_running[id] = 0;
    if (rd_spu_trace())
        fprintf(stderr, "[rd_spu] STOP id=%u status=0x%X stop_code=0x%X\n",
                id, ctx->status, ctx->stop_code);
    return 0;
}

static void rd_run_spu(uint32_t id)
{
    if (id >= SPU_MAX) return;
    if (_InterlockedCompareExchange(&g_spu_running[id], 1, 0) != 0)
        return;                                          /* already running */
    spu_context* ctx = &g_spu_ctx[id];
    if (!g_spu_ready[id]) {
        spu_recomp_register();                           /* register the lifted image once */
        if (g_spu_out_mbox_hook != rd_spu_mbox_hook)     /* chain the runtime hook */
            g_prev_out_mbox_hook = g_spu_out_mbox_hook;
        g_spu_out_mbox_hook = rd_spu_mbox_hook;
        g_spu_ready[id] = 1;
    }
    spu_context_init(ctx, id);
    uint32_t ps = RAW_SPU_PS_BASE + id * RAW_SPU_STRIDE;
    uint32_t lsb = RAW_SPU_LS_BASE + id * RAW_SPU_STRIDE;

    memset(ctx->ls, 0, SPU_LS_SIZE);
    rd_spu_load_image(ctx, SPU0004_GUEST_ELF);            /* code + constants + defaults */
    memcpy(ctx->ls + 0x80,   vm_base + lsb + 0x80,   0x60);
    memcpy(ctx->ls + 0x1100, vm_base + lsb + 0x1100, 0x80);

    ctx->pc = vm_read32(ps + 0x4034);                    /* NPC */
    ctx->status = SPU_STATUS_RUNNING;
    g_out_mbox_wr[id] = g_out_mbox_rd[id] = 0;

    /* Default: run the raw SPU SYNCHRONOUSLY. It does its full init + ready-mailbox
     * handshake, then parks (park_on_empty_inmbox) the first time it idle-polls the
     * inbound mailbox -- deterministic, no async host thread racing the PPU (which
     * caused the nondeterministic shader-load abort). The actual copies are HLE'd
     * (rd_hle_jsasynccopy), so the parked worker needs to do nothing further.
     * RD_SPU_ASYNC=1 restores the old async host-thread path. */
    static int async = -1;
    if (async < 0) { const char* e = getenv("RD_SPU_ASYNC"); async = (e && *e && *e != '0'); }
    if (!async) {
        ctx->park_on_empty_inmbox = 1;
        if (rd_spu_trace()) fprintf(stderr, "[rd_spu] RUN id=%u entry=0x%X (sync-park)\n", id, ctx->pc);
        /* RD_SPU_RAW_INTERP=1: interpret the raw SPU instead of running its
         * LIFTED image. The AsyncCopy bypass exists because the lifted program
         * infinite-loops on an r1 drift across a tail-call chain -- a spu_lifter
         * frame bug, not an SPU-semantics one -- so the interpreter, which keeps
         * r1 in the context, should not hit it. That matters because the real
         * copy is what computes the destination and issues the MFC DMAs; the
         * host-memcpy bypass only moves the bytes the PPU names. */
        {
            static int rawi = -1;
            if (rawi < 0) { const char* e = getenv("RD_SPU_RAW_INTERP"); rawi = (e && *e && *e != '0'); }
            if (rawi) spu_interp_run(ctx, ctx->pc);
            else      spu_run_with_halt(spu_func_000000E0, ctx);
        }
        ctx->park_on_empty_inmbox = 0;
        memcpy(vm_base + lsb, ctx->ls, SPU_LS_SIZE);
        vm_write32(ps + 0x4024, (ctx->stop_code << 16) | ctx->status);
        g_spu_running[id] = 0;
        return;
    }
    if (rd_spu_trace())
        fprintf(stderr, "[rd_spu] RUN id=%u entry=0x%X (async)\n", id, ctx->pc);
    CreateThread(NULL, 1u << 20, rd_spu_thread, (LPVOID)(uintptr_t)id, 0, NULL);
}

static uint32_t g_dma_count = 0;

/* Execute one MFC single-transfer DMA synchronously in the flat VM. */
static void rd_mfc_dma(uint32_t id, uint32_t opcode)
{
    uint32_t ps  = RAW_SPU_PS_BASE + id * RAW_SPU_STRIDE;
    uint32_t lsa = vm_read32(ps + 0x3004);
    uint32_t eah = vm_read32(ps + 0x3008);
    uint32_t eal = vm_read32(ps + 0x300C);
    uint32_t szt = vm_read32(ps + 0x3010);
    uint32_t size = szt & 0xFFFF;
    uint32_t cmd  = opcode & 0xFF;

    /* EA is 32-bit in this title (eah == 0); keep the low 32 as the guest addr. */
    uint32_t ea = eal;
    uint32_t ls = RAW_SPU_LS_BASE + id * RAW_SPU_STRIDE + (lsa & (RAW_SPU_STRIDE - 1));

    int is_get  = (cmd & 0x40) != 0;   /* GET* = main -> LS */
    int is_put  = (cmd & 0x20) != 0;   /* PUT* = LS -> main */
    int is_list = (cmd & 0x04) != 0;   /* GETL/PUTL: DMA list in LS (unhandled) */

    { static int cap = -1;
      if (cap < 0) { const char* e = getenv("RD_DMA_DBG"); cap = e ? atoi(e) : 30; }
      if ((int)g_dma_count < cap)
        fprintf(stderr, "[rd_spu] DMA#%u id=%u cmd=0x%02X %s ls=0x%08X ea=0x%08X size=%u%s\n",
                g_dma_count, id, cmd, is_get ? "GET" : is_put ? "PUT" : "?",
                ls, ea, size, is_list ? " [LIST!]" : ""); }
    g_dma_count++;
    /* Count what gets dropped below: a texture upload is exactly the shape that
     * does -- a DMA LIST, or a transfer past the single-DMA cap. */
    { static uint32_t n_list = 0, n_big = 0, n_ok = 0;
      if (is_list) n_list++; else if (size > 0x4000) n_big++; else n_ok++;
      static int rep = -1; if (rep < 0) rep = getenv("RD_DMA_DBG") ? 1 : 0;
      if (rep && ((n_list + n_big + n_ok) % 500) == 0)
          fprintf(stderr, "[rd_spu] DMA totals: executed=%u dropped-list=%u dropped-oversize=%u\n",
                  n_ok, n_list, n_big); }

    if (!vm_base || size == 0 || size > 0x4000) return;   /* MFC single-DMA <= 16 KB */
    if (is_list) return;                                  /* TODO if the demo needs it */
    if (eah != 0) return;                                 /* 64-bit EA not expected here */

    if (is_get)      memcpy(vm_base + ls, vm_base + ea, size);
    else if (is_put) memcpy(vm_base + ea, vm_base + ls, size);
}

/* Set RD_SPU_TRACE=1 to dump every problem-state MMIO access (used to reverse-
 * engineer the demo's raw-SPU protocol). Off by default. */
static int rd_spu_trace(void)
{
    static int t = -1;
    if (t < 0) { const char* e = getenv("RD_SPU_TRACE"); t = (e && *e && *e != '0'); }
    return t;
}

/* sys_raw_spu_mmio_write(id, off, val) */
void rd_hle_spu_mmio_write(ppu_context* ctx)
{
    uint32_t id  = (uint32_t)ctx->gpr[3];
    uint32_t off = (uint32_t)ctx->gpr[4];
    uint32_t val = (uint32_t)ctx->gpr[5];
    if (rd_spu_trace())
        fprintf(stderr, "[rd_spu] W id=%u off=0x%04X val=0x%08X\n", id, off & 0xFFFF, val);
    vm_write32(RAW_SPU_PS_BASE + id * RAW_SPU_STRIDE + off, val);  /* default store */
    if ((off & 0xFFFF) == 0x3014)                                 /* Class_CMD -> issue */
        rd_mfc_dma(id, val);
    if ((off & 0xFFFF) == 0x400C && id < SPU_MAX) {               /* SPU_In_Mbox: PPU->SPU */
        spu_channel_write(&g_spu_ctx[id].ch_in_mbox, val);        /* feed the running SPU */
    }
    if ((off & 0xFFFF) == 0x401C && (val & 1)) {                  /* RunCntl = RUN */
        /* Run the raw SPU (async worker). This is load-bearing: the AsyncCopy SPU
         * does the ready-mailbox handshake that _jsAsyncCopyInit waits on, so the
         * game only reaches GCM/RSX init once the SPU runs. Set RD_SPU_NORUN=1 to
         * disable (falls back to the old DMA-only spin) for A/B debugging. */
        static int norun = -1;
        if (norun < 0) { const char* e = getenv("RD_SPU_NORUN"); norun = (e && *e && *e != '0'); }
        if (!norun) rd_run_spu(id);
    }
}

/* sys_raw_spu_mmio_read(id, off) -> val */
void rd_hle_spu_mmio_read(ppu_context* ctx)
{
    uint32_t id  = (uint32_t)ctx->gpr[3];
    uint32_t off = (uint32_t)ctx->gpr[4];
    uint32_t r;
    /* The raw SPU runs on an async host thread, so its out-mailbox may not be
     * filled yet when the PPU polls. On real hardware the PPU spin-reads MBox_Stat
     * until a value is available; our async timing means a single read can catch
     * avail==0 and let the PPU proceed with stale state (the nondeterministic
     * cParticleFluidLoadShader abort). Block the PPU here until the SPU produces a
     * mailbox value (or stops) so the handshake is deterministic -- matching the
     * PPU's own spin, just without the race window. Bounded so a genuinely idle
     * SPU can't wedge the PPU forever. */
    if (id < SPU_MAX && ((off & 0xFFFF) == 0x4004 || (off & 0xFFFF) == 0x4014)
        && (g_out_mbox_wr[id] - g_out_mbox_rd[id]) <= 0 && g_spu_running[id]) {
        for (int spins = 0; spins < 2000000 && g_spu_running[id]
             && (g_out_mbox_wr[id] - g_out_mbox_rd[id]) <= 0; spins++)
            YieldProcessor();
    }
    long avail = (id < SPU_MAX) ? (g_out_mbox_wr[id] - g_out_mbox_rd[id]) : 0;
    if (avail < 0) avail = 0;
    switch (off & 0xFFFF) {
        case 0x3104: r = 0xFFFFFFFFu; break;   /* all tag groups complete */
        case 0x3014: r = 0;          break;    /* CMDStatus: command accepted */
        case 0x4004:                           /* SPU_Out_Mbox: pop one */
            if (id < SPU_MAX && avail > 0) { r = g_out_mbox[id][g_out_mbox_rd[id] & 15]; g_out_mbox_rd[id]++; }
            else r = 0;
            break;
        case 0x4014:                           /* SPU_MBox_Stat */
            /* byte3 [0x000000FF] = out-mbox count (the game tests bit 0 = "one
             * available"; the HW out-mbox is 1-deep, read one at a time), byte2
             * [0x0000FF00] = in-mbox FREE slots. */
            r = (uint32_t)(avail > 0 ? 1 : 0) | (0x04u << 8);
            break;
        case 0x4024:                           /* SPU_Status: running(1)/stopped */
            r = (id < SPU_MAX) ? g_spu_ctx[id].status : SPU_STATUS_STOPPED;
            break;
        default:     r = vm_read32(RAW_SPU_PS_BASE + id * RAW_SPU_STRIDE + off); break;
    }
    if (rd_spu_trace())
        fprintf(stderr, "[rd_spu] R id=%u off=0x%04X -> 0x%08X\n", id, off & 0xFFFF, r);
    ctx->gpr[3] = r;
}


/* ---------------------------------------------------------------------------
 * jsGcm AsyncCopy override.
 *
 * fast_memcpy (func_0005A548) delegates large (>=0x80, 16-aligned) copies to the
 * raw-SPU AsyncCopy: _jsAsyncCopy(dst,src,size) issues an SPU-driven DMA and
 * _jsAsyncCopyFinish waits for the SPU program to signal completion. The lifted
 * AsyncCopy SPU program (spu_0004, entry LS 0xE0) infinite-loops (a spu_lifter
 * r1-stack-drift bug), so _jsAsyncCopyFinish blocks forever -- which is where
 * the demo hangs during the 1 MB duck-texture upload.
 *
 * The AsyncCopy is a linear DMA copy (dst,src,size), so do it synchronously on
 * the host and make Finish a no-op. This bypasses the buggy SPU copy loop; the
 * SPU worker itself stays running for the AsyncCopy *init* handshake (which the
 * demo needs -- RD_SPU_NORUN=1 stalls earlier). ponytail: a targeted host copy
 * around a known spu_lifter loop bug; the real fix is SPU r1 frame management.
 * -----------------------------------------------------------------------*/
void rd_hle_jsasynccopy(ppu_context* ctx)      /* _jsAsyncCopy(dst, src, size) */
{
    uint32_t dst  = (uint32_t)ctx->gpr[3];
    uint32_t src  = (uint32_t)ctx->gpr[4];
    uint32_t size = (uint32_t)ctx->gpr[5];
    if (vm_base && size && size < 0x10000000u)
        memcpy(vm_base + dst, vm_base + src, size);
    { static int cap = -1, n = 0;
      if (cap < 0) { const char* e = getenv("RD_COPY_DBG"); cap = e ? atoi(e) : 0; }
      if (cap && n < cap) { n++;
        uint32_t nz = 0;
        for (uint32_t i = 0; vm_base && i < size && i < 0x4000u; i += 37)
            if (vm_base[dst + i]) nz++;
        fprintf(stderr, "[RDCOPY] dst=0x%08X src=0x%08X size=0x%X  dst-nonzero=%u/%u\n",
                dst, src, size, nz, (size < 0x4000u ? size : 0x4000u) / 37 + 1); } }
    ctx->gpr[3] = 0;
}

void rd_hle_jsasynccopyfinish(ppu_context* ctx) /* _jsAsyncCopyFinish() -> already done */
{
    ctx->gpr[3] = 0;
}

/* Preserve r31 across the Cg program-generation calls in _jsCgCreateProgram: a
 * deep callee in that chain corrupts r31 (frame-pointer) by overflowing an
 * ancestor stack frame, so _jsCgCreateProgram's later name-slot read AND its
 * epilogue use a bad r31 -> invalid CGprogram handle + cascading corruption of
 * the context/other handles -> shader-load abort. Snapshot r31 before each
 * generate call and restore it after. ponytail: mitigation for a residual
 * ppu_lifter guest-frame overflow; the real fix is lifter frame handling. */
static unsigned g_rd_r31 = 0;
void     rd_r31_save(unsigned v) { g_rd_r31 = v; }
unsigned rd_r31_load(void)       { return g_rd_r31; }
