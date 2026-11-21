/** \file
 * Polled LiveView "vsync" for EOS M50 1.1.0.
 *
 * Why: mlv_lite is driven by CBR_VSYNC, which ML normally fires from a hook
 * on Canon's EVF_STATE state object (src/state-object.c). On the M50 that
 * hook crashes in EvfCap, so CONFIG_STATE_OBJECT_HOOKS is off and ML gets
 * no per-frame callback - and wait_lv_frames() doesn't even exist.
 *
 * What: a high-priority task that watches for a new LiveView frame and
 * fires the same callbacks vsync_func() would. Frame source, in order:
 *   1. RAW EDMAC port ram_addr changes (Canon double-buffers the RAW stream)
 *   2. YUV display buffer flips (DV_VRAM_PANEL, known-good on this port)
 *   3. plain timer at the current FPS (last resort; may tear)
 * Whichever source changes first after RAW is enabled is locked in.
 *
 * Limits: DryOS msleep() granularity may be coarser than 1 ms (10 ms on some
 * older cams) - measure it with the probe menu before trusting it.
 * Polling at M50_VSYNC_POLL_MS gives up to ~1 ms of jitter after
 * the real end-of-frame. mlv_lite copies the frame out right away, so at
 * 1 ms on a ~41 ms frame (24p) the risk of copying while Canon is
 * overwriting is small but NOT zero. A real EVF_STATE hook (via MMU code
 * patch instead of the state-object spy) is the proper fix.
 *
 * GPLv2, same as Magic Lantern.
 */

#include <dryos.h>
#include <config.h>
#include <consts.h>
#include <tasks.h>
#include <module.h>
#include <fps.h>
#include <propvalues.h>
#include <edmac.h>
#include <raw.h>
#include <mem.h>

#if defined(CONFIG_M50_POLLED_VSYNC) && defined(CONFIG_RAW_LIVEVIEW) && !defined(CONFIG_STATE_OBJECT_HOOKS)

enum { SRC_NONE = 0, SRC_RAW_EDMAC, SRC_YUV_VRAM, SRC_TIMER, SRC_EDMAC_HOOK };

static volatile int vsync_counter = 0;
static volatile int vsync_source = SRC_NONE;
static volatile int vsync_running = 1;

/* ---- exact frame signal from the EDMAC hook (CONFIG_M50_EDMAC_HOOK) ----
 * Canon re-programs LiveView RAW port 3 once per frame from its capture-path
 * task (Controller/CapPath/Path.c -> Mem1Path 0xE080D36C), with the same
 * buffer address every time (measured: ~30 calls/s, 1 address).
 * At that moment the previous frame is complete and the next one is about
 * to be written - the same point where ML's vsync hook runs on DIGIC 5. */
static volatile uint32_t hook_ticks = 0;
static volatile int hook_last_ms = 0;

/* mlv_lite calls raw_lv_redirect_edmac() from its CBR_VSYNC handler, every
 * frame while recording. The redirect is applied only to the frame armed
 * right after that call, so it can never outlive mlv_lite's buffers: once
 * recording stops, Canon's own address is used again on the next frame. */
static volatile uint32_t redirect_addr = 0;
static volatile int redirect_pending = 0;

/* "Sink" buffer: Canon's own LV RAW buffer (0x2DCE7518 on the test camera)
 * is a debug buffer that the memory manager apparently does NOT reserve -
 * mlv_lite got slots there and Canon's DMA overwrote them ("Data corruption
 * at slot 151, frame 176", 2026-11-18). So while the RAW stream runs, every
 * frame mlv_lite does not redirect goes into this buffer, which we own.
 * raw.c then sees it as "Canon's buffer" (it reads ram_addr back from the
 * port), so mlv_lite double-buffers between two buffers that are both
 * really allocated. */
/* sized from the port registers: photo LV = 3668 x 838 (3.07 MB),
 * movie LV = 5460 bytes/line and much taller (~12 MB, MLV header 2026-11-18) */
static void * volatile sink_buf = 0;
static volatile uint32_t sink_size = 0;

extern uint32_t m50_bin_hook(void);
#ifndef M50_SPY_BUILD
extern uint32_t m50_crx_hook(void);
extern int m50_crx_routing(void);
#else
static inline uint32_t m50_crx_hook(void) { return 0; }
static inline int m50_crx_routing(void) { return 0; }
#endif
extern int m50_bin_take_redirect(void *ptr);

