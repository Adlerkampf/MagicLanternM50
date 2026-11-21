/** \file
 * Function overrides needed for M50 1.1.0
 */
/*
 * Copyright (C) 2021 Magic Lantern Team
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the
 * Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 */
 
#include <dryos.h>
#include <property.h>
#include <bmp.h>
#include <config.h>
#include <consts.h>
#include <lens.h>
#include <edmac.h>

/** GUI **/

//Not sure if all args are uint.
extern void gui_enqueue_message(uint32_t, uint32_t, uint32_t, uint32_t);
void GUI_Control(int bgmt_code, int obj, int arg, int unknown){
   gui_enqueue_message(0, bgmt_code, obj, arg);
}

// fake WINSYS_BMP_DIRTY_BIT_NEG
int winsys_bmp_dirty_bit_neg = 0;

/* True only when Canon's GUILockTask display state is TurnOn (0) or
 * MuteOnOSDOnly (1). When Canon mutes (2 = MuteOn) or powers off the
 * display (3 = TurnOff) for idle sleep, returns 0 so ML stops calling
 * XimrExe() and passes wake-up shutter presses through to Canon. */
int m50_display_is_on(void)
{
    /* GUILockTask state at 0x8EB8 + 0x54 (0x8F0C), getter 0xE065B514:
     * 0 = TurnOn, 1 = MuteOnOSDOnly, 2 = MuteOn, 3 = TurnOff */
    return *(volatile uint32_t *)0x8F0C < 2;
}

void LoadCalendarFromRTC(struct tm *tm)
{
   _LoadCalendarFromRTC(tm, 0, 0, 16);
}

int get_task_info_by_id(int unknown_flag, int task_id, void *task_attr)
{
    // task_id is something like two u16s concatenated.  The flag argument,
    // present on D45 but not on D678 allows controlling if the task info request
    // uses the whole thing, or only the low half.
    //
    // ML calls with this set to 1, meaning task_id is used as is,
    // if 0, the high half is masked out first.
    //
    // D678 doesn't have the 1 option, we use the low half as index
    // to find the full value.
    struct task *task = first_task + (task_id & 0xffff);
    return _get_task_info_by_id(task->taskId, task_attr);
}

/** File I/O **/

/**
* _FIO_GetFileSize returns now 64bit size in form of struct.
* This probably should be integrated into fio-ml for CONFIG_DIGIC_VIII
*/
extern int _FIO_GetFileSize64(const char *, void *);
int _FIO_GetFileSize(const char * filename, uint32_t * size){
   uint32_t size64[2];
   int code = _FIO_GetFileSize64(filename, &size64);
   *size = size64[0]; //return "lower" part
   return code;
}

/** WRONG: temporary overrides to get CONFIG_HELLO_WORLD working **/

void SetEDmac(unsigned int channel, void *address, struct edmac_info *ptr, int flags)
{
   return;
}

void ConnectWriteEDmac(unsigned int channel, unsigned int where)
{
   return;
}

void ConnectReadEDmac(unsigned int channel, unsigned int where)
{
   return;
}

void StartEDmac(unsigned int channel, int flags)
{
   return;
}

void AbortEDmac(unsigned int channel)
{
   return;
}

void RegisterEDmacCompleteCBR(int channel, void (*cbr)(void*), void* cbr_ctx)
{
   return;
}

void UnregisterEDmacCompleteCBR(int channel)
{
   return;
}

void RegisterEDmacAbortCBR(int channel, void (*cbr)(void*), void* cbr_ctx)
{
   return;
}

void UnregisterEDmacAbortCBR(int channel)
{
   return;
}

void RegisterEDmacPopCBR(int channel, void (*cbr)(void*), void* cbr_ctx)
{
   return;
}

void UnregisterEDmacPopCBR(int channel)
{
   return;
}

#ifdef CONFIG_RAW_LIVEVIEW
/* RAW video support. See raw_config.h for every camera-specific value. */

void _EngDrvOut(uint32_t reg, uint32_t value)
{
    /* Whitelist: the only engine register ML may write on this port is the
     * PACK32 bit-depth register, and only once it has been identified.
     * Everything else is dropped, exactly like the stock port. */
#if M50_PACK32_MODE_REG != 0
    if (reg == M50_PACK32_MODE_REG)
    {
        *(volatile uint32_t *)reg = value;
    }
#endif
    return;
}

