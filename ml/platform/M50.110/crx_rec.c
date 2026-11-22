/** \file
 * EOS M50: compressed RAW video using Canon's CR3/C-RAW hardware encoder.
 *
 * Debug -> M50 RAW probe -> "CRX recording" (dial on a photo mode, P/Av/M).
 *
 * Pipeline per LiveView frame (any LV RAW stream: photo 2096x838, movie 1080p 2096x1164, 4K 4368x2202):
 *  1. EDMAC hook (Canon capture task): Canon writes the frame into sink[cur];
 *     when it is complete the hook hands it to the repack worker and points
 *     Canon at the other sink. If the worker is busy, the frame is dropped
 *     (Canon overwrites it) - never torn.
 *  2. Repack worker (second CPU core if available): ML/LiveView 14-bit packing
 *     -> Canon photo packing (LSB-first stream) into one of two encoder inputs.
 *     (Verified bit-exact on host; the encoder rejects LiveView packing.)
 *  3. Encoder task: AlgsMgr request 0xE0795850 with
 *       { mode (0 lossless, 4 C-RAW), w, h, 0, 0, 14, input, stride_px }
 *     - the same block Canon uses for the CR3 main image. The callback returns
 *     a handle: +0x08/+0x0C CMP1 header addr/size, +0x18 entries,
 *     +0x1C addr[] / +0x20 size[]: [0] = CRX data header, [1..8] planes.
 *     Headers live in buffers Canon reuses, so they are copied right away.
 *  4. Writer task: writes header block + the planes straight from the
 *     encoder's (uncached) buffers to the card, then frees the handle with
 *     AlgsMgr event 4 (arg = pointer to handle), like Canon's file writer.
 *
 * File: ML/CRX/CRXnnnnn.CRX (chunks .C01, .C02 ... below the FAT32 limit).
 *   file header "CRXV", then per frame "CRXF" blocks holding the CR3 CMP1
 *   header and the CRX "mdat" (data header + planes) of that frame.
 *   Decode with LibRaw's CR3 decoder (crxParseImageHeader + crxLoadRaw).
 * Stats: ML/LOGS/CRXREC.LOG
 *
 * GPLv2, same as Magic Lantern.
 */

#include <dryos.h>
#include <exmem.h>
#include <config.h>
#include <consts.h>
#include <tasks.h>
#include <menu.h>
#include <bmp.h>
#include <raw.h>
#include <mem.h>
#include <fio-ml.h>
#include <timer.h>
#include <propvalues.h>
#include <property.h>
#include <fps.h>

#if defined(CONFIG_RAW_LIVEVIEW) && defined(CONFIG_M50_EDMAC_HOOK) && !defined(M50_SPY_BUILD)

#include "crx_repack.h"

/* ---- Canon (M50 1.1.0) ---- */
#define ALGS_REQUEST        0xE0795851      /* (req*, 1, cbr, ctx), Thumb */
#define FREE_ENC_BUFFER     0xE0795D2F      /* ALGSMGR_FreeEncBuffer(handle, cbr, ctx), Thumb.
                                             * Canon's writer calls it with (h, 0, 0). It mallocs a
                                             * {h, cbr, ctx} block, posts event 4, and the state
                                             * machine later calls cbr(ctx) if set and FreeMemory()s
                                             * the block. test25-28 posted event 4 with a pointer to
                                             * a static array instead: the handler called the next
                                             * array word as a function and freed a static address. */
#define ALGSMGR_PTR         0x5A20          /* *(0x5A20) = AlgsMgr; +4 = state machine */
#define ALGS_GEOMETRY_VALID 0x108           /* one word per codec ID, checked by 0xE024DC3E */
#define CBR_OK              0x24            /* success: cbr(ctx, handle, 0x24); error: cbr(ctx, err, 0) */
#define MAX_TILE_W          3144            /* Canon splits wider images into tiles */

struct algs_req { uint32_t mode, w, h, p3, p4, bits, addr, stride; };

/* ---- settings ---- */
CONFIG_INT("crx.mode", m50_crx_mode, 1);
CONFIG_INT("crx.core", m50_crx_core, 1);
CONFIG_INT("crx.hwpack", m50_crx_hwpack, 1);
CONFIG_INT("crx.sample", m50_crx_sample, 0);
CONFIG_INT("crx.qtest", m50_crx_qtest, 0);
CONFIG_INT("crx.shutter", m50_crx_shutter, 1);
static const uint8_t crx_quant_add[] = { 0, 2, 3, 4, 5, 6 };
static volatile int qtest_active;
static volatile uint32_t qtest_frames;
static volatile uint8_t qtest_first;

extern void m50_crx_load_encoder(void *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);

/* The driver consumes these descriptors synchronously before starting CRX.
 * Preserve Canon's values for any subsequent work on the same descriptors. */
void m50_crx_quant_load(void *config, uint32_t planes, uint32_t levels,
                       uint32_t width, uint32_t bits, uint32_t registers)
{
    uint8_t *descriptors = ((uint8_t **) config)[5];
    uint8_t saved[40];
    int strength = qtest_active;
    int change = strength > 0 && strength < COUNT(crx_quant_add) && planes == 4 && levels == 3 &&
                 descriptors && *(const uint16_t *) descriptors == 0x0FF6;
    if (change)
    {
        for (int band = 0; band < 40; band++)
        {
            uint8_t *q = descriptors + band * 16 + 6;
            saved[band] = *q;
            if (band % 10 >= 4)
                *q = (*q & 0x80) | MIN((*q & 0x7F) + crx_quant_add[strength], 127);
        }
        if (!qtest_frames) qtest_first = saved[9] & 0x7F;
        qtest_frames++;
    }
    m50_crx_load_encoder(config, planes, levels, width, bits, registers);
    if (change)
        for (int band = 0; band < 40; band++)
            descriptors[band * 16 + 6] = saved[band];
}

enum crx_depth { CRX_14BIT = 0, CRX_12BIT_LV = 1 };
CONFIG_INT("crx.depth", m50_crx_depth, CRX_12BIT_LV);
static uint32_t record_bits = 14;
static int record_native;
volatile uint32_t m50_crx_native_pitch;
#define SENSOR_MODE_NOW (*(volatile uint32_t *)(0xEF24 + 8))
CONFIG_INT("crx.crop", m50_crx_crop, 4);
/* crx.crop remains readable for cards configured by older builds. */
CONFIG_INT("crx.aspect", crx_aspect, -1);
CONFIG_INT("crx.resolution", crx_resolution, 4);

enum { ASPECT_WIDE, ASPECT_SENSOR, ASPECT_STANDARD, ASPECT_SQUARE, ASPECT_SCOPE, ASPECT_STREAM };
static const char *aspect_names[] = {
    "16:9", "3:2 (M50 sensor)", "4:3", "1:1", "Scope (~2.39:1)", "Full stream"
};
static const struct { unsigned x, y; } aspects[] = {
    {16, 9}, {3, 2}, {4, 3}, {1, 1}, {239, 100}, {0, 0}
};
/* Scope heights retain the existing recorder's three cinema crops. */
static const struct { const char *name; unsigned width, scope_height; } resolutions[] = {
    { "4K UHD", 3840, 1608 }, { "3.5K", 3584, 1504 }, { "3.4K", 3456, 1448 },
    { "3.2K", 3200, 1336 }, { "3K", 3072, 1288 }, { "2.8K", 2880, 1200 },
    { "2.5K", 2560, 1072 }, { "Full HD", 1920, 800 }, { "HD", 1280, 536 },
    { "1K", 1024, 432 }, { "720 wide", 720, 304 }
};

static int format_height(int aspect, int resolution)
{
    if (aspect == ASPECT_STREAM) return 0;
    if (aspect == ASPECT_SCOPE) return resolutions[resolution].scope_height;
    unsigned x = aspects[aspect].x;
    return 8 * ((resolutions[resolution].width * aspects[aspect].y + 4 * x) / (8 * x));
}

static void normalize_format(void)
{
    if (crx_aspect == -1)
    {
        static const uint8_t legacy[][2] = {
            { ASPECT_STREAM, 0 }, { ASPECT_SCOPE, 1 }, { ASPECT_WIDE, 1 },
            { ASPECT_SCOPE, 4 }, { ASPECT_WIDE, 4 }, { ASPECT_SCOPE, 5 }, { ASPECT_WIDE, 6 }
        };
        unsigned old = (unsigned)m50_crx_crop < COUNT(legacy) ? m50_crx_crop : 4;
        crx_resolution = legacy[old][1];
        crx_aspect = legacy[old][0];
    }
    if ((unsigned)crx_aspect >= COUNT(aspects)) crx_aspect = ASPECT_WIDE;
    if ((unsigned)crx_resolution >= COUNT(resolutions)) crx_resolution = 4;
    while (format_height(crx_aspect, crx_resolution) > 2160) crx_resolution++;
}

static uint32_t enc_w, enc_h, enc_off;   /* encoder input size and crop offset in the capture buffer */
static uint32_t pc_x0b, pc_rowb;                 /* test67 pre-crop: byte offset of the crop's first column, bytes per cropped row */
static uint8_t *pc_buf[2];                       /* cropped or 12-bit input per sink (stride == width) */
static volatile uint32_t st_pc_us, st_pc_max, st_pc_bad;
static volatile int rec_live = 0;           /* 0 = encoder warm-up (first encodes take ~0.4 s): frames not written */                     /* 1: write only 1 frame in 24 (pipeline test on a slow card) */
static volatile uint64_t st_enc_bytes = 0;  /* every encoded frame reaching the writer, written or not */

/* ---- state ---- */
enum { CRX_SD_TEST = 4 };
static volatile int rec_state = 0;          /* 0 idle, 1 starting, 2 recording, 3 stopping, 4 SD write test */
static volatile int routing = 0;            /* hook routes frames to us */
static volatile int stop_req = 0;
static volatile int encoder_stalled;
static volatile int native_encode_busy;      /* queued native job still being handled by the encoder task */

static void *sink_mem[2];
static void * volatile sink[3];             /* uncached */
static int sink_count = 2;
static volatile int sink_owned[3];          /* queued or encoding in three-buffer mode */
static unsigned st_sink_queued;
enum { CRX_MSG_SINK = 2, CRX_MSG_THIRD_SINK = 4 };
static void *encin_mem[2];
static uint8_t * volatile encin[2];         /* uncached, 64k aligned */
static volatile int encin_state[2];         /* 0 free, 1 repacking, 2 encoding */
static volatile uint64_t sink_ts[2];
static volatile int cur = 0;
static volatile uint32_t last_given = 0;
static volatile int worker_busy = 0;
static uint32_t frame_bytes, pitch, width, height, alloc_bytes = 0;

/* ---- hardware packing (ROM 1.1.0 static analysis, 2026-11-18) ----
 * Port 3 (LV RAW) goes through PackUnpack unit 2 (table 0xE0DD7A70 -> 0xE0DD7E00),
 * register 0xD0422224, written by 0xE054B912 as
 *   cfg[1]<<20 | cfg[4]<<16 | cfg[0]<<8 | cfg[2]<<4 | cfg[3]
 * cfg[2] = bit depth (RawPro: 0 = 10, 1 = 12, 2 = 14 bit).
 * Mem1Path (LV RAW, RAM 0x12748+0x64) uses {0,1,2,1,0}; RawPro (still RAW,
 * which C-RAW encodes) uses {0,0,depth,1,0}. The only difference is cfg[1] =
 * bit 20: candidate for "ML/LiveView packing" vs "Canon photo packing".
 * Verified per recording on the first frames (roughness test), with automatic
 * fallback to the CPU repack. */
#define PACK_REG            0xD0422224
#define MEM1PATH_PACK_CFG   0x127AC          /* 0x12748 + 0x64: cfg[0..4] */
/* Candidates, tried in order on the first frames of a recording (roughness test each).
 * test37: bit 20 (cfg1) alone does NOT change the bit order.
 * Now: cfg3 = bit 0. RawPro (still RAW, what C-RAW encodes) uses cfg3 = 1; LiveView RAW has
 * cfg3 = 0 because ML calls lv_set_mm(1), which stores cfg3 = !arg (0xE080CE5E).
 * Same bit depth field (cfg2) in all candidates, so line size and buffers are unchanged. */
static const struct { uint32_t set, clear; int cfg1, cfg3; const char *name; } pack_cand[] = {
    { 0x00000001, 0,          -1, 1, "cfg3=1 (bit 0)" },
    { 0x00000001, 0x00100000,  0, 1, "cfg3=1 + cfg1=0 (= RawPro)" },
};
#define NCAND (int)(sizeof(pack_cand) / sizeof(pack_cand[0]))
static volatile int hwpack_active = 0;      /* 1 testing, 2 confirmed */
static int hwpack_cand = 0;
static uint32_t saved_cfg[5], saved_pack;
static int hw_rough_canon = -1, hw_rough_ml = -1;
static volatile int hw_fell_back = 0;
static int diag_e = 0;
static volatile uint32_t st_copy_us = 0;      /* first-frames diagnostics, reset per recording */

static void hwpack_apply(int k)
{
    volatile uint32_t *cfg = (volatile uint32_t *) MEM1PATH_PACK_CFG;
    for (int i = 0; i < 5; i++) cfg[i] = saved_cfg[i];
    if (pack_cand[k].cfg1 >= 0) cfg[1] = pack_cand[k].cfg1;
    if (pack_cand[k].cfg3 >= 0) cfg[3] = pack_cand[k].cfg3;
    *(volatile uint32_t *) PACK_REG = (saved_pack & ~pack_cand[k].clear) | pack_cand[k].set;
    hwpack_cand = k;
}

static void hwpack_enable(void)
{
    volatile uint32_t *cfg = (volatile uint32_t *) MEM1PATH_PACK_CFG;
    for (int i = 0; i < 5; i++) saved_cfg[i] = cfg[i];
    saved_pack = *(volatile uint32_t *) PACK_REG;
    hwpack_apply(0);
    hwpack_active = 1;
}

static void hwpack_restore(void)
{
    if (!hwpack_active) return;
    hwpack_active = 0;
    volatile uint32_t *cfg = (volatile uint32_t *) MEM1PATH_PACK_CFG;
    for (int i = 0; i < 5; i++) cfg[i] = saved_cfg[i];
    *(volatile uint32_t *) PACK_REG = saved_pack;
}

/* Port 3 uses the same pack configuration as Canon's 12-bit still RAW. */
#define NATIVE_PACK_MASK 0x00100031
#define NATIVE_PACK_BITS 0x00000011

static void native_raw_start(void)
{
    call("lv_set_mm", 1);
    volatile uint32_t *cfg = (volatile uint32_t *) MEM1PATH_PACK_CFG;
    for (int i = 0; i < 5; i++) saved_cfg[i] = cfg[i];
    saved_pack = *(volatile uint32_t *) PACK_REG;
    cfg[1] = 0;
    cfg[2] = 1;
    cfg[3] = 1;
    m50_crx_native_pitch = pitch;
    asm volatile("dsb sy" ::: "memory");
    call("lv_save_raw", 1);
}

static void native_raw_stop(void)
{
    if (!m50_crx_native_pitch) return;
    call("lv_save_raw", 0);
    msleep(50);
    m50_crx_native_pitch = 0;
    hwpack_active = 0;
    volatile uint32_t *cfg = (volatile uint32_t *) MEM1PATH_PACK_CFG;
    for (int i = 0; i < 5; i++) cfg[i] = saved_cfg[i];
    *(volatile uint32_t *) PACK_REG = saved_pack;
}

/* sum |p[i] - p[i+2]| over n pixels of one row, decoded either way */
static int row_roughness(const uint8_t *row, int n, int canon)
{
    int sum = 0, prev[2] = { -1, -1 };
    const uint16_t *h = (const uint16_t *) row;
    for (int i = 0; i < n; i++)
    {
        int v;
        if (canon)
        {
            uint32_t bit = i * 14, byte = bit >> 3, sh = bit & 7;
            uint32_t w = row[byte] | (row[byte + 1] << 8) | (row[byte + 2] << 16);
            v = (w >> sh) & 0x3FFF;
        }
        else
        {
            /* 8 px in 7 LE u16, MSB-first (raw.c layout) */
            const uint16_t *b = h + (i >> 3) * 7; int k = i & 7;
            uint32_t bit = k * 14, wi = bit >> 4, sh = bit & 15;
            uint32_t w = ((uint32_t) b[wi] << 16) | (wi < 6 ? b[wi + 1] : 0);
            v = (w >> (32 - 14 - sh)) & 0x3FFF;
        }
        if (prev[i & 1] >= 0) sum += ABS(v - prev[i & 1]);
        prev[i & 1] = v;
    }
    return sum;
}

/* 1 = frame is in Canon packing */
static int frame_is_canon_packed(const uint8_t *frame)
{
    int rc = 0, rm = 0;
    for (int r = 1; r <= 4; r++)
    {
        const uint8_t *row = frame + (height * r / 5) * pitch + 64 * 14;
        rc += row_roughness(row, 512, 1);
        rm += row_roughness(row, 512, 0);
    }
    hw_rough_canon = rc; hw_rough_ml = rm;
    return rc * 4 < rm * 3;
}


static struct msg_queue *work_mq[2], *enc_mq;
static struct semaphore *enc_sem;
static volatile uint32_t enc_posted, enc_taken;
static volatile int worker_alive[2];

/* write jobs */
#define NJOBS 16
/* Handle: +0x10 tiles, +0x14 shift amounts, +0x18 entry count, +0x1C addr[], +0x20 size[]
 * (free handler 0xE024B1C4). Lossless 2096x838: 9 entries (0x70-byte mdat header + 8);
 * C-RAW: 81 entries with a 544-byte header (test37 log). */
