# EOS M50 1.1.0: RAW video bring-up

Branch `m50-raw-video` on top of `magiclantern_simplified` `dev` (base: 142472e, 2026-09-16).

**Status: untested on hardware.** The code compiles and every module symbol resolves. The camera-specific addresses have not been found yet, because no M50 ROM was available while writing this. Until they are filled in, the build refuses to compile (`#error`). A placeholder build is available for type-checking only, and it refuses to start RAW at runtime.

As of Nov 2026 nobody has shown RAW video on an M50. On DIGIC 8, the only RAW video so far is kitor's unreleased 2024 EOS R proof of concept, which stalls after 30 to 60 frames. Expect the same class of problems here.

## What this branch adds

| File | What it does |
|---|---|
| `features.h` | Turns on `CONFIG_RAW_LIVEVIEW`, `FEATURE_PICSTYLE` (both needed by mlv_lite) and `CONFIG_M50_POLLED_VSYNC`. Has an optional `CONFIG_M50_RAW_DISCOVERY`. |
| `raw_config.h` | **Every unknown value in one place**, each with notes on how to find it. |
| `function_overrides.c` | `shamem_read()` that reads only the configured EDMAC port window. `_EngDrvOut()` whitelist (bit-depth register only). Three frame-copy backends for mlv_lite (CPU, DMA, M2M). Stand-ins for the 4 symbols mlv_lite needs that the port lacked. |
| `raw_vsync.c` | Per-frame callback without state-object hooks, which crash in EvfCap on M50. A task polls the RAW port address, falls back to the YUV display buffer flip, then to a timer, and fires `CBR_VSYNC`. Also provides `wait_lv_frames()`. |
| `raw_probe.c` | Debug menu **M50 RAW probe**: turn the stream on and off, show the live port registers and the vsync source, and dump a RAW frame to the card. |
| `include/platform/mmu_patches.h` | Optional MMU hook that logs every EDMAC port Canon programs. Its only purpose is finding the RAW port. |
| `tools/m50_raw_inspect.py` | Desktop tool. Decodes a dumped frame, writes previews and suggests `skip_left`/`skip_top`. The decoder is checked against ML's own `struct raw_pixblock`. |
| `src/raw.c` | M50 PACK32 block, M50 border skips, placeholder-build refusal, and a guard against a zero `SHAD_GAIN`. |
| `modules.included` / `modules.hidden` | `mlv_lite` is now shipped instead of hidden. |

## Build

```sh
# toolchain: arm-none-eabi-gcc (tested 13.2.1), python3 + docutils
cd platform/M50.110

make M50_RAW_PLACEHOLDERS=1   # compile check only; RAW refuses to start on camera
make                          # real build: fails with #error until raw_config.h is filled in

# pass discovered values without editing files:
make M50_RAW_DEFS="-DM50_RAW_LV_EDMAC_CHANNEL_ADDR=0xD00xxxxx"
make M50_RAW_DEFS="-DM50_RAW_LV_EDMAC_CHANNEL_ADDR=0x... -DM50_FRAME_COPY_BACKEND=2 -DM50_STUB_mem_to_mem_edmac_copy=0x...|1"
```

## Prerequisites (one-time, at your own risk)

1. The camera must be on **firmware 1.1.0**. Stubs differ between 1.0.1, 1.0.2 and 1.1.0. Do not update afterwards.
2. Set the boot flag with a1ex's `BOOT_M50.FIR` (2019, magiclantern.fm forum). This modifies the camera and voids the warranty. The only recovery anyone reports for a bad build is pulling the battery and swapping the card. No unbrick procedure for DIGIC 8 is documented.
3. Dump the ROM with the port's built-in ROM backup (`CONFIG_AUTOBACKUP_ROM`, fixed for D8 in Aug 2026). Load it into Ghidra with base `0xE0000000`, Thumb code, model ID `0x412`.
4. Join the ML Discord (`#development_101`) and post in the forum DIGIC 8 thread. kitor (D8) and names_are_hard (200D raw, mlv_lite) have solved half of these problems before.

## Milestones (do them in order; each one is a go/no-go gate)

**M0: placeholder build boots (risk: low).**
Boot `make M50_RAW_PLACEHOLDERS=1`. Expected: ML menus work as before, and *Debug > M50 RAW probe* exists. Turning *RAW stream* ON shows "placeholder build, disabled".

**M1: find the RAW EDMAC port (risk: medium).**
Goal: `M50_RAW_LV_EDMAC_CHANNEL_ADDR`.
- Static route: in Ghidra, start from the `lv_save_raw` eventproc. raw.c already calls it, together with `lv_set_mm(1)`, which kitor found switches D8 from YUV to RAW. Follow it to where a write port is programmed.
- Dynamic route: fill in `M50_HOOK_SETPORT_*` (address, the first 8 original bytes and the tail asm), then enable `CONFIG_M50_RAW_DISCOVERY`. If the bytes don't match, `patch_mmu.c` refuses to patch, so a wrong address can't overwrite code. Toggle *RAW stream* and diff the log.
- Accept the port when *EDMAC port* shows a nonzero `ram_addr` and `x` is about sensor width × 14 ÷ 8 bytes, and only while the stream is ON.

**M2: one good frame (risk: medium).**
Build with the real port address. Turn *RAW stream* ON, then use *Dump RAW frame*. Then on a computer:
`python3 tools/m50_raw_inspect.py RAW00000.BIN`.
Pass: the preview shows the scene and black is about 2048.
Then put the suggested `skip_left`/`skip_top` into the `CONFIG_M50` block in raw.c. Keep `skip_left = 0` while the copy backend is full-width only.

**M3: frames in step (risk: low).**
*Frame source* should say **RAW EDMAC**. If it says *YUV VRAM* or *timer*, frames can tear. Also check how coarse `msleep(1)` really is on this camera; DryOS ticks may be 10 ms, which would make polling too slow for clean frames. In that case the proper fix is an MMU code hook on the EVF readout-done path, which replaces `raw_vsync.c`.

**M4: first MLV (risk: high; this is where the EOS R stalled).**
Load mlv_lite. Start at **1280×720, 14-bit, 23.976 fps**, which is 38.7 MB/s and well under the M50's roughly 62 MB/s best card write, using the default **CPU copy backend**. Record 10 s and check the file with MLV App.
Run the `bench` module first, because CPU memcpy speed on this camera is unknown and may be the bottleneck. If recording stops after tens of frames as it did on the R, suspect memory and cache coherency first:
- mlv_lite buffers come from `shoot_malloc` only, because SRM doesn't work on M50.
- The CPU copy reads through `UNCACHEABLE()`. Confirm that alias is valid for the RAW buffer on D8.
- RAM above 0x80000000 is shared with the other core; writes there caused problems on the R.

**M5: speed (risk: high).**
Replace CPU copy with **M2M** (`M50_COPY_M2M`, the method that works on the 200D):
- Find `mem_to_mem_edmac_copy` in ROM.
- Pick two ports LiveView never uses (`M50_M2M_CHAN_1/2`).
- Confirm the D8 `edmac_info` layout for this call.

A wrong channel makes the copy hang until LiveView closes.

**M6: 10/12-bit (risk: medium).**
Find the D8 PACK32 register (`M50_PACK32_MODE_REG`). On 200D, 10/12-bit still glitch on every other frame, so expect the same.

**Later, not in this branch:**
- Audio: mlv_snd lacks the ASIF stubs.
- crop_rec: needs `patch_hook_function`.
- Lossless compression: not working on any DIGIC 6/7/8.
- The "card full" dialog handler.
- SD overclock: the 200D `sd_clock` module (Sept 2026) is the template.

## Expected ceiling

These are 23.976 fps data rates. For reference, the M50's best measured Canon writes are about 62 MB/s on UHS-I.

| Resolution | 14-bit | 12-bit | 10-bit |
|---|---|---|---|
| 1920×1080 | 87.0 | 74.6 | 62.1 |
| 1920×800 | 64.4 | 55.2 | 46.0 |
| 1600×900 | 60.4 | 51.8 | 43.2 |
| 1920×540 | 43.5 | 37.3 | 31.1 |
| 1280×720 | 38.7 | 33.1 | 27.6 |

Full-HD 14-bit will not be continuous on this card bus without lossless compression or an SD overclock. Realistic targets:
- **14-bit, which needs no PACK32:** about 1280×720 or 1920×540, continuous.
- **10-bit:** about 1920×800, if M6 works.

## ROM findings, firmware 1.1.0 (2026-11-17)

Dump: ROM0 sha256 `9fa021904be274be86e383e374b0770704f594afeb0faeed99f8c6bc3877b378`,
ROM1 `031389978b2096c9428fb2409c33f0ebeffd301db68c9d92504e7ea97d320213`,
signature from 0xE0040000 = 0x05EDBC80 (matches SIG_M50_110). All 93 THUMB_FN
stubs of the port that point into ROM decode as code.

