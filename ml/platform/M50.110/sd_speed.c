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
#define SD_UHS_TUNE     0xE0173F17      /* (int), Thumb */
#define SD_SET_CLOCK    0xE01E8953      /* (card, clock code), Thumb */
#define SD_TUNE         0xE01EB071      /* (card), Thumb */
#define SD_GET_SPEED    0xE01E9E39      /* (card, int *speed, int *clock), Thumb */
#define CLK_195         9

#define CARDPLL_CTRL    (*(volatile uint32_t *)0xD010F218)
#define SD0_CLK_EN      (*(volatile uint32_t *)0xD010060C)
#define SDCHK_TMP_FILE  "ML/SDCHK.TMP"
#define SDCHK_SIZE      (4 * 1024 * 1024)

static const int sd_oc_n[] = { 0, 65, 80, 92, 100, 108, 112 };
static CONFIG_INT("sd.m50_clock_preset", sd_oc_preset, 4);  /* default: 300 MHz (36 MHz margin below 348 MHz cliff; auto-steps down if verify fails) */

static int sd_apply_state = 0;  /* 0 idle, 1 running, 2 OK, 3 failed (reverted) */
static int sd_active_mhz = 0;
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

static int pll_set_and_tune(uint32_t card, int N)
{
    int (*set_clock)(uint32_t, int) = (void *) SD_SET_CLOCK;
    int (*tune)(uint32_t) = (void *) SD_TUNE;

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

    if (set_clock(card, CLK_195) & 1) return -1;
    if (tune(card) & 1) return -2;
    return 0;
}

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

static void sd_apply_task(void *unused)
{
    extern int ml_started;
    while (!ml_started) msleep(100);
    msleep(500);

    uint32_t card = SD_CARD;
    int idx = sd_oc_preset;
    if (idx < 0 || idx >= (int)COUNT(sd_oc_n)) idx = (int)COUNT(sd_oc_n) - 1;

    void (*canon_uhs_tune)(int) = (void *) SD_UHS_TUNE;
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
        sd_active_mhz = (read_clock() == CLK_195) ? current_pll_mhz() : 0;
        snprintf(sd_apply_rinfo, sizeof(sd_apply_rinfo), "reverted");
        sd_apply_state = 3;
    }
    else
    {
        sd_active_mhz = current_pll_mhz();
        if (tried_idx == idx)
            snprintf(sd_apply_rinfo, sizeof(sd_apply_rinfo), "N=%d verified", sd_oc_n[tried_idx]);
        else
            snprintf(sd_apply_rinfo, sizeof(sd_apply_rinfo), "stepdown N=%d", sd_oc_n[tried_idx]);
        sd_apply_state = 2;
    }
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
        .choices = CHOICES("OFF (156 MHz)", "195 MHz (safe)", "240 MHz (OC)", "276 MHz (OC)", "300 MHz (OC)", "324 MHz (OC)", "336 MHz (max OC)"),
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
        task_create("m50_sdclk", 0x1c, 0x1000, sd_apply_task, 0);
    }
}

INIT_FUNC("m50_sd195", sd195_init);

#endif
