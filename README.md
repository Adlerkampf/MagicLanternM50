# Magic Lantern for Canon EOS M50 (1.1.0) — CRX RAW Video

Hardware-accelerated CRX (CR3 / C-RAW) RAW video recording on the **Canon EOS M50 (firmware 1.1.0, DIGIC 8)**, together with `crx2dng` for converting `.CRX` recordings into CinemaDNG sequences for DaVinci Resolve and other NLEs.

## Features

- **Hardware CRX Encoder (`crx_rec.c`)**
  - Uses Canon's built-in DIGIC 8 CR3/C-RAW hardware encoder (`ALGS_REQUEST`) for real-time compressed RAW video.
  - **12-bit Live View** & **14-bit** capture, **C-RAW (lossy)** and **Lossless** modes, plus configurable C-RAW quantization strength.
  - **PL330 DMA multi-piece frame copy** and **3-buffer capture pipeline** for smooth continuous recording.
  - **Crop modes** (with CMOS vertical row-window cropping in `sensor_crop.c`):
    - Full stream (4K)
    - `3.5K 3584x1504` (2.38:1) / `3.5K 3584x2016` (16:9)
    - `3K 3072x1288` (2.38:1) / `3K 3072x1728` (16:9) — default
    - `2.8K 2880x1200` (2.40:1)
    - `2.5K 2560x1440` (16:9)
  - **NTSC (23.98p) / PAL (25p) Video System toggle** directly from the CRX menu.
  - **Movie shutter button** start/stop recording and live recording HUD (buffer fill, write/need MB/s, SD & camera drop counters).
- **SD Card UHS-I 195 MHz Tuning (`sd_speed.c`)**
  - Overclocks SD UHS-I clock to 195 MHz (SDR104) with hardware phase tuning for sustained high-bitrate RAW writes.
- **Idle Sleep / Wake-up Support**
  - Tracks Canon's `GUILockTask` display state so Magic Lantern pauses Ximr refreshes, EDMAC polling, and shutter interception when the camera enters idle/display-off sleep.
- **`crx2dng` Desktop Converter (`crx2dng/`)**
  - Converts `.CRX` recordings into CinemaDNG frame sequences (`crx2dng` CLI & GUI).

## Repository Layout

- `ml/` — Magic Lantern source tree (`ml/platform/M50.110/` contains the M50 1.1.0 port & CRX recorder).
- `crx2dng/` — Windows C/C++ converter from `.CRX` to CinemaDNG.

## Building Magic Lantern (M50 1.1.0)

Requires `arm-none-eabi-gcc` (tested with Arm GNU Toolchain 13.2) and `python3` / `zip` on Linux or WSL:

```bash
cd ml/platform/M50.110
make ARM_BINPATH=/path/to/arm-gnu-toolchain/bin -j$(nproc)
```

Output archive: `ml/platform/M50.110/build/magiclantern.zip` (extract to the root of a bootable SD card).

### Memory Map Constraint (M50 1.1.0)
When modifying core code, always verify:
- `.bss.mmu_space` is located at `0x00130000`
- `_bss_end` is below `0x00160300`

```bash
arm-none-eabi-objdump -h build/magiclantern | grep -E "mmu_space|\.bss|\.text"
arm-none-eabi-nm build/magiclantern | grep _bss_end
```

## Building `crx2dng`

See `crx2dng/build.bat` (Windows). Place LibRaw / dependencies in `crx2dng/third_party/` before building.

## License

Magic Lantern and the M50 1.1.0 platform code are licensed under **GPLv2** (see `ml/COPYING` / file headers).
