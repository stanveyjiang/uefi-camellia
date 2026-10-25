#!/usr/bin/env python3
"""
patch_versions.py - zero the ELF __versions section of a built .ko.

The stock kernel is a different source snapshot than the local tree, so the
module_layout CRC cannot match and CONFIG_MODULE_FORCE_LOAD is off, which
means MODULE_INIT_IGNORE_* is ineffective. Instead, we empty __versions:
kernel/module.c check_version() then reports "no symbol version for X" once
and passes, while same_magic() still matches the vermagic tail.

Usage: patch_versions.py module.ko [out.ko]
"""
import struct
import sys


def main(argv):
    if len(argv) < 2:
        sys.stderr.write("usage: %s module.ko [out.ko]\n" % argv[0])
        return 2

    src = argv[1]
    dst = argv[2] if len(argv) > 2 else src

    with open(src, "rb") as f:
        data = bytearray(f.read())

    assert data[:4] == b"\x7fELF", "not an ELF file"
    is64 = data[4] == 2
    isle = data[5] == 1
    endian = "<" if isle else ">"

    if is64:
        # Elf64_Ehdr layout: e_shoff at 0x28 (u64), e_shentsize 0x3a (u16),
        # e_shnum 0x3c (u16), e_shstrndx 0x3e (u16)
        shoff = struct.unpack_from(endian + "Q", data, 0x28)[0]
        shentsize = struct.unpack_from(endian + "H", data, 0x3A)[0]
        shnum = struct.unpack_from(endian + "H", data, 0x3C)[0]
        shstrndx = struct.unpack_from(endian + "H", data, 0x3E)[0]
        # Elf64_Shdr: sh_name 0x00 (u32), sh_type 0x04 (u32), sh_flags 0x08 (u64),
        # sh_addr 0x10 (u64), sh_offset 0x18 (u64), sh_size 0x20 (u64)
        S_NAME, S_TYPE, S_FLAGS = 0x00, 0x04, 0x08
        S_ADDR, S_OFFSET, S_SIZE = 0x10, 0x18, 0x20
    else:
        shoff = struct.unpack_from(endian + "I", data, 0x20)[0]
        shentsize = struct.unpack_from(endian + "H", data, 0x2E)[0]
        shnum = struct.unpack_from(endian + "H", data, 0x30)[0]
        shstrndx = struct.unpack_from(endian + "H", data, 0x32)[0]
        S_NAME, S_TYPE, S_FLAGS = 0x00, 0x04, 0x08
        S_ADDR, S_OFFSET, S_SIZE = 0x0C, 0x10, 0x14

    # Section header string table
    shstr_off = shoff + shstrndx * shentsize
    strtab = data[struct.unpack_from(endian + ("Q" if is64 else "I"),
                                     data, shstr_off + S_OFFSET)[0]:]

    def sh_name(off):
        end = strtab.find(b"\x00", off)
        return strtab[off:end].decode("ascii", "replace")

    found = 0
    for i in range(shnum):
        ent = shoff + i * shentsize
        name_off = struct.unpack_from(endian + "I", data, ent + S_NAME)[0]
        name = sh_name(name_off)
        if name != "__versions":
            continue
        size_off = ent + S_SIZE
        size = struct.unpack_from(endian + ("Q" if is64 else "I"),
                                  data, size_off)[0]
        data_off = struct.unpack_from(endian + ("Q" if is64 else "I"),
                                      data, ent + S_OFFSET)[0]
        # Zero the section contents; the loader sees num_versions == 0.
        data[data_off:data_off + size] = b"\x00" * size
        found += 1
        print("zeroed __versions: offset=0x%x size=%d" % (data_off, size))

    if not found:
        sys.stderr.write("no __versions section found\n")
        return 1

    with open(dst, "wb") as f:
        f.write(data)
    print("wrote %s" % dst)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
