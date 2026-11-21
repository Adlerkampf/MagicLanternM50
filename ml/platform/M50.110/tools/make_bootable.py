#!/usr/bin/env python3
"""
Mark an SD card partition as bootable for Canon cameras with the boot flag set.

Canon's bootloader runs AUTOEXEC.BIN only if the partition's boot sector
contains the strings "EOS_DEVELOP" and "BOOTDISK" at fixed offsets:
  FAT16: EOS_DEVELOP @ 0x2B, BOOTDISK @ 0x40
  FAT32: EOS_DEVELOP @ 0x47, BOOTDISK @ 0x5C
  exFAT: BOOTDISK @ 0x7A, EOS_DEVELOP @ 0x82 (Main + Backup VBR + VBR checksum)

Files on the card are not touched.

  Windows: python make_bootable.py H:                      (may need Admin)
  Linux  : sudo python3 make_bootable.py /dev/sdX1         (partition!)
  macOS  : diskutil unmountDisk /dev/diskN
           sudo python3 make_bootable.py /dev/rdiskNs1
  Test   : python3 make_bootable.py card.img               (FAT/exFAT image)
  Undo   : add --undo

Canon Basic script card (--script, FAT16/FAT32 only): writes "SCRIPT" at 0x1F0.
"""
import argparse
import os
import struct
import sys

OFFSETS = {
    "FAT16": (0x2B, 0x40),
    "FAT32": (0x47, 0x5C),
    "exFAT": (0x82, 0x7A),
}


def detect(bs):
    if len(bs) < 512 or bs[510:512] != b"\x55\xAA":
        return None
    if bs[0x03:0x0B] == b"EXFAT   ":
        return "exFAT"
    if bs[0x52:0x5A].startswith(b"FAT32"):
        return "FAT32"
    if bs[0x36:0x3E].startswith(b"FAT16") or bs[0x36:0x3E].startswith(b"FAT12"):
        return "FAT16"
    return None


def exfat_vbr_checksum(vbr_11_sectors):
    s = 0
    for i, b in enumerate(vbr_11_sectors):
        if i in (106, 107, 112):
            continue
        s = ((((s & 1) << 31) | (s >> 1)) + b) & 0xFFFFFFFF
    return s


def patch_exfat(raw, undo):
    bps = 1 << raw[108]
    if bps != 512:
        sys.exit(f"Unsupported exFAT sector size: {bps} (expected 512).")
    need = bps * 24
    if len(raw) < need:
        sys.exit(f"Short read: got {len(raw)} bytes, need {need} for exFAT Main+Backup VBR.")

    dev_off, boot_off = OFFSETS["exFAT"]
    dev = b"\x00" * 11 if undo else b"EOS_DEVELOP"
    boot = b"\x00" * 8 if undo else b"BOOTDISK"

    for base in (0, bps * 12):
        old_calc = exfat_vbr_checksum(raw[base:base + bps * 11])
        old_disk = struct.unpack_from("<I", raw, base + bps * 11)[0]
        if old_calc != old_disk:
            sys.exit(f"Existing exFAT VBR checksum mismatch at 0x{base:X}: "
                     f"calc=0x{old_calc:08X} disk=0x{old_disk:08X}. Refusing to modify.")

        raw[base + boot_off:base + boot_off + 8] = boot
        raw[base + dev_off:base + dev_off + 11] = dev
        new_sum = exfat_vbr_checksum(raw[base:base + bps * 11])
        raw[base + bps * 11:base + bps * 12] = struct.pack("<I", new_sum) * (bps // 4)

    return raw


def open_windows_volume(target):
    import ctypes
    from ctypes import wintypes

    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    GENERIC_READ = 0x80000000
    GENERIC_WRITE = 0x40000000
    FILE_SHARE_READ = 0x00000001
    FILE_SHARE_WRITE = 0x00000002
    OPEN_EXISTING = 3
    INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value
    FSCTL_LOCK_VOLUME = 0x00090018
    FSCTL_UNLOCK_VOLUME = 0x0009001C
    FSCTL_DISMOUNT_VOLUME = 0x00090020
    FILE_BEGIN = 0

    k32.CreateFileW.restype = wintypes.HANDLE
    k32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                                wintypes.LPVOID, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    k32.DeviceIoControl.restype = wintypes.BOOL
    k32.DeviceIoControl.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPVOID,
                                    wintypes.DWORD, wintypes.LPVOID, wintypes.DWORD,
                                    ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID]
    k32.ReadFile.restype = wintypes.BOOL
    k32.WriteFile.restype = wintypes.BOOL
    k32.SetFilePointerEx.restype = wintypes.BOOL
    k32.FlushFileBuffers.restype = wintypes.BOOL
    k32.CloseHandle.restype = wintypes.BOOL

    h = k32.CreateFileW(target, GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, None, OPEN_EXISTING, 0, None)
    if h == INVALID_HANDLE_VALUE:
        err = ctypes.get_last_error()
        raise PermissionError(f"CreateFileW({target}) failed (Win32 error {err}). "
                              f"Run from an Administrator terminal.")

    ret = wintypes.DWORD(0)
    if not k32.DeviceIoControl(h, FSCTL_LOCK_VOLUME, None, 0, None, 0, ctypes.byref(ret), None):
        err = ctypes.get_last_error()
        k32.CloseHandle(h)
        raise PermissionError(f"FSCTL_LOCK_VOLUME({target}) failed (Win32 error {err}). "
                              f"Close Explorer windows on {target} and run as Administrator.")
    if not k32.DeviceIoControl(h, FSCTL_DISMOUNT_VOLUME, None, 0, None, 0, ctypes.byref(ret), None):
        err = ctypes.get_last_error()
        k32.DeviceIoControl(h, FSCTL_UNLOCK_VOLUME, None, 0, None, 0, ctypes.byref(ret), None)
        k32.CloseHandle(h)
        raise OSError(f"FSCTL_DISMOUNT_VOLUME({target}) failed (Win32 error {err}).")

    def read_at_0(n):
        pos = ctypes.c_longlong(0)
        k32.SetFilePointerEx(h, pos, None, FILE_BEGIN)
        buf = ctypes.create_string_buffer(n)
        got = wintypes.DWORD(0)
        if not k32.ReadFile(h, buf, n, ctypes.byref(got), None) or got.value != n:
            raise OSError(f"ReadFile failed (err={ctypes.get_last_error()}, got={got.value})")
        return bytearray(buf.raw)

    def write_at_0(data):
        pos = ctypes.c_longlong(0)
        k32.SetFilePointerEx(h, pos, None, FILE_BEGIN)
        buf = ctypes.create_string_buffer(bytes(data), len(data))
        put = wintypes.DWORD(0)
        if not k32.WriteFile(h, buf, len(data), ctypes.byref(put), None) or put.value != len(data):
            raise OSError(f"WriteFile failed (err={ctypes.get_last_error()}, put={put.value})")
        k32.FlushFileBuffers(h)

    def close_vol():
        k32.DeviceIoControl(h, FSCTL_UNLOCK_VOLUME, None, 0, None, 0, ctypes.byref(ret), None)
        k32.CloseHandle(h)

    return read_at_0, write_at_0, close_vol