#define MAXPL 128
struct job
{
    volatile int used;
    void *handle;
    uint32_t frame_no;
    uint64_t ts;
    uint32_t cmp1_len, mdh_len, nplanes;
    uint32_t plane_addr[MAXPL], plane_len[MAXPL];
    uint8_t cmp1[64];
    uint8_t mdh[1024];
};
/* test59: one RING buffer instead of slots. Frames are packed back to back (pieces padded to 32 bytes);
 * the writer sends big chunks (up to WR_MAX, cut at 4 MB file offsets = SD allocation units).
 * Ring + raw sinks live in Canon's movie RecWork area when it was handed to us, else in fio memory. */
#define WR_MAX   (28u << 20) /* 7 x 4 MiB AU; stays under SDDRV's 0xF800 sector (31.0 MiB) single-CMD25 cap */
#define WR_AU    (4u << 20)
#define RING_TARGET (192u << 20)
#define NFE 256
static uint8_t *ring;                       /* uncached */
static uint32_t ring_size;
static volatile uint32_t r_wp, r_rp, r_wrap = 0xFFFFFFFF;
static volatile uint64_t r_prod, r_cons;    /* bytes produced / written (absolute) */
static volatile uint64_t fe[NFE] __attribute__((section(".bss.crx_frame_ends")));           /* absolute end of each frame (for 4 GB file splits) */
static volatile uint32_t fe_w;
static volatile int wr_flush;
static void * volatile rw_addr, * volatile rw_late;   /* Canon's movie RecWork, ours while recording */
static volatile uint32_t rw_size;
static uint32_t ring_used(void) { uint32_t w = r_wp, r = r_rp; return w >= r ? w - r : (r_wrap - r) + w; }

/* ---- Canon SRM "1st job" memory (static, 2026-11-18) ----
 * SRM_AllocateMemoryResourceFor1stJob = 0xE056A5C6: named by Canon's own error string
 *   (FA_RemoteRelease 0xE0087454 calls it, then logs "...For1stJob failed"). Canon's callback
 *   0xE00873EE is cbr(ctx, buffer): TWO args, no size (ML's exmem.c asserted on a size arg).
 *   Canon waits up to 5 s for it (ML's exmem.c only waited 100 ms).
 * SRM_FreeMemoryResourceFor1stJob = 0xE056D7E6 (buffer, 0, 0): FA_FinishRemoteRelease
 *   0xE008737C frees the same buffer with it. The old stub 0xE056DA22 was a different function.
 * Size: AlgsMgr's grant in test48 held two ranges of exactly 0x2AB1000 (42.7 MB) each; the
 *   200D's 1st-job buffer is 0x2A9C000. We use at most 32 MB of it. */
/* ---- Canon movie-recorder buffers (MovRecResource.c, static 2026-11-18) ----
 * object *(0xEB28): +0 log id, +4 ?, +8 dwClientNum, +0x24/+0x28 stream addr/size,
 * +0x2C/+0x30 RecWork addr/size, +0x34/+0x38 writer work, +0x54 transcode, +0x58 startup flag.
 * MRRSC_Startup(flag, transcode) 0xE02F4F7C: client count +1; first client allocates
 *   (SRM_AllocateMemoryResourceForMovieRecWork 0xE056ABAA, cbr(ctx, addr, size)).
 *   Called from mvrRecStart and AllocateCvram.
 * MRRSC_Shutdown() 0xE02F54DC: count -1; last client frees everything
 *   (SRM_FreeMemoryResourceForMovieRecWork 0xE056DCDE). Called from mvrRecStopped and FreeCvram.
 * Idea: in movie mode Canon holds these buffers for H.264 while we don't use H.264. Give them back
 * for the time we record, then take them again exactly the way Canon does. */
#define MRRSC_OBJ      0xEB28
#define MRRSC_STARTUP  0xE02F4F7D
#define MRRSC_SHUTDOWN 0xE02F54DD
/* Movie: free Canon buffers is always enabled (m50_crx_movrel = 1). */
static int movrel_n = 0, movrel_flag = 0;
static void mrrsc_log(const char *when);
static void mrrsc_release(void);
static void mrrsc_restore(void);
/* SRM 1st-job experiment (test50, hung) removed in test53 - see RAW_VIDEO.md */
static volatile int writer_busy = 0;
static volatile uint32_t enc_seq = 0;       /* ctx of the request we are waiting for */
static void * volatile late_h[8];           /* results that came after we gave up waiting */
static volatile uint32_t late_w = 0, late_r = 0;

/* encoder callback */
static void * volatile cb_handle;
static volatile uint32_t cb_a2;

/* stats */
static volatile uint32_t st_sd_brakes = 0;
static volatile int st_sd_stepdown_mhz = 0;
static volatile uint32_t st_hooked, st_drop_worker, st_drop_slot, st_drop_queue, st_encoded,
                         st_written, st_enc_err, st_free_err;
static volatile uint64_t st_bytes_out;
static volatile uint32_t st_enc_us, st_enc_max, st_write_ms;
static volatile int st_cache_bad = 0;
static uint64_t rec_t0_us;
static volatile unsigned rec_fps;
static volatile int rec_write_rate10, rec_need_rate10;  /* tenths of decimal MB/s, -1 before a sample */
static uint32_t frame_counter = 0;

static uint32_t crx_camera_drop_count(void)
{
    return st_drop_worker + st_drop_slot + st_enc_err;
}

static int crx_take_drops(void)
{
    return (int)(crx_camera_drop_count() + st_drop_queue);
}

/* log */
#define LOGSIZE 32768
static char *logbuf = 0;                    /* allocated at first start */
static int loglen = 0;
#define LOG(...) do { if (logbuf && loglen < LOGSIZE - 256) \
    loglen += snprintf(logbuf + loglen, LOGSIZE - loglen, __VA_ARGS__); } while (0)

/* file */
static FILE *f = 0;
static char fname[64];
static uint32_t chunk = 0, chunk_bytes = 0;
static uint8_t *iobuf = 0;                  /* DMA memory for our header blocks */

static inline uint32_t us_timer(void) { return *(volatile uint32_t *)0xC0242014; }

static void dcache_range(void *p, uint32_t n, int clean)
{
    uint32_t a = (uint32_t)CACHEABLE(p) & ~31u, end = (uint32_t)CACHEABLE(p) + n;
    if (clean == 2) for (; a < end; a += 32) asm volatile("mcr p15, 0, %0, c7, c14, 1" :: "r"(a)); /* DCCIMVAC */
    else if (clean) for (; a < end; a += 32) asm volatile("mcr p15, 0, %0, c7, c10, 1" :: "r"(a)); /* DCCMVAC */
    else       for (; a < end; a += 32) asm volatile("mcr p15, 0, %0, c7, c6, 1"  :: "r"(a)); /* DCIMVAC */
    asm volatile("dsb sy" ::: "memory");
}

/* whole L1 D-cache of THIS core by set/way: ~1000 ops instead of ~47000 per-line ops per frame */
static void l1_setway(int inval)
{
    uint32_t ccsidr;
    asm volatile("mcr p15, 2, %0, c0, c0, 0\n isb" :: "r"(0));          /* CSSELR: L1 data */
    asm volatile("mrc p15, 1, %0, c0, c0, 0" : "=r"(ccsidr));
    uint32_t lsh = (ccsidr & 7) + 4, ways = ((ccsidr >> 3) & 0x3FF) + 1, sets = ((ccsidr >> 13) & 0x7FFF) + 1;
    uint32_t wsh = ways > 1 ? __builtin_clz(ways - 1) : 0;
    for (uint32_t w = 0; w < ways; w++)
        for (uint32_t st = 0; st < sets; st++)
        {
            uint32_t v = (w << wsh) | (st << lsh);
            if (inval) asm volatile("mcr p15, 0, %0, c7, c14, 2" :: "r"(v));   /* DCCISW */
            else       asm volatile("mcr p15, 0, %0, c7, c10, 2" :: "r"(v));   /* DCCSW */
        }
    asm volatile("dsb sy" ::: "memory");
}

/* =================== 1. EDMAC hook (Canon capture task) =================== */

static struct msg_queue *current_work_mq(void)
{
    int c = (m50_crx_core && worker_alive[1]) ? 1 : 0;
    return work_mq[c];
}

/* Native 4K needs no repack. Post directly to the encoder FIFO so the
 * completed buffers are queued in sensor order. The hook alone picks cur. */
static int crx_queue_native_sink(int current)
{
    for (int i = 1; i < 3; i++)
    {
        int next = (current + i) % 3;
        if (sink_owned[next]) continue;
        asm volatile("dmb sy" ::: "memory");
        sink_owned[current] = 1;
        asm volatile("dmb sy" ::: "memory");
        if (!routing || encoder_stalled)
        {
            sink_owned[current] = 0;
            return current;
        }
        unsigned behind = sink_owned[3 - current - next];
        uint32_t tag = CRX_MSG_SINK | (current == 2 ? CRX_MSG_THIRD_SINK : current);
        uint32_t ts = get_us_clock() - rec_t0_us;
        if (!msg_queue_post(enc_mq, tag | (ts & 0xFFFFFFF0)))
        {
            enc_posted++;
            st_sink_queued += behind;
            return next;
        }
        sink_owned[current] = 0;
        break;
    }
    st_drop_worker++;
    return current;
}

static void crx_release_sink(int index)
{
    asm volatile("dmb sy" ::: "memory");
    if (sink_count == 3) sink_owned[index] = 0;
    else worker_busy = 0;
}

static int crx_capture_busy(void)
{
    return worker_busy || (sink_count == 3 && (sink_owned[0] || sink_owned[1] || sink_owned[2]));
}

/* called first from m50_raw_port3_frame(); 0 = not ours */
uint32_t m50_crx_hook(void)
{
    if (!routing) return 0;

    uint32_t yb_xb = *(volatile uint32_t *)(RAW_LV_EDMAC_CHANNEL_ADDR + 0x50);
    uint32_t bytes = ((yb_xb >> 16) + 1) * (yb_xb & 0xFFFF);
    if ((yb_xb & 0xFFFF) != pitch || bytes > frame_bytes ||
        (record_native && ((*(volatile uint32_t *) PACK_REG & NATIVE_PACK_MASK) != NATIVE_PACK_BITS ||
                           (yb_xb >> 16) + 1 != height)))
    {
        routing = 0;        /* stream changed (mode switch): stop using our sinks */
        stop_req = 1;
        return 0;
    }

    uint32_t s_cur = (uint32_t) sink[cur];
    if (last_given == s_cur)
    {
        st_hooked++;
        if (sink_count == 3)
        {
            if (st_hooked > 3 && enc_mq && !encoder_stalled)
                cur = crx_queue_native_sink(cur);
        }
        else
        {
            struct msg_queue *wq = current_work_mq();
            if (!worker_busy && wq)
            {
                worker_busy = 1;
                sink_ts[cur] = get_us_clock();
                msg_queue_post(wq, cur);
                cur ^= 1;
            }
            else st_drop_worker++;
        }
        s_cur = (uint32_t) sink[cur];
    }
    last_given = s_cur;
    return s_cur;
}

int m50_crx_routing(void) { return routing; }
int m50_crx_recording(void) { return rec_state != 0; }

/* =================== 2. repack worker =================== */

static int cache_ok(void *src)
{
    volatile uint32_t *c = (uint32_t *)CACHEABLE(src);
    volatile uint32_t *u = (uint32_t *)UNCACHEABLE(src);
    uint32_t n = frame_bytes / 4;
    for (int i = 1; i <= 8; i++) if (c[n / 9 * i] != u[n / 9 * i]) return 0;
    return 1;
}

static void crx_worker(void *arg)
{
    int core = (int) arg;
    worker_alive[core] = 1;
    while (1)
    {
        uint32_t idx;
        if (!work_mq[core] || msg_queue_receive(work_mq[core], &idx, 0)) { msleep(10); continue; }
        idx &= 1;
        if (!routing && rec_state != 2) { worker_busy = 0; continue; }
        if (hwpack_active)
        {
            if (st_hooked <= 3) { worker_busy = 0; continue; }     /* frames from before the switch */
            if (hwpack_active == 1)
            {
                const uint8_t *fr = (const uint8_t *) UNCACHEABLE(sink[idx]);
                if (frame_is_canon_packed(fr))
                {
                    hwpack_active = 2;
                    LOG("HW pack %s: OK (%d/%d)\n",
                        pack_cand[hwpack_cand].name, hw_rough_canon, hw_rough_ml);
                }
                else if (hwpack_cand + 1 < NCAND)
                {
                    LOG("HW pack %s: retry (%d/%d)\n",
                        pack_cand[hwpack_cand].name, hw_rough_canon, hw_rough_ml);
                    hwpack_apply(hwpack_cand + 1);
                    st_hooked = 0;          /* skip the frames still in flight */
                    worker_busy = 0;
                    continue;
                }
                else
                {
                    LOG("HW pack %s: CPU fallback (%d/%d)\n",
                        pack_cand[hwpack_cand].name, hw_rough_canon, hw_rough_ml);
                    hwpack_restore();
                    hw_fell_back = 1;
                    if (!encin_mem[0]) { LOG("No CPU repack buffer\n"); stop_req = 1; }
                    st_hooked = 0;          /* skip the next frames again (format changes back) */
                    worker_busy = 0;
                    continue;
                }
            }
            /* zero copy: the encoder reads the sink; it stays ours (worker_busy)
             * until the encoder task is done with it */
            uint64_t ts = sink_ts[idx];
            if (!msg_queue_post(enc_mq, CRX_MSG_SINK | idx | ((uint32_t)(ts - rec_t0_us) & 0xFFFFFFF0)))
                enc_posted++;
            continue;
        }
        if (hw_fell_back && st_hooked <= 3) { worker_busy = 0; continue; }   /* after a fallback */

        int e = encin_state[0] == 0 ? 0 : encin_state[1] == 0 ? 1 : -1;
        if (e < 0) { st_drop_slot++; worker_busy = 0; continue; }
        encin_state[e] = 1;

        void *src = sink[idx];
        const uint16_t *rd = (const uint16_t *) UNCACHEABLE(src);
        if (!st_cache_bad)
        {
            dcache_range(src, frame_bytes, 0);
            if (cache_ok(src)) rd = (const uint16_t *) CACHEABLE(src);
            else st_cache_bad++;
        }
        uint8_t *dst = (uint8_t *) CACHEABLE(encin[e]);
        uint32_t written = frame_bytes;
        if (pc_rowb)
        {
            const uint8_t *row = (const uint8_t *)rd + enc_off;
            for (uint32_t y = 0; y < enc_h; y++)
                repack_blocks_ml_to_canon((const uint16_t *)(row + y * pitch),
                                          dst + y * pc_rowb, pc_rowb / 14);
            written = enc_h * pc_rowb;
        }
        else repack_blocks_ml_to_canon(rd, dst, frame_bytes / 14);
        dcache_range(dst, written, 1);      /* encoder DMA reads RAM */
        uint64_t ts = sink_ts[idx];
        asm volatile("dmb sy" ::: "memory");
        worker_busy = 0;                        /* sink[idx] may be reused now */
        if (!msg_queue_post(enc_mq, e | ((uint32_t)(ts - rec_t0_us) & 0xFFFFFFF0)))
            enc_posted++;
    }
}

/* =================== 3. encoder task (main core) =================== */

#define MAX_INFLIGHT 3
static int jobs_in_flight(void);
static void save_log(void);

/* word copy, 32 bytes per step (both pointers 4-aligned), rest via memcpy */
static void fast_copy(uint8_t *d, const uint8_t *s, uint32_t n)
{
    if ((((uint32_t) d | (uint32_t) s) & 3) == 0)
    {
        uint32_t blocks = n >> 5;
        if (blocks)
        {
            asm volatile(
                "1: pld   [%1, #192]\n"
                "   ldmia %1!, {r3-r6}\n"
                "   stmia %0!, {r3-r6}\n"
                "   ldmia %1!, {r3-r6}\n"
                "   stmia %0!, {r3-r6}\n"
                "   subs  %2, %2, #1\n"
                "   bne   1b\n"
                : "+r"(d), "+r"(s), "+r"(blocks) : : "r3","r4","r5","r6","cc","memory");
        }
        n &= 31;
    }
    if (n) memcpy(d, s, n);
}

/* Pre-crop: PL330 DMA (pc_m = 1), falling back to CPU cached (pc_m = 2) if needed. */
static volatile uint32_t pc_m = 1, st_pc_ovw;
static int pc_allow_wide;
static unsigned st_pc_wide;
/* PL330 crop program (Canon's Xdmac is an ARM PL330). */
static uint8_t *pl_prog, *cp_prog;           /* PL330 program slots in RecWork (uncached) */
/* Crop geometry is fixed for a take. Only the addresses, beat size and
 * channel-specific epilogue can change between completed DMA jobs. */
