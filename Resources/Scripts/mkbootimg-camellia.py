#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""mkbootimg.py -- pack an Android boot image (header v2) for camellia.

Pure standard library, no deps. Runs on Windows, Linux and macOS.

Why not AOSP mkbootimg: we need the exact camellia geometry, a --verify mode
that re-parses what it just wrote, and no AOSP checkout.

Measured geometry (docs/camellia-hardware.md section 5b):
    header_version 2, page_size 2048,
    base 0x40000000, kernel_offset 0x00080000 -> kernel at 0x40080000
    ramdisk_offset 0x11100000 -> ramdisk at 0x51100000
    tags_offset 0x07c80000, dtb_offset 0x07c80000
    cmdline "bootopt=64S3,32N2,64N2"

Usage:
    python mkbootimg.py --kernel probe.bin --output probe.img
    python mkbootimg.py --kernel probe.bin --output probe.img --verify
    python mkbootimg.py --kernel k --ramdisk r --dtb d --output boot.img --verify
"""

import argparse
import hashlib
import struct
import sys

PAGE_SIZE = 2048
BASE = 0x40000000
KERNEL_OFFSET = 0x00080000
RAMDISK_OFFSET = 0x11100000
TAGS_OFFSET = 0x07C80000
DTB_OFFSET = 0x07C80000
CMDLINE = "bootopt=64S3,32N2,64N2"
HEADER_VERSION = 2

V0_SIZE = 1632
V1_SIZE = V0_SIZE + 16          # recovery_dtbo
V2_SIZE = V1_SIZE + 12          # dtb_size + dtb_addr
HEADER_SIZE = V2_SIZE

MAGIC = b"ANDROID!"


def pad(data, page=PAGE_SIZE):
    remainder = len(data) % page
    return data if remainder == 0 else data + b"\x00" * (page - remainder)


def compute_id(kernel, ramdisk, second, recovery_dtbo, dtb):
    """AOSP's id[8]: sha1 over each segment followed by its length, zero padded."""
    sha = hashlib.sha1()
    for blob in (kernel, ramdisk, second, recovery_dtbo, dtb):
        sha.update(blob)
        sha.update(struct.pack("<I", len(blob)))
    raw = sha.digest().ljust(32, b"\x00")
    return list(struct.unpack("<8I", raw))


def build_header(kernel, ramdisk, dtb, base, kernel_offset,
                 ramdisk_offset, tags_offset, dtb_offset, page_size, cmdline,
                 os_version=0):
    kernel_size = len(kernel)
    ramdisk_size = len(ramdisk)
    dtb_size = len(dtb)
    kernel_addr = base + kernel_offset
    ramdisk_addr = base + ramdisk_offset
    tags_addr = base + tags_offset
    dtb_addr = base + dtb_offset

    header = bytearray(HEADER_SIZE)
    header[0:8] = MAGIC
    struct.pack_into("<I", header, 8, kernel_size)
    struct.pack_into("<I", header, 12, kernel_addr)
    struct.pack_into("<I", header, 16, ramdisk_size)
    struct.pack_into("<I", header, 20, ramdisk_addr)
    struct.pack_into("<I", header, 24, 0)             # second_size
    struct.pack_into("<I", header, 28, 0)             # second_addr
    struct.pack_into("<I", header, 32, tags_addr)
    struct.pack_into("<I", header, 36, page_size)
    struct.pack_into("<I", header, 40, HEADER_VERSION)  # v1/v2: header_version
    struct.pack_into("<I", header, 44, os_version)
    header[48:48 + 16] = b"".ljust(16, b"\x00")       # name
    header[64:64 + 512] = cmdline.encode("ascii")[:511].ljust(512, b"\x00")
    image_id = compute_id(kernel, ramdisk, b"", b"", dtb)
    for index, word in enumerate(image_id):
        struct.pack_into("<I", header, 576 + index * 4, word)
    # extra_cmdline stays zero
    # v1 fields
    struct.pack_into("<I", header, V0_SIZE, 0)             # recovery_dtbo_size
    struct.pack_into("<Q", header, V0_SIZE + 4, 0)         # recovery_dtbo_offset
    struct.pack_into("<I", header, V0_SIZE + 12, HEADER_SIZE)  # header_size
    # v2 fields
    struct.pack_into("<I", header, V1_SIZE, dtb_size)
    struct.pack_into("<Q", header, V1_SIZE + 4, dtb_addr)
    return bytes(header)


def pack(kernel, ramdisk, dtb, args):
    header = build_header(
        kernel, ramdisk, dtb,
        args.base, args.kernel_offset, args.ramdisk_offset,
        args.tags_offset, args.dtb_offset, args.page_size, args.cmdline,
        args.os_version)

    out = bytearray()
    out += pad(header, args.page_size)
    out += pad(kernel, args.page_size)
    out += pad(ramdisk, args.page_size) if ramdisk else b""
    if dtb:
        out += pad(dtb, args.page_size)
    return bytes(out)


