/** \file
 * EOS M50 1.1.0: SD UHS-I 195 MHz + DIGIC 8 CARDPLL overclock (up to 336 MHz).
 *
 * Canon's SDR104 tuning (0xE0172EEE) copies the clock table at ROM 0xE0AC1F04
 * = { 9 (195 MHz), 8 (156), 7 (130), 6 (111), 0xE end } and pre-increments its
 * index, so after the first failed tune it never tries 195 MHz.
 *
 * Additionally, DIGIC 8 has a dedicated SD card PLL (CARDPLL, 0xD010F218)
 * separate from CPU/RAM/Video/Audio PLLs:
 *   0xD010F218 = (N << 4) | 0x3   (bit 0 = PLL enable, bit 1 = output enable)
 *   VCO = N * 12 MHz, SD clock (code 9, /4) = N * 3 MHz:
 *     N =  65 (0x413) -> 195 MHz (Canon stock)
 *     N =  80 (0x503) -> 240 MHz
 *     N =  92 (0x5C3) -> 276 MHz
 *     N = 100 (0x643) -> 300 MHz
 *     N = 108 (0x6C3) -> 324 MHz
 *     N = 112 (0x703) -> 336 MHz (tested 99.3 MB/s on 32 MB write+verify)
 *
 * Never applied automatically at boot: choose a speed in Prefs -> "SD clock"
 * and press SET on "Apply SD clock". Runs Canon's CMD19 tuning + a 4 MiB
 * write/readback verify; automatically falls back to 195 MHz on any error.
 *
 * GPLv2, same as Magic Lantern.
 */

#include <dryos.h>
#include <config.h>
#include <menu.h>
#include <propvalues.h>
#include <fio-ml.h>
#include <exmem.h>
#include <bmp.h>

#if CONFIG_FW_VERSION == 110 && defined(CONFIG_M50_SD195)

#define SD_CARD         (*(volatile uint32_t *)(0xB130 + 0x10))   /* slot index, 0 = SD */
#define SD_UHS_HOST     (*(volatile uint32_t *)(0xB130 + 0x14))   /* 1=UHS-I host enabled, 0=disabled by Canon brake (0xE0173CFC) */
#define SD_INIT_MODE    (*(volatile uint32_t *)(0xB130 + 0x30))   /* 0=UHS-I (SdCARDInitialize), 1=Legacy 3.3V, 2=UHS-II */
#define SD_UHS_ENABLE(c) (*(volatile uint32_t *)(0xB1CC + (c) * 4)) /* 0=3.3V only, 1=1.8V UHS-I, 2=UHS-II */
#define SD_PSTGDEV_PTR  (*(volatile uint32_t *)0x115B8)           /* pStgDev struct in RAM; +0x18 = ioctl fn ptr, +0x50 = bPowerDown */
#define SD_IOCTL_ORIG   0xE017EEE1u                               /* Canon SDDRV ioctl handler (Thumb) */
#define SD_IOCTL_PC_CMD 0x1004u                                   /* DEVICE_POWER_CYCLE ioctl command */
#define SD_UHS_TUNE     0xE0173F17      /* (int), Thumb */
/* Canon's DEVICE_POWER_CYCLE (0xE017EEB8) first calls 0xE0173CFC which sets SD_UHS_HOST=0!
 * Calling its 5 inner steps directly with SD_UHS_HOST=1 power-cycles the SD slot BACK into 1.8V UHS-I. */
#define SD_PC_POWER_OFF   0xE0174521    /* (void) -> uint32_t saved_sleep, Thumb */
#define SD_PC_HW_REINIT   0xE017453B    /* (void), Thumb (copies SD_UHS_HOST -> 0xB1CC) */
#define SD_PC_POWER_ON    0xE0173107    /* (void), Thumb (SdCARDInitialize + UHS-I + tune) */
#define SD_PC_POST_INIT   0xE0173D05    /* (void), Thumb */
#define SD_PC_RESTORE_SLP 0xE017309B    /* (uint32_t saved_sleep), Thumb */
#define SD_SET_CLOCK    0xE01E8953      /* (card, clock code), Thumb */
#define SD_TUNE         0xE01EB071      /* (card), Thumb */
#define SD_GET_SPEED    0xE01E9E39      /* (card, int *speed, int *clock), Thumb */
#define CLK_195         9

