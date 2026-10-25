#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""bootimg-info.py -- print the header of an Android boot / vendor_boot image.

Pure standard library, no dependencies. Works on Windows, Linux and macOS.

Usage:
    python bootimg-info.py boot.img
    python bootimg-info.py boot.img --extract out_dir

Supported:
    * "ANDROID!" boot image, header versions 0, 1, 2, 3, 4
    * "VNDRBOOT" vendor_boot image, header versions 3, 4

Why this exists: `unpack_bootimg` needs an AOSP checkout. This script only
needs any Python 3, which is enough to answer the questions that matter here:
which header version, what page size, what cmdline, what offsets, is there a
dtb, and how big is each segment.
"""

import argparse
import hashlib
import os
import struct
import sys

BOOT_MAGIC = b"ANDROID!"
VENDOR_MAGIC = b"VNDRBOOT"


def u32(buf, off):
    return struct.unpack_from("<I", buf, off)[0]


def u64(buf, off):
    return struct.unpack_from("<Q", buf, off)[0]


def cstr(buf, off, size):
    raw = buf[off:off + size]
    return raw.split(b"\x00", 1)[0].decode("utf-8", "replace").strip()


def round_up(value, page):
    return (value + page - 1) // page * page


def decode_os_version(value):
    major = (value >> 14) & 0x7F
    minor = (value >> 7) & 0x7F
    patch = value & 0x7F
    year = ((value >> 25) & 0x7F) + 2000
    month = (value >> 21) & 0xF
    day = (value >> 16) & 0x1F
    return "Android %d.%d.%d (patch %04d-%02d-%02d)" % (major, minor, patch, year, month, day)


def field(label, value):
    print("  %-24s %s" % (label + ":", value))


def parse_boot(buf, args):
    raw_version = u32(buf, 40)
    page_at_36 = u32(buf, 36)
    header_size_v34 = u32(buf, 20)

    manifest = []
    if raw_version >= 3:
        version = raw_version
        page_size = 4096
        kernel_size = u32(buf, 8)
        ramdisk_size = u32(buf, 12)
        os_version = u32(buf, 16)
        header_size = header_size_v34
        cmdline = cstr(buf, 44, 1536)
        name = ""
        kernel_addr = ramdisk_addr = tags_addr = second_size = dtb_size = 0
        dtb_addr = 0
        sig_size = u32(buf, 1580) if version >= 4 and len(buf) >= 1584 else 0
        kernel_off = 4096
        ramdisk_off = kernel_off + round_up(kernel_size, 4096)
        second_off = None
        dtb_off = None
        sig_off = ramdisk_off + round_up(ramdisk_size, 4096) if sig_size else None
    else:
        page_size = page_at_36
        if page_size not in (2048, 4096, 8192, 16384, 32768, 65536):
            page_size = 2048
        kernel_size = u32(buf, 8)
        kernel_addr = u32(buf, 12)
        ramdisk_size = u32(buf, 16)
        ramdisk_addr = u32(buf, 20)
        second_size = u32(buf, 24)
        tags_addr = u32(buf, 32)
        os_version = u32(buf, 44)
        name = cstr(buf, 48, 16)
        cmdline = cstr(buf, 64, 512)
        extra = cstr(buf, 608, 1024)
        if extra:
            cmdline = (cmdline + " " + extra).strip()
        version = raw_version if raw_version <= 2 else 0

        off = 1632
        recovery_dtbo_size = 0
        recovery_dtbo_offset = 0
        header_size = 1632
        dtb_size = 0
        dtb_addr = 0
        if version >= 1 and len(buf) >= off + 16:
            recovery_dtbo_size = u32(buf, off)
            recovery_dtbo_offset = u64(buf, off + 4)
            header_size = u32(buf, off + 12)
            off += 16
        if version >= 2 and len(buf) >= off + 12:
            dtb_size = u32(buf, off)
            dtb_addr = u64(buf, off + 4)
            off += 12
        sig_size = 0

        kernel_off = round_up(header_size, page_size)
        ramdisk_off = kernel_off + round_up(kernel_size, page_size)
        second_off = ramdisk_off + round_up(ramdisk_size, page_size) if second_size else None
        dtb_off = None
        if dtb_size:
            base = ramdisk_off + round_up(ramdisk_size, page_size)
            if second_size:
                base += round_up(second_size, page_size)
            if recovery_dtbo_size:
                base += round_up(recovery_dtbo_size, page_size)
            dtb_off = base
        sig_off = None

    print("== boot image: ANDROID! ==")
    field("header_version", version)
    field("page_size", page_size)
    field("header_size", header_size)
    field("os_version", decode_os_version(os_version))
    field("kernel", "size=%d off=%d addr=0x%x" % (kernel_size, kernel_off, kernel_addr))
    field("ramdisk", "size=%d off=%d addr=0x%x" % (ramdisk_size, ramdisk_off, ramdisk_addr))
    if second_size:
        field("second", "size=%d off=%d" % (second_size, second_off))
    if dtb_size:
        field("dtb", "size=%d off=%d addr=0x%x" % (dtb_size, dtb_off, dtb_addr))
    else:
        field("dtb", "none in boot image")
    if sig_size:
        field("v4 signature", "size=%d off=%d" % (sig_size, sig_off))
    field("name", name or "(empty)")
    field("cmdline", cmdline or "(empty)")
    field("file_size", len(buf))
    field("md5", hashlib.md5(buf).hexdigest())

    if args.extract:
        os.makedirs(args.extract, exist_ok=True)
        blobs = [("kernel", kernel_off, kernel_size), ("ramdisk", ramdisk_off, ramdisk_size)]
        if second_size:
            blobs.append(("second", second_off, second_size))
        if dtb_size:
            blobs.append(("dtb", dtb_off, dtb_size))
        for label, off, size in blobs:
            path = os.path.join(args.extract, label)
            with open(path, "wb") as handle:
                handle.write(buf[off:off + size])
            print("  wrote %-10s %s (%d bytes)" % (label, path, size))
        manifest = blobs

    print()
    print("  hint: kernel is a raw Image / gzip / lz4 payload; check the first")
    print("        bytes with: xxd -l 16 <kernel>")
    return manifest


def parse_vendor_boot(buf, args):
    version = u32(buf, 8)
    page_size = u32(buf, 12)
    kernel_addr = u32(buf, 16)
    ramdisk_addr = u32(buf, 20)
    vendor_ramdisk_size = u32(buf, 24)
    cmdline = cstr(buf, 28, 2048)
    tags_addr = u32(buf, 2076)
    name = cstr(buf, 2080, 16)
    header_size = u32(buf, 2096)
    dtb_size = u32(buf, 2100)
    dtb_addr = u64(buf, 2104)

    print("== vendor_boot image: VNDRBOOT ==")
    field("header_version", version)
    field("page_size", page_size)
    field("header_size", header_size)
    field("vendor_ramdisk", "size=%d" % vendor_ramdisk_size)
    field("kernel_addr", "0x%x" % kernel_addr)
    field("ramdisk_addr", "0x%x" % ramdisk_addr)
    field("tags_addr", "0x%x" % tags_addr)
    field("dtb", "size=%d addr=0x%x" % (dtb_size, dtb_addr))
    field("name", name or "(empty)")
    field("cmdline", cmdline or "(empty)")
    field("file_size", len(buf))
    field("md5", hashlib.md5(buf).hexdigest())

    ramdisk_off = round_up(header_size, page_size)
    dtb_off = round_up(ramdisk_off + vendor_ramdisk_size, page_size) if dtb_size else None

    if args.extract:
        os.makedirs(args.extract, exist_ok=True)
        blobs = [("vendor_ramdisk", ramdisk_off, vendor_ramdisk_size)]
        if dtb_size:
            blobs.append(("dtb", dtb_off, dtb_size))
        for label, off, size in blobs:
            path = os.path.join(args.extract, label)
            with open(path, "wb") as handle:
                handle.write(buf[off:off + size])
            print("  wrote %-15s %s (%d bytes)" % (label, path, size))


def main():
    parser = argparse.ArgumentParser(description="Print Android boot / vendor_boot headers.")
    parser.add_argument("image")
    parser.add_argument("--extract", metavar="DIR", help="write kernel/ramdisk/dtb into DIR")
    args = parser.parse_args()

    with open(args.image, "rb") as handle:
        buf = handle.read()

    magic = buf[:8]
    if magic == BOOT_MAGIC:
        parse_boot(buf, args)
    elif magic == VENDOR_MAGIC:
        parse_vendor_boot(buf, args)
    else:
        print("not a boot/vendor_boot image: magic = %r" % magic)
        print("first 64 bytes:", buf[:64].hex(" "))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