static int pl_last_channel = -1;
static unsigned pl_last_beat;
#define PL_CCR(beat_shift, len) (1 | ((beat_shift) << 1) | (((len) - 1) << 4) | (1 << 14) | ((beat_shift) << 15) | (((len) - 1) << 18) | 0x1C00700)
static inline uint8_t *pl_mov(uint8_t *p, int rd, uint32_t v) { p[0] = 0xBC; p[1] = rd; p[2] = v; p[3] = v >> 8; p[4] = v >> 16; p[5] = v >> 24; return p + 6; }
static int pl_build(uint8_t *dst, uint8_t *src, uint32_t spitch, uint32_t ch, uint32_t beat_shift)
{
    uint8_t *p = pl_prog;
    uint32_t beat_bytes = 1U << beat_shift;
    uint32_t burst_bytes = 16 * beat_bytes;
    if (((uint32_t) src | (uint32_t) dst | spitch | pc_rowb) & (beat_bytes - 1)) return 0;
    uint32_t nb = pc_rowb / burst_bytes, rem = (pc_rowb % burst_bytes) / beat_bytes, gap = spitch - pc_rowb;
    if (nb < 1 || nb > 256 || gap > 0xFFFF) return 0;
    p = pl_mov(p, 0, (uint32_t) src); p = pl_mov(p, 2, (uint32_t) dst);
    for (uint32_t left = enc_h; left; )
    {
        uint32_t n = MIN(256, left); left -= n;
        *p++ = 0x22; *p++ = n - 1;                          /* DMALP lc1 */
        uint8_t *l1 = p;
        p = pl_mov(p, 1, PL_CCR(beat_shift, 16));
        *p++ = 0x20; *p++ = nb - 1;                         /* DMALP lc0 */
        uint8_t *l0 = p;
        *p++ = 0x04; *p++ = 0x08;                           /* DMALD, DMAST */
        p[0] = 0x38; p[1] = p - l0; p += 2;                 /* DMALPEND lc0: backward jump = LPEND offset - body start */
        if (rem) { p = pl_mov(p, 1, PL_CCR(beat_shift, rem)); *p++ = 0x04; *p++ = 0x08; }
        *p++ = 0x54; *p++ = gap; *p++ = gap >> 8;           /* DMAADDH SAR, gap */
        if (p - l1 > 255) return 0;
        p[0] = 0x3C; p[1] = p - l1; p += 2;                 /* DMALPEND lc1 */
    }
    *p++ = 0x13;                                            /* DMAWMB */
    ((void (*)(uint32_t, uint8_t **)) 0xE0095C8D)(ch, &p);  /* Canon's epilogue: SEV (if this channel interrupts) + END */
    asm volatile("dsb sy" ::: "memory");
    return p - pl_prog;
}
static int pl_run(uint8_t *dst, uint8_t *src, uint32_t spitch, uint32_t beat_shift)
{
    if (!pl_prog) return -1;
    uint32_t *slot = ((uint32_t *(*)(void)) 0xE02E035B)();
    if (!slot) return -1;
    uint32_t ch = *slot;
    int r = -1;
    if (ch < 4)
    {
        if ((int)ch != pl_last_channel || beat_shift != pl_last_beat)
        {
            pl_last_channel = -1;
            if (!pl_build(dst, src, spitch, ch, beat_shift)) goto release;
            pl_last_channel = ch;
            pl_last_beat = beat_shift;
        }
        else
        {
            pl_mov(pl_prog, 0, (uint32_t)src);
            pl_mov(pl_prog + 6, 2, (uint32_t)dst);
            asm volatile("dsb sy" ::: "memory");
        }
        void *sem = ((void **) 0x572DC)[ch];
        if (((int (*)(void *, int)) 0xE0577C5D)(sem, 0) == 0)
        {
            static uint32_t dummy[4] __attribute__((aligned(32)));
            uint32_t job[3] = { ch, (uint32_t) dummy, 1 };
            dummy[0] = (uint32_t) dst; dummy[1] = (uint32_t) src; dummy[2] = 32; dummy[3] = 0;
            uint32_t bufs[4] = { (uint32_t) pl_prog, (uint32_t) pl_prog, (uint32_t) pl_prog, (uint32_t) pl_prog };
            ((void (*)(uint32_t *, int, uint32_t *)) 0xE0145BFD)(job, 1, bufs);
            ((void (*)(void)) 0xE0796A17)(); ((void (*)(void)) 0xE0796AA1)(); ((void (*)(void)) 0xE0796BE1)();
            ((void (*)(uint32_t, uint32_t, int)) 0xE0095CB1)(ch, (uint32_t) pl_prog, 0);
            r = ((int (*)(uint32_t *, int)) 0xE0145BA3)(job, 1);
        }
    }
release:
    ((void (*)(uint32_t *)) 0xE02E0685)(slot);
    return r;
}
static void pc_cpu_copy(uint8_t *dst, uint8_t *src)
{
    l1_setway(1);
    uint8_t *cd = (uint8_t *) CACHEABLE(dst), *cs = (uint8_t *) CACHEABLE(src);
    for (uint32_t y = 0; y < enc_h; y++) fast_copy(cd + y * pc_rowb, cs + y * pitch, pc_rowb);
    l1_setway(0);
}

static void precrop(uint8_t *dst, uint8_t *src)
{
    int m = (int) pc_m;
    /* Four adjacent rows cover all four source alignments modulo 16 at pitch 7644. */
    uint32_t r = frame_counter % (enc_h - 3);
    uint8_t snap[4][2][64];
    uint32_t tail = pc_rowb - 64;
    for (int q = 0; q < 4; q++)
    {
        uint8_t *sr = src + (r + q) * pitch;
        memcpy(snap[q][0], sr, 64);
        memcpy(snap[q][1], sr + tail, 64);
    }
    uint32_t t = us_timer();
    int good = 1;
    /* Native 12-bit rows can support eight-byte beats. Keep four-byte beats
     * for unaligned crops and for the existing 14-bit path. */
    int wide = m == 1 && pc_allow_wide &&
               !(((uintptr_t)src | (uintptr_t)dst | pitch | pc_rowb) & 7);
    if (m == 1) good = pl_run(dst, src, pitch, wide ? 3 : 2) == 0;
    else pc_cpu_copy(dst, src);
    int changed = 0;
    for (int q = 0; q < 4; q++)
    {
        uint8_t *dr = dst + (r + q) * pc_rowb, *sr = src + (r + q) * pitch;
        good &= !memcmp(snap[q][0], dr, 64) && !memcmp(snap[q][1], dr + tail, 64);
        changed |= memcmp(snap[q][0], sr, 64) || memcmp(snap[q][1], sr + tail, 64);
    }
    if (changed) st_pc_ovw++;
    good &= !changed;
    if (!good && m < 2)
    {
        st_pc_bad++;
        if (wide) pc_allow_wide = 0;  /* next crop can still use four-byte DMA */
        else pc_m = 2;
        pc_cpu_copy(dst, src);
    }
    else if (wide) st_pc_wide++;
    st_pc_us = (us_timer() - t) & 0xFFFFF;
    if (st_pc_us > st_pc_max) st_pc_max = st_pc_us;
}

/* runs in Canon's AlgsMgr task (0xE024C2B2) */
static void enc_cbr(void *ctx, void *handle, uint32_t a2, uint32_t a3)
{
    if ((uint32_t) ctx != enc_seq)
    {
        /* late result of a request we timed out on: free it from our task */
        if (a2 == CBR_OK && handle && late_w - late_r < 8) late_h[late_w++ & 7] = handle;
        return;
    }
    cb_handle = (a2 == CBR_OK) ? handle : 0;
    cb_a2 = (a2 == CBR_OK) ? a2 : (uint32_t) handle;    /* error code */
    give_semaphore(enc_sem);
}

static void free_handle(void *h)
{
    if (!h) return;
    if (!*(volatile uint32_t *) ALGSMGR_PTR) { st_free_err++; return; }
    int (*algs_free)(void *, void *, void *) = (void *) FREE_ENC_BUFFER;
    int r = algs_free(h, 0, 0);
    if (r & 1) st_free_err++;
}

static void free_late(void)
{
    while (late_r != late_w) free_handle(late_h[late_r++ & 7]);
}

/* Replicates ALGSMGR_StartEncode (0xE0795850), raising dwEncBufMargin (offset
 * 0x50) from Canon's 14-bit photo default of 35% to 80%. With 12-bit input
 * (1.5 B/px vs 1.75 B/px), 35% of input size is often exceeded by individual
 * wavelet sub-band groups, causing algsmgrEndEncode (0xE024BDDA) to abort with
 * "NG Result" / "### RetryMargin" and re-encode each tile 2-4x per frame.
 * AlgsMgr shrinks the reservation to the actual compressed size upon tile
 * completion (algsmgrUpdateFreeEncBuffer 0xE024EDE8), so a larger initial
 * margin has zero memory footprint after encode completion. */
static void crx_algs_request(struct algs_req *req, int count, void *cbr, void *ctx)
{
    uint32_t *mgr = *(uint32_t * volatile *) ALGSMGR_PTR;
    void *(*canon_alloc)(uint32_t) = (void *) 0xE055B755;
    void (*canon_free)(void *) = (void *) 0xE055B24D;
    int (*try_post_event)(uint32_t, void *, int, void *, int) = (void *) 0xE0559101;
    if (mgr && req && cbr && count > 0)
    {
        uint32_t *param = canon_alloc(0x64);
        if (param)
        {
            memset(param, 0, 0x64);
            uint32_t req_bytes = (uint32_t) count * sizeof(*req);
            void *req_copy = canon_alloc(req_bytes);
            if (req_copy)
            {
                memcpy(req_copy, req, req_bytes);
                param[2] = (uint32_t) req_copy;
                param[3] = (uint32_t) cbr;
                param[4] = (uint32_t) ctx;
                param[5] = 0;
                param[6] = (uint32_t) count;
                param[8] = 0;
                /* For 2-tile 4K (4368x2160), algsmgrSetBuffer (0xE024E4C6)
                 * sizes each tile from req.w / 2 = 2184 even though Tile 0 is
                 * 3144 wide. Margin 120 (<= max 125) gives Tile 0 an effective
                 * 83.4% margin, eliminating RetryMargin re-encodes in 4K & 3.5K. */
                param[20] = 120;
                if (try_post_event(mgr[1], mgr, 2, param, 0x64) >= 0)
                    return;
                canon_free(req_copy);
            }
            canon_free(param);
        }
    }
    ((void (*)(struct algs_req *, int, void *, void *)) ALGS_REQUEST)(req, count, cbr, ctx);
}

/* =================== 3b. copier (core 0, priority 0x1a) =================== */
static struct msg_queue *cp_mq;
static struct semaphore *cp_sem;
static void * volatile cp_h;
static volatile int cp_state = 0;             /* 0 idle, 1 copying, 2 done */
static volatile uint32_t cp_frame, cp_ts;
static volatile int cp_quant;
static void * volatile pending_h;
static uint32_t pending_frame, pending_ts;
static int pending_quant;
static volatile uint32_t st_cpA_sum, st_cpA_n, st_cp_bad, st_cp_cpu;

/* encoder task: wait for the copier, then ensure the handle is freed */
static void cp_wait_and_free(void)
{
    if (cp_state) take_semaphore(cp_sem, 2000);
    if (cp_state == 2)
    {
        if (cp_h) { free_handle(cp_h); cp_h = 0; }
        cp_state = 0;
    }
}

static void cp_kick_pending(void)
{
    if (!pending_h) return;
    void *h = pending_h;
    pending_h = 0;
    cp_wait_and_free();
    if (cp_state == 1) { LOG("copier stuck\n"); stop_req = 1; free_handle(h); return; }
    cp_h = h; cp_frame = pending_frame; cp_ts = pending_ts; cp_quant = pending_quant;
    asm volatile("dmb sy" ::: "memory");
    cp_state = 1;
    msg_queue_post(cp_mq, 0);
}

/* copier: allocates ring space, DMA-copies all n pieces in a single PL330
 * program (pl_copy_pieces, 8-byte beats, 1 interrupt per frame instead of
 * 13-25 separate HPCopy calls), falling back to HPCopy / CPU copy if busy. */
#define HPCOPY 0xE02E0397
static volatile int cp_dma = 1;
CONFIG_INT("crx.upper", m50_crx_upper, 0);

static int pl_build_pieces(uint8_t *d, uint32_t off, uint32_t n,
                           const uint32_t *addrs, const uint32_t *lens, uint32_t ch)
{
    uint8_t *p = cp_prog, *limit = cp_prog + 0x7F00;
    uint32_t total = 0;
    for (uint32_t i = 0; i < n; i++)
    {
        if (!addrs[i] || !lens[i]) continue;
        uint32_t src = (uint32_t) UNCACHEABLE(addrs[i]);
        uint32_t dst = (uint32_t) (d + off);
        uint32_t l = (lens[i] + 31) & ~31u;
        if ((src | dst | l) & 3) return 0;
        uint32_t beat_shift = ((src | dst | l) & 7) ? 2 : 3;
        uint32_t beat_bytes = 1u << beat_shift;
        uint32_t burst_bytes = 16u * beat_bytes;
        uint32_t nb = l / burst_bytes;
        uint32_t rem = (l % burst_bytes) / beat_bytes;
        if (p + 64 > limit) return 0;
        p = pl_mov(p, 0, src);
        p = pl_mov(p, 2, dst);
        if (nb)
        {
            p = pl_mov(p, 1, PL_CCR(beat_shift, 16));
            while (nb >= 256)
            {
                uint32_t outer = MIN(256u, nb >> 8);
                if (p + 16 > limit) return 0;
                *p++ = 0x22; *p++ = outer - 1;
                uint8_t *l1 = p;
                *p++ = 0x20; *p++ = 255;
                uint8_t *l0 = p;
                *p++ = 0x04; *p++ = 0x08;
                p[0] = 0x38; p[1] = p - l0; p += 2;
                p[0] = 0x3C; p[1] = p - l1; p += 2;
                nb -= outer << 8;
            }
            if (nb)
            {
                *p++ = 0x20; *p++ = nb - 1;
                uint8_t *l0 = p;
                *p++ = 0x04; *p++ = 0x08;
                p[0] = 0x38; p[1] = p - l0; p += 2;
            }
        }
        if (rem)
        {
            p = pl_mov(p, 1, PL_CCR(beat_shift, rem));
            *p++ = 0x04; *p++ = 0x08;
        }
        off += l;
        total += l;
    }
    if (!total) return 0;
    *p++ = 0x13;                                            /* DMAWMB */
    ((void (*)(uint32_t, uint8_t **)) 0xE0095C8D)(ch, &p);  /* Canon's epilogue: SEV + END */
    asm volatile("dsb sy" ::: "memory");
    return p - cp_prog;
}

static int pl_copy_pieces(uint8_t *d, uint32_t off, uint32_t n,
                          const uint32_t *addrs, const uint32_t *lens)
{
    if (!cp_prog) return -1;
    uint32_t *slot = ((uint32_t *(*)(void)) 0xE02E035B)();
    if (!slot) return -1;
    uint32_t ch = *slot;
    int r = -1;
    if (ch < 4 && pl_build_pieces(d, off, n, addrs, lens, ch))
    {
        void *sem = ((void **) 0x572DC)[ch];
        if (((int (*)(void *, int)) 0xE0577C5D)(sem, 0) == 0)
        {
            static uint32_t cp_dummy[4] __attribute__((aligned(32)));
            uint32_t job[3] = { ch, (uint32_t) cp_dummy, 1 };
            cp_dummy[0] = (uint32_t) (d + off);
            cp_dummy[1] = (uint32_t) UNCACHEABLE(addrs[0]);
            cp_dummy[2] = 32;
            cp_dummy[3] = 0;
            uint32_t bufs[4] = { (uint32_t) cp_prog, (uint32_t) cp_prog, (uint32_t) cp_prog, (uint32_t) cp_prog };
            ((void (*)(uint32_t *, int, uint32_t *)) 0xE0145BFD)(job, 1, bufs);
            ((void (*)(void)) 0xE0796A17)(); ((void (*)(void)) 0xE0796AA1)(); ((void (*)(void)) 0xE0796BE1)();
            ((void (*)(uint32_t, uint32_t, int)) 0xE0095CB1)(ch, (uint32_t) cp_prog, 0);
            r = ((int (*)(uint32_t *, int)) 0xE0145BA3)(job, 1);
        }
    }
    ((void (*)(uint32_t *)) 0xE02E0685)(slot);
    return r;
}

/* Canon writes the stock Q indices in the mdat header. Correct the ring
 * copy after the encoder returns, before publishing it to the writer. */
static int fix_quant_header(uint8_t *p, uint32_t len, uint32_t tiles, int strength)
{
    if ((unsigned)strength >= COUNT(crx_quant_add)) return 0;
    uint32_t bands = 0;
    while (len >= 4)
    {
        uint32_t size = (p[2] << 8) | p[3];
        if (!size || size > len - 4) break;
        if (p[0] == 0xFF && p[1] == 3 && size == 8)
        {
            uint32_t band = p[8] >> 4;
            if (band != bands % 10) return 0;
            if (band >= 4)
            {
                uint32_t q = ((p[8] & 7) << 5) | (p[9] >> 3);
                q = MIN(q + crx_quant_add[strength], 127);
                p[8] = (p[8] & 0xF8) | (q >> 5);
                p[9] = (p[9] & 7) | (q << 3);
            }
            bands++;
        }
        p += size + 4;
        len -= size + 4;
    }
    return bands == tiles * 40;
}

