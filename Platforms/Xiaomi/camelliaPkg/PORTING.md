# camellia (MT6833) bring-up skeleton

> **Not hardware validated. Do not flash this firmware yet.**

This package is derived from `lancelotPkg` and `MT6789Pkg`. It is the compileable
starting point for the Redmi Note 10 5G (`camellia`, MediaTek MT6833 /
Dimensity 700). The first milestone is only to produce an FD and inspect its
size and contents.

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
6. `SimpleFbDxe` cannot be assumed to produce a visible screen: L0 proved that
   the LK framebuffer is not still scanned after handoff. A real DisplayDxe is
   required.

## First milestone

- Probe includes a camellia package and MT6833 package.
- Generate `Build/camelliaPkg/{DEBUG,RELEASE}_*/FV/SILICIUM_UEFI.fd`.
- Inspect FV contents and confirm the image fits its intended window.
- Do not create a flashable boot image until the DTB, memory map and display
  path have real camellia data.
