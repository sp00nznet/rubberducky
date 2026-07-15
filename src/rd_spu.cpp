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
#include <cstdio>
#include <cstring>
#include <cstdlib>   /* getenv */

extern "C" uint8_t* vm_base;

#define RAW_SPU_LS_BASE   0xE0000000u   /* per-SPU local store  */
#define RAW_SPU_PS_BASE   0xE0040000u   /* per-SPU problem state */
#define RAW_SPU_STRIDE    0x00100000u

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
}

/* sys_raw_spu_mmio_read(id, off) -> val */
void rd_hle_spu_mmio_read(ppu_context* ctx)
{
    uint32_t id  = (uint32_t)ctx->gpr[3];
    uint32_t off = (uint32_t)ctx->gpr[4];
    uint32_t r;
    switch (off & 0xFFFF) {
        case 0x3104: r = 0xFFFFFFFFu; break;   /* all tag groups complete */
        case 0x3014: r = 0;          break;    /* CMDStatus: command accepted */
        default:     r = vm_read32(RAW_SPU_PS_BASE + id * RAW_SPU_STRIDE + off); break;
    }
    if (rd_spu_trace())
        fprintf(stderr, "[rd_spu] R id=%u off=0x%04X -> 0x%08X\n", id, off & 0xFFFF, r);
    ctx->gpr[3] = r;
}