static void crx_copier(void *unused)
{
    while (1)
    {
        uint32_t m;
        if (msg_queue_receive(cp_mq, &m, 0)) continue;
        if (cp_state != 1) continue;
        uint32_t *hw = (uint32_t *) cp_h;
        uint32_t cmp1_addr = hw[2], cmp1_len = hw[3], n = hw[6];
        uint32_t *addrs = (uint32_t *) hw[7], *lens = (uint32_t *) hw[8];
        uint32_t hdr = (64 + 4 * n + 31) & ~31;
        uint32_t need = hdr + ((cmp1_len + 31) & ~31);
        for (uint32_t i = 0; i < n; i++) need += (lens[i] + 31) & ~31;
        /* ring space */
        uint32_t w = r_wp, r = r_rp, at;
        if (w >= r && ring_size - w >= need) at = w;
        else if (w >= r && need < r) at = 0;
        else if (w < r && need < r - w) at = w;
        else { st_drop_queue++; void *h = cp_h; cp_h = 0; free_handle(h); cp_state = 2; give_semaphore(cp_sem); continue; }
        uint32_t t1 = us_timer();
        uint8_t *d = ring + at;
        uint32_t *fh = (uint32_t *) d;
        memset(fh, 0, hdr);
        fh[0] = 0x33585243;          /* "CRX3" */
        fh[1] = need;
        fh[2] = cp_frame;
        fh[3] = cp_ts;               /* us since start */
        fh[4] = cmp1_len;
        fh[5] = n;                   /* pieces, Canon's order: piece 0 = mdat header */
        fh[6] = hdr;
        for (uint32_t i = 0; i < n; i++) fh[16 + i] = addrs[i] ? lens[i] : 0;
        uint32_t off = hdr;
        memcpy(d + off, (void *) UNCACHEABLE(cmp1_addr), cmp1_len); off += (cmp1_len + 31) & ~31;
        uint8_t *mdat = d + off;
        int (*hpcopy)(void *, void *, uint32_t) = (void *) HPCOPY;
        int cpu = !cp_dma;
        if (cpu) l1_setway(1);
        int pl_ok = !cpu && pl_copy_pieces(d, off, n, addrs, lens) == 0;
        for (uint32_t i = 0; i < n; i++)
        {
            if (!addrs[i] || !lens[i]) continue;
            uint8_t *src = (uint8_t *) UNCACHEABLE(addrs[i]);
            uint32_t l = (lens[i] + 31) & ~31;
            if (!pl_ok)
            {
                if (cpu) fast_copy((uint8_t *) CACHEABLE(d + off), (uint8_t *) CACHEABLE(src), lens[i]);
                else if (hpcopy(d + off, src, l)) { fast_copy(d + off, src, lens[i]); st_cp_cpu++; }
            }
            if (!cpu && i == 1 + (cp_frame % (n - 1)))
            {
                uint32_t k = MIN(lens[i], 256u);
                if (memcmp(d + off, src, k) || memcmp(d + off + lens[i] - k, src + lens[i] - k, k))
                { cp_dma = 0; st_cp_bad++; fast_copy(d + off, src, lens[i]); }
            }
            off += l;
        }
        if (cpu) l1_setway(0);
        if (cp_quant && !fix_quant_header(mdat, lens[0], hw[4], cp_quant))
        {
            LOG("CRX: unsupported Q header\n");
            stop_req = 1;
            void *h = cp_h; cp_h = 0; free_handle(h);
            cp_state = 2;
            give_semaphore(cp_sem);
            continue;
        }
        uint32_t us = (us_timer() - t1) & 0xFFFFF;
        st_copy_us = us; st_cpA_sum += us; st_cpA_n++;
        if (!at && w) r_wrap = w;
        asm volatile("dmb sy" ::: "memory");
        r_wp = at + need;
        r_prod += need;
        fe[fe_w % NFE] = r_prod; fe_w++;
        void *h = cp_h; cp_h = 0; free_handle(h);
        cp_state = 2;
        give_semaphore(cp_sem);
    }
}

static void crx_encoder(void *unused)
{
    while (1)
    {
        /* Retire the native job only after its output was handed to the copier. */
        if (native_encode_busy) native_encode_busy = 0;
        uint32_t msg;
        free_late();
        if (msg_queue_receive(enc_mq, &msg, 0)) { cp_kick_pending(); msleep(10); continue; }
        enc_taken++;
        int e = (msg & CRX_MSG_THIRD_SINK) ? 2 : (msg & 1);
        int from_sink = (msg & CRX_MSG_SINK) != 0;     /* zero copy: e is a sink index */
        int sink_held = from_sink;
        int native_crop = from_sink && record_native && pc_rowb;
        int triple_sink = from_sink && sink_count == 3;
        uint32_t ts_rel = msg & 0xFFFFFFF0;

        if ((native_crop || triple_sink) && encoder_stalled)
        {
            if (pending_h) { free_handle(pending_h); pending_h = 0; }
            crx_release_sink(e);
            continue;
        }
        if (triple_sink) native_encode_busy = 1;
        /* Canon's encoder keeps every output until we free it; test25 hung
         * after ~13 frames when the card fell behind. Never hold more than
         * MAX_INFLIGHT encoded frames: drop before encoding instead. */
        if (!m50_crx_sample && ring_size - ring_used() < frame_bytes * 7 / 10)
        {
            cp_kick_pending();
            st_drop_queue++;
            if (from_sink) crx_release_sink(e); else encin_state[e] = 0;
            continue;
        }
        if (!from_sink) encin_state[e] = 2;

        static struct algs_req req;
        req.mode = m50_crx_mode ? 4 : 0;
        req.w = enc_w; req.h = enc_h; req.p3 = 0; req.p4 = 0; req.bits = record_bits;
        req.addr = (from_sink ? (uint32_t) sink[e] : (uint32_t) encin[e]) + enc_off; req.stride = width;
        if (pc_rowb && !from_sink)
        {
            /* The CPU worker packed the selected Bayer rectangle directly. */
            req.addr = (uint32_t)encin[e];
            req.stride = enc_w;
        }
        else if (pc_rowb)
        {
            /* Finish any in-flight copier DMA before pre-crop touches Xdmac. */
            cp_wait_and_free();
            uint8_t *src = (uint8_t *) req.addr, *dst = pc_buf[record_native ? 0 : e];
            precrop(dst, src);
            req.addr = (uint32_t) dst; req.stride = enc_w;
            if (native_crop)
            {
                /* Crop and encode run in this one task, so a queued frame
                 * cannot reach pc_buf until this job finishes. */
                native_encode_busy = 1;
                crx_release_sink(e);
                sink_held = 0;
            }
        }

        cb_handle = 0; cb_a2 = 0;
        if (diag_e == 0)
        {
            diag_e = 1;
            /* RAW mode 0 and C-RAW mode 4 use codec IDs 0 and 4 respectively.
             * Reinitialize the selected codec before the first job of this take. */
            uint32_t mgr = *(volatile uint32_t *) ALGSMGR_PTR;
            if (mgr) ((volatile uint32_t *) (mgr + ALGS_GEOMETRY_VALID))[req.mode] = 0;
        }
        while (take_semaphore(enc_sem, 1) == 0) ;          /* drain stale gives */
        uint32_t seq = enc_seq + 1;
        if (seq == 0) seq = 1;
        enc_seq = seq;
        asm volatile("dmb sy" ::: "memory");
        uint32_t t0 = us_timer();
        int quant = req.mode == 4 && (unsigned)m50_crx_qtest < COUNT(crx_quant_add) ? m50_crx_qtest : 0;
        uint32_t quant_loads = qtest_frames;
        qtest_active = quant;
        crx_algs_request(&req, 1, enc_cbr, (void *) seq);
        /* While the CRX hardware encoder ASIC runs this frame, copy and free
         * the previous frame's handle on Core 0 + Xdmac HPCopy in parallel. */
        cp_kick_pending();
        int ok = take_semaphore(enc_sem, 1000) == 0;
        qtest_active = 0;
        if (!ok)
        {
            uint32_t mgr = *(volatile uint32_t *) ALGSMGR_PTR;
            if (mgr)
                LOG("AlgsMgr timeout: blocked %d, buffers %d %d %d %d\n",
                    *(volatile uint32_t *)(mgr + 0x150), *(volatile uint32_t *)(mgr + 0xec), *(volatile uint32_t *)(mgr + 0xf0),
                    *(volatile uint32_t *)(mgr + 0x394), *(volatile uint32_t *)(mgr + 0x398));
            enc_seq = 0;    /* a late callback gets freed */
        }
        uint32_t us = (us_timer() - t0) & 0xFFFFF;
        void *h = cb_handle;

        if (!ok || !h)
        {
            st_enc_err++;
            LOG("encode error: %s, code %x\n", ok ? "encoder returned an error" : "no callback in 1 s", cb_a2);
            if (ok)
            {
                /* finished with an error: the input is free again */
                if (sink_held) crx_release_sink(e);
                else if (!from_sink) encin_state[e] = 0;
            }
            else if (from_sink) { encoder_stalled = 1; stop_req = 1; }
            if (st_enc_err >= 3) stop_req = 1;
            continue;
        }
        if (sink_held) crx_release_sink(e);
        else if (!from_sink) encin_state[e] = 0;
        st_enc_us = us; if (us > st_enc_max) st_enc_max = us;
        st_encoded++;

        uint32_t *hw = (uint32_t *) h;
        uint32_t cmp1_addr = hw[2], cmp1_len = hw[3], n = hw[6];
        uint32_t *addrs = (uint32_t *) hw[7], *lens = (uint32_t *) hw[8];
        if (!cmp1_addr || cmp1_len == 0 || cmp1_len > 256 || !addrs || !lens
            || n < 2 || n > 256 || lens[0] == 0 || lens[0] > 4096)
        {
            st_enc_err++;
            LOG("bad handle %x: cmp1 %x/%d n %d hdr %d\n", (uint32_t) h, cmp1_addr, cmp1_len, n, lens ? lens[0] : 0);
            free_handle(h);
            if (st_enc_err >= 3) stop_req = 1;
            continue;
        }
        /* CMP1's geometry and precision must describe the encoded input. */
        const uint32_t *cmp = (const uint32_t *) UNCACHEABLE(cmp1_addr);
        if (cmp1_len < 32 || __builtin_bswap32(cmp[2]) != enc_w ||
            __builtin_bswap32(cmp[3]) != enc_h || (cmp[6] & 0xFF) != record_bits)
        {
            LOG("CMP1 mismatch: expected %dx%d %d-bit\n", enc_w, enc_h, record_bits);
            st_enc_err++;
            free_handle(h);
            stop_req = 1;
            continue;
        }
        uint32_t data = 0;
        for (uint32_t i = 0; i < n; i++) if (addrs[i]) data += lens[i];
        const uint32_t hdr = 48;                /* CRX2 block: 32 bytes + 1 length, padded */
        uint32_t total = hdr + cmp1_len + data;
        uint32_t frame_no = frame_counter++;
        st_enc_bytes += total;

        if (!rec_live || (m50_crx_sample && frame_no % 24)) { free_handle(h); continue; }   /* warm-up / pipeline test */


        if ((native_crop || triple_sink) && encoder_stalled) { free_handle(h); continue; }
        pending_h = h;
        pending_frame = frame_no;
        pending_ts = ts_rel;
        pending_quant = qtest_frames != quant_loads ? quant : 0;
        /* If another frame is already queued in enc_mq (and pre-crop will run
         * next), defer kicking the copier until right after precrop() and
         * crx_algs_request() on the next frame so precrop() starts immediately
         * with exclusive Xdmac bandwidth and the copier runs in parallel with
         * the CRX hardware encoder ASIC. Otherwise kick the copier right now. */
        if (enc_posted == enc_taken)
            cp_kick_pending();
    }
}

/* =================== 4. writer task =================== */

#define CHUNK_LIMIT (4000u * 1000u * 1000u)

static int crx_sc = 0; /* 1 = current chunk was opened via Canon MovWriter CreateSC (exFAT AU speed-class + CMD20) */
static FILE *crx_create(const char *name)
{
    extern int m50_sd_write_opt_mode(void);
    if (m50_sd_write_opt_mode() >= 2)
    {
        int (*create_sc)(const char *) = (void *) 0xE0546865;
        char path[80];
        snprintf(path, sizeof(path), "%s:/%s", get_ml_card()->drive_letter, name);
        int fd = create_sc(path);
        if (fd > 0) { crx_sc = 1; return (FILE *) fd; }
    }
    crx_sc = 0;
    return FIO_CreateFile(name);
}
static void crx_close(FILE *ff)
{
    int (*close_sc)(int, int) = (void *) 0xE0546E17;
    if (crx_sc) close_sc((int) ff, get_ml_card()->drive_letter[0] - 'A');
    else FIO_CloseFile(ff);
}

static int open_chunk(void)
{
    char name[64];
    if (chunk == 0) snprintf(name, sizeof(name), "%s.CRX", fname);
    else            snprintf(name, sizeof(name), "%s.C%02d", fname, chunk);
    f = crx_create(name);
    chunk_bytes = 0;
    if (!f) return 0;
    /* file header */
    uint32_t *h = (uint32_t *) iobuf;
    memset(h, 0, 128);
    h[0] = 0x56585243;          /* "CRXV" */
    h[1] = 1;                   /* version */
    h[2] = width; h[3] = height; h[4] = width * record_bits / 8; h[5] = record_bits;
    h[6] = m50_crx_mode ? 4 : 0;
    h[7] = raw_info.black_level >> (14 - record_bits);
    h[8] = raw_info.white_level >> (14 - record_bits);
    h[9] = raw_info.active_area.x1; h[10] = raw_info.active_area.y1;
    h[11] = raw_info.active_area.x2; h[12] = raw_info.active_area.y2;
    h[13] = fps_get_current_x1000();
    h[14] = raw_info.cfa_pattern;
    h[15] = chunk;
    h[16] = 0x4D353000;         /* "M50" */
    h[17] = 128;                /* header size */
    h[18] = enc_w; h[19] = enc_h; h[20] = enc_off;   /* encoded area: full-width rows */
    if (record_bits == 12 && !record_native) h[20] = enc_off * 6 / 7;
    h[21] = 0;                  /* retain the entire encoded width */
    h[10] = 0; h[12] = enc_h;   /* active area relative to the encoded rows (top OB is cut) */
    if (pc_rowb) { h[9] = 0; h[11] = enc_w; }   /* pre-cropped: no OB left, nothing for the converter to cut */
    FIO_WriteFile(f, UNCACHEABLE(iobuf), 128);
    chunk_bytes = 128;
    return 1;
}

static void save_log(void);
static void crx_writer(void *unused)
{
    extern int m50_sd_brake_detected(void);
    extern int m50_sd_brake_recover(void);
    extern uint32_t m50_sd_intercept_count(void);
    extern int m50_sd_intercept_stepdown_mhz(void);
    extern int m50_sd_extreme_mode(void);
    int idle_ms = 0;
    int brake_split = 0;
    uint32_t last_ic = 0;
    while (1)
    {
        uint32_t w = r_wp, r = r_rp;
        if (r_wrap != 0xFFFFFFFF && r == r_wrap && w < r) { r_rp = r = 0; r_wrap = 0xFFFFFFFF; }
        int wrapped = (w < r);
        uint32_t avail = wrapped ? r_wrap - r : w - r;
        if (!avail || !f) { if (!avail) { msleep(5); continue; } }
        /* collect big writes: wait (max 400 ms) until 4 MB is there;
         * never wait on a wrap tail because r_wrap - r cannot grow. */
        if (!wrapped && !brake_split && avail < (4u << 20) && !wr_flush && idle_ms < 400) { msleep(5); idle_ms += 5; continue; }
        idle_ms = 0;
        uint32_t wr_max = m50_sd_extreme_mode() ? (16u << 20) : WR_MAX;
        uint32_t want = MIN(avail, wr_max);
        uint32_t au = chunk_bytes % WR_AU;
        if (au)
        {
            uint32_t to_au = WR_AU - au;
            if (want > to_au)
                want = to_au + ((want - to_au) & ~(WR_AU - 1));
        }
        else if (want >= WR_AU)
        {
            want &= ~(WR_AU - 1);
        }
        if (f && brake_split)
        {
            /* Finish the current frame cleanly, close file, power-cycle SD back to 1.8V UHS-I, open next chunk */
            uint64_t next_end = 0;
            for (uint32_t k = 0; k < NFE; k++)
                if (fe[k] > r_cons && (!next_end || fe[k] < next_end)) next_end = fe[k];
            if (next_end && next_end <= r_cons + avail)
                want = (uint32_t)(next_end - r_cons);
            else if (!next_end)
            {
                /* Already on a frame boundary: close, power-cycle + re-tune, open next chunk */
                crx_close(f); f = 0;
                int br = m50_sd_brake_recover();
                if (br != 0) st_sd_brakes++;
                if (br > 0) LOG("SD brake: power-cycled & re-tuned %d MHz\n", br);
                else if (br < -1) { st_sd_stepdown_mhz = -br; LOG("SD brake x4: power-cycled & stepped down to %d MHz\n", -br); }
                else if (br == -1) { st_sd_stepdown_mhz = 48; LOG("SD brake: recovery failed, staying at 48 MHz\n"); }
                save_log();
                brake_split = 0;
                chunk++;
                if (!open_chunk()) stop_req = 1;
                continue;
            }
        }
        if (f && chunk_bytes + want > CHUNK_LIMIT)
        {
            /* split files only between frames */
            uint64_t lim = r_cons + (CHUNK_LIMIT - chunk_bytes), best = 0;
            for (uint32_t k = 0; k < NFE; k++) if (fe[k] > r_cons && fe[k] <= lim && fe[k] > best) best = fe[k];
            if (best) want = MIN(want, (uint32_t)(best - r_cons));
            else { crx_close(f); f = 0; chunk++; if (!open_chunk()) stop_req = 1; continue; }
        }
        writer_busy = 1;
        asm volatile("dmb sy" ::: "memory");
        if (f)
        {
            uint32_t t0 = get_ms_clock();
            int ok = FIO_WriteFile(f, ring + r, want) == (int) want;
            uint32_t dt = get_ms_clock() - t0;
            st_write_ms += dt;
            if (!ok) { LOG("write error (card full?)\n"); stop_req = 1; }
            chunk_bytes += want;
            st_bytes_out += want;
            st_written++;            /* card writes (not frames) */
            uint32_t ic = m50_sd_intercept_count();
            if (ic < last_ic) last_ic = 0;
            if (ic != last_ic)
            {
                last_ic = ic;
                int sd_mhz = m50_sd_intercept_stepdown_mhz();
                if (sd_mhz) st_sd_stepdown_mhz = sd_mhz;
                int pll_mhz = (int)(((*(volatile uint32_t *)0xD010F218) >> 4) & 0xFF) * 3;
                LOG("SD brake intercepted (#%d, %d ms write, now %d MHz, no split)\n", (int)ic, (int)dt, pll_mhz);
            }
            if (ok && !brake_split && m50_sd_brake_detected())
                brake_split = 1;
            else if (ok && brake_split)
            {
                /* Just finished the frame boundary: close, power-cycle SD back to 1.8V UHS-I, open next chunk */
                crx_close(f); f = 0;
                int br = m50_sd_brake_recover();
                if (br != 0) st_sd_brakes++;
                if (br > 0) LOG("SD brake: power-cycled & re-tuned %d MHz\n", br);
                else if (br < -1) { st_sd_stepdown_mhz = -br; LOG("SD brake x4: power-cycled & stepped down to %d MHz\n", -br); }
                else if (br == -1) { st_sd_stepdown_mhz = 48; LOG("SD brake: recovery failed, staying at 48 MHz\n"); }
                save_log();
                brake_split = 0;
                chunk++;
                if (!open_chunk()) stop_req = 1;
            }
            if (f && chunk_bytes >= CHUNK_LIMIT) { crx_close(f); f = 0; chunk++; if (!open_chunk()) stop_req = 1; }
        }
        asm volatile("dmb sy" ::: "memory");
        r_cons += want;
        r_rp = r + want;
        writer_busy = 0;
    }
}