/* The stock port returns 0 for every shadow-memory read. raw.c needs to
 * read the RAW EDMAC port (ram_addr, yb_xb). On DIGIC 8 we don't know the
 * shadow-memory layout, so read the MMIO directly - but only inside the
 * single window configured in raw_config.h. */
uint32_t shamem_read(uint32_t addr)
{
#ifdef CONFIG_RAW_PLACEHOLDER_ADDRS
    return 0; /* placeholder build: never touch hardware */
#else
    if (addr >= M50_MMIO_READ_WINDOW_START && addr < M50_MMIO_READ_WINDOW_END
        && (addr & 3) == 0)
    {
        return *(volatile uint32_t *)addr;
    }
    return 0;
#endif
}

/* ---- frame copy for mlv_lite (replaces src/edmac-memcpy.c paths) ---- */

void edmac_memcpy_res_lock() {}
void edmac_memcpy_res_unlock() {}

/* mlv_lite links against these even though our copy path ignores them */
uint32_t edmac_read_chan = 0x6;
uint32_t edmac_write_chan = 0x6;

#if M50_FRAME_COPY_BACKEND == M50_COPY_M2M
/* Layout copied from the working 200D implementation. On DIGIC 8 the
 * edmac_info struct has extra "s" fields (edmac.h, CONFIG_DIGIC_8X);
 * the m2m wrapper's argument layout is UNVERIFIED for M50. */
struct m2m_edmac_dst_src_info {
    struct edmac_info *src_region;
    struct edmac_info *dst_region;
    uint8_t *src;
    uint8_t *dst;
};
struct m2m_channel_res_info {
    uint32_t *resIds;
    uint32_t resIdCount;
    uint32_t channel_1;
    uint32_t channel_2;
};
struct m2m_copy_info {
    struct m2m_edmac_dst_src_info *edmac_regions;
    struct m2m_channel_res_info *res_info;
};
#ifndef M50_STUB_mem_to_mem_edmac_copy
#error "M50_COPY_M2M needs M50_STUB_mem_to_mem_edmac_copy (Thumb address, |1) - find the 200D-equivalent in ROM 1.1.0"
#endif
#endif

#if M50_FRAME_COPY_BACKEND == M50_COPY_DMA
#ifndef M50_STUB_dma_memcpy
#error "M50_COPY_DMA needs M50_STUB_dma_memcpy (Thumb address, |1)"
#endif
#endif

volatile int m50_cached_copy_bad = 0;