def normalize_target(p):
    if os.name == "nt":
        s = p.rstrip("\\/")
        if len(s) == 2 and s[1] == ":" and s[0].isalpha():
            return rf"\\.\{s.upper()}"
    return p


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("partition", help="Partition device (e.g. H:, \\\\.\\H:, /dev/sdX1, or image file)")
    ap.add_argument("--undo", action="store_true", help="blank the strings again")
    ap.add_argument("--script", action="store_true", help="mark as Canon Basic script card (SCRIPT @ 0x1F0)")
    a = ap.parse_args()

    target = normalize_target(a.partition)
    if target.rstrip("0123456789") == target and target.startswith("/dev/sd"):
        sys.exit("That looks like a whole disk (/dev/sdX). Give the partition, e.g. /dev/sdX1.")

    use_win_vol = (os.name == "nt" and target.startswith("\\\\.\\"))
    if use_win_vol:
        read_at_0, write_at_0, close_vol = open_windows_volume(target)
    else:
        f = open(target, "r+b", buffering=0)
        def read_at_0(n):
            f.seek(0)
            return bytearray(f.read(n))
        def write_at_0(data):
            f.seek(0)
            f.write(data)
            f.flush()
        def close_vol():
            f.close()

    try:
        bs = read_at_0(512)
        fs = detect(bs)
        if fs is None:
            sys.exit("No FAT/exFAT boot sector found. Wrong device, or you passed the whole disk.")

        if a.script:
            if fs == "exFAT":
                sys.exit("Canon Basic SCRIPT flag is only supported on FAT16/FAT32, not exFAT.")
            bs[0x1F0:0x1F6] = b"\0" * 6 if a.undo else b"SCRIPT"
            write_at_0(bs)
            print(f"{fs}: {'cleared' if a.undo else 'wrote'} SCRIPT@0x1F0")
            return

        if fs == "exFAT":
            raw = read_at_0(512 * 24)
            patch_exfat(raw, a.undo)
            write_at_0(raw)
            verify = read_at_0(512 * 24)
            assert verify == raw, "Read-back verification failed!"
            main_sum = struct.unpack_from("<I", verify, 512 * 11)[0]
            bak_sum = struct.unpack_from("<I", verify, 512 * 23)[0]
            action = "cleared" if a.undo else "wrote"
            print(f"exFAT: {action} BOOTDISK@0x7A, EOS_DEVELOP@0x82 on Main+Backup VBR "
                  f"(csum=0x{main_sum:08X} / 0x{bak_sum:08X}, verified!)")
            return

        dev_off, boot_off = OFFSETS[fs]
        dev = b" " * 11 if a.undo else b"EOS_DEVELOP"
        boot = b" " * 8 if a.undo else b"BOOTDISK"
        bs[dev_off:dev_off + 11] = dev
        bs[boot_off:boot_off + 8] = boot
        write_at_0(bs)
        verify = read_at_0(512)
        assert verify == bs, "Read-back verification failed!"
        print(f"{fs}: {'cleared' if a.undo else 'wrote'} EOS_DEVELOP@0x{dev_off:X}, BOOTDISK@0x{boot_off:X} (verified!)")
    finally:
        close_vol()


if __name__ == "__main__":
    main()
