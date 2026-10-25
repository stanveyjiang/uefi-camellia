# camellia (MT6833) bring-up

> **Work in progress. Do not treat this branch as a release firmware.**

This package is derived from `lancelotPkg` and `MT6789Pkg`. It is the compileable
starting point for the Redmi Note 10 5G (`camellia`, MediaTek MT6833 /
Dimensity 700).

## Current status

The Linux hot-jump path is hardware validated:

```text
LK
-> project Linux kernel
-> minimal initramfs
-> linear DRM framebuffer
-> uefi_jump.ko
-> UEFI
-> UniXNU
-> DarwinBoot
-> XNU entry
```

The UEFI firmware, scatter-page framebuffer shadow, RamDisk payload and
UniXNU menu work on hardware. DarwinBoot reaches the MMU-off XNU entry.
XNU currently stops in its first bootstrap page-table mapping; this branch is
therefore a development snapshot rather than a bootable release.

## Known hardware values

```text
SoC                 MT6833
CPU                 6 x Cortex-A55 + 2 x Cortex-A76
GICD                0x0C000000
GICR                0x0C040000
UART                0x11002000
Pinctrl             0x10005000
UFS HCI             0x11270000
UFS PHY             0x11FA0000
Display subsystem   0x14000000
DSI0                0x14013000
Frame buffer        0x7BA10000
Frame buffer size   0x01DE2000
Frame buffer mode   1088 x 7200 x 32 bpp (three 1088 x 2400 buffers)
Pixel format        B,G,R,X (GOP PixelBlueGreenRedReserved8BitPerPixel)
```

The boot image configuration uses the measured stock values:

```text
base           = 0x40000000
kernel_offset  = 0x00080000
ramdisk_offset = 0x11100000
dtb_offset     = 0x07C80000
tags_offset    = 0x07C80000
header_version = 2
```

## Explicit placeholders

The following items are intentionally incomplete and must not be treated as
hardware support:

1. `Resources/DTBs/camellia.dtb` is the unmodified FDT extracted from this
   device's official V13.0.9.0 boot image. LK runtime fixups still need to be
   verified on hardware.
2. ACPI still uses the lancelot placeholder table.
3. `MemoryMapLib` still carries lancelot reservations and register windows.
4. `ClockImplLib`, `GpioImplLib`, `PmicWrapperImplLib` and `MsdcImplLib` are
   copied from MT6789 and need MT6833/MT6359 implementation work.
5. `PcdStorageIsEMMC` is disabled because camellia uses UFS 2.2. There is no
   UFS DXE yet.
6. `SimpleFbDxe` uses the page map produced by `uefi_jump`. It provides a
   linear shadow to GOP consumers and copies completed updates into the
   scattered scanout pages.

## Next milestone

- Diagnose the XNU bootstrap page-table loop without bypassing mappings.
- Confirm runtime boot args, fixed-window ownership and MMU handoff.
- Reach XNU `arm_init`, then add the platform and display drivers.
- Remove all remaining bring-up diagnostics before packaging a release image.
