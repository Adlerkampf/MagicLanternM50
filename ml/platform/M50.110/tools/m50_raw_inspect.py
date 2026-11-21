#!/usr/bin/env python3
"""
Inspect a LiveView RAW frame dumped by "Debug > M50 RAW probe > Dump RAW frame".

Input : RAW00000.BIN (+ RAW00000.TXT sidecar written next to it)
Output: <name>_mosaic.pgm   16-bit bayer mosaic, full size
        <name>_preview.ppm  8-bit half-size RGB preview (2x2 binning, RGGB)
        a text report: black level, clipping, and suggested skip_left /
        skip_top for src/raw.c (CONFIG_M50 block), found by locating where
        the dark optical-black border ends.

What a GOOD dump looks like:
  * the preview shows the scene (colours may be off - no white balance)
  * black ~ 2048 (14-bit Canon), white near 16383 when overexposed
  * a dark band on the left and/or top = optical black
What a BAD dump looks like and what it means:
  * pure noise / stripes        -> wrong bpp, or pitch != width*bpp/8,
                                   or the port is not the RAW port (YUV?)
  * image repeated 2-3x across  -> width too small (try --width)
  * all zeros                   -> stream not running or wrong ram_addr

Usage:
  python3 m50_raw_inspect.py ML/LOGS/RAW00000.BIN
  python3 m50_raw_inspect.py dump.bin --width 1920 --height 1080 --bpp 14
  python3 m50_raw_inspect.py --selftest
"""
import argparse
import os
import sys

import numpy as np


def read_sidecar(bin_path):
    txt = os.path.splitext(bin_path)[0] + ".TXT"
    if not os.path.exists(txt):
        txt = os.path.splitext(bin_path)[0] + ".txt"
    meta = {}
    if os.path.exists(txt):
        for line in open(txt, encoding="ascii", errors="replace"):
            for part in line.split():
                if "=" in part:
                    k, v = part.split("=", 1)
                    meta[k.strip()] = v.strip()
    return meta


