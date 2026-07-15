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

static void rd_spu_mbox_hook(uint32_t /*grp*/, uint32_t sid, uint32_t which, uint32_t val)
{
    if (which != 0 || sid >= SPU_MAX) return;            /* which 0 = WrOutMbox */
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

    if (g_dma_count < 30)
        fprintf(stderr, "[rd_spu] DMA#%u id=%u cmd=0x%02X %s ls=0x%08X ea=0x%08X size=%u%s\n",
                g_dma_count, id, cmd, is_get ? "GET" : is_put ? "PUT" : "?",
                ls, ea, size, is_list ? " [LIST!]" : "");
    g_dma_count++;

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