static void start_tasks(void);

/* =================== control =================== */

static int jobs_in_flight(void)
{
    return (ring_used() + 0xFFFFF) >> 20;     /* MB still in the ring */
}

static char logname[32] = "ML/LOGS/CRXREC.LOG";
static void pick_logname(void)
{
    /* test62: one log per recording, CRXREC1.LOG, CRXREC2.LOG, ... (never overwritten) */
    for (int i = 1; i < 1000; i++)
    {
        uint32_t sz;
        snprintf(logname, sizeof(logname), "ML/LOGS/CRXREC%d.LOG", i);
        if (FIO_GetFileSize(logname, &sz) != 0) return;
    }
}
static void save_log(void)
{
    FILE *lf = FIO_CreateFile(logname);
    if (lf && logbuf) { FIO_WriteFile(lf, logbuf, loglen); FIO_CloseFile(lf); }
}

extern int raw_lv_is_enabled(void);
extern int m50_bin_enabled;

static const char *crx_start_blocker(void)
{
    if (encoder_stalled) return "Restart camera";
    if (m50_bin_enabled) return "Turn Full-sensor bin OFF";
    if (!lv) return "Live View needed";
    if (RECORDING) return "Another recorder is active";
    normalize_format();
    if (SENSOR_MODE_NOW == 6 && crx_aspect != ASPECT_STREAM &&
        (resolutions[crx_resolution].width > 1992 || format_height(crx_aspect, crx_resolution) > 1128))
        return "Select a smaller size or Canon 4K";
    if (m50_crx_depth == CRX_12BIT_LV)
    {
        if (!is_movie_mode()) return "Switch to movie mode for 12-bit LV";
        if (raw_lv_is_enabled()) return "Turn RAW stream off for 12-bit LV";
    }
    return 0;
}

/* test47: no full shoot_malloc_suite(0) grab any more (test45/46 crashed inside it in movie
 * mode). Each buffer: fio_malloc first (worked in photo mode, test40), else one contiguous
 * shoot memory suite of exactly that size. Log saved before every attempt. */
#define NBUF 5
static struct memSuite *buf_suite[NBUF];
static void *buf_fio[NBUF];

static void free_buffers(void)
{
    for (int i = 0; i < NBUF; i++)
    {
        if (buf_fio[i]) { fio_free(buf_fio[i]); buf_fio[i] = 0; }
        if (buf_suite[i]) { shoot_free_suite(buf_suite[i]); buf_suite[i] = 0; }
    }
    for (int i = 0; i < 2; i++) { sink_mem[i] = 0; sink[i] = 0; encin_mem[i] = 0; encin[i] = 0; encin_state[i] = 3; }
    sink[2] = 0;
    sink_count = 2;
    ring = 0; ring_size = 0; pc_buf[0] = pc_buf[1] = 0; pl_prog = cp_prog = 0;
    alloc_bytes = 0;
}

static void *get_buf(int k, uint32_t size, const char *what, int quiet)
{
    if (!quiet) { LOG("  alloc %s: %d KB ...", what, size >> 10); save_log(); }
    void *p = fio_malloc(size + 0x10000);
    if (p) { buf_fio[k] = p; if (!quiet) LOG(" fio_malloc ok\n"); return (void *)(((uint32_t) UNCACHEABLE(p) + 0xFFFF) & ~0xFFFF); }
    if (!quiet) { LOG(" fio_malloc failed, trying shoot memory ..."); save_log(); }
    struct memSuite *su = shoot_malloc_suite_contig(size + 0x10000);
    if (su)
    {
        buf_suite[k] = su;
        void *a = GetMemoryAddressOfMemoryChunk(GetFirstChunkFromSuite(su));
        if (!quiet) LOG(" shoot memory ok\n");
        return (void *)(((uint32_t) UNCACHEABLE(a) + 0xFFFF) & ~0xFFFF);
    }
    if (!quiet) LOG(" failed\n");
    return 0;
}

static int alloc_buffers(uint32_t upper_base)
{
    free_buffers();
    extern int GetFreeMemForAllocateMemory();
    LOG("memory: malloc free %d KB, AllocateMemory free %d KB\n", GetFreeMemForMalloc() >> 10, GetFreeMemForAllocateMemory() >> 10);
    uint32_t fb = (frame_bytes + 0xFFFF) & ~0xFFFF;
    uint32_t pcb = (pc_rowb * enc_h + 0xFFFF) & ~0xFFFF;
    uint32_t pc_count = record_native ? 1 : 2;
    uint32_t pl_size = 0x10000;
    uint32_t reserved = 2 * fb + pc_count * pcb + pl_size + 0x10000;
    /* Use three capture sinks for 12-bit LV 4K and 3.5K/crop modes in RecWork.
     * Reserve up to 64 KiB for the alignment of the RecWork base. */
    if (record_native && width >= 3840 && rw_addr &&
        rw_size >= reserved + fb + (160u << 20))
    {
        sink_count = 3;
        reserved = upper_base ? pc_count * pcb + pl_size + 0x10000 : reserved + fb;
    }
    if (record_bits == 12 && (!rw_addr || rw_size <= reserved + (32u << 20)))
    {
        LOG("12-bit needs RecWork for packed encoder input\n");
        return 0;
    }
    if (rw_addr && rw_size > reserved + (32u << 20))
    {
        /* RecWork holds the ring and any buffers without a separate grant. */
        uint32_t base = ((uint32_t) UNCACHEABLE(rw_addr) + 0xFFFF) & ~0xFFFF;
        uint32_t capture_base = upper_base ? upper_base : base;
        sink_mem[0] = sink[0] = (void *) capture_base;
        sink_mem[1] = sink[1] = (void *) (capture_base + fb);
        if (sink_count == 3) sink[2] = (void *) (capture_base + 2 * fb);
        pc_buf[0] = (uint8_t *) (base + (upper_base ? 0 : sink_count * fb));
        pc_buf[1] = pc_buf[0] + (pc_count > 1 ? pcb : 0);
        pl_prog = pc_buf[0] + pc_count * pcb;
        cp_prog = pl_prog + 0x8000;
        ring = pl_prog + pl_size;
        /* Full 4K and 3-sink crop modes can use the remainder of the Canon grant. */
        uint32_t available = rw_size - reserved;
        ring_size = (sink_count == 3 ? available : MIN(available, RING_TARGET)) & ~0xFFFFu;
    }
    else for (int i = 0; i < 2; i++)
    {
        sink_mem[i] = sink[i] = get_buf(i, frame_bytes, i ? "sink 1" : "sink 0", 0);
        if (!sink[i]) { free_buffers(); return 0; }
    }
    for (int i = 0; i < sink_count; i++) { dcache_range(sink[i], frame_bytes, 1); dcache_range(sink[i], frame_bytes, 0); }
    for (int i = 0; i < 2; i++)
    {
        encin_state[i] = 3;
        if (m50_crx_hwpack || record_native) continue;   /* CPU repack input only without hardware packing */
        encin_mem[i] = encin[i] = get_buf(2 + i, frame_bytes, "encoder input", 0);
        if (!encin[i]) { if (i == 0) { free_buffers(); return 0; } continue; }
        encin_state[i] = 0;
    }
    if (!iobuf) iobuf = fio_malloc(4096);
    if (pc_rowb && !pc_buf[0])
    {
        LOG("No memory for selected crop\n");
        free_buffers();
        return 0;
    }
    if (!ring)
    {
        ring_size = (frame_bytes * 3 / 2 + 0xFFFF) & ~0xFFFF;   /* fio: keep Canon's pool usage ~13 MB */
        ring = get_buf(4, ring_size, "ring", 0);
        if (!ring) { free_buffers(); return 0; }
    }
    r_wp = r_rp = 0; r_wrap = 0xFFFFFFFF; r_prod = r_cons = 0; fe_w = 0; wr_flush = 0;
    for (int i = 0; i < NFE; i++) fe[i] = 0;
    LOG("  ring %08x %d KB (%s)\n", (uint32_t) ring, ring_size >> 10, rw_addr ? "RecWork" : "fio");
    return iobuf != 0;
}

static void mrrsc_log(const char *when)
{
    uint32_t o = *(volatile uint32_t *) MRRSC_OBJ;
    if (!o) { LOG("MRRSC %s: no object\n", when); return; }
    volatile uint32_t *w = (volatile uint32_t *) o;
    LOG("MRRSC %s: clients %d, trans %d\n", when, w[2], w[0x54/4]);
}

static unsigned crx_log_memory_owners(const char *when, unsigned previous)
{
    /* M50 1.1.0 smemShowStat (E01475A4) reads the map from the object.
     * E0524156 tests area IDs against the two ownership words at 0x65EC. */
    uint32_t obj = *(volatile uint32_t *) 0x66D4;
    if (!obj) return previous;
    uint32_t map = *(volatile uint32_t *) (obj + 0x150);
    uint32_t used_low = *(volatile uint32_t *) 0x65EC;
    unsigned upper = (used_low >> 5) & 1, playback = (used_low >> 18) & 1;
    uint32_t used_high = *(volatile uint32_t *) 0x65F0;
    unsigned twain_upper = (used_high >> 1) & 1; /* area 33, paired with area 5 */
    unsigned observed = (map << 3) | (twain_upper << 2) | (playback << 1) | upper;
    if (observed != previous)
        LOG("SRM %s: map %d, owners %08x/%08x, upper capture %d, upper Twain %d, movie playback %d\n",
            when, map, used_low, used_high, upper, twain_upper, playback);
    return observed;
}
static void mrrsc_release(void)
{
    uint32_t o = *(volatile uint32_t *) MRRSC_OBJ;
    movrel_n = 0;
    if (!o) return;
    volatile uint32_t *w = (volatile uint32_t *) o;
    if (w[2] != 1 || w[0x54/4]) { LOG("MRRSC: %d clients (trans %d): not touching it\n", w[2], w[0x54/4]); return; }
    movrel_flag = w[0x58/4];
    LOG("MRRSC: giving Canon's movie buffers back (MRRSC_Shutdown) ...\n"); save_log();
    void (*sd)(void) = (void *) MRRSC_SHUTDOWN;
    sd();
    movrel_n = 1;
    mrrsc_log("after shutdown");
    save_log();
}
static void mrrsc_restore(void)
{
    if (!movrel_n) return;
    LOG("MRRSC: taking the movie buffers again (MRRSC_Startup(%d, 0)) ...\n", movrel_flag); save_log();
    void (*su)(int, int) = (void *) MRRSC_STARTUP;
    su(movrel_flag, 0);
    movrel_n = 0;
    mrrsc_log("restored");
    save_log();
}

/* ---- big write queue in Canon's movie RecWork area (only while MRRSC is shut down) ----
 * SRM_AllocateMemoryResourceForMovieRecWork 0xE056ABAA(0x100000, cbr, ctx), cbr(ctx, addr, size)
 * (Canon's MRRSC_Startup passes 0x100000, see 0xE02F52EC/0xE02F5414).
 * SRM_FreeMemoryResourceForMovieRecWork 0xE056DCDE(addr, cbr, ctx). */
static struct semaphore *rw_sem;
static volatile int rw_wait;
static void rw_cbr(void *ctx, void *addr, uint32_t size)
{
    if (rw_wait) { rw_addr = addr; rw_size = size; give_semaphore(rw_sem); }
    else rw_late = addr;
}
static void rw_free_cbr(void *ctx, void *addr, uint32_t size) { give_semaphore(rw_sem); }
static void rw_free(void * volatile *pa)
{
    if (!*pa) return;
    void (*f)(void *, void *, void *) = (void *) 0xE056DCDF;
    while (take_semaphore(rw_sem, 1) == 0) ;
    f(*pa, rw_free_cbr, 0);
    int ok = take_semaphore(rw_sem, 3000) == 0;
    LOG("RecWork %08x given back%s\n", (uint32_t) *pa, ok ? "" : " (no confirmation in 3 s)");
    *pa = 0;
}
static void rw_get(void)
{
    if (!rw_sem) rw_sem = create_named_semaphore("crx_rw", 0);
    while (take_semaphore(rw_sem, 1) == 0) ;
    rw_addr = 0; rw_wait = 1;
    void (*a)(int, void *, void *) = (void *) 0xE056ABAB;
    a(0x100000, rw_cbr, 0);
    int ok = take_semaphore(rw_sem, 3000) == 0;
    rw_wait = 0;
    if (!ok || !rw_addr) { LOG("RecWork: no answer in 3 s\n"); return; }
    LOG("RecWork %08x %d KB\n", (uint32_t) rw_addr, rw_size >> 10);
}

/* M50 1.1.0 CapBuf Add (E0571ACA) posts SRM type 0x46. On map 0,
 * E051F48A sets areas 5 and 33 and calls back with a 40-byte descriptor
 * on its stack. Keep the grant until RAW capture and encoding have drained.
 *
 * Use the priority free envelope from CapBuf Free (E0571B4A) with type
 * 0x46. Its map-0 handler E05210AC releases 5 and 33 for that type.
 * The stock wrapper posts type 0x45 and would also release the lower pair. */
enum upper_capture_phase
{
    UPPER_IDLE, UPPER_STARTING, UPPER_SKIPPED, UPPER_REQUESTED, UPPER_WAITING,
    UPPER_REFUSED, UPPER_GRANTED, UPPER_RETURNING, UPPER_RETURN_WAITING, UPPER_RETURNED
};
static volatile enum upper_capture_phase upper_phase;
static volatile int upper_grant_ready, upper_return_ready, upper_can_return;
static uint32_t upper_descriptor[10];
static uint32_t upper_grant_low, upper_grant_high, upper_return_low, upper_return_high;

#define SRM_UPPER_CAPTURE (1u << 5)
#define SRM_MOVIE_PLAYBACK (1u << 18)
#define SRM_UPPER_TWAIN (1u << 1)  /* Area 33 is in the high ownership word. */
#define SRM_OWNERS_LOW (*(volatile uint32_t *) 0x65EC)
#define SRM_OWNERS_HIGH (*(volatile uint32_t *) 0x65F0)

static void upper_grant_cbr(void *ctx, const uint32_t *descriptor)
{
    /* Canon's error path can pass a small integer in the descriptor argument. */
    if ((uintptr_t) descriptor >= 0x1000 && !((uintptr_t) descriptor & 3))
        memcpy(upper_descriptor, descriptor, sizeof(upper_descriptor));
    upper_grant_low = SRM_OWNERS_LOW;
    upper_grant_high = SRM_OWNERS_HIGH;
    asm volatile("dmb sy" ::: "memory");
    upper_grant_ready = 1;
}

static void upper_return_cbr(void *ctx, uint32_t unused1, uint32_t unused2)
{
    upper_return_low = SRM_OWNERS_LOW;
    upper_return_high = SRM_OWNERS_HIGH;
    asm volatile("dmb sy" ::: "memory");
    upper_return_ready = 1;
}

static void upper_capture_task(void *unused)
{
    volatile uint32_t *srm = *(volatile uint32_t * volatile *) 0x51CC;
    if (stop_req || rec_state != 1 || !lv || !is_movie_mode() ||
        !srm || !srm[0xAC / 4] || srm[0x150 / 4] != 0 || srm[0x32C / 4] ||
        (SRM_OWNERS_LOW & (SRM_UPPER_CAPTURE | SRM_MOVIE_PLAYBACK)) ||
        (SRM_OWNERS_HIGH & SRM_UPPER_TWAIN))
    {
        upper_phase = UPPER_SKIPPED;
        return;
    }

    /* Both blocks are passed to Canon, which frees them after dispatch.
     * Allocate them before requesting the grant so release has no allocation dependency. */
    void *(*canon_alloc)(uint32_t) = (void *) 0xE055B095;
    void (*canon_free)(void *) = (void *) 0xE055B24D;
    uint32_t *release = canon_alloc(0x38);
    uint32_t *envelope = canon_alloc(8);
    if (!release || !envelope)
    {
        if (release) canon_free(release);
        if (envelope) canon_free(envelope);
        upper_phase = UPPER_SKIPPED;
        return;
    }

    void (*request)(void (*)(void *, const uint32_t *), void *) = (void *) 0xE0571ACB;
    upper_phase = UPPER_REQUESTED;
    uint32_t start = get_ms_clock();
    request(upper_grant_cbr, 0);
    while (!upper_grant_ready)
    {
        /* SRM may defer a conflicting request. Keep this task available for
         * the late callback so a grant can still be returned after the take. */
        if (get_ms_clock() - start >= 1000) upper_phase = UPPER_WAITING;
        msleep(25);
    }
    asm volatile("dmb sy" ::: "memory");

    if (!(upper_grant_low & SRM_UPPER_CAPTURE) ||
        !(upper_grant_high & SRM_UPPER_TWAIN) || !upper_descriptor[0])
    {
        canon_free(release);
        canon_free(envelope);
        upper_phase = UPPER_REFUSED;
        return;
    }

    upper_phase = UPPER_GRANTED;
    while (!upper_can_return) msleep(25);

    memset(release, 0, 0x38);
    release[0x08 / 4] = 0x46;
    memcpy(release + 0x14 / 4, upper_descriptor, 32);
    release[0x34 / 4] = (uint32_t) upper_return_cbr;
    envelope[0] = 1;  /* Priority queue free operation. */
    envelope[1] = (uint32_t) release;
    uint32_t *queued = envelope;
    void (*post)(uint32_t, uint32_t **, uint32_t) = (void *) 0xE056A405;
    void (*wake)(uint32_t, uint32_t, uint32_t, void *, uint32_t) = (void *) 0xE056948B;
    upper_phase = UPPER_RETURNING;
    start = get_ms_clock();
    post(srm[0xAC / 4], &queued, 8);
    wake(srm[1], (uint32_t) srm, 0x29, 0, 0);
    while (!upper_return_ready)
    {
        if (get_ms_clock() - start >= 1000) upper_phase = UPPER_RETURN_WAITING;
        msleep(25);
    }
    asm volatile("dmb sy" ::: "memory");
    upper_phase = UPPER_RETURNED;
}

