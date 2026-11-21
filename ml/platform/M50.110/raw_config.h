/**
 * EOS M50 1.1.0 - RAW LiveView / mlv_lite configuration
 *
 * EVERY value in this file that is marked "UNVERIFIED" was NOT taken from
 * an M50 ROM.  It must be found in a ROM dump of firmware 1.1.0 (Ghidra),
 * or with the discovery hooks in include/platform/mmu_patches.h, and then
 * confirmed on a real camera before anyone records with this build.
 *
 * A build made with   make M50_RAW_PLACEHOLDERS=1   compiles with dummy
 * values so the code can be type-checked, but raw.c refuses to start the
 * RAW stream at runtime (see CONFIG_RAW_PLACEHOLDER_ADDRS in raw.c).
 *
 * Search guide for each value is next to it.  Order of work:
 *   1. M50_RAW_LV_EDMAC_CHANNEL_ADDR   (first light: nothing works without it)
 *   2. M50_FRAME_COPY_BACKEND          (start with CPU, then M2M)
 *   3. M50_PACK32_MODE_REG             (only needed for 10/12-bit)
 */
#ifndef __M50_RAW_CONFIG_H__
#define __M50_RAW_CONFIG_H__

/* ---------------------------------------------------------------------
 * 1. LiveView RAW EDMAC channel ("port" on DIGIC 8)
 *
 * The MMIO block of the EDMAC channel that Canon's LiveView pipeline
 * writes the bayer stream to, after call("lv_set_mm", 1) and
 * call("lv_save_raw", 1).  raw.c reads ->ram_addr and ->yb_xb from it.
 *
 * How to find (ROM, firmware 1.1.0):
 *  - Ghidra: find the string "lv_save_raw" -> its eventproc handler ->
 *    follow to the function that programs a write port.  It is passed
 *    a channel/port number and a struct edmac_info (D8 layout, edmac.h).
 *  - Or enable CONFIG_M50_RAW_DISCOVERY and fill in the hook addresses in
 *    mmu_patches.h; the hook logs every (port, ram_addr, yb_xb) Canon
 *    programs. Toggle lv_save_raw and diff the log.
 *  - Cross-check: on 200D (DIGIC 7) the answer was 0xd0058000 (channel 24).
 *    DIGIC 8 port MMIO is believed to be in the 0xD0xxxxxx range too,
 *    but the layout differs (see struct edmac_mmio, CONFIG_DIGIC_8X).
 * ------------------------------------------------------------------- */
/* FOUND in ROM 1.1.0 (2026-11-17), confidence: high, NOT yet verified on camera.
 *  - Canon's EDMAC driver (ELD/Edmac/Edmac.c) keeps a port table at
 *    0xE0DD7BA0: 76 entries of {MMIO base, ModeInfo}. Ports 0..38 are write,
 *    39..75 read. Port 3 = 0xD0420200 (ModeInfo 0x82411, write).
 *  - "set address" is 0xE054A9AE: str r1, [table[port].base + 0xA0]
 *    -> ram_addr at +0xA0, matching struct edmac_mmio (CONFIG_DIGIC_8X).
 *  - Controller/CapPath/Mem1Path/Mem1Path.c, function 0xE080D36C:
 *    if lv_save_raw is on (0x12748+0x40, set by the lv_save_raw eventproc
 *    0xE00DB9BC -> 0xE080D4F2), it programs PORT 3 with
 *    xb = width*7/4 (14-bit packed line), yb = height-1, address = frame
 *    buffer, pack mode from 0x12748+0x64 (lv_set_mm changes +0x70).
 *  - Ports 5/6 in the same module are the dual-pixel AF raw (DafRaw A/B),
 *    NOT the image.
 * Verify on camera: Debug > M50 RAW probe > RAW stream ON; "EDMAC port"
 * must show a nonzero ram_addr and x = width*14/8. */
#ifndef M50_RAW_LV_EDMAC_CHANNEL_ADDR
  #ifdef CONFIG_RAW_PLACEHOLDER_ADDRS
    #define M50_RAW_LV_EDMAC_CHANNEL_ADDR 0xD00FFF00 /* PLACEHOLDER - never dereferenced */
  #else
    #define M50_RAW_LV_EDMAC_CHANNEL_ADDR 0xD0420200 /* EDMAC write port 3 */
  #endif
