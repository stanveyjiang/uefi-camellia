# Camellia UEFI platform

This repository is a cleaned source snapshot for the Xiaomi Redmi Note 10 5G
(`camellia`, MediaTek MT6833 / Dimensity 700).

The baseline is the hardware-validated UEFI package from commit
`8221489a628aa47daa0776322ee17ce8dc7a9981`. It deliberately does not include
the later persistent-trace experiments that regressed early UEFI startup.

## Validated scope

- Linux hot-jump into UEFI, when using the matching hot-jump FD base.
- SEC, DXE, GOP and the scatter-page framebuffer shadow.
- RAM-disk publication and the external payload handoff.
- A documented integration point for another XNU/UniXNU/DarwinBoot project.

Known-good diagnostic artifacts:

```text
uefi-hotjump-1gwindow.fd
  size    2097152
  SHA-256 68184f8a1451d08a90fafc61b1163e6e2056469bac174ebc5c71b38f23bfc406
  C entry 0x00000000A0001FC0

uefi_jump-mapdiag.ko
  SHA-256 bec844cf909a8032efe33b58c879ba6ff499036263c824672a47afdc9f3a71c6
```

The binaries are not included in this source repository. They document the
hardware-validated build and are useful for comparing a local rebuild.

## Build dependencies

```bash
git submodule update --init --recursive
./setup_env.sh -p apt
```

Ubuntu/Debian is the tested Linux build host. The build also requires Python
3.11 or newer and clang/lld 18. Later LLVM versions change UEFI code generation
and can produce a firmware image that is not hardware-compatible with this
validated baseline.

## Cold-boot FD

The checked-in configuration builds at the cold-boot address
`0x40080000`:

```bash
./build-camellia-coldboot.sh
```

Output:

```text
Build/camelliaPkg/DEBUG_CLANGPDB/FV/SILICIUM_UEFI.fd
Mu-camellia.img
```

## Linux hot-jump FD

The Linux loader must place the FD at `0xA0000000`, so the FD must be linked at
that address. The UEFI CPU stack must also use the hot-jump-owned window at
`0xA0200000`; `0x40000000` is not a safe stack after Android has occupied it.
Do not reuse the cold-boot FD.

```bash
./build-camellia-hotjump.sh
```

Output:

```text
dist/SILICIUM_UEFI-hotjump.fd
```

The script validates offset `0x1098` and rejects an FD whose C entry is not in
`0xA0000000..0xA0200000`.

It also checks that the SEC stack setup matches `0xA0200000`. A build that
changes only the FD base to `0xA0000000` is not a valid hot-jump image: it
keeps the stack at `0x40000000`, which Android has already occupied.

## Hardware facts

```text
SoC                 MT6833
CPU                 6 x Cortex-A55 + 2 x Cortex-A76
GICD                0x0C000000
GICR                0x0C040000
UART                0x11002000, 26 MHz
Pinctrl             0x10005000
UFS HCI             0x11270000
UFS PHY             0x11FA0000
Display subsystem   0x14000000
DSI0                0x14013000
Framebuffer         0x7BA10000
Framebuffer mode    1088 x 7200 x 32 bpp
```

## Known limitations

- The UEFI platform is a bring-up baseline, not a production firmware release.
- ACPI still uses the inherited Lancelot placeholder tables.
- Clock, GPIO, PMIC and MSDC libraries are inherited from MT6789 and need
  MT6833/MT6359 validation.
- There is no native UFS DXE driver yet.
- The hot-jump path has a historically intermittent red-screen failure before
  UEFI starts. Use the exact known-good module and FD pair above.
- The diagnostic `CAFE....` framebuffer probes from bring-up are not part of
  this clean source snapshot.
- The RAM disk, trust cache and all XNU payloads are supplied separately.

## Source provenance

The complete source attribution and upstream links are in `README.md`.
The exact submodule revisions are in `REPRODUCIBILITY.md`.
Device-specific dependencies and likely debugging points are listed in
`DEPENDENCIES.md`.