void* edmac_copy_rectangle_cbr_start(void *dst, void *src,
                                     int src_width, int src_x, int src_y,
                                     int dst_width, int dst_x, int dst_y,
                                     int w, int h,
                                     void (*cbr_r)(void *), void (*cbr_w)(void *), void *cbr_ctx)
{
    /* src_width = full line pitch in bytes, w = bytes per line to copy */
    if (src == NULL || dst == NULL || w <= 0 || h <= 0)
    {
        ASSERT(0);
        return NULL;
    }

    uint8_t *s = (uint8_t *)src + src_x + src_y * src_width;
    uint8_t *d = (uint8_t *)dst + dst_x + dst_y * dst_width;

#if M50_FRAME_COPY_BACKEND == M50_COPY_CPU
    /* Line by line, so cropping works too.
     * Source was written by Canon's DMA: invalidate the source range in the
     * data cache, then read it through the cache with a spot check fallback. */
    if (!m50_cached_copy_bad)
    {
        uint8_t *cs = (uint8_t *)CACHEABLE(s);
        uint32_t span = (uint32_t)src_width * (h - 1) + w;
        uint32_t a = (uint32_t)cs & ~31u, end = (uint32_t)cs + span;
        for (; a < end; a += 32) asm volatile("mcr p15, 0, %0, c7, c6, 1" :: "r"(a)); /* DCIMVAC */
        asm volatile("dsb sy" ::: "memory");
        volatile uint32_t *cw = (uint32_t *)((uint32_t)cs & ~3u);
        volatile uint32_t *uw = (uint32_t *)UNCACHEABLE(cw);
        uint32_t nw = span / 4;
        for (int i = 1; i <= 8; i++)
            if (cw[nw / 9 * i] != uw[nw / 9 * i]) m50_cached_copy_bad++;
        s = m50_cached_copy_bad ? (uint8_t *)UNCACHEABLE(s) : cs;
    }
    else
    {
        s = (uint8_t *)UNCACHEABLE(s);
    }
    if (w == src_width && w == dst_width)
    {
        memcpy(d, s, (size_t)w * h);
    }
    else
    {
        for (int y = 0; y < h; y++)
        {
            memcpy(d + y * dst_width, s + y * src_width, w);
        }
    }
    if (d == CACHEABLE(d))
    {
        sync_caches(); /* destination is later written to card by DMA */
    }

#elif M50_FRAME_COPY_BACKEND == M50_COPY_DMA
    /* BLTDMA linear copy: full width only (mlv_lite forces res_x = max). */
    ASSERT(w == src_width);
    if (src == CACHEABLE(src)) sync_caches();
    void *(*dma_memcpy_fn)(void *, void *, size_t) = (void *)(M50_STUB_dma_memcpy);
    dma_memcpy_fn(d, s, (size_t)w * h);

#elif M50_FRAME_COPY_BACKEND == M50_COPY_M2M
    ASSERT(dst == UNCACHEABLE(dst));
    if (src == CACHEABLE(src)) sync_caches();

    /* DIGIC 8 edmac_info adds "s" fields (3rd dimension?), left at 0 */
    struct edmac_info src_region = {
        .off1b = src_width - w,
        .xb = w,
        .yb = h - 1,
    };
    struct edmac_info dst_region = {
        .off1b = 0,
        .xb = w,
        .yb = h - 1,
    };
    struct m2m_edmac_dst_src_info regions = {
        .src_region = &src_region,
        .dst_region = &dst_region,
        .src = (uint8_t *)((uint32_t)s & 0x1FFFFFFF), /* UNVERIFIED mask for D8 map */
        .dst = (uint8_t *)((uint32_t)d & 0x1FFFFFFF),
    };
    uint32_t resIds[1] = { 0x00000080 }; /* as on 200D; UNVERIFIED */
    struct m2m_channel_res_info res = {
        .resIds = resIds,
        .resIdCount = 1,
        .channel_1 = M50_M2M_CHAN_1,
        .channel_2 = M50_M2M_CHAN_2,
    };
    struct m2m_copy_info info = { .edmac_regions = &regions, .res_info = &res };

    void (*m2m_copy)(struct m2m_copy_info *) = (void *)(M50_STUB_mem_to_mem_edmac_copy);
    m2m_copy(&info); /* blocking; hangs if channels clash with LiveView */
#else
#error "unknown M50_FRAME_COPY_BACKEND"
#endif

    goto done;
done:
    /* All backends above are synchronous: report completion now. */
    if (cbr_r) cbr_r(cbr_ctx);
    if (cbr_w) cbr_w(cbr_ctx);
    return dst;
}

void edmac_copy_rectangle_adv_cleanup() {}

/* NOTE: M50_COPY_DMA is full-width only. If you select it, add the M50 to
 * the 7D2 full-width special case in mlv_lite.c update_resolution_params()
 * and resolution_update(). CPU and M2M backends can crop. */

/* ---- symbols mlv_lite.mo links against, missing on this port ---- */

/* SRM is disabled on M50 (CONFIG_MEMORY_SRM_NOT_WORKING). mlv_lite treats
 * a NULL suite as "no SRM memory" and uses shoot_malloc only. Expect fewer
 * frame buffers than on DIGIC 5 cams => shorter bursts at high data rates. */
#include <exmem.h>
struct memSuite *srm_malloc_suite(int num_requested_buffers) { return NULL; }
void srm_free_suite(struct memSuite *suite) {}

/* Only lossless.c calls this, and only for 5D3/6D/70D/700D-class cams. */
struct LockEntry *CreateResLockEntry(uint32_t *resIds, uint32_t resIdCount)
{
    return NULL;
}

/* mlv_lite compares the current dialog handler to this to detect the
 * "card full" dialog. UNVERIFIED: find the real handler in ROM (200D:
 * xref of "ErrCardForLVApp"); until then that check never matches and a
 * full card is only caught by mlv_lite's own write errors. */
void ErrCardForLVApp_handler(void) {}

#else /* !CONFIG_RAW_LIVEVIEW: stock behaviour */

void _EngDrvOut(uint32_t reg, uint32_t value)
{
   return;
}

uint32_t shamem_read(uint32_t addr)
{
   return 0;
}

#endif /* CONFIG_RAW_LIVEVIEW */

void _engio_write(uint32_t* reg_list)
{
   return;
}

unsigned int UnLockEngineResources(struct LockEntry *lockEntry)
{
   return 0;
}