#define CARDPLL_CTRL    (*(volatile uint32_t *)0xD010F218)
#define SD0_CLK_EN      (*(volatile uint32_t *)0xD010060C)
#define SDCHK_TMP_FILE  "ML/SDCHK.TMP"
#define SDCHK_SIZE      (4 * 1024 * 1024)

static const int sd_oc_n[] = { 0, 65, 80, 92, 96, 100, 104, 108, 112 };
static CONFIG_INT("sd.m50_clock_preset", sd_oc_preset, 4);  /* default: 288 MHz (auto-recovers & steps down on brake) */

static int sd_apply_state = 0;  /* 0 idle, 1 running, 2 OK, 3 failed (reverted) */
static int sd_active_idx = 0;   /* currently active index into sd_oc_n[] */
static int sd_active_mhz = 0;
#define SD_BRAKE_WINDOW_MS 30000 /* 4 hits within 30 s triggers 1-notch step-down */
static int sd_brake_hits = 0;   /* brake hits inside current sliding window */
static int sd_brake_ts[4];      /* get_ms_clock() timestamps of recent hits */
static int sd_brake_dead = 0;   /* 1 if 1.8V UHS-I restore failed */
static volatile uint32_t sd_intercept_count = 0; /* brakes intercepted inside SDDRV ioctl(0x1004) */
static volatile int sd_intercept_stepdown_mhz = 0;
static int sd_ioctl_hooked = 0;
static char sd_apply_rinfo[24] = "";

static int read_clock(void)
{
    int speed = -1, clock = -1;
    int (*get_speed)(uint32_t, int *, int *) = (void *) SD_GET_SPEED;
    get_speed(SD_CARD, &speed, &clock);
    return clock;
}

static int current_pll_mhz(void)
{
    int n = (int)((CARDPLL_CTRL >> 4) & 0xFF);
    return n * 3;
}

static void pll_set_raw(int N)
{
    SD0_CLK_EN = 0;
    CARDPLL_CTRL &= ~2u;
    CARDPLL_CTRL &= ~1u;
    asm volatile("dmb sy" ::: "memory");
    msleep(1);

    CARDPLL_CTRL = ((uint32_t)N << 4) | 1u;
    asm volatile("dmb sy" ::: "memory");
    msleep(2);

    CARDPLL_CTRL = ((uint32_t)N << 4) | 3u;
    asm volatile("dmb sy" ::: "memory");
    msleep(1);
}

static int pll_set_and_tune(uint32_t card, int N)
{
    int (*set_clock)(uint32_t, int) = (void *) SD_SET_CLOCK;
    int (*tune)(uint32_t) = (void *) SD_TUNE;

    pll_set_raw(N);
    if (set_clock(card, CLK_195) & 1) return -1;
    if (tune(card) & 1) return -2;
    return 0;
}

/* Power-cycle ONLY the SD slot back into 1.8V UHS-I (caller MUST have zero open files).
 * Returns 1 if the card came back in 1.8V UHS-I (speed >= 2, clock <= 9), 0 if still 3.3V. */
static int sd_last_pc_speed = -1, sd_last_pc_clock = -1;
static uint32_t sd_last_pc_m0 = 0, sd_last_pc_u0 = 0;

static int sd_power_cycle_to_uhs(void)
{
    uint32_t (*pc_off)(void) = (void *) SD_PC_POWER_OFF;
    void (*pc_reinit)(void) = (void *) SD_PC_HW_REINIT;
    void (*pc_on)(void) = (void *) SD_PC_POWER_ON;
    void (*pc_post)(void) = (void *) SD_PC_POST_INIT;
    void (*pc_restore)(uint32_t) = (void *) SD_PC_RESTORE_SLP;
    void (*canon_uhs_tune)(int) = (void *) SD_UHS_TUNE;
    int (*get_speed)(uint32_t, int *, int *) = (void *) SD_GET_SPEED;
    uint32_t card = SD_CARD;

    sd_last_pc_m0 = SD_UHS_HOST;
    sd_last_pc_u0 = SD_UHS_ENABLE(card);

    /* Reset CARDPLL to stock 780 MHz and re-enable UHSHost (0xB144 = 1) before power-cycling.
     * Skip 0xE0173CFC (which sets 0xB144 = 0) and run DEVICE_POWER_CYCLE's 5 inner steps directly. */
    pll_set_raw(65);
    SD0_CLK_EN = 1;
    SD_INIT_MODE = 0;
    SD_UHS_HOST = 1;
    SD_UHS_ENABLE(card) = 1;
    asm volatile("dmb sy" ::: "memory");
    msleep(2);

    uint32_t saved_sleep = pc_off();
    pc_reinit();
    pc_on();
    pc_post();
    pc_restore(saved_sleep);
    msleep(10);
    canon_uhs_tune(1);

    int speed = -1, clock = -1;
    get_speed(card, &speed, &clock);
    sd_last_pc_speed = speed;
    sd_last_pc_clock = clock;
    return (speed >= 2 && clock <= CLK_195);
}