| Item | Value | Evidence | Confidence |
|---|---|---|---|
| EDMAC port table | 0xE0DD7BA0, 76 x {MMIO base, ModeInfo}; 0..38 write, 39..75 read | Edmac.c init fn 0xE0549E8C asserts `Port < ELD_EDMAC_NUM` (0x4C) | high |
| EDMAC set-address | 0xE054A9AE: `table[port].base + 0xA0 = addr` | disassembly; matches `ram_addr` offset of D8 `struct edmac_mmio` | high |
| EDMAC set-size/info | 0xE054A9BA (port, info*) | same callers | high |
| EDMAC start / stop | 0xE054AEB0 (port, mode) / 0xE054A46E (port) | same callers | medium |
| EDMAC write pack mode | 0xE054B99A (port, cfg*) | asserts on `PackUnpackInfo` | high |
| `lv_save_raw` handler | 0xE00DB9BC, sets 0x12458+4 and Mem1Path 0x12748+0x40 | eventproc registration | high |
| `lv_set_mm` handler | 0xE080CE5E, sets 0x12748+0x44 and pack cfg 0x12748+0x70 | eventproc registration | high |
| **LiveView RAW port** | **port 3 = 0xD0420200** (ram_addr at 0xD04202A0) | Mem1Path.c fn 0xE080D36C: if lv_save_raw, programs port 3 with xb = width*7/4 (14-bit), yb = height-1, addr = frame buffer | **high (static); camera test pending** |
| Dual-pixel AF raw | ports 5/6 (0xD0420400/0xD0420500) | debug strings `WrDmacAddrDafRawA/B` | high |
| Mem-to-mem copier | EHD/General/MemoryToMemoryEsub5: 0xE084DE9C init, 0xE084DF70 addresses, 0xE084DF8A sizes+start, 0xE084DF26 term; read ports 58..63 | disassembly | medium; not wired up |
| SD UHS tuning | fn 0xE0172EEE; SDR104 index table 0xE0AC1F04 = {9:195M, 8:156M, 7:130M, 6:111M, 0xE end}; loop pre-increments, so 195M is never tried (same bug as 200D) | disassembly + names table | high |
| SD 195M patch (like 200D) | 0xE0AC1F08: 8 -> 9, 0xE0AC1F0C: 7 -> 8 | as 200D module | medium; rerun entry unknown (candidate 0xE0173F16 with r0=1) |
| ErrCardForLVApp | no string in ROM | | not found |
| dma_memcpy (BLTDMA) | not identified | | not found |

Bit depth on D8 is not one register: Mem1Path hands a 5-byte pack config at
0x12748+0x64 to 0xE054B99A for port 3. 10/12-bit = change that config.

## Values still unknown (all in `raw_config.h`)

| Value | Needed for | Default in branch |
|---|---|---|
| `M50_RAW_LV_EDMAC_CHANNEL_ADDR` | everything | **0xD0420200 (port 3), from ROM, camera test pending** |
| discovery hook | optional verification | **filled in: 0xE054A9AE**, enable `CONFIG_M50_RAW_DISCOVERY` |
| `M50_STUB_mem_to_mem_edmac_copy`, `M50_M2M_CHAN_1/2` | fast copy | CPU copy used instead |
| `M50_STUB_dma_memcpy` | alternative copy | not used |
| `M50_PACK32_MODE_REG` | 10/12-bit | 0, so 14-bit only |
| `skip_left`, `skip_top` (raw.c) | black level, framing | 0 / 28, guesses |
| `SHAD_GAIN_REGISTER` | photo-mode raw histogram | 0, so no correction |
| `ErrCardForLVApp_handler` | card-full dialog detection | dummy |

## Sensor readout (CMOS) - ROM findings 2026-11-18 (for a future crop_rec-style port)

| What | Where (1.1.0) | Notes |
|---|---|---|
| Sensor driver dir | `SENSOR_TYPE/CANON_APSC_24M/` | Sio0Driver.c, ImageSequenceLv.c, ImageSequenceCap.c, ImageTuneSetting.c |
| CMOS list writer | 0xE0824D02 | r0 = list of u32 entries, clocked out over SIO0 (MMIO ~0xD0130298); ends with 0xFFFFFFFF, next word = "Excel Tab Number" (mode sheet ID). Debug print "[REG] CMOS:[%08x]" only when an internal flag is set. Reached indirectly (no direct callers found). |
| CMOS DMA writer | 0xE0824E34 | "[REG] Start CMOSDMA[%lx]size:%d" |
| LV skip points | 0xE07EC600 | asserts wHSkipPoint < LV_CMOS_H_SKIP_POINT_NUM, wVSkipPoint < ..._V_... |
| Zoom window | 0xE0422968 | pNextMagnify->wCmosHskipStart / wCmosVskipStart |
| LV mode patching | ImageSequenceLv.c ~0xE0834000-0xE0836000 | edits entries in lists pCMOS3_3 / 3_4 / 3_18 (R_HMDx, R_VMDx, R_FRMx, R_SSG0, R_PSAVEx, R_CLMP20) |
| Entry format | - | register = entry >> 22 (10 bits), value = entry & 0x3FFFFF (22 bits) |
| Register ids seen in asserts | - | 0x29/0x2A (FRM), 0x46, 0x50, 0x68 (VMD5), 0x6D/0x6E (HMD), 0x93 (CLMP20), 0x9D |

Next step: logger hooking 0xE0824D02 (prologue has a PC-relative ldr: r5 = 0x8BA24, emulate it)
that copies each list + tab number; switch photo LV / FHD / 4K / zoom and diff.
Needs one MMU patch page -> logger-only build (like the spy build) until ML moves to the AllocMem pool.

### Static analysis results (2026-11-18, later)

Tools: `/home/claude/tools/cmos_tables.py`, `cmos_lists.py` (ROM scans).

**CMOS sheet "3-4" (tab 0x0304000x): 12 per-mode variants at 0xE19E3BC0 + n*0x6C**
(22 registers each, entry = reg<<22 | value, end 0xFFFFFFFF, then tab number).
Registers that differ between modes:

| reg | name | values seen | meaning (inferred) |
|---|---|---|---|
| 0x68 | R_VMD5 | E7F6, 5C756, 5BF5C, F6627, 10A5F2, 17452F | vertical window, 2 x 11-bit (first, last) in 2-row units: 28..2038 = full height; 184..1878 = 16:9 (1080p); 492..1575 = 4K crop; 744..1327 = zoom |
| 0x6D,6F,71,73 | R_HMD2/4/6/8 | 10C (default), 60C (4K-crop-like sheet) | horizontal readout, per channel |
| 0x6E,70,72,74 | R_HMD3/5/7/9 | 2014 (default), 1B14 (4K-like), 160B (zoom) | horizontal readout, per channel |
| 0x2D | ? | 0 / 3 | mode switch (candidate: skip/bin mode) |
| 0x50 | ? | FDC, 271, 168, 177, 1C7, 51, 9B6 | per-mode count; FDC also appears in engio D04061A4 |
| 0x93 | R_CLMP20 | FD9, 23C, 199, 154, 125, 77, 89B, 4B3 | clamp/timing |
| 0x9D | ? | 0, 4DD, 4F, 175, 1C5, 9B3 | per-mode |
| 0x29 | (FRM1?) | 3FFF, 200, 150, 30 | frame |

R_VMD5 alone also has a 9-entry table (0xE0B3AAB8, fn 0xE0835B46): 583-row window moved in
179-row steps = zoom-box positions. R_HMD2..9 2-variant table at 0xE0B3AA54 (fn 0xE08343CA).

**DIGIC 8 image-engine timing lists** (address/value pairs ending 0xFFFFFFFF, 168 lists in ROM0):
D0406198 / D040619C = 1C5, 215, 2D1, 303, 427 (cf. EOS M C0F06008/0C "line length" 0x25B);
D04061A4 = 55 ... FDC ... FFFF (shares FDC with CMOS reg 0x50 -> per-mode line count / frame length candidate);
D0406184 = 1, D04061B8 = 0.

Anamorphic recipe on EOS M (Danne, crop_rec.c CROP_PRESET_anamorphic_rewired_EOSM):
CMOS[5]=0x20 (vertical first|last), CMOS[7]=0x1 (read every line, keep 3-column bin from mv1080),
then C0F06804 (raw window h<<16|w), C0F06008/0C/10 (line length), C0F06014 (frame length = fps),
C0F0713C (line count), C0F383D4/DC (output window). M50 counterparts to find: line-skip control
(0x2D / 0x50 / 0x9D candidates), D0406198/9C (line length), D04061A4 (frame/lines), raw window regs.
Next: on-camera logger of the CMOS writer (0xE0824D02) in photo LV vs 1080p vs 4K to confirm which
sheet is which mode.

## Line-skip search: multi-variant CMOS sheets (static)

The CMOS sheets are inside the FixData block at 0xE19E0000 ("FixData.bin" strings at
E01DF6F4 etc.). Nothing in code points at a sheet directly; firmware looks them up
by tab id at runtime. So variant index -> LV mode cannot be read from pointer tables.

Registers that change between variants of the per-mode sheets:

