/** \file
 * EOS M50 1.1.0: let the SD card run at 195 MHz (SDR104), without patching ROM.
 *
 * Canon's SDR104 tuning (0xE0172EEE) copies the clock table at ROM 0xE0AC1F04
 * = { 9 (195 MHz), 8 (156), 7 (130), 6 (111), 0xE end } and pre-increments its
 * index, so after the first failed tune it never tries 195 MHz.
 *
 * Old version: patched that table in ROM (one 64 kB MMU page = 64 kB of boot RAM).
 * This version calls the same building blocks Canon's tuning loop uses:
 *   0xE0173F16(1)            switch to SDR104 + Canon's own tuning (ends at <= 156 MHz)
 *   0xE01E8952(card, clock)  SdChangeClockSpeed, returns error in bit 0
 *   0xE01EB070(card)         run tuning at the current clock, error in bit 0
 * card = *(0xB130 + 0x10). We try 195 MHz; if tuning fails we run Canon's
 * sequence again so the card ends at a clock that tuned. Static analysis 2026-11-18.
 *
 * Not applied at startup: Prefs -> "SD 195 MHz" after every boot.
 *
 * GPLv2, same as Magic Lantern.
 */

#include <dryos.h>
#include <config.h>
#include <menu.h>
#include <propvalues.h>

#if CONFIG_FW_VERSION == 110 && defined(CONFIG_M50_SD195)

#define SD_CARD         (*(volatile uint32_t *)(0xB130 + 0x10))   /* slot index, 0 = SD */
#define SD_UHS_TUNE     0xE0173F17      /* (int), Thumb */
#define SD_SET_CLOCK    0xE01E8953      /* (card, clock code), Thumb */
#define SD_TUNE         0xE01EB071      /* (card), Thumb */
#define SD_GET_SPEED    0xE01E9E39      /* (card, int *speed, int *clock), Thumb */
#define CLK_195         9

static int sd195_state = 0;     /* 0 off, 1 running, 2 195 MHz OK, 3 195 failed (Canon clock), -1 no card */
static int sd195_clock_before = -1, sd195_clock_after = -1;

static int read_clock(void)
{
    int speed = -1, clock = -1;
    int (*get_speed)(uint32_t, int *, int *) = (void *) SD_GET_SPEED;
    get_speed(SD_CARD, &speed, &clock);
    return clock;
}

static void sd195_task(void *unused)
{
    extern int ml_started;
    while (!ml_started) msleep(100);    /* never during Canon's own SD init */
    msleep(500);                        /* let pending card writes finish */

    /* card is a slot index (Canon indexes 0xB1D8[card]); 0 is the normal SD slot */
    uint32_t card = SD_CARD;

    void (*canon_uhs_tune)(int) = (void *) SD_UHS_TUNE;
    int (*set_clock)(uint32_t, int) = (void *) SD_SET_CLOCK;
    int (*tune)(uint32_t) = (void *) SD_TUNE;

    sd195_clock_before = read_clock();

    /* make sure the card is in SDR104 with a working clock first */
    canon_uhs_tune(1);

    int ok = !(set_clock(card, CLK_195) & 1) && !(tune(card) & 1);
    if (!ok)
    {
        canon_uhs_tune(1);              /* back to a clock Canon's tuning accepts */
        sd195_state = 3;
    }
    else sd195_state = 2;

    sd195_clock_after = read_clock();
}

static MENU_SELECT_FUNC(sd195_select)
{
    if (sd195_state == 1) return;
    if (!NOT_RECORDING) { NotifyBox(2000, "Stop recording first"); return; }
    sd195_state = 1;
    task_create("m50_sd195", 0x1c, 0x800, sd195_task, 0);
}

static MENU_UPDATE_FUNC(sd195_update)
{
    switch (sd195_state)
    {
        case 0:  MENU_SET_VALUE("OFF"); break;
        case 1:  MENU_SET_VALUE("tuning..."); break;
        case 2:  MENU_SET_VALUE("195 MHz OK"); break;
        case 3:  MENU_SET_VALUE("195 failed"); break;
        default: MENU_SET_VALUE("no card"); break;
    }
    if (sd195_state >= 2) MENU_SET_RINFO("clk %d -> %d", sd195_clock_before, sd195_clock_after);
    MENU_SET_HELP("Tries 195 MHz with Canon's own clock change + tuning (no ROM patch). Re-run after each boot. Check with SD speed test.");
}

/* read-only: card's CMD6 status (cached by Canon's driver; pointer 0xE042E9D0(port) = *(0xB77B8 + 4*port), status at +0x448).
 * bytes 12-13 = group 1 support mask (bit 4 = DDR50, bit 3 = SDR104), byte 16 low nibble = selected function.
 * Canon's USetAcMd 0xE018B842 only handles modes 0-3 (SDR12..SDR104): there is no DDR50 path in this firmware. */
static MENU_UPDATE_FUNC(sdinfo_update)
{
    uint32_t card = SD_CARD;
    uint8_t *ptr = card < 3 ? *(uint8_t **)(0xB77B8 + 4 * card) : 0;
    if (!ptr) { MENU_SET_VALUE("no card info"); return; }
    uint8_t *st = ptr + 0x448;
    int mask = (st[12] << 8) | st[13], fn = st[16] & 0xF;
    static const char *names[] = { "SDR12", "SDR25", "SDR50", "SDR104", "DDR50" };
    MENU_SET_VALUE("%s, clk %d", fn < 5 ? names[fn] : "?", read_clock());
    MENU_SET_RINFO("mask %04x", mask);
    MENU_SET_HELP("Card %s DDR50 (bit 4), %s SDR104 (bit 3). Canon's driver has no DDR50 mode, so it can't be switched on safely.",
        (mask & 0x10) ? "supports" : "lacks", (mask & 0x08) ? "supports" : "lacks");
}

static struct menu_entry sd195_menu[] = {
    {
        .name = "SD card mode",
        .update = sdinfo_update,
    },
    {
        .name = "SD 195 MHz",
        .select = sd195_select,
        .update = sd195_update,
    },
};

static void sd195_init(void *unused)
{
    menu_add("Prefs", sd195_menu, COUNT(sd195_menu));
}

INIT_FUNC("m50_sd195", sd195_init);

#endif