/* Intercept Canon's SDDRV ioctl(dev, 0x1004, arg) (DEVICE_POWER_CYCLE) in RAM!
 * Normally Canon's brake calls 0xE0173CFC (UHSHost=0 -> 3.3V 48 MHz), finishes the 16 MiB
 * write at 22 MB/s (~700 ms), then crx_writer had to split the file and power-cycle a 2nd time
 * (~1.8 s total). Intercepting 0x1004 keeps UHSHost=1, runs ONE clean 1.8V power-cycle + re-tune
 * (~300 ms), and lets Canon's retry finish at full 130+ MB/s with ZERO file split! */
static int sd_ioctl_hook(uint32_t a0, uint32_t cmd, uint32_t a2)
{
    int (*orig)(uint32_t, uint32_t, uint32_t) = (void *) SD_IOCTL_ORIG;
    if (cmd == SD_IOCTL_PC_CMD && sd_active_idx > 1 && !sd_brake_dead)
    {
        uint32_t dev = SD_PSTGDEV_PTR;
        if (dev && *(volatile uint32_t *)(dev + 0x50) == 0) /* bPowerDown == 0 */
        {
            uint32_t card = SD_CARD;
            int now_ms = get_ms_clock();
            int kept = 0;
            for (int i = 0; i < sd_brake_hits && i < 4; i++)
            {
                if ((uint32_t)(now_ms - sd_brake_ts[i]) <= SD_BRAKE_WINDOW_MS)
                    sd_brake_ts[kept++] = sd_brake_ts[i];
            }
            if (kept < 4) sd_brake_ts[kept++] = now_ms;
            sd_brake_hits = kept;

            int stepped = 0;
            if (sd_brake_hits >= 4 && sd_active_idx > 1)
            {
                sd_active_idx--;
                sd_brake_hits = 0;
                stepped = 1;
            }

            if (sd_power_cycle_to_uhs())
            {
                int target_n = sd_oc_n[sd_active_idx];
                if (pll_set_and_tune(card, target_n) == 0)
                {
                    sd_active_mhz = current_pll_mhz();
                    sd_intercept_count++;
                    if (stepped)
                    {
                        sd_intercept_stepdown_mhz = sd_active_mhz;
                        snprintf(sd_apply_rinfo, sizeof(sd_apply_rinfo), "brake->%dM", sd_active_mhz);
                        NotifyBox(3500, "SD brake x4 -> %d MHz (fast)", sd_active_mhz);
                    }
                    else
                    {
                        NotifyBox(2000, "SD brake #%d/3 -> %d MHz (fast)", sd_brake_hits, sd_active_mhz);
                    }
                    return 0;
                }
                while (sd_active_idx > 1)
                {
                    sd_active_idx--;
                    if (pll_set_and_tune(card, sd_oc_n[sd_active_idx]) == 0)
                    {
                        sd_active_mhz = current_pll_mhz();
                        sd_brake_hits = 0;
                        sd_intercept_count++;
                        sd_intercept_stepdown_mhz = sd_active_mhz;
                        snprintf(sd_apply_rinfo, sizeof(sd_apply_rinfo), "brake->%dM", sd_active_mhz);
                        NotifyBox(3500, "SD brake -> %d MHz (fast)", sd_active_mhz);
                        return 0;
                    }
                }
            }
            /* Fallback: let Canon's stock DEVICE_POWER_CYCLE run */
        }
    }
    return orig(a0, cmd, a2);
}

static void sd_install_ioctl_hook(void)
{
    if (sd_ioctl_hooked) return;
    uint32_t dev = SD_PSTGDEV_PTR;
    if (!dev) return;
    volatile uint32_t *slot = (volatile uint32_t *)(dev + 0x18);
    if (*slot == SD_IOCTL_ORIG)
    {
        *slot = (uint32_t)sd_ioctl_hook | 1u;
        asm volatile("dmb sy" ::: "memory");
        sd_ioctl_hooked = 1;
    }
}

