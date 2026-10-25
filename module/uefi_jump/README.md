# uefi_jump kernel module

Load a kernel module into the camellia kernel and, eventually, hand control
to the bare-metal UEFI (`SILICIUM_UEFI.fd`, rebuilt for FD base
`0xA0000000` / size `0x200000`).

## Status

- **Stage 0 (done):** builds, loads with `insmod`, resolves symbols. Does not
  jump. Current file: `uefi_jump.c`.
- **Stage 1 (todo):** reserve + identity-map a 2 MiB buffer, copy the FD,
  stop secondary CPUs, turn off the MMU and jump into UEFI SEC.

## Current base kernel

The device now runs the self-built official `camellian-t-oss` (Android 13)
kernel, version `4.14.186-perf-jiang352`, with `KALLSYMS_ALL=y`. The kernel
source and build trees are:

- source: `/home/jiangshihao/kernel-camellia-t`
- build:  `/home/jiangshihao/kernel-camellia-t-out`

Because the module is built against the exact same tree as the running kernel,
`module_layout` and all symbol CRCs match: **plain `insmod` works, no patch**.

## Build + load (A13, no patch)

```sh
make KDIR=/home/jiangshihao/kernel-camellia-t-out
adb push uefi_jump.ko /data/local/tmp/
adb shell su -c 'insmod /data/local/tmp/uefi_jump.ko'
adb shell su -c 'dmesg | grep uefi_jump'
adb shell su -c 'rmmod uefi_jump'
```

## A12 fallback (historical)

On the stock Android 12 kernel the `module_layout` CRC did not match and
`CONFIG_MODULE_FORCE_LOAD` was off, so IGNORE flags were useless. The
workaround was to empty the `__versions` section with `patch_versions.py`
and load the resulting `.ko` with plain `insmod`. Kept here in case we ever
flash back to the A12 kernel.

## Symbol resolution result (stage 0, on A13)

Resolved (8): `smp_send_stop`, `memblock_reserve`, `memblock_find_in_range`,
`memremap`, `ioremap_cache`, `cpu_do_switch_mm`, `psci_ops`,
`secondary_holding_pen`.

Not resolvable (static / inline / CFI-mangled / `.idmap.text`, never in
kallsyms): `flush_cache_all`, `local_flush_tlb_all`, `cpu_soft_restart`,
`__cpu_soft_restart`, `soft_restart`, `psci_sys_reset`.

The jump stage therefore needs its own MMU-off trampoline instead of calling
`__cpu_soft_restart`.