| sheet | reg | values | note |
|---|---|---|---|
| 3-5, 3-8, 3-9 | 0x28 | 0x43xx / 0x42xx (bit 8) | flips in step with 0x78 bit 18 |
| 3-5, 3-8 | 0x78 | 0x40AE9 / 0xAE9 (bit 18) | same split as 0x28 |
| 3-5, 3-8 | 0x6B | 0x40/0x440 vs 0x17D/0x27D/0x57D/0x67D | 0x40 group = one readout family, x7D group = the other |
| 3-8, 3-9 | 0x63 | 0x200, 0x203, 0x004, 0x014, 0xA04, 0xA14 | low nibble 0/3/4 + bits 4, 9, 11 |
| 3-4 | 0x2D | 0 (v0-2) / 3 (v3-10) | |

Variants split into two families: A = {0x28 bit8=1, 0x78 bit18=1, 0x63=0x20x, 0x6B=0x40/0x440}
and B = {bit8=0, bit18=0, 0x63=0x?04/0x?14, 0x6B=0x?7D}. One family is pixel-addition/skip
readout, the other is 1:1. Best line-skip candidates: 0x63 (low nibble) and 0x6B.
Not confirmed: needs the CMOS writer logger (log which variant is written per LV mode).

## EDMAC pack unit: hardware packing for the encoder (static, 2026-11-18)

- RAM .data is copied at boot from ROM 0xE12D7170 to 0x4000..0x551E8 (loops at 0xE004003C).
- 0xE054B99A(port, u32 cfg[5]) -> 0xE054B912: port -> PackUnpack unit (0xE0DD7A70[port]),
  unit table 0xE0DD7E00 {base, edmac+0xD0 value, mode}; register base+0x24 =
  (old & 0xFFECFECE) | cfg1<<20 | cfg4<<16 | cfg0<<8 | cfg2<<4 | cfg3.
  Read side 0xE054BA44(port, cfg[4]) maps to the same register without the bit-20 field.
- Port 3 (LV RAW) -> unit 2, register 0xD0422224.
- cfg2 = bit depth (RawPro 0xE08458xx: 10-bit -> 0, 12-bit -> 1, 14-bit -> 2; xb = w*bits/8).
- Mem1Path (LV RAW) cfg at 0x12748+0x64, initial {0,1,2,1,0}; lv_set_mm writes cfg3 (=!arg).
- RawPro (still RAW, the data C-RAW encodes) uses {0,0,depth,1,0}: only cfg1 (bit 20) differs.
  Working theory: bit 20 = ML/LiveView packing vs Canon photo packing (LSB-first stream).
- test29 clears it (RAM 0x127B0 = 0 and bit 20 in 0xD0422224), checks the first frame with a
  roughness test both ways and logs the result; falls back to the CPU repack if wrong.
- 10/12-bit LV RAW: cfg2 = 0/1, plus port 3 xb (hard-coded w*7/4 at 0xE080D3DA) must change.

## AlgsMgr lifecycle (static, 2026-11-18) - why test25-28 crashed / stalled

State table 0xE0AA72CC, 9 events x 3 states {next, fn}. Event 2 = request (state 1 -> 2
algsmgrStartEncode; in state 2 queued, "algsmgrWaitEncode"), event 3 = EndEncode (2 -> 1),
posted by the encoder interrupt CBR or the timeout CBR (status 9 -> assert = crash), event 4 =
FreeEncBuffer (states 1, 2).
- Free: ALGSMGR_FreeEncBuffer 0xE0795D2E(handle, cbr, ctx). Canon's writer calls (h, 0, 0).
  It mallocs {h, cbr, ctx} and posts it. The handler (0xE024B0BA) stores the block as pParam,
  frees the result buffers, calls cbr(ctx) if non-zero and FreeMemory()s the block.
  Our old code posted &static_array[i]: cbr = next array word (a handle) was called as code
  and a static address was freed -> crash / dead AlgsMgr task -> no more callbacks.
- Callback: success = cbr(ctx, handle, 0x24) (0xE024C2B2); error = cbr(ctx, errcode, 0) (0xE024C18A).
- Jobs without an output buffer are parked (list +0xC0) until a free ("WaitEncStart").
- job+0x50 = dwEncBufMargin (% of input), 0x23/0x3C by mode, grows on "NG" up to 0x7D.
test30: Canon's free function, ctx = sequence number, result accepted only with a2 == 0x24,
late results freed.

## Sensor mode -> CMOS tables, solved statically (2026-11-18)

Selection chain (no camera log needed):
- SensorDrive.c 0xE00DA4D0 "SensorMode(%d,%d,%d)": drive type b (param +0xC), c (+8) -> sensor mode:
  b=5 -> 6, b=1 -> 7, b=6 -> 7 (c 0/1) or 8 (c 10/11), b=2 -> 9, b=16 -> 10, b=19 -> 11,
  b=11 -> 12, b=13 -> 13. m_SensorMode lives at 0xEF24+8 (0..13).
- ImageSequenceStore.c: table 0xE0B3AB64, 14 columns per row:
  +0x000 DEP_CAP, +0x038 DEP_LV (8 = none), +0x070 DEP_LVDAF, +0x0A8 DEP_LVSR (0xE045002A),
  +0x0E0 DEP_CAPLVSR, +0x118 DEP_ALL (0xE0450096, identity).
- LV_Prepare 0xE08349B4 writes sheet 3-8 (base+0x3594 + LVSR*0x24), sheet 3-4
  (base+0x2F14 + LVSR*0x6C), then the engio list *(0xEEDC+0x1C)+0x6ED4 + DEP_ALL*0x58.
  base = *(0xEEDC+0x18) = RAM copy of FixData; base+X <-> ROM 0xE19E0CAC+X
  (3-5 at +0x33B8 stride 0x20, 3-9 at +0x3784 stride 0x1C, 3-20 at +0x39A4 stride 0xB0,
  unknown 96-word sheet at +0x2314 stride 0x180). All 76 writer call sites: tools/cmos_callers.

| mode | guess | rows (VMD5) | 0x28 | 0x6B | 0x78 | 0x63 | 0x50 | HMD2/3 |
|---|---|---|---|---|---|---|---|---|
| 3 | still capture A | 56-4076 | 4305 | 440 | 40AE9 | 200 | FDC | 10C/2014 |
| 4 | still capture B | 56-4076 | 4305 | 440 | 40AE9 | 200 | FDC | 10C/2014 |
| 5 | still capture C | 56-4076 | 4305 | 440 | 40AE9 | 200 | FDC | 10C/2014 |
| 6 | 1080p 16:9 binned | 368-3756 | 4205 | 67D | AE9 | A04 | 271 | 10C/2014 |
| 7 | photo LV 3:2 binned | 56-4074 | 4205 | 67D | AE9 | A14 | 271 | 10C/2014 |
| 8 | 16:9 binned, short frame (60p?) | 366-3768 | 4205 | 57D | AE9 | A04 | 168 | 10C/2014 |
| 9 | 3:2 binned variant | 56-4072 | 4205 | 67D | AE9 | A14 | 177 | 10C/2014 |
| 10 | 3:2 binned variant | 56-4070 | 4205 | 67D | AE9 | 14 | 1C7 | 10C/2014 |
| 11 | 16:9 crop window, binned | 1064-3044 | 4205 | 17D | AE9 | 4 | 51 | 10C/2014 |
| 12 | 4K crop 1:1 | 984-3150 | 4305 | 440 | 40AE9 | 4 | 9B6 | 60C/1B14 |
| 13 | zoom 1:1 | 1488-2654 | 4305 | 440 | 40AE9 | 4 | 271 | 10C/160B |

Result: 1:1 readout (modes 3-5, 12, 13) = {0x28 bit 8 set, 0x6B = 0x440, 0x78 bit 18 set};
binned/skipped LV (modes 6-11) = {0x28 bit 8 clear, 0x6B = 0x?7D, 0x78 bit 18 clear}.
0x63 does NOT split cleanly (mode 11 binned has 4 like the 1:1 modes) - earlier guess withdrawn.
0x6B bits 8-10 vary only among binned modes (6 / 5 / 1): best candidate for the vertical
skip/bin pattern; low byte 0x7D vs 0x40: best candidate for horizontal binning.
Anamorphic test: in mode 6, override 0x6B bits 8-10 to the 1:1 value (4) keeping low 0x7D,
via a hook on the CMOS writer 0xE0824D02 (one patch slot), plus the matching 0x50/0x93/0x9D
and engio frame timing.

## Per-mode timing (engio), static (2026-11-18)

Engio list per sensor mode: ROM 0xE19EBA70 + mode*0x58 (RAM *(0xEEDC+0x1C)+0x6ED4 + DEP_ALL*0x58).

| mode | D0406198/9C line length | D04061A4 frame length | fps if clock 27 MHz |
|---|---|---|---|
| 0-2, 3 | 0x303 / 0x215 / 0x427 | 0xFDC | still readout |
| 6 (1080p) | 0x2D1 | 0x270 | 59.8 (24/25/30 = longer frame at runtime) |
| 7 (photo LV) | 0x2D1 | 0x26F | 59.9 |
| 8 | 0x215 | 0x1A4 | 120 (720p120?) |
| 9 | 0x2D1 | 0x176 | 99.7 |
| 10 | 0x215 | 0x1C6 | 111 |
| 11 | 0x215 | 0x55 | (AF/scan mode?) |
| 12 (4K crop) | 0x1C5 | 0x94D | 24.97 |
| 13 (zoom) | 0x2D1 | 0x270 | 59.8 |