uint32_t m50_sd_intercept_count(void) { return sd_intercept_count; }
int m50_sd_intercept_stepdown_mhz(void) { return sd_intercept_stepdown_mhz; }

static int quick_verify_4mb(void)
{
    uint32_t sz = SDCHK_SIZE;
    struct memSuite *suite = shoot_malloc_suite_contig(sz * 2);
    uint8_t *wbuf = 0, *rbuf = 0;
    if (suite)
    {
        wbuf = UNCACHEABLE(GetMemoryAddressOfMemoryChunk(GetFirstChunkFromSuite(suite)));
        rbuf = wbuf + sz;
    }
    else
    {
        sz = 512 * 1024;
        wbuf = fio_malloc(sz);
        rbuf = fio_malloc(sz);
    }
    if (!wbuf || !rbuf)
    {
        if (suite) shoot_free_suite(suite);
        else { if (wbuf) fio_free(wbuf); if (rbuf) fio_free(rbuf); }
        return 1; /* skip verify if out of RAM; tuning already succeeded */
    }

    uint32_t *wp = (uint32_t *)wbuf;
    for (uint32_t i = 0; i < sz / 4; i++) wp[i] = 0x5A3C0000u ^ (i * 0x9E3779B1u);

    int ok = 1;
    FILE *f = FIO_CreateFile(SDCHK_TMP_FILE);
    if (!f || FIO_WriteFile(f, wbuf, sz) != (int)sz) ok = 0;
    if (f) FIO_CloseFile(f);

    if (ok)
    {
        FILE *rf = FIO_OpenFile(SDCHK_TMP_FILE, O_RDONLY | O_SYNC);
        if (!rf || FIO_ReadFile(rf, rbuf, sz) != (int)sz) ok = 0;
        if (rf) FIO_CloseFile(rf);
        if (ok)
        {
            uint32_t *rp = (uint32_t *)rbuf;
            for (uint32_t i = 0; i < sz / 4; i += 8)
            {
                if (wp[i] != rp[i]) { ok = 0; break; }
            }
        }
    }
    FIO_RemoveFile(SDCHK_TMP_FILE);

    if (suite) shoot_free_suite(suite);
    else { fio_free(wbuf); fio_free(rbuf); }
    return ok;
}

static void sd_apply_task(void *is_boot)
{
    extern int ml_started;
    while (!ml_started) msleep(100);
    /* On boot, wait for module_load_task (.mo/.sym reads) and Canon's cold-boot SD init to finish */
    msleep(is_boot ? 2500 : 200);

    uint32_t card = SD_CARD;
    int idx = sd_oc_preset;
    if (idx < 0 || idx >= (int)COUNT(sd_oc_n)) idx = (int)COUNT(sd_oc_n) - 1;

    void (*canon_uhs_tune)(int) = (void *) SD_UHS_TUNE;
    if (read_clock() >= 10)
    {
        if (!sd_power_cycle_to_uhs())
        {
            sd_active_idx = 0;
            sd_active_mhz = 0;
            snprintf(sd_apply_rinfo, sizeof(sd_apply_rinfo), "3.3V stuck");
            sd_apply_state = 3;
            return;
        }
    }
    else
        canon_uhs_tune(1);

    if (idx == 0)
    {
        /* OFF: restore stock 780 MHz PLL and Canon's default SDR104 clock */
        pll_set_and_tune(card, 65);
        canon_uhs_tune(1);
        sd_active_mhz = 156;
        snprintf(sd_apply_rinfo, sizeof(sd_apply_rinfo), "stock");
        sd_apply_state = 2;
        return;
    }

    /* Try requested preset, stepping down through lower presets if tuning or 4 MiB verify fails */
    int tried_idx = idx;
    int ok = 0;
    while (tried_idx >= 1)
    {
        int n = sd_oc_n[tried_idx];
        if (pll_set_and_tune(card, n) == 0 && (n <= 65 || quick_verify_4mb()))
        {
            ok = 1;
            break;
        }
        tried_idx--;
    }

    if (!ok)
    {
        pll_set_and_tune(card, 65);
        canon_uhs_tune(1);
        sd_active_idx = 0;
        sd_active_mhz = (read_clock() == CLK_195) ? current_pll_mhz() : 0;
        snprintf(sd_apply_rinfo, sizeof(sd_apply_rinfo), "reverted");
        sd_apply_state = 3;
    }
    else
    {
        sd_active_idx = tried_idx;
        sd_active_mhz = current_pll_mhz();
        sd_brake_hits = 0;
        sd_brake_dead = 0;
        sd_install_ioctl_hook();
        if (tried_idx == idx)
            snprintf(sd_apply_rinfo, sizeof(sd_apply_rinfo), "N=%d verified", sd_oc_n[tried_idx]);
        else
            snprintf(sd_apply_rinfo, sizeof(sd_apply_rinfo), "stepdown N=%d", sd_oc_n[tried_idx]);
        sd_apply_state = 2;
    }
}

