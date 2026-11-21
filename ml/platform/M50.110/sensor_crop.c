/** \file
 * EOS M50 1.1.0: CMOS sensor vertical row window helper for CRX crops.
 *
 * Adjusts sensor register 0x68 (first/last row in 2-row units) in the
 * active LiveView sensor table so cropped CRX modes read only the active
 * rows, reducing frame readout time and bandwidth.
 *
 * GPLv2, same as Magic Lantern.
 */

#include <dryos.h>
#include <config.h>

#if CONFIG_FW_VERSION == 110

#define TBL_PTRS        0xEEDC
#define CMOS_BASE       (*(volatile uint32_t *)(TBL_PTRS + 0x18))
#define ENGIO_BASE      (*(volatile uint32_t *)(TBL_PTRS + 0x1C))
#define CMOS_SIZE       0x3EF0
#define SENSOR_MODE     (*(volatile uint32_t *)(0xEF24 + 0x08))
#define MODE_TABLE      0xE0B3AB64
#define DEP_LVSR(m)     (*(volatile uint32_t *)(MODE_TABLE + 0xA8 + 4 * (m)))
#define S34_OFF         0x2F14
#define S34_STRIDE      0x6C

static int st_tables = 0;       /* 0 unchecked, 1 RAM, 2 copied to RAM, -1 error */
static uint32_t st_cmos_base = 0;

static volatile uint32_t *saved_reg = 0;
static uint32_t saved_val = 0;
static int srows = 0, srows_mode = -1;

static void ensure_ram_tables(void)
{
    st_cmos_base = CMOS_BASE;
    if (!st_cmos_base || !ENGIO_BASE) { st_tables = -1; return; }
    if (st_cmos_base >= 0xE0000000)
    {
        uint32_t *copy = malloc(CMOS_SIZE);
        if (!copy) { st_tables = -1; return; }
        memcpy(copy, (void *) st_cmos_base, CMOS_SIZE);
        CMOS_BASE = (uint32_t) copy;
        st_cmos_base = (uint32_t) copy;
        st_tables = 2;
    }
    else
    {
        st_tables = 1;
    }
}

static volatile uint32_t *find_reg(uint32_t list, int reg)
{
    volatile uint32_t *w = (volatile uint32_t *) list;
    for (int i = 0; i < 200 && w[i] != 0xFFFFFFFF; i++)
        if ((int)(w[i] >> 22) == reg) return &w[i];
    return 0;
}

static void restore_crop_reg(void)
{
    if (saved_reg)
    {
        *saved_reg = saved_val;
        saved_reg = 0;
    }
    srows = 0;
    srows_mode = -1;
}

int m50_sensor_crop_rows(void)
{
    return (srows && srows_mode == (int) SENSOR_MODE) ? srows : 0;
}

const char *m50_sensor_crop_set(int rows)
{
    restore_crop_reg();
    if (!rows) return 0;
    if (!st_tables) ensure_ram_tables();
    if (st_tables <= 0) return "sensor tables not usable";

    uint32_t m = SENSOR_MODE;
    if (m >= 14 || DEP_LVSR(m) >= 11) return "not a LiveView sensor mode";

    volatile uint32_t *r68 = find_reg(st_cmos_base + S34_OFF + DEP_LVSR(m) * S34_STRIDE, 0x68);
    if (!r68) return "row window register not found";

    uint32_t v = *r68 & 0x3FFFFF;
    int first = ((v >> 11) & 0x7FF) * 2;
    int last  = (v & 0x7FF) * 2;
    if (rows + 8 >= last - first) return "crop taller than the sensor window";

    int nf = ((first + last) / 2 - rows / 2) & ~1;
    saved_reg = r68;
    saved_val = *r68;
    *r68 = (saved_val & 0xFFC00000) | (((nf / 2) << 11) | ((nf + rows) / 2));
    srows = rows;
    srows_mode = m;
    return 0;
}

#endif
