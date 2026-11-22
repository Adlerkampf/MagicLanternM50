# EOS M50 1.1.0 (DIGIC 8) — SD UHS-I Write Speed: ~30 MB/s → 141 MB/s

Code: [`sd_speed.c`](ml/platform/M50.110/sd_speed.c), [`crx_rec.c`](ml/platform/M50.110/crx_rec.c) (`main` @ `15049cd`)  
Test media: Lexar Professional SILVER PLUS 256 GB (exFAT, UHS-I V30)

---

## Step-by-Step Breakdown (Active Changes Only)

### 1. Clean Sequential AU-Aligned Writer (`~30–45 MB/s` → `~65–70 MB/s`)
- **Problem:** Early CRX builds wrote smaller unaligned chunks, stalled on ring-buffer wraps, and flushed `CRXREC.LOG` mid-recording. Writing to a second file mid-recording forced exFAT metadata updates and random-access card writes, cutting throughput in half.
- **Change (`885bfc9`, `b8e5435`):**
  - Removed all mid-recording `CRXREC.LOG` writes so only the active `.CRX` file is written during capture.
  - Stopped waiting for the 4 MiB fill threshold on ring-wrap tails (`r_wrap - r` cannot grow).
  - Aligned every write boundary to **4 MiB Allocation Units (AU)** and merged wrap realignment into one write.
- **Result:** Reached the full ceiling of Canon's stock `156 MHz` bus (`~65–70 MB/s`).

### 2. Enable Canon's Built-In `195 MHz` UHS-I Mode (`~65–70 MB/s` → `~80–85 MB/s`)
- **Problem:** Canon runs the M50 SD slot at `156 MHz` by default even on SDR104 cards.
- **Change (`f9c8bee`):** Called Canon's `SD_SET_CLOCK(card, 9)` (`0xE01E8953`) followed by `SD_TUNE(card)` (`0xE01EB071`, SD `CMD19` hardware phase tuning). This switches `CARDPLL` to `N=65` (`195 MHz`), still inside the 208 MHz UHS-I SDR104 spec.
- **Result:** Clean ~20% gain to `~80–85 MB/s`.

### 3. Direct DIGIC 8 `CARDPLL` Overclocking + 4 MiB Verify (`~80–85 MB/s` → `~115–125 MB/s`)
- **Problem:** `SD_SET_CLOCK` has no clock preset above `195 MHz`.
- **Change (`63cf0f0`):**
  - Programmed `CARDPLL_CTRL` (`0xD010F218`) directly: gate `SD0_CLK_EN` (`0xD010060C = 0`), disable PLL, write `((N << 4) | 1)` where `SD_CLK = N × 3 MHz` (via the `÷4` divider at `0xD0100604=0`, `0xD0100618=1`), wait for lock, enable output + `SD0_CLK_EN`, then run `SD_TUNE(card)` (`CMD19`).
  - Added an automatic **4 MiB write/readback verify** (`ML/SDCHK.TMP`) on boot/apply; if tuning or verify fails, it steps down one notch automatically (`336 → 324 → 312 → 300 → 288 → 276 → 240 → 195 MHz`).
- **Result:** First break past the 104 MB/s UHS-I spec (`~115–125 MB/s`), but any single CRC error triggered Canon's SD error brake (dropping the slot to 3.3V / 48 MHz = `~22 MB/s` until reboot).

### 4. RAM Hook on `SDDRV ioctl(0x1004)` + 5 ms In-Place Recovery & SD0 Pin Drive (`~128 MiB/s`)
- **Problem:** On a CRC glitch at 300+ MHz, Canon's `SDDRV` calls `ioctl(0x1004)` (`DEVICE_POWER_CYCLE`), clears `UHSHost` (`0xB144 = 0`), and permanently downgrades the card to 3.3V / 48 MHz.
- **Change (`786b06b`, `0989fbb`, `15049cd`):**
  - Hooked `pStgDev->ioctl` in RAM (`*(0x115B8)`, originally `0xE017EEE1`) to intercept `ioctl(0x1004)` **before** Canon drops 1.8V power.
  - Inside the hook, we first run an **in-place ~5 ms `pll_set_and_tune()`** (keeping 1.8V UHS-I alive with zero file splits). Only if in-place tuning fails do we fall back to a single 1.8V power-cycle (preserving `0xB144 = 1`).
  - Tracked retries in a 30 s sliding window; once hits exceed the configured threshold (`Prefs → SD stepdown hits`: `3/6/9/12/15/18`), the hook steps `CARDPLL` down 1 notch cleanly (or holds indefinitely in `Extreme` mode).
  - Exposed DIGIC 8 SD0 GPIO pin drive (`0xD0130128..3C`, bits `[5:4]`), including `High (data only)` to boost `DAT0..3 + CMD` while keeping `SD_CLK` clean at Middle.
- **Result:** Reduced brake hiccups from ~1.8 s + file split to ~50 ms + zero splits (`127.8 MiB/s` sustained over `5.36 GB`).