/* Fast 10 ns check called by crx_writer() after each FIO_WriteFile.
 * Returns 1 if Canon's SD brake dropped the clock (0x19130 != 9), 0 if healthy. */
int m50_sd_brake_detected(void)
{
    if (sd_active_idx <= 1 || sd_brake_dead) return 0;
    return (*(volatile uint32_t *)0x19130) != CLK_195;
}

/* Called by crx_writer() ONLY AFTER cleanly closing the current .CRX chunk (zero open files).
 * If Canon dropped to 3.3V (clk >= 10), power-cycles ONLY the SD slot back to 1.8V UHS-I.
 * Hits 1..3: re-tune at current speed. Hit 4: step down 1 notch + NotifyBox.
 * Returns: >0 = recovered MHz, < -1 = stepped down to -ret MHz, -1 = unrecoverable. */
int m50_sd_brake_recover(void)
{
    if (sd_active_idx <= 1 || sd_brake_dead) return 0;
    uint32_t clk = *(volatile uint32_t *)0x19130;
    if (clk == CLK_195) return 0;

    uint32_t card = SD_CARD;
    int now_ms = get_ms_clock();
    /* Drop hits older than SD_BRAKE_WINDOW_MS (60 s) so rare glitches minutes apart don't step down */
    int kept = 0;
    for (int i = 0; i < sd_brake_hits && i < 4; i++)
    {
        if ((uint32_t)(now_ms - sd_brake_ts[i]) <= SD_BRAKE_WINDOW_MS)
            sd_brake_ts[kept++] = sd_brake_ts[i];
    }
    if (kept < 4) sd_brake_ts[kept++] = now_ms;
    sd_brake_hits = kept;

    int stepped = 0;
    if (sd_brake_hits >= 4 && sd_active_idx > 1)
    {
        sd_active_idx--;
        sd_brake_hits = 0;
        stepped = 1;
    }

    /* If Canon power-cycled the card down to 3.3V (clock >= 10), power-cycle back to 1.8V UHS-I.
     * NEVER force clock=9 / CMD19 tuning if the card is still in 3.3V (that locks it up to 0 MB/s!). */
    if (clk >= 10)
    {
        if (!sd_power_cycle_to_uhs())
        {
            sd_brake_dead = 1;
            NotifyBox(5000, "SD 1.8V fail: sp=%d ck=%d (was m=%d u=%d)",
                      sd_last_pc_speed, sd_last_pc_clock, (int)sd_last_pc_m0, (int)sd_last_pc_u0);
            return -1;
        }
    }

    int target_n = sd_oc_n[sd_active_idx];
    if (pll_set_and_tune(card, target_n) == 0)
    {
        sd_active_mhz = current_pll_mhz();
        if (stepped)
        {
            snprintf(sd_apply_rinfo, sizeof(sd_apply_rinfo), "brake->%dM", sd_active_mhz);
            NotifyBox(4000, "SD brake x4 -> clocked down to %d MHz", sd_active_mhz);
            return -sd_active_mhz;
        }
        NotifyBox(2500, "SD brake #%d/3 -> retuned %d MHz", sd_brake_hits, sd_active_mhz);
        return sd_active_mhz;
    }

    /* Step down until CMD19 tuning succeeds */
    while (sd_active_idx > 1)
    {
        sd_active_idx--;
        if (pll_set_and_tune(card, sd_oc_n[sd_active_idx]) == 0)
        {
            sd_active_mhz = current_pll_mhz();
            sd_brake_hits = 0;
            snprintf(sd_apply_rinfo, sizeof(sd_apply_rinfo), "brake->%dM", sd_active_mhz);
            NotifyBox(4000, "SD brake -> clocked down to %d MHz", sd_active_mhz);
            return -sd_active_mhz;
        }
    }

    sd_brake_dead = 1;
    NotifyBox(4000, "SD brake -> 48 MHz (reboot to restore)");
    return -1;
}