static uint32_t upper_get(void)
{
    if (!rw_addr || !record_native || pc_rowb || width < 3840) return 0;
    uint32_t slot_size = (frame_bytes + 0xFFFF) & ~0xFFFFu;
    if (rw_size < 3 * slot_size + 0x10000 + RING_TARGET) return 0;
    if (upper_phase != UPPER_IDLE && upper_phase != UPPER_SKIPPED &&
        upper_phase != UPPER_REFUSED && upper_phase != UPPER_RETURNED)
    {
        LOG("Upper capture: previous SRM request still pending\n");
        return 0;
    }
    if (upper_phase == UPPER_RETURNED &&
        ((upper_return_low & SRM_UPPER_CAPTURE) || (upper_return_high & SRM_UPPER_TWAIN)))
    {
        LOG("Upper capture: previous release left ownership set\n");
        return 0;
    }

    memset(upper_descriptor, 0, sizeof(upper_descriptor));
    upper_grant_ready = upper_return_ready = upper_can_return = 0;
    upper_phase = UPPER_STARTING;
    LOG("Upper capture: requesting Canon's additional capture buffer\n");
    save_log();
    if (task_create("crx_upper", 0x1f, 0x1000, upper_capture_task, 0) & 1)
        upper_phase = UPPER_SKIPPED;

    for (int i = 0; i < 40 && !stop_req &&
         (upper_phase == UPPER_STARTING || upper_phase == UPPER_REQUESTED); i++)
        msleep(25);

    if (upper_phase == UPPER_GRANTED && !stop_req)
    {
        asm volatile("dmb sy" ::: "memory");
        uint32_t start = upper_descriptor[0], end = upper_descriptor[6];
        uint32_t base = (start + 0xFFFF) & ~0xFFFFu;
        LOG("Upper capture: grant %08x-%08x, owners %08x/%08x\n",
            start, end, upper_grant_low, upper_grant_high);
        /* Map 0 returns one primary range and a reservation at its end.
         * Use the documented subrange inside CAPMEM1_AREA_U only. */
        if (start == 0x71B88000 && end == 0x74639000 && upper_descriptor[4] == 1 &&
            upper_descriptor[8] == 1 && end - base >= 3 * slot_size)
        {
            LOG("Upper capture: room for three DMA sinks at %08x (%d KB per sink)\n", base, slot_size >> 10);
            return base;
        }
        LOG("Upper capture: unexpected descriptor, keeping sinks in RecWork\n");
    }
    else
        LOG("Upper capture: grant unavailable (status %d), keeping sinks in RecWork\n", upper_phase);

    upper_can_return = 1;
    return 0;
}

static void upper_put(void)
{
    upper_can_return = 1;
    for (int i = 0; i < 40 && (upper_phase == UPPER_GRANTED ||
         upper_phase == UPPER_RETURNING); i++) msleep(25);
    if (upper_phase == UPPER_RETURNED)
    {
        asm volatile("dmb sy" ::: "memory");
        LOG("Upper capture: SRM release acknowledged, owners %08x/%08x\n",
            upper_return_low, upper_return_high);
    }
    else
        LOG("Upper capture: SRM release pending; cleanup task still active\n");
}

static void crx_control(void *from_shutter)
{
    int raw_requested = 0;
    uint32_t upper_base = 0;
    rec_state = 1;
    loglen = 0;
    stop_req = 0;
    if (!logbuf) logbuf = malloc(LOGSIZE);
    /* (test42 still allocated 34 KB of unused job structs from the small malloc heap here) */
    if (!logbuf) { NotifyBox(3000, "CRX: no memory"); rec_state = 0; return; }
    start_tasks();
    pick_logname();
    if (from_shutter) LOG("Trigger: physical SW2\n");

    /* wait for the ML menu to close, like the RAW rec toggle */
    for (int i = 0; i < 200 && gui_menu_shown(); i++) msleep(50);
    msleep(300);

    const char *blocker = crx_start_blocker();
    if (blocker) { NotifyBox(4000, "CRX: %s", blocker); goto out; }
    record_native = m50_crx_depth == CRX_12BIT_LV;
    record_bits = m50_crx_depth == CRX_14BIT ? 14 : 12;

    raw_lv_request();
    raw_requested = 1;
    int geo_ok = 0;
    for (int i = 0; i < 40 && !geo_ok; i++)
    {
        msleep(50);
        uint32_t yb_xb = *(volatile uint32_t *)(RAW_LV_EDMAC_CHANNEL_ADDR + 0x50);
        pitch = yb_xb & 0xFFFF; height = (yb_xb >> 16) + 1;
        width = pitch * 8 / 14;
        geo_ok = raw_lv_is_enabled() && pitch && width % 16 == 0 && height >= 200 && (height & 1) == 0
                 && width <= 6144 && pitch * 8 == width * 14;
    }
    if (!geo_ok)
    {
        NotifyBox(5000, "CRX: need a 2096-wide LV RAW stream (got %dx%d)", width, height);
        goto out;
    }
    frame_bytes = pitch * height;
    raw_update_params();
    {
        /* test67: crop = copy the rectangle into a compact buffer first (like mlv_lite does), then encode that with
         * stride == width (the encoder needs it; test62's stride != width gave broken frames). No crop: full-width rows,
         * top optical black cut (test63). Canon keeps the full readout for a live, centered software crop. */
        int ax1 = raw_info.active_area.x1, ay1 = raw_info.active_area.y1;
        uint32_t y0 = (ay1 + 3) & ~3;
        enc_w = width; enc_h = (height - y0) & ~7; pc_rowb = 0; pc_x0b = 0;
        normalize_format();
        uint32_t cw = resolutions[crx_resolution].width, ch = format_height(crx_aspect, crx_resolution);
        uint32_t right = MIN((uint32_t)width, (uint32_t)raw_info.active_area.x2);
        if (ch && (right < cw + ax1 || ch > enc_h))
        {
            NotifyBox(4000, "CRX: %dx%d exceeds this RAW stream", cw, ch);
            goto out;
        }
        if (ch)
        {
            y0 = (y0 + (enc_h - ch) / 2) & ~3;
            /* Retain the 64-pixel DMA alignment when the active area permits it. */
            uint32_t x0 = (ax1 + (right - ax1 - cw) / 2) & ~63u;
            if (x0 < (uint32_t)ax1) x0 = (ax1 + 15) & ~15u;
            if (x0 + cw > right) { NotifyBox(4000, "CRX: crop alignment exceeds stream"); goto out; }
            enc_w = cw; enc_h = ch; pc_x0b = x0 * 14 / 8; pc_rowb = cw * 14 / 8;
        }
        enc_off = y0 * pitch + pc_x0b;
    }
    if (record_native)
    {
        raw_lv_release();
        raw_requested = 0;
        if (raw_lv_is_enabled())
        {
            LOG("12-bit LV: another feature owns the RAW stream\n");
            goto out;
        }
        enc_off = enc_off * 6 / 7;
        pc_x0b = pc_x0b * 6 / 7;
        pc_rowb = pc_rowb * 6 / 7;
        pitch = width * 3 / 2;
        frame_bytes = pitch * height;
    }

    LOG("CRX framing: encoded %dx%d, source row %d, column byte %d\n",
        enc_w, enc_h, enc_off / pitch, pc_x0b);
    LOG("stage 1: stream %dx%d pitch %d, movie mode %d, sensor mode %d\n", width, height, pitch,
        is_movie_mode(), *(volatile uint32_t *)(0xEF24 + 8));
    mrrsc_log("at start");
    crx_log_memory_owners("before release", UINT32_MAX);
    if (is_movie_mode()) { mrrsc_release(); if (movrel_n) rw_get(); }
    if (rw_addr) crx_log_memory_owners("after RecWork grant", UINT32_MAX);
    save_log();
    for (int i = 0; i < 1000 && (writer_busy || jobs_in_flight()); i++) msleep(10);   /* previous recording */
    upper_base = m50_crx_upper ? upper_get() : 0;
    if (stop_req) goto out;
    if (!alloc_buffers(upper_base)) { NotifyBox(4000, "CRX: not enough memory\n(see ML/LOGS/CRXREC*.LOG)"); goto out; }

    if (record_native)
    {
        native_raw_start();
        for (int i = 0; i < 40; i++)
        {
            msleep(50);
            uint32_t geometry = *(volatile uint32_t *)(RAW_LV_EDMAC_CHANNEL_ADDR + 0x50);
            uint32_t pack = *(volatile uint32_t *) PACK_REG;
            if (geometry == ((height - 1) << 16 | pitch) &&
                (pack & NATIVE_PACK_MASK) == NATIVE_PACK_BITS)
            {
                hwpack_active = 2;
                break;
            }
        }
        LOG("12-bit LV: port 3 geometry %08x, pack %08x, %s\n",
            *(volatile uint32_t *)(RAW_LV_EDMAC_CHANNEL_ADDR + 0x50),
            *(volatile uint32_t *) PACK_REG, hwpack_active ? "verified" : "FAILED");
        if (!hwpack_active)
        {
            NotifyBox(4000, "12-bit LV: packing setup failed");
            goto out;
        }
    }

    LOG("stage 2: memory ok (sinks %08x %08x)\n", (uint32_t) sink[0], (uint32_t) sink[1]);
    save_log();
    /* file */
    FIO_CreateDirectory("ML/CRX");
    for (int i = 1; i < 99999; i++)
    {
        snprintf(fname, sizeof(fname), "ML/CRX/CRX%05d", i);
        char probe[64]; snprintf(probe, sizeof(probe), "%s.CRX", fname);
        uint32_t sz;
        if (FIO_GetFileSize(probe, &sz) != 0) break;
    }
    chunk = 0;
    if (!open_chunk()) { NotifyBox(4000, "CRX: file error"); goto out; }

    st_hooked = st_drop_worker = st_drop_slot = st_drop_queue = st_encoded = st_written = 0;
    st_enc_err = st_bytes_out = st_free_err = 0; st_enc_bytes = 0; diag_e = 0;
    qtest_frames = 0;
    st_enc_max = st_write_ms = 0;
    rec_fps = 0;
    rec_write_rate10 = rec_need_rate10 = -1;
    frame_counter = 0;
    rec_t0_us = get_us_clock();
    {
        extern void m50_sd_brake_reset_session(void);
        extern int m50_sd_write_opt_mode(void);
        extern int m50_sd_extreme_mode(void);
        extern uint32_t m50_sd_sscg_stock(void);
        m50_sd_brake_reset_session();
        st_sd_brakes = 0;
        st_sd_stepdown_mhz = 0;
        int sp = -1, ck = -1;
        int (*get_speed)(uint32_t, int *, int *) = (void *) 0xE01E9E39;   /* storage API: speed mode + clock id */
        get_speed(*(volatile uint32_t *)(0xB130 + 0x10), &sp, &ck);
        int pll_mhz = (int)(((*(volatile uint32_t *)0xD010F218) >> 4) & 0xFF) * 3;
        LOG("test105: %s copy, ring %d MB; SD mode %d, clock %d (%d MHz), cache %d, wopt %d, extreme %d, sscg %08x->%08x\n",
            "DMA", ring_size >> 20, sp, ck, pll_mhz, *(volatile uint32_t *) 0x19130,
            m50_sd_write_opt_mode(), m50_sd_extreme_mode(),
            m50_sd_sscg_stock(), *(volatile uint32_t *) 0xD010F224);
    }
    LOG("%s.CRX stream %dx%d pitch %d, encoded %dx%d (offset %d), %s %d-bit, black %d white %d, fps %d, repack core %d (alive %d)\n",
        fname, width, height, pitch, enc_w, enc_h, enc_off, m50_crx_mode ? "C-RAW" : "lossless",
        record_bits, raw_info.black_level >> (14 - record_bits),
        raw_info.white_level >> (14 - record_bits), fps_get_current_x1000(),
        (m50_crx_core && worker_alive[1]) ? 1 : 0, worker_alive[1]);

    hw_rough_canon = hw_rough_ml = -1; hw_fell_back = 0;
    if (m50_crx_hwpack && !record_native) hwpack_enable();
    LOG("packing: %s (pack reg %08x -> %08x, Mem1Path cfg3 %d)\n",
        record_native ? "12-bit Live View hardware" : hwpack_active ? "hardware test" : "CPU repack", saved_pack,
        *(volatile uint32_t *) PACK_REG, *(volatile uint32_t *)(MEM1PATH_PACK_CFG + 12));

    st_cpA_sum = st_cpA_n = st_cp_bad = st_cp_cpu = 0; cp_dma = 1; st_pc_max = st_pc_bad = st_pc_ovw = 0;
    pl_last_channel = -1;
    pc_m = pl_prog ? 1 : 2;
    pc_allow_wide = record_native && pc_rowb;
    st_pc_wide = 0;
    cp_state = 0; cp_h = 0; pending_h = 0;
    enc_posted = enc_taken = 0;
    LOG("stage 3: routing on\n");
    save_log();
    {
        extern void m50_sd_rec_write_begin(const char *path, int *df_ret, int *cmd20_ret);
        int df_r = -99, c20_r = -99;
        m50_sd_rec_write_begin(fname, &df_r, &c20_r);
        LOG("SD write opt: DelayFlush=%d CreateSC=%d (%s)\n", df_r, crx_sc, crx_sc ? "speed class" : "plain FIO");
    }
    cur = 0; last_given = 0; worker_busy = 0;
    for (int i = 0; i < COUNT(sink_owned); i++) sink_owned[i] = 0;
    st_sink_queued = 0;
    if (sink_count == 3) LOG("4K: three capture buffers, %d KB write ring\n", ring_size >> 10);
    rec_state = 2;
    uint32_t capture_start = get_ms_clock();
    rec_live = 1;
    asm volatile("dmb sy" ::: "memory");
    routing = 1;

    int t_last = get_ms_clock(); uint32_t w_last = 0, h_last = 0, e_last = 0;
    uint64_t b_last = 0, written_last = 0;
    extern uint32_t m50_daf_dma_programs(unsigned channel);
    uint32_t daf_a = m50_daf_dma_programs(0), daf_b = m50_daf_dma_programs(1);
    unsigned owners = UINT32_MAX;
    while (!stop_req)
    {
        msleep(100);
        int now = get_ms_clock();
        int secs = (now - (int)(rec_t0_us / 1000));
        if (st_encoded) owners = crx_log_memory_owners("encoding", owners);
        if (upper_base && (!(SRM_OWNERS_LOW & SRM_UPPER_CAPTURE) ||
                           !(SRM_OWNERS_HIGH & SRM_UPPER_TWAIN)))
        {
            LOG("Upper capture: SRM ownership changed during recording\n");
            stop_req = 1;
        }
        if (now - t_last >= 1000)
        {
            uint32_t encoded = st_encoded;
            uint64_t encoded_bytes = st_enc_bytes, written = st_bytes_out;
            uint32_t frames = encoded - e_last;
            uint32_t bytes = (uint32_t)(encoded_bytes - b_last);
            uint32_t elapsed_ms = now - t_last;
            rec_fps = frames * 1000 / elapsed_ms;
            rec_write_rate10 = (int)((written - written_last + (uint64_t)elapsed_ms * 50) / ((uint64_t)elapsed_ms * 100));
            /* Extrapolate recent frame sizes to the target rate even if frames
             * were skipped before encoding because the write buffer was full. */
            int target_fps1000 = fps_get_current_x1000();
            if (frames && bytes && target_fps1000 > 0)
            {
                uint64_t scale = (uint64_t)frames * 100000000;
                rec_need_rate10 = ((uint64_t)bytes * target_fps1000 + scale / 2) / scale;
            }
            LOG("%6d ms: hooked %d encoded %d (+%d) writes %d (+%d) drops w/s/q %d/%d/%d, ring %d MB, crop %d us (max %d), encode %d us (max %d), copy %d us, stream %d KB/s, out %d MB, write %d ms total, phase AF gate %d, DMA arm A/B %d/%d\n",
                secs, st_hooked, encoded, frames, st_written, st_written - w_last,
                st_drop_worker, st_drop_slot, st_drop_queue,
                jobs_in_flight(), st_pc_us, st_pc_max, st_enc_us, st_enc_max, st_copy_us,
                bytes >> 10, (int)(written >> 20), st_write_ms,
                *(volatile uint32_t *)(0xEF24 + 0x34),
                m50_daf_dma_programs(0) - daf_a, m50_daf_dma_programs(1) - daf_b);
            st_pc_max = st_enc_max = 0;
            t_last = now; w_last = st_written; h_last = st_hooked; e_last = encoded; b_last = encoded_bytes;
            written_last = written;
        }
        if (!lv) stop_req = 1;
    }
    (void) h_last;

    /* stop: no new frames, let the pipeline drain */
    rec_state = 3;
    routing = 0;
    uint32_t capture_ms = get_ms_clock() - capture_start;
    crx_log_memory_owners("at stop", UINT32_MAX);
    int native_buffered = (record_native && pc_rowb) || sink_count == 3;
    int drain_limit = native_buffered ? 500 : 300;
    for (int i = 0; i < drain_limit &&
         (crx_capture_busy() || encin_state[0] == 1 || encin_state[1] == 1 || native_encode_busy); i++) msleep(10);
    if (native_buffered && (crx_capture_busy() || native_encode_busy))
    {
        encoder_stalled = 1;
        LOG("stop: native capture still in flight\n");
    }
    msleep(200);
    if (record_native) native_raw_stop();
    else hwpack_restore();
    cp_kick_pending();
    cp_wait_and_free();
    wr_flush = 1;
    for (int i = 0; i < 3000 && (ring_used() || writer_busy); i++) msleep(10);   /* writer drains (<= 30 s) */
    if (ring_used()) LOG("stop: %d KB not written (dropped)\n", ring_used() >> 10);
    { FILE *ff = f; f = 0; asm volatile("dmb sy" ::: "memory"); for (int i = 0; i < 3000 && writer_busy; i++) msleep(10); if (ff) crx_close(ff); }
    { extern void m50_sd_rec_write_end(void); m50_sd_rec_write_end(); }
    r_rp = r_wp; r_wrap = 0xFFFFFFFF;
    msleep(300);                 /* Canon is back on its own LV buffer */
    if (crx_capture_busy())
    {
        LOG("stop: encoder may still read a sink\n");
        if (upper_base) encoder_stalled = 1;
    }
    if (sink_count == 3) LOG("4K: %d frames queued behind a busy capture buffer\n", st_sink_queued);
    if (!encoder_stalled)
    {
        if (upper_base) upper_put();
        rw_free(&rw_addr);
        rw_free(&rw_late);
        if (!crx_capture_busy()) free_buffers();
    }
    int dur = (int)((get_us_clock() - rec_t0_us) / 1000);
    LOG("STOP after %d ms: hooked %d, encoded %d, %d card writes (%d.%d/s), %d MB, drops worker %d slot %d queue %d, enc errors %d, free errors %d, cache fallback %d\n",
        dur, st_hooked, st_encoded, st_written,
        dur ? st_written * 1000 / dur : 0, dur ? (st_written * 10000 / dur) % 10 : 0,
        (int)(st_bytes_out >> 20), st_drop_worker, st_drop_slot, st_drop_queue, st_enc_err, st_free_err, st_cache_bad);
    {
        int kbs = dur ? (int)(st_enc_bytes * 1000 / 1024 / dur) : 0;
        LOG("encoded %d frames (%d.%d fps avg), %d MB in total = card would need %d.%d MB/s%s\n",
            st_encoded, dur ? st_encoded * 1000 / dur : 0, dur ? (st_encoded * 10000 / dur) % 10 : 0,
            (int)(st_enc_bytes >> 20), kbs / 1024, (kbs % 1024) * 10 / 1024,
            m50_crx_sample ? " (pipeline test: only 1 frame in 24 was written)" : "");
    }
    if (record_native) LOG("12-bit LV: %d frames during %d ms of capture, %d.%d fps\n",
        st_encoded, capture_ms, capture_ms ? st_encoded * 1000 / capture_ms : 0,
        capture_ms ? st_encoded * 10000 / capture_ms % 10 : 0);
    else if (record_bits == 12) LOG("12-bit conversion: last %d us\n", st_pc_us);
    if (m50_crx_qtest) LOG("C-RAW Q test %d: %d frames, stock high-band Q %d\n",
        m50_crx_qtest, qtest_frames, qtest_first);
    LOG("pre-crop: last %d us, max %d us, %d bad copies, %d source-changed\n", st_pc_us, st_pc_max, st_pc_bad, st_pc_ovw);
    if (record_native && pc_rowb)
        LOG("native crop: %d verified 8-byte DMA copies\n", st_pc_wide);
    LOG("copy: %d frames avg %d us, DMA %s (%d spot-check failures, %d CPU pieces), %d card writes avg %d KB\n",
        st_cpA_n, st_cpA_n ? st_cpA_sum / st_cpA_n : 0, cp_dma ? "kept" : "OFF", st_cp_bad, st_cp_cpu,
        st_written, st_written ? (int)(st_bytes_out / st_written) >> 10 : 0);
    LOG("card: %d MB in %d ms of writing = %d.%d MB/s while busy (%d%% of the time busy)\n", (int)(st_bytes_out >> 20), st_write_ms,
        st_write_ms ? (int)((st_bytes_out >> 10) / st_write_ms) : 0, st_write_ms ? (int)((st_bytes_out >> 10) * 10 / st_write_ms) % 10 : 0,
        dur ? (int)(st_write_ms * 100 / dur) : 0);
    save_log();
    NotifyBox(5000, "CRX: %d frames encoded, %d dropped, %d MB",
              st_encoded, crx_take_drops(), (int)(st_bytes_out >> 20));
out:
    { extern void m50_sd_rec_write_end(void); m50_sd_rec_write_end(); }
    native_raw_stop();
    if (raw_requested) raw_lv_release();
    if (rec_state == 1)
    {
        if (upper_base) upper_put();
        rw_free(&rw_addr);
        rw_free(&rw_late);
    }
    if (!encoder_stalled) mrrsc_restore();
    save_log();
    rec_state = 0;
}

