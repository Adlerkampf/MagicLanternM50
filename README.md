# Magic Lantern for Canon EOS M50 (1.1.0) — CRX RAW Video

Hardware-accelerated CRX (CR3 / C-RAW) RAW video recording on the **Canon EOS M50 (firmware 1.1.0, DIGIC 8)**, together with `crx2dng` for converting `.CRX` recordings into CinemaDNG sequences for DaVinci Resolve and other NLEs.

## Features

- **Hardware CRX Encoder (`crx_rec.c`)**
  - Uses Canon's built-in DIGIC 8 CR3/C-RAW hardware encoder (`ALGS_REQUEST`) for real-time compressed RAW video.
  - **12-bit** (default) & **14-bit** capture, **C-RAW (lossy)** and **Lossless** modes, plus configurable C-RAW quantization strength.
  - **PL330 DMA multi-piece frame copy** and **3-buffer capture pipeline** for smooth continuous recording.
  - **Aspect ratio** offers 16:9, 3:2 (M50 sensor shape), 4:3, 1:1, Scope (~2.39:1), and Full stream.
  - **Resolution** shows the output dimensions for the selected shape. Widths include 720, 1024, HD (1280), Full HD (1920), and 2.5K, 2.8K, 3K, 3.2K, 3.4K, 3.5K, and 4K UHD (3840). Taller shapes offer the widths that fit within 2160 rows. The encoder uses multiples of eight rows.
  - **C-RAW strength** offers Stock through +6. Cards with the former Max diagnostic setting start at Stock.
  - **Full stream** keeps the original native capture option. **16:9 / 4K UHD** copies a 3840x2160 rectangle into the encoder input and stores exactly those dimensions in the CRX file.
  - **SD speed test** writes a temporary 64 MiB file in 1 MiB blocks, shows the average sequential write speed in MB/s, and removes the file on completion.
  - **NTSC (23.98p) / PAL (25p) Video System toggle** directly from the CRX menu.
  - **Movie shutter button** start/stop recording and live recording HUD (buffer fill, write/need MB/s, SD & camera drop counters).
- **`crx2dng` Desktop Converter (`crx2dng/`)**
  - Converts `.CRX` recordings into CinemaDNG frame sequences (`crx2dng` CLI & GUI).

## How to Record

1. **Prepare the SD card:** Make sure your Canon EOS M50 (firmware **1.1.0**) has the bootflag enabled and a bootable UHS-I V30/V60 SD card (`ml/platform/M50.110/tools/make_bootable.py`). Extract `magiclantern.zip` to the root of the SD card.
2. **Switch to Movie mode (4K):** Turn the mode dial to **Movie** and set Canon's movie recording size to **4K** (for crop modes) or **1080p**.
3. **Configure CRX (optional):** Press the **Trash** button to open the Magic Lantern menu, then go to **Debug → CRX recording**.
   - Default settings are ready to shoot: **3K 3072×1728 (16:9)**, **12-bit**, **C-RAW (lossy)**, **Movie shutter = Start/stop CRX**.
   - Use **Video system** to switch between **NTSC (23.98p)** and **PAL (25p)**, then open **Aspect ratio** or **Resolution** with **SET**, highlight a choice, and press **SET** to close the list. Close the menu to see the centered frame in Live View. The recorder crops the incoming RAW frame at the selected size.
4. **Record:** Close the menu and wait for **`CRX READY`** on the Live View HUD. Press the **shutter button** to start recording, and press it again to stop.
5. **Convert to CinemaDNG:** Copy the `.CRX` files from `ML/CRX/` on your SD card to your PC and drag-and-drop them onto `crx2dng/convert_to_resolve.bat` (or open them in `crx2dng_gui.exe`) to generate DaVinci Resolve–ready CinemaDNG folders.

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

Run `crx2dng\build.bat` on Windows (requires Visual Studio / C++ Build Tools; auto-detected via `vswhere.exe`).

## License

Magic Lantern and the M50 1.1.0 platform code are licensed under **GPLv2** (see `LICENSE` / `ml/COPYING`).