The 27 MHz clock is inferred (4K -> 25.0 fps, mode 8 -> 120.1 fps), not measured.
D04061A4 = M50 equivalent of the old C0F06014 (frame length): FPS override target.
D04061B8 = 0, D0406184 = 1 in every mode (latch/enable?), D04061D0 = 0 in LV modes.

Anamorphic budget: 1080p line = 722 clocks = 26.7 us per row (all 6000 columns, 3x binned).
24 fps = 41.7 ms -> at most ~1500 rows read every line (minus blanking). With a narrower
horizontal window the line gets shorter (4K: 4368 columns in 454 clocks = 16.8 us).

## LV RAW output sizes (SapPath), static (2026-11-18)

SapPath.c "SapSetPath[%d][%d]" (0xE080C3E0): geometry = 0xE0F7CDA8 + path*16 = {w, h, w2, h2},
copied to Mem1Path +0x18.. (0xE080D4AA); port 3 programmed with xb = w*7/4, yb = h-1 (0xE080D3D8).

| path | size | likely mode (from sensor windows) |
|---|---|---|
| 0 | 2096 x 838 | photo LV (measured) |
| 1 | 4368 x 2202 | 4K crop 1:1 (measured) |
| 2 | 2096 x 608 | high-fps binned (60p/120p?) |
| 3 | 2096 x 1164 | 1080p movie: full width, ~3x3 binned (window 3388 rows / 1164 = 2.9) |
| 4 | 4288 x 1202 | zoom 1:1 (window 1166 rows) |
| 5 | 2096 x 258 | AF / scan |
| 6 | 2096 x 714 | high-fps binned |
| 7 | 2096 x 146 | AF / scan |
| 8 | 2096 x 838 + 2096 x 714 | dual stream |

Frame length units: binned modes 1 unit = 2 output rows (1080p: 1164/2 = 582 + 43 blank = 625 = 0x270+1),
1:1 4K: 1 unit = 1 row (2202 + blank = 2382). Anamorphic (vertical 1:1, horizontal binned) at 24 fps:
1500-3000 rows depending on which unit applies; needs a camera test.

## The vertical readout register: 0x65 (static, 2026-11-18)

LV_SetGreset1 (0xE083486C) writes, per mode: bulk list base+0x151C (0x1BA entries), sheet 3-5
[DEP_CAPLVSR] (+0x33B8), and the big per-LV-mode sheet base+0x2314 + DEP_LV*0x180
(ROM 0xE19E2FC0, 88-91 registers, DEP_LV 0..7 = sensor modes 6..13). LV_Prepare then writes
3-8 and 3-4 (later writes win).

Key rows of the DEP_LV sheet:

| reg | m6 1080p | m7 photo LV | m8 | m9 | m10 | m11 | m12 4K 1:1 | m13 zoom 1:1 |
|---|---|---|---|---|---|---|---|---|
| 0x65 | 101301 | 202502 | 202502 | 303703 | 912 | 912 | 100 | 100 |
| 0x64 | 222202 | 222203 | 222203 | 222204 | 211012 | 211012 | 11101 | 11101 |
| 0x50 frame (rows) | 4DF | 271 | 168 | 271 | 83 | 51 | 9B6 | 4DF |
| 0x51 line length | 2D1 | 2D1 | 215 | 2D1 | 215 | 215 | 1C5 | 2D1 |
| 0x1E6-0x1ED | 55AA 6633 99CC AA55 x2 | same | same | same | same | same | FFFF | FFFF |

0x65 = n*0x101001 | (2n+1)<<8: vertical period 2n+1 = 3 (1080p), 5 (photo LV), 7 (mode 9),
1 (1:1 modes, n = 0 -> 0x100). Matches the measured output heights (1080p 3388/1164 = 2.9,
photo LV 4018/838 = 4.8). 0x64 low digit = n+1 (1:1: 0x11101).
0x1E6-0x1ED look like the horizontal column-combine masks (binned: 55AA/6633/99CC/AA55, 1:1: FFFF).
0x51 = line length, same value as engio D0406198. 0x50 = frame length in CMOS lines; binned
modes run 2 CMOS lines per engio line (1080p: 0x4DF = 2 x 0x270 - 1), 1:1 modes 1 per line.

Anamorphic recipe (mode 6, 1080p): 0x65 -> 0x100, 0x64 -> 0x222201, keep 0x1E6-0x1ED / 0x6B,
shrink the VMD5 window (0x68) to the rows that fit, raise 0x50/0x93/0x9D and engio D04061A4,
and grow port 3 height (SapPath path 3 = 2096 x 1164). If the 2-lines-per-engio-line readout
holds, a 24 fps frame fits ~2900 rows (13.4 us per row at 27 MHz).

## Overrides without MMU patches (static, 2026-11-18)

- CMOS table block = FixData item 0x10003 (0x3EF0 bytes, ROM image 0xE19E0CAC), engio block =
  item 0x10004 (0xA7A4 bytes, ROM image 0xE19E4B9C). Looked up once (0xE08355CE ->
  0xE0573D92) into *(0xEEDC+0x18) and *(0xEEDC+0x1C); every LV writer re-reads those pointers.
  E-shutter/still code uses its own copy at 0x15EA8.
- Canon patches engio lists in place (0xE0834738 / 0xE083BF58: find reg, store value), so the
  engio block is RAM. Plan: check the pointer at runtime; if RAM, edit the tables in place,
  otherwise copy the block to RAM and repoint *(0xEEDC+0x18/0x1C). Then re-enter LiveView so
  LV_SetGreset1 / LV_Prepare send the edited lists. No patch slot needed.
- The CMOS writer 0xE0824D02 calls optional hooks *(0xEF84+0x20)(0, list, 0) before and
  *(0xEF84+0x24)(0, list_end, ret, 0) after each list, registered by Sio0Driver (0xE08258E0).
  Usable as a free CMOS logger.
- Where each key register lives (block offsets): 0x64/0x65 only in sheet 3-1 (+0, +E24, +1C1C,
  still modes) and the DEP_LV sheet (+0x2318 + DEP_LV*0x180). 0x68 VMD5, 0x50, 0x93, 0x9D are
  also in 3-4 (+0x2F18 + LVSR*0x6C), written last by LV_Prepare -> 3-4 decides window/timing.
  0x6B final value from 3-8. 0x1E6-0x1ED only in 3-1 and DEP_LV.
- 0x68 = (first_row/2) << 11 | (last_row/2). 1080p (3-4, LVSR 3): 0x5C756 = rows 368..3756.

## Runtime frame rate (static, 2026-11-18)

m_VSize at 0xEF24+0x10 (setter 0xE044FE64, getter 0xE044FE7A) holds the frame length.
ImageControllerLvState gets it in a message (0xE082C016) and calls FLK_End1B 0xE0835866, which
writes VSize-1 into the D04061A4 entry of the DEP_ALL engio list and sends the list.
FPS override = set m_VSize, call 0xE0835866 (Canon's own path; no direct register poking).

## Encoder bit depth

Not settled statically. The CRX parser (0xE02D8F14) accepts BitDepth >= 8; whether the
hardware encoder takes 10/12-bit input via req+0x14 needs a one-frame camera test.

## test34: sensor_tune.c (Debug -> M50 sensor tuning)

- Sensor tables: shows where *(0xEEDC+0x18) points and how many words differ from the ROM
  image (base 0xE19E0CAC verified: 3-4 at +0x2F14, DEP_LV at +0x2314, lists padded with
  0xFFFFFFFF after the tab number). ROM pointer -> RAM copy + repoint.
- Vertical readout: current sensor mode's DEP_LV 0x65 -> 0x100 (+ optional 0x64 low digit 1),
  3-4 VMD5 shrunk by the factor around the same centre. Same output size and timing.
  Expected 1080p result: 2096x1164 frame, picture 3x magnified vertically.
- Sensor log: chained pre-hook at *(0xEF84+0x20), logs list pointer + sensor mode, saves
  ML/LOGS/CMOSLOG.TXT with block offsets and tab numbers.
- FPS override: VSize = current VSize * measured fps / target, via 0xE044FE64 + 0xE0835866.

## Freeing the second MMU page (static, 2026-11-18)

generic_mmu_space (patch_mmu.c) is in .bss: every remap page moves _bss_end by 64 kB.
2 pages put _bss_end at ~0x16FE00; 1 page gives 0x15FA00 (under the 0x160000 that is known
to boot). SD 195 MHz no longer patches ROM: Canon's tuning loop (0xE0172EEE) is just
  for clk in table: SdChangeClockSpeed 0xE01E8952(card, clk); tune 0xE01EB070(card)
with card = *(0xB130+0x10), error in bit 0. sd_speed.c runs Canon's 0xE0173F16(1) first
(SDR104 + its own tuning), then tries clock code 9 (195 MHz) + tune, and reruns Canon's
sequence if that fails. test35 = 1 page only.

## Taller RAW frames (static, 2026-11-18)

SapSetPath (0xE080C3D8, state 0x126E0: +0x10 done flag, +0x14 path) does:
  0xE080C53E (engine resource lock, list 0xE0F7CE38), 0xE080C526(5),
  for each SAP module in 0xE0F7CC28 (32-byte entries, setup fn at +0xC:
  choff, chgain, yossie, degeen28_1, gain, ..., RSHD, dafprm): fn(&{path, &geom[path], 0}),
  then Mem1Path 0xE080D4AA(geom, geom+8) -> port 3 size. geom = ROM 0xE0F7CDA8 + path*16.
Every block between the sensor and port 3 is sized from that ROM entry, so a taller sensor
window alone gives a frame cut at 1164 rows. Two ways to make it taller:
  a) patch the geometry entry (ROM page 0xE0F70000 = the page freed above), or
  b) after Canon's SapSetPath, call the same module setups + 0xE080D4AA with a RAM
     geometry {2096, H, ...} under the same lock (0xE080C53E / 0xE080C57C).