def unpack(data, width, height, pitch, bpp):
    """Canon/ML LiveView packing: 16-bit little-endian words forming a
    big-endian bitstream (see src/raw.h). Works for 10/12/14 bpp."""
    need = pitch * height
    if len(data) < need:
        raise ValueError(f"file has {len(data)} bytes, need pitch*height = {need}")
    rows = np.frombuffer(data[:need], dtype=np.uint8).reshape(height, pitch)
    if pitch % 2:
        raise ValueError("pitch must be even (16-bit words)")
    # byte-swap each 16-bit word -> plain big-endian bitstream
    swapped = rows.reshape(height, pitch // 2, 2)[:, :, ::-1].reshape(height, pitch)
    bits = np.unpackbits(swapped, axis=1)  # MSB first
    usable = (bits.shape[1] // bpp)
    if width > usable:
        raise ValueError(f"width {width} needs {width * bpp} bits/line, pitch gives {bits.shape[1]}")
    bits = bits[:, : width * bpp].reshape(height, width, bpp).astype(np.uint16)
    weights = (1 << np.arange(bpp - 1, -1, -1)).astype(np.uint16)
    return (bits * weights).sum(axis=2).astype(np.uint16)


def pack(img, bpp):
    """Inverse of unpack(), for the self-test."""
    h, w = img.shape
    bits = ((img[:, :, None] >> np.arange(bpp - 1, -1, -1)) & 1).astype(np.uint8)
    bits = bits.reshape(h, w * bpp)
    pad = (-bits.shape[1]) % 16
    if pad:
        bits = np.pad(bits, ((0, 0), (0, pad)))
    be = np.packbits(bits, axis=1)
    return be.reshape(h, -1, 2)[:, :, ::-1].reshape(h, -1).tobytes()


def find_border(profile, black_guess=None):
    """Index where the dark optical-black border ends: first position whose
    mean leaves the border's noise band. Returns 0 if there is no border."""
    n = len(profile)
    edge = profile[: max(8, n // 50)]
    border = float(np.median(edge))
    noise = float(np.std(edge))
    scene = float(np.median(profile))
    if scene - border < 64:  # no visible optical-black band
        return 0, border
    thresh = border + max(32.0, 6.0 * noise)
    idx = int(np.argmax(profile > thresh))
    return idx, border


def write_pgm16(path, img):
    h, w = img.shape
    with open(path, "wb") as f:
        f.write(f"P5\n{w} {h}\n65535\n".encode())
        f.write(img.astype(">u2").tobytes())


def write_preview(path, img, black, white):
    h, w = img.shape
    h2, w2 = h // 2, w // 2
    m = img[: h2 * 2, : w2 * 2].astype(np.float32)
    r = m[0::2, 0::2]
    g = (m[0::2, 1::2] + m[1::2, 0::2]) / 2
    b = m[1::2, 1::2]
    rgb = np.stack([r, g, b], axis=2)
    rgb = np.clip((rgb - black) / max(1.0, white - black), 0, 1) ** (1 / 2.2)
    with open(path, "wb") as f:
        f.write(f"P6\n{w2} {h2}\n255\n".encode())
        f.write((rgb * 255).astype(np.uint8).tobytes())


def analyse(img, bpp):
    maxv = (1 << bpp) - 1
    col = img.mean(axis=0)
    row = img.mean(axis=1)
    skip_left, black_l = find_border(col, None)
    skip_top, black_t = find_border(row, None)
    # black level: from whichever border exists, else 1st percentile
    if skip_left > 0:
        black = float(np.median(img[:, : max(1, skip_left - 2)]))
    elif skip_top > 0:
        black = float(np.median(img[: max(1, skip_top - 2), :]))
    else:
        black = float(np.percentile(img, 1))
    white = float(img.max())
    clipped = float((img >= maxv - 16).mean() * 100)
    # round skips up to raw.c-friendly values (even for bayer, 8 for pixblocks)
    return {
        "black": black,
        "white": white,
        "clipped_pct": clipped,
        "skip_left": (skip_left + 7) // 8 * 8,
        "skip_top": (skip_top + 1) // 2 * 2,
        "border_left_raw": skip_left,
        "border_top_raw": skip_top,
        "noise_like": bool(np.std(np.diff(img.astype(np.int32), axis=1)) > 0.25 * maxv),
    }


def selftest():
    rng = np.random.default_rng(1)
    for bpp in (10, 12, 14):
        w, h = 64, 16
        img = rng.integers(0, 1 << bpp, size=(h, w), dtype=np.uint16)
        data = pack(img, bpp)
        pitch = len(data) // h
        out = unpack(data, w, h, pitch, bpp)
        assert np.array_equal(img, out), f"roundtrip failed at {bpp} bpp"
    # synthetic frame with optical black: left 72 px, top 20 px
    bpp, w, h = 14, 1024, 256
    img = np.full((h, w), 2048, dtype=np.uint16)
    img[20:, 72:] = 2048 + rng.integers(1000, 9000, size=(h - 20, w - 72))
    a = analyse(img, bpp)
    assert a["border_left_raw"] == 72 and a["border_top_raw"] == 20, a
    assert abs(a["black"] - 2048) < 1, a
    print("selftest OK (pack/unpack 10/12/14 bpp, border + black detection)")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("bin", nargs="?")
    ap.add_argument("--width", type=int)
    ap.add_argument("--height", type=int)
    ap.add_argument("--pitch", type=int, help="bytes per line (default from sidecar or width*bpp/8)")
    ap.add_argument("--bpp", type=int)
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()

    if a.selftest:
        selftest()
        return
    if not a.bin:
        ap.error("give a .BIN dump or --selftest")

    meta = read_sidecar(a.bin)
    bpp = a.bpp or int(meta.get("bpp", 14))
    width = a.width or int(meta.get("width", 0))
    height = a.height or int(meta.get("height", 0))
    pitch = a.pitch or int(meta.get("pitch", 0)) or width * bpp // 8
    data = open(a.bin, "rb").read()
    if not width:
        sys.exit("width unknown: no sidecar .TXT and no --width")
    if not height:
        height = len(data) // pitch

    img = unpack(data, width, height, pitch, bpp)
    r = analyse(img, bpp)
    base = os.path.splitext(a.bin)[0]
    write_pgm16(base + "_mosaic.pgm", img)
    write_preview(base + "_preview.ppm", img, r["black"], max(r["white"], r["black"] + 1))

    print(f"frame      : {width} x {height}, {bpp} bpp, pitch {pitch}")
    if meta:
        print(f"camera says: black={meta.get('black')} white={meta.get('white')} active={meta.get('active')}")
    print(f"measured   : black~{r['black']:.0f} max={r['white']:.0f} clipped={r['clipped_pct']:.2f}%")
    print(f"opt. black : left {r['border_left_raw']} px, top {r['border_top_raw']} px")
    print(f"raw.c      : skip_left = {r['skip_left']}; skip_top = {r['skip_top']};  (CONFIG_M50 block)")
    if r["noise_like"]:
        print("WARNING    : neighbouring pixels look uncorrelated - wrong bpp/pitch, or not a bayer stream")
    print(f"wrote      : {base}_mosaic.pgm, {base}_preview.ppm")


if __name__ == "__main__":
    main()