### 5. Disable `CARDPLL` Spread-Spectrum Clocking (`SSCG = 0`) (`~130.6 MB/s` avg / `138.2 MB/s` peak)
- **Problem:** Canon leaves `CARDPLL_SSCG` (`0xD010F224`) enabled (`0x00F61010`, bit 12 = `0x1000`), deliberately dithering `SD_CLK` for EMI compliance. At 300+ MHz, clock dithering eats directly into the sampling window and causes extra CRC retries.
- **Change (`ce63d75`):** Clear `CARDPLL_SSCG = 0` while `CARDPLL` is powered down before writing the new `N`.
- **Result:** Cleaner signal eye and higher sustained speed (`130.6 MB/s` busy avg over `9.86 GB` / `74.3 s`, `138.2 MB/s` decimal clean window).

### 6. `28 MiB` Single-`CMD25` Burst Sizing
- **Problem:** `SDDRV` (`0xE017F034`) caps each single `CMD25` multi-block write at `0xF800` sectors (`31.0 MiB`). Writing `32 MiB` silently split every burst into `31 MiB + 1 MiB`, adding extra command overhead and unaligned tails.
- **Change (`ce63d75`):** Set `WR_MAX = 28 MiB` (`7 × 4 MiB` AU) in Auto mode so every `FIO_WriteFile` maps to one unbroken `CMD25` transaction.

### 7. `FIO_DelayFlush` + Canon MovWriter `CreateSC` / `CloseSC` (`134.3 MB/s` avg @ 312 MHz, `141.4 MB/s` peak @ 324 MHz)
- **Problem:** Standard `FIO_CreateFile` bypasses Canon's exFAT Speed-Class allocator and flushes exFAT metadata more aggressively than Canon's own movie recorder.
- **Change (`b6e7469`, `15049cd`):**
  - Call `FIO_DelayFlush("B:", 1)` (`0xE054811D`) before recording starts and `FIO_DelayFlush("B:", 0)` after closing the file.
  - Open CRX chunks via Canon MovWriter's **`CreateSC` (`0xE0546865`)** and close via **`CloseSC` (`0xE0546E17`)** (falling back to `FIO_CreateFile` / `FIO_CloseFile` if `CreateSC` returns `<= 0`). This activates Canon's native exFAT AU speed-class allocator (`DryEFatEnterSpeedClassXC`) and manages SD `CMD20` automatically across the file lifecycle.
- **Result:** Consistent **~+3 MB/s** at the exact same SD clock:
  - **312 MHz:** **`134.3 MB/s`** log busy avg over `4.15 GB` (`30.8 s`, 1 brake); **`133.1 MiB/s` (`139.6 MB/s` decimal)** clean window.
  - **324 MHz:** **`134.8 MiB/s` (`141.4 MB/s` decimal)** over `11.3 s` (`1.60 GB`, 0 brakes).

---

## What Applies Directly to Other DIGIC 8 Cameras (`850D`, `M6II`, `R`, `RP`, `SX70`, `SX740`)

Looking at upstream ML (`ml/modules/sd_clock/` and `ml/modules/raw_video/mlv_lite/mlv_lite.c`), **none of these are implemented for DIGIC 8 yet**:
- **`sd_clock` module:** Currently supports only DIGIC 4 (`sd_pll.c`), DIGIC 5 (`sd_uhs.c`), and 200D DIGIC 7 `192 MHz` autotune (`sd_dryos_autotune.c`). Steps **2–5** (`CARDPLL` `0xD010F218`, `SSCG=0` `0xD010F224`, SD0 GPIO drive `0xD0130128..3C`, and the RAM `pStgDev->ioctl(0x1004)` 5 ms recovery hook) can be ported directly into `sd_clock` for all DIGIC 8 bodies — with **zero ROM/MMU patching**.
- **`mlv_lite` (`mlv_lite.c`):** Currently opens/closes chunks with plain `FIO_CreateFile` / `FIO_CloseFile` and does not call `FIO_DelayFlush`. Steps **6–7** (`CreateSC`/`CloseSC` + `FIO_DelayFlush` + keeping max write burst `<= 28 MiB` under `SDDRV`'s `31 MiB` `CMD25` split cap) are a drop-in speedup for `mlv_lite` on any DIGIC 8 camera, even at stock clocks.

---

## Key Addresses (M50 1.1.0)

- **Clocks:** `CARDPLL_CTRL` `0xD010F218` (`N << 4`), `CARDPLL_SSCG` `0xD010F224` (clear to `0` only while PLL off; stock `0x00F61010`), `SD0_CLK_EN` `0xD010060C`.
- **GPIO:** `0xD0130128..3C` (6 pins: `DAT0..3, CMD, CLK`), bits `[5:4]` = drive strength (do not modify bits `[10,8]`, 1.8V/3.3V cell).
- **SDDRV RAM:** `SD_CARD` `*(0xB140)`, `UHSHost` `*(0xB144)`, cached clock `*(0x19130)` (`9` = 1.8V UHS-I, `>= 10` = 3.3V), `pStgDev->ioctl` `*(0x115B8)` (`0xE017EEE1`).
- **ROM funcs (Thumb):** `SD_SET_CLOCK` `0xE01E8953`, `SD_TUNE` `0xE01EB071`, `SD_GET_SPEED` `0xE01E9E39`, `FIO_DelayFlush` `0xE054811D`, `CreateSC` `0xE0546865`, `CloseSC` `0xE0546E17`.