Canon's LV RAW buffer is sized for path 1 (4368 x 2202 x 14 bit = 16.8 MB), so 2096 x H
fits up to H ~ 4500.
CMOS 0x93 follows the output height: 1:1 modes 0x93 = rows + 1 (4K 0x89B, zoom 0x4B3),
binned modes rows/2 - 10 (1080p 0x23C, photo LV 0x199). Needed when the window grows.
HMD (0x6D-0x74, start/end pairs): binned 0x10C..0x2014, 4K 0x60C..0x1B14; units not
resolved (ratio to output columns 1.23-1.26, OB handling unclear).

## Horizontal window HMD (static, 2026-11-18)

Registers 0x6D/0x6E, 0x6F/0x70, 0x71/0x72, 0x73/0x74 = 4 identical (start, end) pairs (set all 4).
Known values (final ones come from sheet 3-4):
- full width (still, all binned LV modes): 0x10C .. 0x2014 (span 7944)
- 4K crop (mode 12):                     0x60C .. 0x1B14 (span 5384)
- zoom (mode 13), ROM table 0xE0B3AA54:  left 0x10C .. 0x160B, right 0xB15 .. 0x2014 (span 5375),
  plus 9 vertical positions for 0x68 (0xE0B3AAB8, rows 56-1222 ... in ~358-row steps) and 0x93 = 0x4B3.
All windows are centred on 0x1090 (full and 4K exactly).

Units: 4 HMD units = 3 sensor columns = 1 binned output pixel.
  binned: 7944 / 4 = 1986 output px vs 1992 active (2096 - 104 OB)
  4K:     5384 * 3/4 = 4038 columns vs 4104 active (4368 - 264 OB)
  zoom:   5375 * 3/4 = 4031 columns vs 4024 active (4288 - 264 OB)
OB columns are added outside the window. Residuals of 6-66 columns: the OB/margin count per mode
is not exactly known, so allow a few pixels of slack.
Crop formula: binned N px -> span 4N; 1:1 N columns -> span 4N/3; start = 0x1090 - span/2.
Line length (0x51 / engio D0406198) is per mode, not derived from HMD: zoom keeps 0x2D1 with a
narrower window, 4K uses 0x1C5. A narrower window allows a shorter line (faster rows), which is
what an anamorphic crop with more rows at 24 fps would use; the minimum safe value needs testing.

## HMD units, exact (2026-11-18, from the two raw dumps we have)

Column means of RAW00000.BIN (photo LV 2096x838) and RAW00001.BIN (4K 4368x2202):
- optical black = 264 sensor columns in both: 4K cols 0-263, binned px 0-87 (264/3 = 88);
  image starts at col 264 / px 88; 34 OB rows on top in both. (raw.c keeps skip_left 104
  for binned: 16 px safety margin, harmless.)