#endif
#define RAW_LV_EDMAC_CHANNEL_ADDR M50_RAW_LV_EDMAC_CHANNEL_ADDR

/* MMIO window we allow shamem_read() to touch directly (function_overrides.c).
 * Must contain RAW_LV_EDMAC_CHANNEL_ADDR. Reads outside return 0, which is
 * what the stock M50 port does for everything. */
#define M50_MMIO_READ_WINDOW_START  (RAW_LV_EDMAC_CHANNEL_ADDR)
#define M50_MMIO_READ_WINDOW_END    (RAW_LV_EDMAC_CHANNEL_ADDR + 0x100)

/* ---------------------------------------------------------------------
 * 2. Frame copy backend (function_overrides.c)
 *
 *  M50_COPY_CPU  - plain memcpy by the ARM core. Needs no stubs. Slow
 *                  (expect well under card speed for 1080p) but it is the
 *                  safest way to get first frames onto the card.
 *                  Forces full-width capture like the 7D2 port.
 *  M50_COPY_DMA  - Canon dma_memcpy() (BLTDMA). Needs stub M50_STUB_dma_memcpy.
 *                  Full width only.
 *  M50_COPY_M2M  - Canon mem-to-mem EDMAC rectangle copy, as on 200D.
 *                  Needs stub M50_STUB_mem_to_mem_edmac_copy and two free
 *                  EDMAC ports (M50_M2M_CHAN_1/2). Allows arbitrary crops.
 * ------------------------------------------------------------------- */
#define M50_COPY_CPU 0
#define M50_COPY_DMA 1
#define M50_COPY_M2M 2

#ifndef M50_FRAME_COPY_BACKEND
#define M50_FRAME_COPY_BACKEND M50_COPY_CPU
#endif

/* M50 ROM: Canon's generic mem-to-mem object is
 * EHD/General/MemoryToMemoryEsub5 (0xE084DE9C init, 0xE084DF70 set addresses,
 * 0xE084DF8A set sizes + start, 0xE084DF26 term); it pairs a write port with
 * read ports 58..63. Not wired up yet - M2M backend still needs porting.
 * UNVERIFIED: 200D used 48/19 after the defaults (43/17) clashed with LV.
 * On M50, log m2m calls Canon makes (hook in mmu_patches.h) and pick
 * ports never used while LiveView + H.264 recording runs. */
#define M50_M2M_CHAN_1 48
#define M50_M2M_CHAN_2 19

/* ---------------------------------------------------------------------
 * 3. Bit depth: PACK32 mode register (raw.c, _EngDrvOut whitelist)
 *
 * On 200D this is 0xd0008094 (14-bit=0x20, 12=0x10, 10=0x0, and 10/12
 * still glitch every other frame). On DIGIC 8 unknown. If left at 0,
 * _EngDrvOut() drops every write and only 14-bit is usable - which is
 * fine for first light.
 * ------------------------------------------------------------------- */
#ifndef M50_PACK32_MODE_REG
#define M50_PACK32_MODE_REG 0   /* 0 = unknown -> 14-bit only */
#endif
/* M50 ROM lead (not used yet): on DIGIC 8 bit depth is not one register.
 * Mem1Path passes a 5-byte pack config at RAM 0x12748+0x64 to the EDMAC
 * driver's write-pack setter 0xE054B99A ("PackUnpackInfo") for port 3.
 * 10/12-bit = changing that config before Mem1Path programs the port. */

/* UNVERIFIED: used only in photo-mode LV for raw histogram white level.
 * Reads outside the MMIO window return 0; raw.c guards against 0. */
#define SHAD_GAIN_REGISTER 0

/* ---------------------------------------------------------------------
 * 4. Polled vsync (raw_vsync.c)
 *
 * The M50 port keeps CONFIG_STATE_OBJECT_HOOKS off (hooking EVF_STATE
 * crashes in EvfCap). Without it ML never gets a per-frame callback, so
 * mlv_lite would never be told a frame is ready. raw_vsync.c instead runs
 * a task that polls for a new frame and fires CBR_VSYNC.
 * ------------------------------------------------------------------- */
#define M50_VSYNC_POLL_MS 1

#endif /* __M50_RAW_CONFIG_H__ */
