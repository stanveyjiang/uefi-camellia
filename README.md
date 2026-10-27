# Project Silicium UEFI - Camellia (MT6833)

Clean, upload-ready source snapshot for the Xiaomi Redmi Note 10 5G
(`camellia`, MediaTek MT6833 / Dimensity 700).

This repository is derived from
[Project-Silicium/Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium).
The Camellia platform integrates UEFI, a scatter-page framebuffer shadow,
RAM-disk publication and a generic external payload handoff. XNU, UniXNU and
DarwinBoot binaries are not bundled.

## Start here

- [Camellia platform and build guide](CAMELLIA.md)
- [Release artifacts and variable names](ARTIFACTS.zh-CN.md)
- [Detailed Chinese porting and maintenance guide](GUIDE.zh-CN.md)
- [Boot image packaging and validation](BOOT-IMAGE.zh-CN.md)
- [Dependencies and debugging map](DEPENDENCIES.md)
- [Pinned source revisions](REPRODUCIBILITY.md)
- [Upstream build documentation](Building.md)
- [Camellia porting notes](Platforms/Xiaomi/camelliaPkg/PORTING.md)

## Quick build

```bash
git submodule update --init
./setup_env.sh -p apt

# Cold boot at 0x40080000
./build-camellia-coldboot.sh

# Linux hot-jump at 0xA0000000
./build-camellia-hotjump.sh
```

Do not build the hot-jump image by changing only `[uefi_fd].base` to
`0xA0000000`. The UEFI CPU stack must also move to `0xA0200000`;
`build-camellia-hotjump.sh` performs both changes and verifies the resulting
FD. A base-only image can enter UEFI and then fail or hang because it reuses
the Linux-occupied `0x40000000` stack.

## Validation

The baseline source commit,
`8221489a628aa47daa0776322ee17ce8dc7a9981`, is the version associated with the
hardware-validated hot-jump FD:

```text
SHA-256 68184f8a1451d08a90fafc61b1163e6e2056469bac174ebc5c71b38f23bfc406
```

The matching hot-jump module is:

```text
uefi_jump-mapdiag.ko
SHA-256 bec844cf909a8032efe33b58c879ba6ff499036263c824672a47afdc9f3a71c6
```

The FD and module are a tested pair. Use them together when recovering from a
failed local build.

`UEFI_FD` and `UEFI_KO` are build-script variable names, not file names.
`UEFI_KO` points to the Linux module `uefi_jump.ko`. The final prebuilt boot
image already embeds both files. See
[ARTIFACTS.zh-CN.md](ARTIFACTS.zh-CN.md) for the complete mapping.

See [CAMELLIA.md](CAMELLIA.md) for the exact use and limitations.

## Repository scope

- `Platforms/Xiaomi/camelliaPkg`: device platform package.
- `Silicon/MediaTek/MT6833Pkg`: initial MT6833 libraries.
- `Resources/Configs/camellia.toml`: boot image and FD geometry.
- `Resources/DTBs/camellia.dtb`: stock device tree baseline.
- `module/uefi_jump`: optional Linux-to-UEFI loader module source.
- `Platforms/Xiaomi/camelliaPkg/Drivers/CamelliaUniXNULoaderDxe`: integration
  point for an external XNU project.

The source tree intentionally excludes generated `Build/`, local `.venv/`,
firmware images, XNU payloads and any private device data.

## License and credits

The upstream Project Silicium licensing applies. Code outside
`GPLDrivers`, `GPLApplications` and `GPLLibrary` is BSD-2-Clause; those GPL
components remain GPLv2-or-later. See [LICENSE](LICENSE) and component
licenses.

Major upstream projects:

- [Microsoft Project Mu](https://microsoft.github.io/mu/)
- [Project Silicium](https://github.com/Project-Silicium)
- [WOA-Project mu_andromeda_platforms](https://github.com/WOA-Project/mu_andromeda_platforms)
- [Project Aloha](https://github.com/Project-Aloha)