- active: binned 2008 px = 6024 columns (the M50's 6024-column active area), 4K 4104 columns.
- fit: active_columns = 3/4 * (HMD_end - HMD_start) + 66   (slope exactly 3/4 from 2 points:
  (6024-4104)/(7944-5384) = 1920/2560). Output width = 264 + active (binned: /3).
- zoom: the table window (span 5375) gives 4097 + 264 = 4361 columns, SapPath path 4 is 4288
  wide: the pipeline drops ~73 columns (or path 4 is not zoom). Needs a zoom dump to settle.
Crop recipe: want A active columns -> span = (A - 66) * 4/3, start = 0x1090 - span/2.

## Line length (row time) vs window: what decides it

Two ADC/readout configurations exist (DEP_LV sheet, regs 0x5B-0x60, 0xD0-0xDE, 0x119, 0x16A,
0x177, 0x196, 0x19A, 0x1B9, 0x1BD, 0x1E5, 0x1F5, 0x1F9/0x1FA, 0xE2-0xE6):
- "slow": modes 6, 7, 9, 13 (0x5B=AA0A6, 0x60=101F1): line 0x2D1 = 721 clocks
- "fast": modes 8, 10, 11, 12 (0x5B=3FF800, 0x60=FFF): line 0x215 = 533 (binned) / 0x1C5 = 453 (4K 1:1)
Width does not set the line: zoom reads ~4100 columns and keeps 721; 4K reads 4104 columns at 453
only because it uses the fast set. Highest output rate seen: 4K, 4368 px / 453 clocks = 9.6 px per
clock -> data limit: line >= pixels_per_line / 9.6.
Binned modes send 2 output rows per line (0x50 = 2 x VSize). Row budget at 24 fps (27 MHz,
1124k clocks per frame, minus ~3% blanking):
| readout | line | 2 rows per line | 1 row per line |
|---|---|---|---|
| slow (1080p now) | 721 | ~3000 rows | ~1500 rows |
| fast (mode 8 set) | 533 | ~4000 rows (whole sensor) | ~2000 rows |
| data limit, 2096 px rows | ~436 | (4192 px per line) | ~4900 / ~2400 |
So the lever for more rows is the fast readout set, not a narrower window. Mode 8 (binned
16:9, window 366-3768, fast set, 120 fps) is the natural base for an anamorphic mode:
mode 8 + 0x65 -> 0x100 + taller window + VSize for 24 fps. Fast-set image quality (ADC
bit depth / noise) is unknown: compare a mode 8 frame against mode 6.

## Camera results test35/36 (2026-11-18 evening)

- Sensor tables pointer = 0x405FCCAC: uncached RAM -> editable in place, confirmed.
- 1080p25 movie = sensor mode 6, VSize 1496 -> 27e6 / (722 x 1496) = 25.00 fps: the 27 MHz clock
  and VSize meaning are confirmed.
- "Vertical readout 1:1 (0x65)" in 1080p: picture changed (taller look, image only in the upper
  ~half, grey below). Fits 0x65 working while 0x64 low digit (still 2) sets a period of 2:
  1130 window rows / 2 = 565 rows = ~49% of 1164. Next: "1:1 (0x65+0x64)" should fill the frame.
- CRX in movie 1080p: 0 encoded, 340 worker drops in 14 s (worker_busy never cleared).
  test37 adds diag lines (worker got frame / request sent / callback came) to locate it.
- FPS override refused because it needed the RAW stream; test37 computes fps from
  27 MHz / (line x VSize) instead.
- test37 chessboard (0x65+0x64, window 1/3): squares ~1.5x taller than wide, image in the top
  ~half: effective vertical period 2 instead of 1 (1130 / 2 = 565 rows, 3/2 stretch).
  test38: options +0x6B (middle digit 4, as in 1:1 modes) and +0x28/0x78 bits, set in every
  per-mode list (3-5, DEP_LV, 3-8, 3-4); window 1/3, 2/3 (fills the frame at period 2), full.
- test37 CRX log (photo LV, C-RAW): pack bit 20 is NOT the bit order (roughness Canon-order
  10.25M vs ML-order 171k): fallback worked, hardware packing now default off.
  Encoder works: request -> callback a2 0x24 every frame. C-RAW handles have 81 entries
  (+0x18 = entry count, +0x10 = tiles) and a 544-byte mdat header; the parser only allowed 9
  entries / 512 bytes -> "bad handle". test39: up to 128 entries (merged when contiguous),
  1 KB header, frame block format "CRX2" (crx_tool.py reads both).
- Pack config, second look: the test37 register value c0100020 has bit 0 (cfg3) = 0 because ML
  calls lv_set_mm(1) (cfg3 = !arg). Canon's default (mm 0) and RawPro (still RAW) use cfg3 = 1.
  test40 tries cfg3=1, then cfg3=1+cfg1=0 (exact RawPro config), roughness-checked each,
  CPU repack fallback. Bit depth field unchanged, so line size stays w*7/4.

## Every pack/unpack config in the firmware (static, 2026-11-18, work/packcfg.py)

PACK (0xE054B99A, cfg0..4 -> bit8, bit20, bits4-5, bit0, bits16-17), constants recovered:
  E084D162/E084D8F0 [1,0,2,1,0], E0856F0A [?,0,2,1,0], E085948A [0,0,-,1,0], E08D9572 [0,0,2,1,0],
  E08DB286/E08DB31E [-,0,2,1,0], E08DB448..E08DCFD6 [?,?,2,1,?], E011DEDC/E087BF48 cfg3 = 1.
UNPACK (0xE054BA44, read side, bit0 = 3rd field): [0,2,1,0] / [1,2,1,0] everywhere for 14-bit.
=> Every Canon RAW writer and reader uses bit0 = 1 and bit20 = 0. The only exception is LiveView
RAW (Mem1Path {0,1,2,1,0}, and bit0 forced to 0 by ML's lv_set_mm(1)). The encoder, defect
correction and development all read the bit0=1/bit20=0 format, so that is "Canon packing";
ML's LV format differs in both bits. test40's second candidate is exactly that config.

## Camera results test40 (2026-11-18 20:30)

- HARDWARE PACKING FOUND: pack reg bit 0 = 1 (cfg3 = 1, what lv_set_mm(1) clears) gives Canon
  packing: roughness Canon-order 391k vs ML-order 10.96M. Repack 0 us, encoder reads the LV
  buffer directly. Bit 20 is not needed.
- C-RAW from LV: 1.2-1.44 MB per 2096x838 frame (2.54:1), CMP1 valid (2096x838, 14 bit, 4 planes,
  encType 3, 544-byte mdat header starting ff01/ff02/ff03 = standard CR3 C-RAW layout).
  Encode typ. 5-8 ms, spikes 60-96 ms.
- Bottleneck was the writer: 40 FIO writes per frame straight from Canon's buffers took ~4 s per
  frame on the 10 MB/s card, with the 3 job slots blocked -> encoder starved. test41: copy into one
  slot, free Canon's buffers at once, one FIO write per frame, 8 slots.
- Vertical 1:1 WORKS: "+0x6B+0x28+0x78" (0x65=0x100, 0x64 low 1, 0x6B mid 4, 0x28 bit 8, 0x78 bit 18)
  with window 1/3: full 2096x1164 frame, chessboard squares ~3x taller = every row read.
  "+0x6B" alone: back to 3x (normal shape, image in top third). So all five registers are needed.
- FPS override: no effect, and crashed when switching views -> removed from the menu.

## Why the encoder does not answer in movie mode (test47 log + static, 2026-11-18)

test47 movie 1080p: memory ok (fio_malloc), HW packing CONFIRMED in movie mode too, but
request 1 -> callback TIMEOUT.
StartEncode (0xE024BBA4 -> 0xE024BC20): if mgr+0x150 != 0 the job waits; otherwise it asks the
resource manager for its encode resource: RscMgr 0xE0571970(alloc CBR 0xE024B757, mgr, free CBR
0xE024B791), request type 0x47 (RscMgr.c), then waits on mgr+0x1C. The grant CBR
(_algsmgrAllocateResourceCompleteCBR) copies a 0x248-byte descriptor to mgr+0x154 and gives the
semaphore. Free CBR -> ALGSMGR_SubEncBuffer (0xE0795EF0, event 1): arg -1 = "EncPermit" (clears
mgr+0x150, starts a waiting job), other = block. AlgsMgr also follows PROP_PIC_QUALITY /
PROP_PIC_QUALITY2 (0x8000002F/30, event 5, handler 0xE024C784).
Most likely: in movie mode RscMgr does not grant resource 0x47 (movie pipeline holds it), so the
AlgsMgr task waits forever. test48 logs encode state, mgr+0x150, EncBufNum and the 0x248-byte
descriptor (photo mode: what a granted resource looks like; movie mode: where it is stuck).
Possible fix: grant it ourselves with a descriptor that points at our own memory.

## SRM 1st-job memory (static, 2026-11-18, test50)
- SRM_AllocateMemoryResourceFor1stJob = 0xE056A5C6 (confirmed: FA_RemoteRelease 0xE0087454 calls it right before
  logging "SRM_AllocateMemoryResourceFor1stJob failed"). Callback is cbr(ctx, buffer) - 2 args, no size.
  Canon waits 5 s (0x1388 ms) for it.
- SRM_FreeMemoryResourceFor1stJob = 0xE056D7E6 (buffer, 0, 0) (FA_FinishRemoteRelease 0xE008737C).
  The old commented stub 0xE056DA22 is a different, generic function (200+ callers).
- Likely reasons the old ML attempt "crashed": ASSERT on a size arg that isn't passed, 100 ms timeout
  (late callbacks), wrong free stub.
- AlgsMgr's encoder grant (test48) = two ranges of exactly 0x2AB1000 bytes, the same size class as the 200D's
  1st-job buffer (0x2A9C000). Hypothesis: in movie mode these buffers belong to movie recording, so the
  encoder's grant never comes.
- test50: after 2 frames are encoded, crx_rec asks for one 1st-job buffer and uses 32 MB of it for write slots;
  given back at stop. Menu: CRX recording > Canon SRM memory.
- test50 RESULT (photo M mode, encoder running): the log ends at "SRM: asking Canon for a 1st-job buffer". Neither
  "got" nor "timeout" was logged, so the control task never got past the request + 3 s wait. ML lagged badly and
  the camera crashed at ~40 s. This is the same symptom kitor saw. Most likely reading: the SRM pool is used up (AlgsMgr's grant
  holds two 1st-job-size buffers; descriptor +0x240 = 2, 2) and our request is queued as a "prior message"
  (0xE056A404) ahead of Canon's own requests, which stalls Canon. Not proven. SRM is OFF by default from test51.

## Canon memory-area table (static, 2026-11-18)
- Table of {size, name_ptr, 0} entries at 0xE09D8F00..0xE09D9584 (names at 0xE125B1xx..0xE125BCxx); full dump in
  /home/claude/work/memmap_table.txt. Caution: whether each size belongs to its own row or the next one is not verified
  (L/U pairs look shifted).
- Names: ALGS_AREA_1/2 (L/U), EXALGS, CAPMEM1, DECMEM1, TWAIN, SS, YUV 1st/2nd, LV_WORK, LV_SERVO_WORK,
  MOVIE_STREAM (~116 MB), MOVIE_RECWORK (77 MB), MOVIE_RECYUV (186 MB), IMGPLAY_WORK (264 MB),
  SLIDE_SHOW_WORK (76 MB), HDR/GIS_*, INDEV_*, DP_*. Many entries are 0x2AB1000 = AlgsMgr's grant size.
- The sizes add up to far more than the RAM, so these are overlapping per-mode layouts (playback, LV, movie, HDR,
  in-camera RAW development) placed over the same physical RAM.
- SRM_AllocateMemoryResourceForMovieRecWork: function 0xE056ABAA (string ref 0xE056ABDE); message 0xA8 at
  0xE056AC12 asserts "pfCompleteCBR != NULL" (callback based, like 1st job). Not tried.

## Movie-recorder buffers (MovRecResource.c, static 2026-11-18, test52)
- Object *(0xEB28): +8 dwClientNum, +0x24/+0x28 stream, +0x2C/+0x30 RecWork, +0x34/+0x38 writer work,
  +0x54 transcode, +0x58 startup flag.
- MRRSC_Startup(flag, transcode) 0xE02F4F7C. Callers: mvrRecStart (0xE02F0228), AllocateCvram (0xE02F2CCE),
  mvrTransEncInit, and one more. The first client allocates the stream buffer (0xE056AC12 / 0xE0571EA8, cbr 0xE02F4BF1) and RecWork
  (0xE056ABAA / 0xE0571F4A, cbr 0xE02F4C19 = cbr(ctx, addr, size)). It waits up to 120 s, then asserts.
- MRRSC_Shutdown() 0xE02F54DC. Callers: mvrRecStopped, FreeCvram. The last client frees everything
  (SRM_FreeMemoryResourceForMovieRecWork 0xE056DCDE(addr, cbr, ctx), MovieStreamNormal free 0xE056DD36).
- test52: logs the MRRSC state at start. Optional "Movie: free Canon buffers": if clients == 1 (no transcode), call
  Shutdown before recording, then Startup(saved flag, 0) at stop. Risk: if AlgsMgr keeps its grant after we stop,
  Startup can wait up to 120 s and then assert.

## BSS cliff: movie-mode boot crash in test52 (2026-11-18)
- patch_mmu.o has a 64 KB-aligned BSS array, so the whole .bss output section is 64 KB aligned. When text+data grows past
  0x120000 (test52: _data.end 0x1201E1), .bss moves from 0x120000 to 0x130000 and ML takes 64 KB more of
  DryOS user_mem. With that build the camera crashed 3 s after booting into movie mode (photo mode was fine; without ML no crash).
- test53: SRM code removed and the descriptor dump shortened -> _data.end 0x11FC41, .bss back at 0x120000
  (about 1 KB of room left). CHECK EVERY BUILD: `grep "^\.bss " build/location.map` must say 0x00120000.
- REAL cause of the movie-mode boot crash (test49-53): the zips had the OLD ML/modules/M50_110.sym (from test48)
  with a new autoexec.bin. Modules (mlv_lite etc.) linked to wrong addresses and crashed in movie mode. test47 had a matching
  .sym and was fine. Always package with /home/claude/tools/pack.sh N (copies the matching .sym, checks the BSS).
  test50/51 photo runs (lag, crash at ~40 s) also had the mismatched .sym, so those results are suspect.

## TODO (user asked to keep this): big Canon-approved write queue in movie mode
- After MRRSC_Shutdown (test54 log: movie mode, 25 fps, 2096x1164) Canon gave back stream 77 MB, RecWork 214 MB and
  writer 34 MB. The encoder's grant takes 2 x 42.7 MB; the rest sits unused while CRX records.
- Plan: request part of it ourselves via SRM_AllocateMemoryResourceForMovieRecWork 0xE056ABAA(param, cbr, ctx).
  cbr(ctx, addr, size), see Canon's mrrscAllocateRecWorkBufferCBR 0xE02F4C18. Message 0xA7; it logs its param with %d
  (Canon's caller passes sl - work out what that value means first). Free with SRM_FreeMemoryResourceForMovieRecWork
  0xE056DCDE(addr, cbr, ctx); Canon's free cbr 0xE02F4C40 just gives a semaphore. Order: MRRSC_Shutdown -> encoder's first
  frame (its grant) -> our RecWork request (with timeout) -> write slots in it; at stop: free ours BEFORE MRRSC_Startup.
- Use: a 50-150 MB write queue that rides out slow-card stalls without touching the small fio pool.
- Also still open: slow copy (test55 measures cached vs uncached on core 1), taller anamorphic frame (SapPath),
  FPS control, PC converter.

## test55 result / test56 (2026-11-18)
- test55 (movie, 25 fps LV, 2096x1164 C-RAW ~1.2-1.45 MB/frame): 7.7 fps, 233 frames, 0 errors. Copy on core 1: cached
  51 ms avg (19 ms of that is cache maintenance), uncached 79 ms -> cached only from now on. The bottleneck has moved to the writer and the
  2-slot queue: the writer was busy 19.5 s of 29.9 s (~17 MB/s while writing) and queue drops were 227. The writer sat idle while the only
  free slot was still being copied.
- test56: RecWork write queue implemented (TODO above, param 0x100000 confirmed at 0xE02F52EC), up to 24 slots in
  RecWork. Fixed `sl &= NSLOT-1` (wrong once NSLOT stopped being a power of 2). sd_test.o removed from the build to make room
  (_data.end 0x11F4A1).

## test57 result / test58 (2026-11-18)
- test57: set/way L1 maintenance works (0 spot-check failures out of 410 frames). Copy 24 ms avg, cache ops 0.09 ms (was 16.6 ms).
  Encoding bursts to 15-17 fps. Bugs: write_mq held only 18 entries with 24 slots, so posts were lost and ~5 slots leaked
  (the queue never went below 5, and at stop "5 slots still busy" meant RecWork was NOT given back). MRRSC_Startup still got the same
  RecWork address -> SRM doesn't hand it out exclusively (fixed area?); do not rely on it as a guard.
  Card: 469 MB in 28.9 s of write time = ~16 MB/s -> ~11 fps at 1.45 MB/frame is the old card's ceiling.
- test58: write_mq = NSLOT+4, a failed post frees the slot (counted), encoder back to priority 0x19 (copy is on core 1).

## test59 (2026-11-19): ring buffer, DMA copy, big aligned writes
- HPCopy(dst, src, len) = 0xE02E0396 (System/HPCopy.c, 23 callers): gets an Xdmac channel (returns 1 if none is free),
  runs a sync transfer 0xE0670224, asserts if that fails. No alignment checks seen in Xdmac. We pad pieces to 32 bytes; a spot check
  on every frame falls back to the CPU copy.
- Ring: frames back to back ("CRX3": header 64 + 4*n lengths, padded to 32; cmp1 and pieces padded to 32). The writer writes up to
  8 MB per call, cut at 4 MB file offsets; it waits (max 150 ms) for >= 1 MB. 4 GB file splits only between frames (frame-end FIFO).
- Movie mode: RecWork is taken right after MRRSC_Shutdown, before encoding; raw sinks and the ring (<= 192 MB) both live in it.
  Photo mode: fio sinks plus a fio ring of 1.5 x raw size.
- The geometry check now allows widths up to 6144 (3.5K tests; Canon tiles above 3144 - untested).
- crx_tool.py reads CRX3.
- Handoff "DDR50 investigation": not pursued. DDR50 = max 50 MB/s, below SDR104 (104). Only taken from it: SD clock id at
  RAM 0x19130 (+4*port), read-only, now logged (3 = 48.8 MHz, 9 = 195 MHz per that note - not verified).

## SD clock (2026-11-19, test60)
- Agent claim verified statically: clock id cache at RAM 0x19130 + 4*port. The setter (just before 0xE044655C) stores it and calls
  the frequency selector 0xE01D3E0C. Id -> MHz (3 = 48.8, 9 = 195) is the agent's mapping, not verified.
- Conflict: the agent says Canon's default is id 3 (48.8 MHz -> max 24.4 MB/s at 4 bits). My earlier tuning analysis (table
  {9,8,7,6}, pre-increment) says Canon ends at 156 MHz (id 8) when tuning passes. The old SDTEST.LOG showed id 9 after
  SD195 with err 0/0; its "0 write" was the test running out of memory, not a write failure.
- test60 logs speed mode + clock id (driver getter 0xE01E9E39 + cache) at start, and card MB/s while busy at stop.
  Plan: one run with SD 195 OFF (Canon's default), one ON. A slow card limits both; the fast card decides.
- DDR50: verified that Canon's UHS-I access-mode setter USetAcMd 0xE018B842 only handles modes 0-3 (SDR12/25/50/104); anything else goes to
  the error branch. There is no DDR50 implementation to call. Switching it would mean guessing a host register while the card is in use.
  Not done. test61 adds a read-only menu entry Prefs > "SD card mode" (selected function, clock id, CMD6 support mask, DDR50 bit).

## DDR50 host search, continued (static, 2026-11-19)
- USetAcMd 0xE018B842: modes 0..3 call sddomSwitchSDR12 0xE018D9EA / SDR25 0xE0187DFE / SDR50 0xE018DA4A / SDR104 0xE018DAB6.
  These are card side only (CMD6 arg 0x00FFFFF0+fn after checking the support bit in status byte 13). They store "speed mode" 2/3/4/5
  in [port struct + 0x4C], so the log's "speed mode 5" = SDR104 (card side). Around each switch: SetSDClkFrequency helpers gate
  D0100634 (1 before, 0 after) and set up D0100610.. (0xE01D3B92).
- Clock-id selector 0xE01D3E0C: ids 0..13 (tbb). Each id writes a 0x84-byte timing profile from RAM (0x1913C + k*0x84; ids 0..9 use
  0x1913C, 0x191C0, 0x19244, 0x192C8, 0x1934C, 0x19454, 0x193D0, 0x194D8, 0x1955C, ...) via 0xE01D3D34, then 0xE042EB44(port, 5).
  Ids 0..9 write 0 to D010F100 + port offset (0xE01D3B82). Ids 10..13 reuse the profiles of ids 0..3 but write 0xC (port 0) / 0xA (port 1)
  there (0xE01D3DCE): same clocks with a different pad setting, most likely 1.8 V vs 3.3 V signalling. NOT a DDR mode.
- SDCON offsets seen through the accessors: 0x10000..0x1003C, 0x10404, 0x1040C (DMA block?), 0x40014, 0x4005C, 0x40068 (+ the agent's
  0x4000C, 0x40020/24, 0x40058/64). Canon never configures DDR, so no code path marks a DDR bit. It cannot be found statically;
  only by trying bits on hardware with a logic analyzer (the agent's step 5). Parked.

## test61 results / test62 (2026-11-19)
- test61 (movie, 2096x1164 C-RAW, ~1.1 MB/frame): 25 fps steady for 30 s, 0 drops after the first second. DMA copy 3-9 ms, 0 spot-check
  failures. Old card 30 MB/s with ~7.4 MB writes (it was 16 MB/s with one write per frame), but only 24.8 MB/s once the ring was nearly
  empty and writes shrank to ~1.6 MB. Clock id 9 (195 MHz) right after boot, without SD195: Canon already runs 195 MHz, so SD195 is moot.
- Startup drops (~20-30) = the first 1-2 encodes take 0.36-0.48 s (AlgsMgr gets its resource).
- test62: encoder warm-up (frames before the 3rd encode aren't written; counters reset after), writer minimum 4 MB / 400 ms,
  centred crop presets (encoder gets addr + y0*pitch + x0*14/8, stride = stream width; x0 on 64 px, y0 on 4 rows) for the 4K movie
  stream 4368x2202 (1:1, known from raw.c: OB 264 left / 34 top). Untested: stride != width, and widths above 3144 (Canon tiles).
  File header h[18..20] = encoded w, h, byte offset.

## test62 crop result / test63 (2026-11-19)
- Decoded on the PC (LibRaw crxLoadRaw, harness /home/claude/work/crxdec): 1080p frames are perfect. The test62 crop frames
  (stride 4368, w 2560) have an mdat header for 2 full-width tiles (4368x2202, 3.33 + 3.31 MB), but the handle only
  delivers 81 pieces = exactly tile 2's size. They decode only after rebuilding the header, and the bottom ~18% is garbage.
  -> Canon's encoder needs stride == width.
- test63: the encoder always gets full-width rows. The top optical black is always cut (y0 = ay1 rounded up to 4 rows: 1080p
  1128 rows, 4K 2160 rows), and presets cut only the height. File header h[21] = final width; the converter
  (crx2video) cuts the sides; h[10] = 0, h[12] = encoded height.
- PC converter: /home/claude/out/crx2video_win.zip (crx2video.exe built with zig for x86_64-windows-gnu with LibRaw,
  get_ffmpeg.bat, crx2video.bat). Real-time playback from the frame timestamps.

## test64 (2026-11-19): sensor row window for crops
- The test63 height crop decoded clean on the user's PC (4368x1440, 0 undecodable). That withdrew my wrong "encoder ignores w/h"
  claim: only stride != width breaks it.
- test64: choosing a Crop preset in 4K movie mode (sensor mode 12) also narrows the sensor row window, 0x68 in this mode's 3-4 list
  (sensor_tune.c m50_sensor_crop_set), centred, to the preset height. It applies after a mode-dial cycle. The recorder then encodes
  the top srows rows (where the read rows land; seen in 1080p as a black bar at the bottom). Untested in mode 12; if the image lands
  elsewhere the encoded rows are black. Code room left: 0xB7 bytes before .bss moves (0x11FF49).

## test64/65 results, test66 (2026-11-19)
- test65 sensor row window in mode 12 WORKS (live view black bar at the bottom; the encoded 1440 rows are all image). Frame size is
  unchanged vs test63, because test63 already encoded only 1440 rows. My "half size" prediction was wrong. Gain: less rolling shutter.
- Recorder timestamps: warm-up reset rec_t0_us -> the first frame was stamped 1-3 s later than the second -> the converter's uint32
  difference wrapped (1:12:02 video). Fixed in both: recorder test65 keeps t0, converter uses a robust Clock.
- test66: horizontal sensor window for crops. HMD 0x6D..0x74 in DEP_LV + 3-4 of mode 12, span = (A-66)*4/3, centred on 0x1090,
  A = crop width + 64. Expected: the image is left-aligned after the 264 OB columns and the right part is black/blank (cheap to encode).
  Header h[22] = A; the converter crops inside [ax, ax+A). raw_probe "Dump RAW frame" removed for code room.

## test66 result + static follow-up (2026-11-19): column window
- test66 (HMD narrowed to span 3408 centred, in both lists that hold mode 12's 0x60C/0x1B14: CMOS +0x2C7C (DEP_LV 6) and +0x3310
  (3-4 #9)): no effect. Live view and the recording are full width; the row window in the same lists did work.
- The only HMD writer in code is 0xE08343CA (asserts pCMOS3_4Ptr[...R_HMD2..9_INDEX]); its only caller is 0xE0834604, and only for
  LVSR index 10 (= zoom, mode 13). It copies HMD pairs from ROM 0xE0B3AA54 by MagIndexH (left/right). So nothing rewrites HMD in mode 12.
- Canon itself only ever shifts the 1:1 window (zoom: 0x10C..0x160B or 0xB15..0x2014, span 5375 ~ the 4K span 5384) and never
  changes its span. Hypothesis: the span is tied to the readout/ADC timing (line length 0x1C5 etc.) and is not a free crop.
- The capture size comes from the processor side (SapPath geometry table 0xE0F7CDA8: path 1 = 4368x2202, path 4 = 4288x1202 zoom);
  engio D04061B8 = per-mode line count (0x89A = 2202, 0x48C = 1164, 0xFDC = 4060). No per-mode width register found in engio.
- Discriminating test (not built): in mode 12 SHIFT the window like zoom does (same span) instead of narrowing it. If live view
  shifts, HMD works in mode 12 and only the span is fixed; if not, HMD is ignored in mode 12.

## test67 (2026-11-19): real crop = DMA pre-crop into a compact frame
- test66 result: the sensor column window (HMD 0x6D..0x74) has no effect in mode 12 -> reverted sensor_tune.c to the row window only.
- New: crop rectangle (x0 centred, 64 px aligned; y0 centred or top when the sensor row window is on, 4-row aligned) is copied
  row by row into pc_buf[e] (2 buffers carved from RecWork before the ring), then encoded with w = stride = crop width.
- DMA: one Xdmac channel per frame (0xE02E035A / 0xE02E0684). Jobs {ch, desc*, n<=8} with 16-byte descriptors {dst, src, len, 0}
  run synchronously by 0xE014585A(jobs, 1, 0) (verified: 0xE0145C82 walks desc + i*16; 0xE0145CF8 asserts n <= 8).
  Return != 0 or a spot-check mismatch (odd row, first/last 64 bytes) -> CPU copy (uncached, slow) for the rest of the take.
- Header: h[9]=0, h[11]=enc_w, h[21]=0 when pre-cropped, so the converter needs no change. Photo mode (no RecWork): crop off.
- Open questions: does the encoder accept narrow widths with stride == w (test62 only tested stride != w); copy time per frame.

## test67 results (CRXREC13/14) -> test68
- The encoder accepts the narrow frame: 2560x1440, stride == w, 1 tile / 81 entries, 0 encode errors. Not yet decoded on the PC.
- The pre-crop is the bottleneck: DMA 1 channel, 8 rows per job = 40-56 ms per frame (~35 us per row descriptor; bandwidth is
  not the limit: whole 33 KB pieces take ~90 us). The encoder thread does crop + encode serially -> ~12.5 fps.
- CRXREC14: one spot-check mismatch -> uncached CPU copy = 270 ms per frame (3.4 fps), and the copier slowed down too (bus).
- Frame size on that scene: 2.68 MB -> 25 fps would need ~67 MB/s; the old card did 26 MB/s.
- test68: the pre-crop times three methods on the 3 warm-up frames (DMA 1 channel, DMA up to 4 channels with 8 descriptors per call,
  CPU with cached reads/writes plus a whole-L1 set/way clean) and keeps the fastest one that copied correctly. A mismatch where the
  source row changed under the copy counts as "source-changed", not as a bad copy. The old full-sensor bin recorder
  (raw_bin.c, 3 tasks) is replaced by stubs (bin_stub.c) for code room.

## test68 crash -> test69
- CRASH00.LOG: ASSERT Clkgen/ClkgenGearApi.c:852 in interrupt 0x13E at 0xE0145A9C (Xdmac completion handler, clock-gear call
  0xE0796C16). Cause: method 1 put several jobs (channels) into ONE 0xE014585A call - one clock "on" per call, one "off" per channel.
  Rule: one job per call, always. HPCopy-style single-job calls from different tasks at the same time are fine (test67: crop + copier).
- test69 method 1 = encoder task + helper task "crx_pc2" (core 1, stack 0x1400: 0xE0145CF8 alone uses 0xA94 of stack), each with
  its own channel, alternating 8-row blocks.

## test69 results -> test70: the crop as one PL330 program
- CRXREC2 (crop): DMA per row 39 ms in warm-up (ok); "DMA 2 tasks" BAD (second task most likely got no free channel -> CPU redo,
  114 ms). Recording stopped after 3.9 s with 0 frames, reason not logged. CRXREC1 (full 4K, crop off): log ends after warm-up,
  CRASH00 = ASSERT ImageController.c:905 in INT 0x130 (LV image path). Full 4K C-RAW frames on that scene: 6.7 MB (~170 MB/s at 25 fps).
- Xdmac = ARM PL330. Canon's emitters: 0xE00F5B7C DMAMOV (0xBC), 0xE00F5BEE WMB (0x13), 0xE00F5BFC RMB, 0xE00F5C0A SEV (0x34),
  0xE00F5C1E WFP, 0xE00F5C32 FLUSHP, 0xE00F5B3C END. Per descriptor 0xE0095BF2: MOV SAR/DAR, chunks (0xE0095B20 plan: 8-byte beats,
  burst 16 by default; 0xE00950F4 emits CCR + loops), WMB. Epilogue 0xE0095C8C: SEV if the channel's mask bit is set, END.
- Run path for one job: sem *(0x572DC + 4*ch) taken (0xE0577C5C); 0xE0145BFC records the job (cache ops on its descriptors);
  0xE0796A16/AA0/BE0; 0xE0095CB0(ch, prog, 0) = DMAGO via debug regs; 0xE0145BA2 waits for the event; the interrupt handler
  (0xE0145A28..) invalidates the recorded dst ranges, gives the semaphore (0xE0577CD6) and sets the event (0xE05534D8).
- Canon's planner does not look at address alignment (sizes from len only), so its rows already use full bursts. The ~27 us per
  row is probably the unaligned source (pitch 7644 = 4 mod 16) or per-descriptor overhead: test70 logs a timing-only run with
  32-byte aligned rows to tell which.
- test70 method 1: own program (CCR 8-byte beats, burst 16, prot 0x1C00700; lc0 bursts per row, tail burst, ADDH SAR gap; rows in
  blocks of <= 256 on lc1), 130-151 bytes, verified for all 4 presets in a PL330 simulator (work/plsim_test70.py).
  Spot check now covers 4 consecutive rows (all 4 source alignments).