void m50_raw_set_redirect(void *ptr)
{
    /* full-sensor bin mode: mlv_lite's buffer only names where the next
     * binned frame goes; Canon's DMA keeps writing into our sinks */
    if (ptr && m50_crx_routing()) { redirect_pending = 0; return; }   /* never hand mlv_lite buffers to Canon now */
    if (ptr && m50_bin_take_redirect(ptr))
    {
        redirect_pending = 0;
        return;
    }
    redirect_addr = ptr ? (uint32_t) UNCACHEABLE(ptr) : 0;
    redirect_pending = (ptr != 0);
}

uint32_t m50_raw_port3_frame(uint32_t canon_addr)
{
    hook_ticks++;
    hook_last_ms = get_ms_clock();

    redirect_pending = 0;
    vsync_source = SRC_EDMAC_HOOK;
    vsync_counter++;

    /* CRX (hardware-compressed) recording owns the stream while it runs */
    uint32_t crx_addr = m50_crx_hook();
    if (crx_addr)
        return crx_addr;

    /* full-sensor bin mode: frames go to our two sinks; the bin task fires
     * CBR_VSYNC once each binned frame is ready (raw_bin.c) */
    uint32_t bin_addr = m50_bin_hook();
    if (bin_addr)
        return bin_addr;

#ifdef CONFIG_MODULES
    module_exec_cbr(CBR_VSYNC);
#endif
    if (redirect_pending && redirect_addr)
    {
        redirect_pending = 0;
        return redirect_addr;
    }
    if (sink_buf)
    {
        /* size Canon is about to write: pitch x lines from the port registers */
        uint32_t yb_xb = *(volatile uint32_t *)(RAW_LV_EDMAC_CHANNEL_ADDR + 0x50);
        uint32_t bytes = ((yb_xb >> 16) + 1) * (yb_xb & 0xFFFF);
        if (bytes && bytes <= sink_size)
            return (uint32_t) UNCACHEABLE(sink_buf);
    }
    return canon_addr;
}

static volatile uint32_t daf_edmac_calls[2];

/* Called from the EDMAC set-address hook (mmu_patches.h) for every port. */
uint32_t m50_edmac_hook(uint32_t port, uint32_t addr)
{
    if (port == 3)
        return m50_raw_port3_frame(addr);
    if (port == 5 || port == 6)
        daf_edmac_calls[port - 5]++;
    return addr;
}

uint32_t m50_daf_dma_programs(unsigned channel)
{
    return channel < 2 ? daf_edmac_calls[channel] : 0;
}

/* allocate the sink once the RAW stream is on (not allowed in the hook) */
static volatile int sink_fail = 0;
int m50_sink_info(uint32_t *size) { if (size) *size = sink_size; return sink_fail ? -1 : (sink_buf != 0); }

static void sink_alloc_step(void)
{
    if (!raw_lv_is_enabled()) return;
    uint32_t yb_xb = *(volatile uint32_t *)(RAW_LV_EDMAC_CHANNEL_ADDR + 0x50);
    uint32_t need = ((yb_xb >> 16) + 1) * (yb_xb & 0xFFFF);
    if (!need || need > 64 * 1024 * 1024) return;   /* port not set up yet */
    if (sink_buf && need <= sink_size) return;

    need = (need + 0x10000) & ~0xFFFF;              /* + margin, 64 KiB steps */
    void *b = fio_malloc(need);
    if (!b) { sink_fail = 1; return; }
    /* An older, smaller sink is intentionally NOT freed: the hook may have
     * just handed its address to Canon's DMA for the frame in flight.
     * (At most one leak per mode change, test builds only.) */
    sink_size = need;
    sink_buf = b;
    sink_fail = 0;
}

static int hook_is_ticking(void)
{
    return hook_ticks && (get_ms_clock() - hook_last_ms) < 200;
}

extern int raw_lv_is_enabled(void);
extern uint32_t shamem_read(uint32_t addr);

static uint32_t read_raw_edmac_addr(void)
{
#if defined(CONFIG_RAW_LIVEVIEW) && !defined(CONFIG_RAW_PLACEHOLDER_ADDRS)
    /* only touch the EDMAC block while Canon's RAW stream is actively ticking */
    if (!raw_lv_is_enabled() || !hook_is_ticking()) return 0;
    volatile struct edmac_mmio *ch = (struct edmac_mmio *)RAW_LV_EDMAC_CHANNEL_ADDR;
    return shamem_read((uint32_t)&ch->ram_addr);
#else
    return 0;
#endif
}