/* The native stream is also accessible from the 16:9 resolution list. */
static int native_resolution_index = COUNT(resolutions);

static MENU_SELECT_FUNC(aspect_select)
{
    if (rec_state) return;
    normalize_format();
    crx_aspect = MOD(crx_aspect + delta, COUNT(aspects));
    normalize_format();
    if (SENSOR_MODE_NOW == 6 && crx_aspect != ASPECT_STREAM)
        while (resolutions[crx_resolution].width > 1992 || format_height(crx_aspect, crx_resolution) > 1128)
            crx_resolution++;
}

static MENU_SELECT_FUNC(resolution_select)
{
    if (rec_state || !delta) return;
    normalize_format();
    int wide = crx_aspect == ASPECT_WIDE || crx_aspect == ASPECT_STREAM;
    int index = crx_aspect == ASPECT_STREAM ? COUNT(resolutions) : crx_resolution;
    do {
        index = MOD(index + (delta > 0 ? 1 : -1), COUNT(resolutions) + wide);
        if (index == COUNT(resolutions))
        {
            crx_aspect = ASPECT_STREAM;
            return;
        }
    } while (format_height(wide ? ASPECT_WIDE : crx_aspect, index) > 2160);
    if (wide) crx_aspect = ASPECT_WIDE;
    crx_resolution = index;
}

static MENU_UPDATE_FUNC(aspect_update)
{
    normalize_format();
    MENU_SET_VALUE("%s", aspect_names[crx_aspect]);
}

static MENU_UPDATE_FUNC(resolution_update)
{
    normalize_format();
    static int cached_aspect = -1;
    static char labels[COUNT(resolutions)][24];
    static const char *choices[COUNT(resolutions) + 1];
    int aspect = crx_aspect == ASPECT_STREAM ? ASPECT_WIDE : crx_aspect;
    if (cached_aspect != aspect)
    {
        for (int i = 0; i < COUNT(resolutions); i++)
        {
            snprintf(labels[i], sizeof(labels[i]), "%s %dx%d", resolutions[i].name,
                     resolutions[i].width, format_height(aspect, i));
            choices[i] = labels[i];
        }
        choices[COUNT(resolutions)] = "Native stream";
        cached_aspect = aspect;
    }
    int first = 0;
    while (format_height(aspect, first) > 2160) first++;
    entry->min = first;
    entry->max = COUNT(resolutions) - 1 + (aspect == ASPECT_WIDE);
    entry->priv = crx_aspect == ASPECT_STREAM ? &native_resolution_index : &crx_resolution;
    entry->choices = choices + first;
    entry->icon_type = IT_DICE;
    MENU_SET_VALUE("%s", choices[MENU_INT(entry)]);
}

/* Claim the same idle state as the shutter and menu recording triggers. */
static char sd_result[40] = "Start";
static volatile unsigned sd_test_mib;

static void sd_test_task(void *unused)
{
    const unsigned total = 64u << 20;
    unsigned block = 4u << 20;
    const char *path = "ML/CRXTEST.TMP";
    const char *error = 0;
    void *fio_buf = 0;
    struct memSuite *suite = 0;
    void *buffer = 0;
    FILE *file = 0;
    uint32_t size, elapsed = 0;
    struct card_info *card = get_ml_card();
    if (RECORDING) { error = "Stop Canon recording"; goto done; }
    if (card->cluster_size > 0 && (uint64_t)(unsigned)card->free_space_raw * card->cluster_size < total + (1u << 20))
    { error = "Need 65 MiB free"; goto done; }
    if (FIO_GetFileSize(path, &size) == 0)
    { error = "Move ML/CRXTEST.TMP first"; goto done; }
    fio_buf = fio_malloc(block);
    if (fio_buf) buffer = fio_buf;
    else
    {
        suite = shoot_malloc_suite_contig(block);
        if (suite) buffer = GetMemoryAddressOfMemoryChunk(GetFirstChunkFromSuite(suite));
        else
        {
            block = 1u << 20;
            fio_buf = fio_malloc(block);
            buffer = fio_buf;
        }
    }
    if (!buffer) { error = "No test buffer"; goto done; }
    memset(buffer, 0x5a, block);
    file = FIO_CreateFile(path);
    if (!file) { error = "Cannot create test file"; goto done; }
    uint32_t start = get_ms_clock();
    for (unsigned written = 0; written < total; written += block)
    {
        if (RECORDING) { error = "Stopped for recording"; break; }
        if (FIO_WriteFile(file, buffer, block) != (int)block) { error = "SD write failed"; break; }
        sd_test_mib += block >> 20;
    }
    FIO_CloseFile(file);
    elapsed = (uint32_t)get_ms_clock() - start;
    if (!error && (!elapsed || FIO_GetFileSize(path, &size) || size != total))
        error = "Incomplete speed test";
    if (FIO_RemoveFile(path)) error = "Remove ML/CRXTEST.TMP";
done:
    if (fio_buf) fio_free(fio_buf);
    if (suite) shoot_free_suite(suite);
    if (error) snprintf(sd_result, sizeof(sd_result), "%s", error);
    else
    {
        unsigned rate = total / 100 / elapsed; /* decimal MB/s, in tenths */
        snprintf(sd_result, sizeof(sd_result), "%d.%d MB/s write", rate / 10, rate % 10);
    }
    NotifyBox(3500, "SD speed test: %s", sd_result);
    __sync_lock_release(&rec_state);
}

static MENU_SELECT_FUNC(sd_test_select)
{
    if (RECORDING || !__sync_bool_compare_and_swap(&rec_state, 0, CRX_SD_TEST)) return;
    sd_test_mib = 0;
    if (task_create("crx_sd_test", 0x1c, 0x1800, sd_test_task, 0) & 1)
    {
        snprintf(sd_result, sizeof(sd_result), "Cannot start test");
        __sync_lock_release(&rec_state);
    }
}

static MENU_UPDATE_FUNC(sd_test_update)
{
    if (rec_state == CRX_SD_TEST) MENU_SET_VALUE("Writing %d/64 MiB", sd_test_mib);
    else MENU_SET_VALUE("%s", sd_result);
}

static MENU_SELECT_FUNC(crx_reset_settings)
{
    if (rec_state) return;
    static struct config_var *const settings[] = {
        &__config_m50_crx_shutter,
        &__config_m50_crx_mode, &__config_m50_crx_qtest, &__config_m50_crx_depth,
        &__config_m50_crx_sample, &__config_m50_crx_hwpack,
        &__config_m50_crx_core, &__config_m50_crx_upper
    };
    for (unsigned i = 0; i < COUNT(settings); i++)
        *settings[i]->value = settings[i]->default_value;
    crx_aspect = ASPECT_WIDE;
    crx_resolution = 4;
    NotifyBox(2000, "CRX settings reset");
}

static void crx_toggle_recording(void *from_shutter)
{
    if (__sync_bool_compare_and_swap(&rec_state, 0, 1))
    {
        if (task_create("m50_crx_ctl", 0x1a, 0x2000, crx_control, from_shutter) & 1) rec_state = 0;
    }
    else if (rec_state == 2)
    {
        stop_req = 1;
    }
}

static MENU_SELECT_FUNC(crx_toggle)
{
    crx_toggle_recording(0);
}

/* M50 1.1.0 (ROM0.BIN 0xE0236ED2 / 0xE023804E / 0xE0A910CC):
 * - Video system (0 = NTSC, 1 = PAL) is property 0x02040001 (4 bytes), stored
 *   at (*(uint32_t **)0x8088)[0x19C / 4].
 * - PROP_VIDEO_MODE (0x80000039) is 8 words (32 bytes), stored at
 *   (*(uint32_t **)0x8088)[0x1A0 / 4 .. 0x1BC / 4]:
 *   { 0, res (5=4K, 0=FHD), fps_x100 (2398/2500/2997/5000/5994), gop (12/15/24/30), 0, 3, 1, 0 }. */
#define M50_PROP_VIDEO_SYSTEM 0x02040001
#define M50_SHOOT_STATE_PTR   (*(volatile uint32_t **)0x8088)

static int m50_crx_vsys;
extern void _prop_request_change(unsigned property, const void *addr, size_t len);
extern uint32_t video_mode_raw[10];
extern unsigned video_mode_raw_len;

static int crx_current_pal(void)
{
    volatile uint32_t *st = M50_SHOOT_STATE_PTR;
    if (st)
    {
        uint32_t fps100 = st[0x1a8 / 4];
        if (fps100 == 2398 || fps100 == 2997 || fps100 == 5994 || fps100 == 11988) return 0;
        if (fps100 == 2500 || fps100 == 5000 || fps100 == 10000) return 1;
        return st[0x19c / 4] ? 1 : 0;
    }
    if (video_mode_fps == 24 || video_mode_fps == 30 || video_mode_fps == 60) return 0;
    if (video_mode_fps == 25 || video_mode_fps == 50) return 1;
    return video_system_pal ? 1 : 0;
}

static MENU_SELECT_FUNC(crx_video_system_select)
{
    uint32_t next = (uint32_t)MOD(crx_current_pal() + delta, 2);
    m50_crx_vsys = (int)next;
    _prop_request_change(M50_PROP_VIDEO_SYSTEM, &next, sizeof(next));
    uint32_t vm[8] = { 0, 5, 2500, 12, 0, 3, 1, 0 };
    volatile uint32_t *st = M50_SHOOT_STATE_PTR;
    if (st)
    {
        for (int i = 0; i < 8; i++) vm[i] = st[0x1a0 / 4 + i];
    }
    else if (video_mode_raw_len >= sizeof(vm))
    {
        memcpy(vm, video_mode_raw, sizeof(vm));
    }
    if (vm[1] == 5)
    {
        vm[2] = next ? 2500 : 2398;
        vm[3] = 12;
    }
    else
    {
        vm[2] = next ? 2500 : 2997;
        vm[3] = next ? 12 : 15;
    }
    _prop_request_change(PROP_VIDEO_MODE, vm, sizeof(vm));
}

static MENU_UPDATE_FUNC(crx_video_system_update)
{
    m50_crx_vsys = crx_current_pal();
}

/* M50 1.1.0: PhySwIsr task dispatches SW2 through E01E4654.
 * It strips the pressed bit from 0x80000001, passing (1, 1) on press
 * and (1, 0) on release to the callback at 0x5A54. SW1 uses ID 0. */
typedef void (*physical_switch_fn)(uint32_t id, uint32_t pressed);
#define CANON_PHYSICAL_SWITCH ((physical_switch_fn) 0xE03D8D9B)
#define PHYSICAL_SWITCH_SLOT (*(physical_switch_fn volatile *) 0x5A54)

static void crx_physical_switch(uint32_t id, uint32_t pressed)
{
    extern int ml_started;
    static int held;
    if (id == 1)
    {
        if (held)
        {
            held = pressed != 0;
            return;
        }
        if (pressed && ml_started && m50_crx_shutter && lv &&
            DISPLAY_IS_ON && (SENSOR_MODE_NOW != 0 || rec_state != 0) &&
            (shooting_mode == SHOOTMODE_MOVIE || shooting_mode_custom == SHOOTMODE_MOVIE) &&
            !gui_menu_shown() && !MENU_MODE && !PLAY_MODE &&
            gui_state != GUISTATE_QMENU && !RECORDING)
        {
            held = 1;
            crx_toggle_recording((void *) 1);
            return;
        }
    }
    CANON_PHYSICAL_SWITCH(id, pressed);
}

static MENU_UPDATE_FUNC(crx_update)
{
    static const char *s[] = { "Start", "starting...", "STOP (recording)", "stopping..." };
    MENU_SET_VALUE("%s", rec_state == CRX_SD_TEST ? "SD test running" : s[rec_state & 3]);
    MENU_SET_RINFO("%d fr, %d MB", st_written, (int)(st_bytes_out >> 20));
    MENU_SET_HELP("Files in ML/CRX. Logs in ML/LOGS.");
}

enum {
    CRX_GRAPH_SAMPLES = 50, CRX_GRAPH_HEIGHT = 20,
    CRX_GRAPH_MARKER_HEIGHT = 4, CRX_GRAPH_MARKER_STEP = 8,
    CRX_GRAPH_MARKER_SPACE = 3 * CRX_GRAPH_MARKER_STEP,
    CRX_GRAPH_SD_BRAKE = 0x20, CRX_GRAPH_SD_DROP = 0x40, CRX_GRAPH_CAMERA_DROP = 0x80
};

