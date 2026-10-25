# Reproducibility

## Baseline

```text
upstream project  Project-Silicium/Mu-Silicium
device            Xiaomi camellia (MT6833)
baseline commit   8221489a628aa47daa0776322ee17ce8dc7a9981
build target      DEBUG
toolchain         CLANGPDB / AArch64
```

## Submodules

```text
Binaries
  a1501e0c2e642d6ac0d8f52c85af2a67b75a7799
  https://github.com/Project-Silicium/Device-Binaries.git

Common/Mu
  b8d46c71610318e36a661819e1e3c0b8bdda413a
  https://github.com/microsoft/mu_plus.git

Common/Mu_OEM_Sample
  b70b4b7bb02c43bf9db2172dfb3ed0457e2361d6
  https://github.com/microsoft/mu_oem_sample.git

Mu_Basecore
  3c9c7b2e211e62d2a954ad760a36fe05adc43aa3
  https://github.com/microsoft/mu_basecore.git

Silicium-ACPI
  1c8bd63a7ffcaecf3c8764a6b01a11e053457543
  https://github.com/Project-Silicium/Silicium-ACPI.git

Silicon/Silicium/OpensslPkg/Library/OpensslLib/openssl
  67b5686b4419b4cb8caa502711c41815f5279751
  https://github.com/openssl/openssl.git
```

Restore them with:

```bash
git submodule update --init --recursive
```

## Build checks

For a hot-jump FD:

```bash
./build-camellia-hotjump.sh
```

Expected properties:

```text
FD size       0x200000
reset entry   AArch64 branch from 0xA0000000 to 0xA0001000
C entry       inside 0xA0000000..0xA0200000
stack setup   bytes at 0x1058 equal the known-good 0xA0200000 setup
FV signature  _FVH at offset 0x28
```

Byte-for-byte equality with the historical FD is not expected because build
timestamps and compressed FV metadata can differ. The boot address, C entry,
platform source and submodule revisions are the compatibility contract.

`build_uefi.py` applies `Resources/MuPatches/Camellia-Essential.patch` after
the five upstream Mu compatibility patches. This patch contains the hardware
bring-up changes that were previously present only as uncommitted local
changes in `Mu_Basecore`.