static uint32_t read_vram_addr(void)
{
    return (uint32_t)YUV422_LV_BUFFER_DISPLAY_ADDR;
}

/* the same work src/state-object.c vsync_func() does, minus the D45 bits */
static void FAST fire_vsync(void)
{
    vsync_counter++;
#ifdef CONFIG_MODULES
    module_exec_cbr(CBR_VSYNC);
#endif
}

int wait_lv_frames(int num_frames)
{
    int start = vsync_counter;
    int fps = fps_get_current_x1000();
    if (fps <= 0) fps = 30000;
    int timeout_ms = num_frames * (1000000 / fps) * 2 + 100;

    for (int t = 0; t < timeout_ms; t += 10)
    {
        if (vsync_counter - start >= num_frames) return 1;
        if (!lv) return 0;
        msleep(10);
    }
    return 0;
}

static void m50_vsync_task(void *unused)
{
    uint32_t last_raw = 0, last_vram = 0;
    int last_tick_ms = get_ms_clock();

    while (vsync_running)
    {
        msleep(M50_VSYNC_POLL_MS);

        if (!lv || !DISPLAY_IS_ON || !*(volatile uint32_t *)0x127C4)
        {
            vsync_source = SRC_NONE;
            last_raw = last_vram = 0;
            msleep(100);
            continue;
        }

        if (hook_is_ticking())
        {
            sink_alloc_step();
            /* the EDMAC hook fires CBR_VSYNC exactly once per frame */
            last_raw = last_vram = 0;
            msleep(20);
            continue;
        }
        if (vsync_source == SRC_EDMAC_HOOK) vsync_source = SRC_NONE;

        int now = get_ms_clock();
        int fps = fps_get_current_x1000();
        int frame_ms = fps > 0 ? 1000000 / fps : 33;

        uint32_t raw  = read_raw_edmac_addr();
        uint32_t vram = read_vram_addr();
        int raw_changed  = raw  && last_raw  && raw  != last_raw;
        int vram_changed = vram && last_vram && vram != last_vram;
        last_raw = raw;
        last_vram = vram;

        /* lock in a source once the RAW stream is up */
        if (vsync_source == SRC_NONE || (vsync_source != SRC_RAW_EDMAC && raw_changed))
        {
            if (raw_changed)       vsync_source = SRC_RAW_EDMAC;
            else if (vram_changed) vsync_source = SRC_YUV_VRAM;
            else if (now - last_tick_ms > 4 * frame_ms) vsync_source = SRC_TIMER;
        }

        int fire = 0;
        switch (vsync_source)
        {
            case SRC_RAW_EDMAC: fire = raw_changed; break;
            case SRC_YUV_VRAM:  fire = vram_changed; break;
            case SRC_TIMER:     fire = (now - last_tick_ms) >= frame_ms; break;
            default: break;
        }

        if (fire)
        {
            last_tick_ms = now;
            fire_vsync();
        }
    }
}

int m50_vsync_get_source(void) { return vsync_source; }

/* frame rate from the EDMAC hook, snapped to Canon's standard rates;
 * 0 if the hook is not ticking (RAW stream off) */
int m50_measured_fps_x1000(void)
{
    static int t0 = 0, last = 0;
    static uint32_t n0 = 0;
    if (!hook_is_ticking()) { t0 = 0; return 0; }
    int now = get_ms_clock();
    if (!t0) { t0 = now; n0 = hook_ticks; return last; }
    int dt = now - t0;
    if (dt >= 2000)
    {
        int f = (int)((hook_ticks - n0) * 1000000ull / dt);   /* fps x1000 */
        static const int std[] = { 23976, 25000, 29970, 50000, 59940, 119880 };
        int best = f, bd = 1 << 30;
        for (int i = 0; i < (int) COUNT(std); i++)
        {
            int d = f > std[i] ? f - std[i] : std[i] - f;
            if (d < bd) { bd = d; best = std[i]; }
        }
        last = (bd * 20 < best) ? best : f;   /* snap if within 5% */
        t0 = now; n0 = hook_ticks;
    }
    return last;
}

/* priority just above LiveView-ish ML tasks; small stack, no allocations */
TASK_CREATE("m50_vsync", m50_vsync_task, 0, 0x17, 0x800);

#endif