void m50_sd_brake_reset_session(void)
{
    sd_brake_hits = 0;
    sd_brake_dead = 0;
    sd_intercept_count = 0;
    sd_intercept_stepdown_mhz = 0;
    if (sd_active_idx > 1) sd_install_ioctl_hook();
}

static MENU_SELECT_FUNC(sd_apply_select)
{
    if (sd_apply_state == 1) return;
    if (!NOT_RECORDING) { NotifyBox(2000, "Stop recording first"); return; }
    sd_apply_state = 1;
    sd_apply_rinfo[0] = 0;
    task_create("m50_sdclk", 0x1c, 0x1000, sd_apply_task, 0);
}

static MENU_UPDATE_FUNC(sd_apply_update)
{
    switch (sd_apply_state)
    {
        case 0:  MENU_SET_VALUE("press SET"); break;
        case 1:  MENU_SET_VALUE("tuning..."); break;
        case 2:  MENU_SET_VALUE("%d MHz OK", sd_active_mhz); break;
        case 3:  MENU_SET_VALUE("FAIL -> %d MHz", sd_active_mhz ? sd_active_mhz : 156); break;
        default: MENU_SET_VALUE("OFF"); break;
    }
    if (sd_apply_rinfo[0]) MENU_SET_RINFO("%s", sd_apply_rinfo);
    MENU_SET_HELP("Applies selected SD clock via CARDPLL + CMD19 tune + 4 MB verify. Auto-reverts to 195 MHz on error. Re-apply after boot.");
}

/* read-only: card's CMD6 status (cached by Canon's driver; pointer 0xE042E9D0(port) = *(0xB77B8 + 4*port), status at +0x448). */
static MENU_UPDATE_FUNC(sdinfo_update)
{
    uint32_t card = SD_CARD;
    uint8_t *ptr = card < 3 ? *(uint8_t **)(0xB77B8 + 4 * card) : 0;
    if (!ptr) { MENU_SET_VALUE("no card info"); return; }
    uint8_t *st = ptr + 0x448;
    int mask = (st[12] << 8) | st[13], fn = st[16] & 0xF;
    static const char *names[] = { "SDR12", "SDR25", "SDR50", "SDR104", "DDR50" };
    int clk = read_clock();
    if (clk == CLK_195)
        MENU_SET_VALUE("%s %d MHz", fn < 5 ? names[fn] : "?", current_pll_mhz());
    else
        MENU_SET_VALUE("%s, clk %d", fn < 5 ? names[fn] : "?", clk);
    MENU_SET_RINFO("mask %04x", mask);
    MENU_SET_HELP("Current SD bus mode and clock. Use 'SD clock' + 'Apply SD clock' below to overclock CARDPLL up to 336 MHz.");
}

static MENU_SELECT_FUNC(sd_preset_select)
{
    menu_numeric_toggle(priv, delta, 0, COUNT(sd_oc_n) - 1);
    if (sd_apply_state != 1 && NOT_RECORDING)
    {
        sd_apply_state = 1;
        sd_apply_rinfo[0] = 0;
        task_create("m50_sdclk", 0x1c, 0x1000, sd_apply_task, 0);
    }
}

static struct menu_entry sd195_menu[] = {
    {
        .name = "SD card mode",
        .update = sdinfo_update,
    },
    {
        .name = "SD clock",
        .priv = &sd_oc_preset,
        .select = sd_preset_select,
        .max = COUNT(sd_oc_n) - 1,
        .choices = CHOICES("OFF (156 MHz)", "195 MHz (safe)", "240 MHz (OC)", "276 MHz (OC)", "288 MHz (OC)", "300 MHz (OC)", "312 MHz (OC)", "324 MHz (OC)", "336 MHz (max OC)"),
        .help = "Target SD bus clock via DIGIC 8 CARDPLL (0xD010F218). Auto-applies on boot & auto-steps down if card fails verify.",
    },
    {
        .name = "Apply SD clock",
        .select = sd_apply_select,
        .update = sd_apply_update,
    },
};

static void sd195_init(void *unused)
{
    menu_add("Prefs", sd195_menu, COUNT(sd195_menu));
    if (sd_oc_preset > 0)
    {
        sd_apply_state = 1;
        task_create("m50_sdclk", 0x1c, 0x1000, sd_apply_task, (void *)1);
    }
}

INIT_FUNC("m50_sd195", sd195_init);

#endif
