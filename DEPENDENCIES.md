# Dependencies and debugging map

## Public dependencies

All Git submodules use public repositories and exact pinned commits in
[REPRODUCIBILITY.md](REPRODUCIBILITY.md).

| Dependency | Role | Portability |
| --- | --- | --- |
| `Mu_Basecore` | Microsoft Project Mu / EDK II core | Generic, public |
| `Common/Mu` | Project Mu common packages | Generic, public |
| `Common/Mu_OEM_Sample` | Project Mu OEM sample packages | Generic, public |
| `Silicium-ACPI` | ACPI platform packages | Public; Camellia still uses inherited tables |
| OpenSSL | UEFI crypto library | Generic, public |
| `Binaries` | Public Project-Silicium device-binary collection | Repository is shared; individual vendor blobs remain device/vendor-specific |

The source and configuration contain no discovered IMEI, serial number or
per-device unique-chip identifier.

## Camellia-specific source

These files are device-model specific and are the main places that may need
changes on another phone, hardware revision or firmware baseline:

1. `Resources/DTBs/camellia.dtb`
   - Stock DT baseline.
   - Debug if memory reservations, display, UFS or interrupt topology differ.

2. `Platforms/Xiaomi/camelliaPkg/Library/MemoryMapLib/MemoryMapLib.c`
   - Memory reservations, IO windows and framebuffer handoff.
   - Highest-risk area for a different RAM layout or display configuration.

3. `Platforms/Xiaomi/camelliaPkg/Drivers/CamelliaBootMemoryDxe/CamelliaBootMemoryDxe.c`
   - Framebuffer geometry and RAM-disk publication.
   - May need panel-specific width, height, pitch or scanout runs.

4. `Silicon/MediaTek/MT6833Pkg/Library/ClockImplLib/ClockImplLib.c`
   - MT6833 clock tree.
   - Debug if UART, UFS, SD/eMMC, PMIC or display clocks differ.

5. `Silicon/MediaTek/MT6833Pkg/Library/PmicWrapperImplLib/PmicWrapperImplLib.c`
   - MT6359-era PMIC wrapper register layout.
   - May need changes for different PMIC silicon or regulator names.

6. `Silicon/MediaTek/MT6833Pkg/Library/MsdcImplLib/MsdcImplLib.c`
   - GPIO, clock and regulator setup for SD/eMMC.
   - This project uses UFS on Camellia and currently disables eMMC publication.

7. `Silicon/MediaTek/MT6833Pkg/Library/GpioImplLib/GpioImplLib.c`
   - Pin names and GPIO numbering.
   - Debug if keys, display or storage GPIOs differ.

8. `Platforms/Xiaomi/camelliaPkg/Drivers/CamelliaUniXNULoaderDxe/`
   - RAM-disk device publication and launch of the UniXNU application.
   - Mostly generic, but the RAM partition layout is specific to this boot
     payload.

## Prebuilt component

The following files are prebuilt binaries, not rebuilt from this source tree:

```text
Platforms/Xiaomi/camelliaPkg/Binaries/UniXNU/UniXNU.efi
Platforms/Xiaomi/camelliaPkg/Binaries/UniXNU/UniXNUDxe.efi
```

They belong to the existing UniXNU/DarwinBoot integration. Replace or rebuild
them when integrating a different XNU solution.

## Incomplete or likely to require debugging

- Native UFS DXE support is absent.
- ACPI tables are inherited from the Lancelot platform and are placeholders.
- The Linux hot-jump module depends on a kernel that exports
  `camellia_jump_to` and may need adaptation for a different Android kernel.
- The hot-jump path is kernel-version and memory-layout sensitive.
- Secure Boot and production variable provisioning are not validated.
- USB, touch and GPU support are not part of this verified baseline.

## Sharing across phones

The firmware is not tied to one handset identity. It can be shared with other
Redmi Note 10 5G (`camellia`) users with a compatible hardware revision and
bootloader/DTB layout. Treat it as model-specific, not universal MT6833
firmware. A different panel, PMIC, UFS part or firmware revision may require
the files listed above to be adjusted.