static void crx_fill_panel(int color, int x, int y, int width, int height)
{
    /* Pixel-center insets for an eight-pixel quarter circle. */
    static const uint8_t inset[] = { 5, 3, 2, 1, 1 };
    int rows = COUNT(inset);
    bmp_fill(color, x, y + rows, width, height - 2 * rows);
    for (int row = 0; row < rows; row++)
    {
        int edge = inset[row];
        bmp_fill(color, x + edge, y + row, width - 2 * edge, 1);
        bmp_fill(color, x + edge, y + height - 1 - row, width - 2 * edge, 1);
    }
}

static void crx_draw_buffer_graph(int x, int y, int width, const uint8_t *history,
                                  int count, int next)
{
    int baseline = y + CRX_GRAPH_HEIGHT;
    bmp_fill(COLOR_GRAY(35), x, baseline + 1, width, 1);
    int sample = (next + CRX_GRAPH_SAMPLES - count) % CRX_GRAPH_SAMPLES;
    int previous_x = 0, previous_y = 0;
    for (int i = 0; i < count; i++)
    {
        int point = history[sample];
        int level = point & ~(CRX_GRAPH_SD_BRAKE | CRX_GRAPH_SD_DROP | CRX_GRAPH_CAMERA_DROP);
        int px = x + (CRX_GRAPH_SAMPLES - count + i) * (width - 1) / (CRX_GRAPH_SAMPLES - 1);
        int py = baseline - level;
        int color = level >= CRX_GRAPH_HEIGHT * 4 / 5 ? COLOR_YELLOW : COLOR_GREEN1;
        if (i) draw_line(previous_x, previous_y, px, py, color);
        if (point & CRX_GRAPH_SD_BRAKE)
            bmp_fill(COLOR_CYAN, px - 1, y - CRX_GRAPH_MARKER_SPACE, 4, CRX_GRAPH_MARKER_HEIGHT);
        if (point & CRX_GRAPH_SD_DROP)
            bmp_fill(COLOR_MAGENTA, px - 1, y - 2 * CRX_GRAPH_MARKER_STEP, 4, CRX_GRAPH_MARKER_HEIGHT);
        if (point & CRX_GRAPH_CAMERA_DROP)
            bmp_fill(COLOR_RED, px - 1, y - CRX_GRAPH_MARKER_STEP, 4, CRX_GRAPH_MARKER_HEIGHT);
        previous_x = px;
        previous_y = py;
        sample = (sample + 1) % CRX_GRAPH_SAMPLES;
    }
}

/* Panel coordinates are 720x480 on the M50. Scale both axes with the
 * same factor so a square RAW frame has a square Live View opening. */
struct crx_matte { int left, top, right, bottom, image_top, image_bottom; };

static struct crx_matte crx_preview_matte(int mode)
{
    struct crx_matte m = {0};
    if (crx_aspect == ASPECT_STREAM || (mode != 6 && mode != 12) || lv_dispsize > 1) return m;
    int source_w = mode == 12 ? 3840 : 1920;
    int source_h = source_w * 9 / 16;
    int width = resolutions[crx_resolution].width;
    int height = format_height(crx_aspect, crx_resolution);
    if (width > source_w || height > source_h) return m;
    int display_w = os.x_ex;
    int display_h = display_w * 9 / 16;
    if (display_w <= 0 || display_h > os.y_ex) return m;
    m.image_top = os.y0 + (os.y_ex - display_h) / 2;
    m.image_bottom = m.image_top + display_h;
    int w = width * display_w / source_w, h = height * display_w / source_w;
    m.left = os.x0 + (display_w - w) / 2;
    m.right = m.left + w;
    m.top = m.image_top + (display_h - h) / 2;
    m.bottom = m.top + h;
    return m;
}

static void crx_paint_matte(struct crx_matte m, int color)
{
    if (!m.image_bottom) return;
    if (m.top > m.image_top) bmp_fill(color, os.x0, m.image_top, os.x_ex, m.top - m.image_top);
    if (m.bottom < m.image_bottom) bmp_fill(color, os.x0, m.bottom, os.x_ex, m.image_bottom - m.bottom);
    if (m.left > os.x0) bmp_fill(color, os.x0, m.top, m.left - os.x0, m.bottom - m.top);
    if (m.right < os.x_max) bmp_fill(color, m.right, m.top, os.x_max - m.right, m.bottom - m.top);
}

static void crx_overlay_task(void *unused)
{
    /* The startup task reads magic.cfg before creating this task. */
    normalize_format();
    if ((unsigned)m50_crx_depth > CRX_12BIT_LV) m50_crx_depth = CRX_12BIT_LV;
    if ((unsigned)m50_crx_qtest >= COUNT(crx_quant_add)) m50_crx_qtest = 0;
    enum { PAD = 9, GAP = 4 };
    uint8_t history[CRX_GRAPH_SAMPLES];
    int count = 0, next = 0;
    uint32_t previous_sd_drops = 0, previous_camera_drops = 0, previous_sd_brakes = 0;
    struct crx_matte previous_matte = {0};
    int old_x = 0, old_y = 0, old_w = 0, old_h = 0;
    for (;;)
    {
        msleep(200);
        int state = rec_state;
        uint32_t capacity = ring_size >> 8;
        int fill = 0, drops = 0;
        if (state == 2 && rec_live && capacity)
        {
            fill = MIN(ring_used() >> 8, capacity) * 100 / capacity;
            uint32_t sd_drops = st_drop_queue;
            uint32_t camera_drops = crx_camera_drop_count();
            extern uint32_t m50_sd_intercept_count(void);
            uint32_t sd_brakes = st_sd_brakes + m50_sd_intercept_count();
            drops = sd_drops + camera_drops;
            int point = fill * CRX_GRAPH_HEIGHT / 100;
            if (sd_brakes > previous_sd_brakes) point |= CRX_GRAPH_SD_BRAKE;
            if (sd_drops > previous_sd_drops) point |= CRX_GRAPH_SD_DROP;
            if (camera_drops > previous_camera_drops) point |= CRX_GRAPH_CAMERA_DROP;
            history[next] = point;
            previous_sd_drops = sd_drops;
            previous_camera_drops = camera_drops;
            previous_sd_brakes = sd_brakes;
            next = (next + 1) % CRX_GRAPH_SAMPLES;
            if (count < CRX_GRAPH_SAMPLES) count++;
        }
        else
        {
            count = next = 0;
            previous_sd_drops = previous_camera_drops = previous_sd_brakes = 0;
        }

        if (!DISPLAY_IS_ON || !SENSOR_MODE_NOW)
        {
            old_w = 0;
            continue;
        }
        if (!LV_NON_PAUSED || (!state && !is_movie_mode()) ||
            gui_menu_shown() || gui_state != GUISTATE_IDLE)
        {
            if (previous_matte.image_bottom)
            {
                AcquireRecursiveLock(bmp_lock, 0);
                crx_paint_matte(previous_matte, COLOR_EMPTY);
                ReleaseRecursiveLock(bmp_lock);
                previous_matte = (struct crx_matte){0};
                redraw();
            }
            if (old_w) { old_w = 0; redraw(); }
            continue;
        }
        char title[80], detail[52], rates[52];
        int title_color = COLOR_WHITE;
        if (count)
        {
            int elapsed = (get_us_clock() - rec_t0_us) / 1000000;
            snprintf(title, sizeof(title), "CRX REC  %d:%02d  |  %d fps  |  %d MB",
                     elapsed / 60, elapsed % 60, (int)rec_fps, (int)(st_bytes_out >> 20));
            /* Pass the percent sign as string data to Canon's formatter. */
            snprintf(detail, sizeof(detail), "Buffer %d%sTotal dropped: %d", fill, "%  |  ", drops);
            int write = rec_write_rate10, need = rec_need_rate10;
            if (write < 0)
                snprintf(rates, sizeof(rates), "%s", "Write -- MB/s  |  Need -- MB/s");
            else if (need < 0)
                snprintf(rates, sizeof(rates), "Write %d.%d MB/s  |  Need -- MB/s", write / 10, write % 10);
            else
                snprintf(rates, sizeof(rates), "Write %d.%d MB/s  |  Need ~%d.%d MB/s",
                         write / 10, write % 10, need / 10, need % 10);
        }
        else if (state == CRX_SD_TEST)
            snprintf(title, sizeof(title), "SD speed test  |  %d/64 MiB", sd_test_mib);
        else if (state == 3)
            snprintf(title, sizeof(title), "CRX Saving...  |  Total dropped: %d", crx_take_drops());
        else if (state)
            snprintf(title, sizeof(title), "%s", "CRX Starting...");
        else
        {
            const char *blocker = crx_start_blocker();
            if (blocker)
            {
                title_color = COLOR_YELLOW;
                snprintf(title, sizeof(title), "CRX: %s", blocker);
            }
            else
            {
                char size[32];
                if (crx_aspect == ASPECT_STREAM) snprintf(size, sizeof(size), "Full stream");
                else snprintf(size, sizeof(size), "%dx%d", resolutions[crx_resolution].width,
                              format_height(crx_aspect, crx_resolution));
                snprintf(title, sizeof(title), "CRX READY  |  %s  |  %d-bit  |  %s",
                         size, m50_crx_depth == CRX_14BIT ? 14 : 12, m50_crx_mode ? "C-RAW" : "Lossless");
            }
        }

        int bg = COLOR_BLACK;
        int title_font = FONT(FONT_MED, title_color, bg);
        int detail_font = FONT(FONT_MONO_20, drops || fill >= 80 ? COLOR_YELLOW : COLOR_WHITE, bg);
        int rate_font = FONT(FONT_MONO_20, COLOR_WHITE, bg);
        int title_width = bmp_string_width(title_font, title) + (count ? 14 : 0);
        int detail_width = count ? bmp_string_width(detail_font, detail) : 0;
        int rate_width = count ? bmp_string_width(rate_font, rates) : 0;
        int w = MAX(MAX(title_width, detail_width), rate_width) + PAD * 2;
        int title_h = fontspec_height(title_font);
        int detail_h = count ? fontspec_height(detail_font) : 0;
        int legend_h = count ? fontspec_height(FONT_SMALL) + GAP : 0;
        int graph_h = CRX_GRAPH_HEIGHT + CRX_GRAPH_MARKER_SPACE + 2;
        int h = title_h + PAD * 2 + (count ? 2 * detail_h + legend_h + graph_h + GAP * 3 : 0);
        int x = (os.x0 + os.x_max - w) / 2;
        int y = MIN(os.y_max, 480) - h - 26;
        AcquireRecursiveLock(bmp_lock, 0);
        if (old_w && (x != old_x || y != old_y || w != old_w || h != old_h))
            crx_fill_panel(COLOR_EMPTY, old_x, old_y, old_w, old_h);
        struct crx_matte matte = crx_preview_matte(SENSOR_MODE_NOW);
        if (memcmp(&matte, &previous_matte, sizeof(matte)))
            crx_paint_matte(previous_matte, COLOR_EMPTY);
        crx_paint_matte(matte, COLOR_BLACK);
        previous_matte = matte;
        crx_fill_panel(bg, x, y, w, h);
        int tx = x + (w - title_width) / 2;
        int ty = y + PAD;
        if (count)
        {
            bmp_fill(COLOR_RED, tx, ty + (title_h - 7) / 2, 7, 7);
            tx += 14;
        }
        bmp_puts(title_font, &tx, &ty, title);
        if (count)
        {
            tx = x + (w - detail_width) / 2;
            ty = y + PAD + title_h + GAP;
            bmp_puts(detail_font, &tx, &ty, detail);
            tx = x + (w - rate_width) / 2;
            ty = y + PAD + title_h + detail_h + GAP * 2;
            bmp_puts(rate_font, &tx, &ty, rates);
            int graph_y = y + h - PAD - legend_h - CRX_GRAPH_HEIGHT - 2;
            crx_draw_buffer_graph(x + PAD, graph_y,
                                  w - PAD * 2, history, count, next);
            int key_x = x + PAD;
            int key_y = graph_y + CRX_GRAPH_HEIGHT + 2 + GAP;
            bmp_puts(FONT(FONT_SMALL, COLOR_MAGENTA, bg), &key_x, &key_y, "SD drops");
            key_x += 10;
            bmp_puts(FONT(FONT_SMALL, COLOR_RED, bg), &key_x, &key_y, "Camera drops");
            key_x += 10;
            char brake_key[24];
            if (st_sd_stepdown_mhz)
                snprintf(brake_key, sizeof(brake_key), "SD->%dM", st_sd_stepdown_mhz);
            else
                snprintf(brake_key, sizeof(brake_key), "%s", "SD retune");
            bmp_puts(FONT(FONT_SMALL, COLOR_CYAN, bg), &key_x, &key_y, brake_key);
        }
        ReleaseRecursiveLock(bmp_lock);
        old_x = x;
        old_y = y;
        old_w = w;
        old_h = h;
    }
}

TASK_CREATE("crx_overlay", crx_overlay_task, 0, 0x1f, 0x1000);

static struct menu_entry crx_menu[] = {
    {
        .name = "CRX recording",
        .select = menu_open_submenu,
        .help = "RAW video via Canon's CR3 encoder.",
        .children = (struct menu_entry[]) {
            { .name = "Record",        .select = crx_toggle, .update = crx_update },
            { .name = "Aspect ratio", .priv = &crx_aspect, .choices = aspect_names,
              .max = COUNT(aspects) - 1, .select = aspect_select, .update = aspect_update,
              .help = "Choose the frame shape. Full stream keeps all available columns." },
            { .name = "Resolution", .priv = &crx_resolution, .icon_type = IT_DICE,
              .max = COUNT(resolutions) - 1, .select = resolution_select, .update = resolution_update,
              .help = "SET opens sizes. Close the menu to see the selected frame." },
            { .name = "Movie shutter", .priv = &m50_crx_shutter, .max = 1,
              .choices = CHOICES("Canon", "Start/stop CRX") },
            { .name = "Upper buffer",  .priv = &m50_crx_upper, .max = 1,
              .choices = CHOICES("OFF (RecWork only)", "ON (4K extra sinks)"),
              .help = "Off: stay inside 200 MB RecWork. On: grab Canon upper SRM buffer." },
            { .name = "Compression",   .priv = &m50_crx_mode, .max = 1,
              .choices = CHOICES("Lossless", "C-RAW (lossy)"),
              .help = "C-RAW reduces detail and file size." },
            { .name = "C-RAW strength", .priv = &m50_crx_qtest, .max = COUNT(crx_quant_add) - 1,
              .choices = CHOICES("Stock", "+2", "+3", "+4", "+5", "+6"),
              .help = "Adds +2..+6 to high-band Q to cut bitrate." },
            { .name = "Video system", .priv = &m50_crx_vsys, .max = 1,
              .choices = CHOICES("NTSC (23.98p)", "PAL (25p)"),
              .select = crx_video_system_select, .update = crx_video_system_update,
              .help = "Switches Canon PROP_VIDEO_SYSTEM between 23.98p and 25p." },
            { .name = "Bit depth", .priv = &m50_crx_depth, .max = CRX_12BIT_LV,
              .choices = CHOICES("14-bit", "12-bit"),
              .help = "12-bit or 14-bit RAW capture." },
            { .name = "Card writes",   .priv = &m50_crx_sample, .max = 1,
              .choices = CHOICES("All frames", "1 in 24 (pipeline test)"),
              .help = "Encodes all frames, saves one in 24." },
            { .name = "Packing",       .priv = &m50_crx_hwpack, .max = 1,
              .choices = CHOICES("CPU repack", "Hardware (auto-check)"),
              .help = "Falls back to CPU if the check fails." },
            { .name = "Repack on core", .priv = &m50_crx_core, .max = 1,
              .choices = CHOICES("main (0)", "second (1)") },
            { .name = "SD speed test", .select = sd_test_select, .update = sd_test_update,
              .help = "Measure sequential write speed with a temporary 64 MiB file." },
            { .name = "Reset settings", .select = crx_reset_settings },
            MENU_EOL,
        },
    },
};

static int tasks_started = 0;
static void start_tasks(void)
{
    if (tasks_started) return;
    tasks_started = 1;
    work_mq[0] = (void *) msg_queue_create("crx_w0q", 2);
    work_mq[1] = (void *) msg_queue_create("crx_w1q", 2);
    enc_mq     = (void *) msg_queue_create("crx_encq", 4);
    enc_sem    = create_named_semaphore("crx_enc", SEM_CREATE_LOCKED);
    task_create("crx_w0", 0x1e, 0x1000, crx_worker, (void *) 0);
    task_create_ex("crx_w1", 0x18, 0x1000, crx_worker, (void *) 1, 1);
    task_create("crx_enc", 0x19, 0x1000, crx_encoder, 0);   /* test58: copy is on core 1 now; at 0x1f Canon's LV work delayed every request */   /* test51: was 0x19, above ML menu (0x1a) and redraw (0x1e): the per-frame copy starved the GUI */
    task_create("crx_wr", 0x1b, 0x1000, crx_writer, 0);
    cp_mq = (void *) msg_queue_create("crx_cpq", 2);
    cp_sem = create_named_semaphore("crx_cpd", SEM_CREATE_LOCKED);
    task_create("crx_cp", 0x1a, 0x1000, crx_copier, 0);   /* test135: priority 0x1a above crx_wr (0x1b), DMA copy on core 0 */
    for (int i = 0; i < 50 && !worker_alive[1]; i++) msleep(10);   /* give core 1 a moment */
}

static void crx_init(void *unused)
{
    menu_add("Debug", crx_menu, COUNT(crx_menu));
    if (PHYSICAL_SWITCH_SLOT == CANON_PHYSICAL_SWITCH)
        PHYSICAL_SWITCH_SLOT = crx_physical_switch;
}

INIT_FUNC("m50_crx", crx_init);

#endif