def parse(image):
    """Minimal parser used by --verify."""
    info = {}
    if image[:8] != MAGIC:
        raise ValueError("bad magic: %r" % image[:8])
    info["kernel_size"] = struct.unpack_from("<I", image, 8)[0]
    info["kernel_addr"] = struct.unpack_from("<I", image, 12)[0]
    info["ramdisk_size"] = struct.unpack_from("<I", image, 16)[0]
    info["ramdisk_addr"] = struct.unpack_from("<I", image, 20)[0]
    info["second_size"] = struct.unpack_from("<I", image, 24)[0]
    info["tags_addr"] = struct.unpack_from("<I", image, 32)[0]
    info["page_size"] = struct.unpack_from("<I", image, 36)[0]
    info["header_version"] = struct.unpack_from("<I", image, 40)[0]
    info["os_version"] = struct.unpack_from("<I", image, 44)[0]
    raw_cmdline = image[64:576]
    info["cmdline"] = raw_cmdline.split(b"\x00", 1)[0].decode("ascii", "replace")
    info["header_size"] = struct.unpack_from("<I", image, V0_SIZE + 12)[0]
    info["dtb_size"] = struct.unpack_from("<I", image, V1_SIZE)[0]
    info["dtb_addr"] = struct.unpack_from("<Q", image, V1_SIZE + 4)[0]

    page = info["page_size"]
    info["kernel_offset"] = page
    info["ramdisk_offset"] = page + ((info["kernel_size"] + page - 1) // page) * page
    info["dtb_offset"] = info["ramdisk_offset"] + ((info["ramdisk_size"] + page - 1) // page) * page
    info["total_size"] = len(image)
    return info


def verify(image, kernel, ramdisk, dtb, args):
    info = parse(image)
    problems = []

    def check(name, got, want):
        if got != want:
            problems.append("%s: got %s, want %s" % (name, got, want))

    check("kernel_size", info["kernel_size"], len(kernel))
    check("ramdisk_size", info["ramdisk_size"], len(ramdisk))
    check("dtb_size", info["dtb_size"], len(dtb))
    check("kernel_addr", info["kernel_addr"], args.base + args.kernel_offset)
    check("ramdisk_addr", info["ramdisk_addr"], args.base + args.ramdisk_offset)
    check("tags_addr", info["tags_addr"], args.base + args.tags_offset)
    check("dtb_addr", info["dtb_addr"], args.base + args.dtb_offset)
    check("page_size", info["page_size"], args.page_size)
    check("header_version", info["header_version"], HEADER_VERSION)
    check("header_size", info["header_size"], HEADER_SIZE)
    check("cmdline", info["cmdline"], args.cmdline)

    # segment bytes must round-trip
    start = info["kernel_offset"]
    if image[start:start + len(kernel)] != kernel:
        problems.append("kernel segment differs")
    if ramdisk:
        start = info["ramdisk_offset"]
        if image[start:start + len(ramdisk)] != ramdisk:
            problems.append("ramdisk segment differs")
    if dtb:
        start = info["dtb_offset"]
        if image[start:start + len(dtb)] != dtb:
            problems.append("dtb segment differs")

    print("== verify ==")
    for key in ("header_version", "header_size", "page_size", "kernel_size",
                "kernel_addr", "ramdisk_size", "ramdisk_addr", "tags_addr",
                "dtb_size", "dtb_addr", "cmdline", "total_size"):
        print("  %-16s %s" % (key, hex(info[key]) if isinstance(info[key], int) and key.endswith("addr") else info[key]))
    if problems:
        print("  RESULT: FAILED")
        for item in problems:
            print("    - " + item)
        return 1
    print("  RESULT: OK")
    return 0


def read(path):
    if path is None or path == "none":
        return b""
    with open(path, "rb") as handle:
        return handle.read()


def main():
    parser = argparse.ArgumentParser(description="Pack a camellia boot image (header v2).")
    parser.add_argument("--kernel", required=True)
    parser.add_argument("--ramdisk", default=None)
    parser.add_argument("--dtb", default=None)
    parser.add_argument("--output", required=True)
    parser.add_argument("--base", type=lambda s: int(s, 0), default=BASE)
    parser.add_argument("--kernel_offset", type=lambda s: int(s, 0), default=KERNEL_OFFSET)
    parser.add_argument("--ramdisk_offset", type=lambda s: int(s, 0), default=RAMDISK_OFFSET)
    parser.add_argument("--tags_offset", type=lambda s: int(s, 0), default=TAGS_OFFSET)
    parser.add_argument("--dtb_offset", type=lambda s: int(s, 0), default=DTB_OFFSET)
    parser.add_argument("--page_size", type=lambda s: int(s, 0), default=PAGE_SIZE)
    parser.add_argument("--cmdline", default=CMDLINE)
    parser.add_argument("--os_version", type=lambda s: int(s, 0), default=0)
    parser.add_argument("--verify", action="store_true")
    args = parser.parse_args()

    kernel = read(args.kernel)
    ramdisk = read(args.ramdisk)
    dtb = read(args.dtb)
    if not kernel:
        print("kernel is empty")
        return 2

    image = pack(kernel, ramdisk, dtb, args)
    with open(args.output, "wb") as handle:
        handle.write(image)
    print("wrote %s (%d bytes, %.1f KiB)" % (args.output, len(image), len(image) / 1024.0))

    if args.verify:
        return verify(image, kernel, ramdisk, dtb, args)
    return 0


if __name__ == "__main__":
    sys.exit(main())
