# Camellia development status

**Device:** Xiaomi Redmi Note 10 5G

**Codename:** camellia

**SoC:** MediaTek MT6833 / Dimensity 700

**State:** Bring-up source baseline

## Verified

- Builds a 2 MiB AArch64 UEFI firmware volume.
- Cold-boot FD base: `0x40080000`.
- Linux hot-jump FD base: `0xA0000000`.
- Hardware-validated hot-jump FD from the `8221489a` source baseline:
  `68184f8a1451d08a90fafc61b1163e6e2056469bac174ebc5c71b38f23bfc406`.
- Matching hot-jump module:
  `bec844cf909a8032efe33b58c879ba6ff499036263c824672a47afdc9f3a71c6`.
- SEC/DXE/GOP, scatter-page framebuffer shadow, RAM-disk publication,
  and the external payload handoff.
- The current source build path uses `build-camellia-hotjump.sh`, which moves
  both the FD base and the UEFI CPU stack.

## Not yet production

- Native UFS DXE support.
- Device-specific MT6359 PMIC, MT6833 clock/GPIO and UFS phase support.
- Production ACPI tables.
- A stable cold-boot BootShim chain.
- Production display, touch, GPU and USB support.
- Bring-up-only `CAFE....` progress probes, which are intentionally excluded
  from the clean source package.
- XNU, UniXNU and DarwinBoot binaries are supplied by an external project.

See [CAMELLIA.md](CAMELLIA.md) for build instructions and known-good artifact
details.
