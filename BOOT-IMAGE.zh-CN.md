# Camellia Boot Image 打包与验证记录

本文记录 camellia 从 LK 进入 UEFI 的 boot image 路线，以及哪些路线已经
真机验证、哪些路线已被否定。

## 1. 验证结论

| 路线 | 状态 | 说明 |
| --- | --- | --- |
| Android boot v2 打包 | 通过 | 与 stock `boot.img` 的头部和重复打包数据逐字节验证 |
| `fastboot boot` | 不可用 | 本机实测失败；原厂可启动镜像也同样失败，不能作为验证依据 |
| LK 直接执行自建 BootShim/UEFI | 失败 | 多次固定在约 30 秒复位，已放弃 |
| LK → Linux → 最小 initramfs → `drm_linear` → `uefi_jump` → UEFI | 真机通过 | 不启动 Android/Magisk/ADB |
| UEFI → `vendor_boot_a` 原始 payload → UniXNU 菜单 | 真机通过 | UniXNU 能识别 payload；XNU 后续问题在 UEFI 之外处理 |

当前推荐路线不是把 UEFI FD 直接伪装成 Linux kernel：

```text
LK
  -> 已验证的 jump2 Linux kernel
  -> 最小 initramfs
  -> drm_linear 建立线性 framebuffer
  -> uefi_jump 热跳到 UEFI
  -> UEFI GOP / BDS
  -> vendor_boot_a 中的原始 payload
  -> 外部 UniXNU/XNU 项目
```

## 2. fastboot 限制

本设备不能使用：

```bash
fastboot boot boot.img
```

实测会先显示 `Sending ... OKAY`，随后出现 `usb_read failed`。原厂正常
boot image 也是如此，这不是自制镜像格式损坏。

真机验证必须使用 `fastboot flash boot_a`。刷写前必须先备份原厂：

```text
boot_a
vendor_boot_a
```

本项目不附带原厂分区备份或用户数据。

## 3. 已否定的直接 BootShim 路线

曾尝试把：

```text
BootShim + 2 MiB UEFI FD
```

作为 Android kernel 交给 LK 执行。测试过标准历史 BootShim、10/15/20/30 秒
不同超时镜像，均在固定约 30 秒复位。

结论：

```text
LK 直接执行自建 BootShim/UEFI：FAIL
```

这条路线不再作为正式启动路径。不要根据“过了 30 秒才重启”判断 DXE
已经运行；该复位来自 LK 侧超时/复位，而不是 UEFI watchdog。

## 4. 已验证的预启动 initramfs 路线

已验证链路：

```text
LK
  -> Linux
  -> minimal initramfs PID 1
  -> /dev/dri/card0
  -> drm_linear
  -> /dev/block/sdc31
  -> uefi_jump.ko
  -> UEFI
```

关键点：

- 不启动 Android 用户态。
- 不启动 Magisk。
- 不依赖 ADB。
- initramfs 自己创建 DRM 和 payload block device 节点。
- `drm_linear` 获取 DRM master，建立线性 XRGB8888 framebuffer。
- `uefi_jump` 建立散页 framebuffer map 并跳转到热跳 FD。
- 64 MiB payload 从 `vendor_boot_a` 原始读取，不使用标准 vendor_boot 头部。

历史真机通过产物：

```text
camellia-preboot-uefi-diag.img
sha256 00fa59e9af560f7dec37bc459a67bf3d4593e35d7fcee6b2cd8baaaf28be2667
```

后续内存窗口修复版本：

```text
camellia-preboot-uefi-diag.img
sha256 af390979824c758b0d29b974e0b8e47e6b5165338521dd63cf0294d06e34b624
```

第二个版本在记录时只完成构建，仍需结合当时对应的 UEFI FD/KO 做真机
复核。不要把历史哈希与当前 Release 的 FD/KO 混配。

## 5. Boot image 几何参数

已验证的 Android boot header 参数：

```text
header_version  2
page_size       2048
base            0x40000000
kernel_offset   0x00080000  -> kernel  0x40080000
ramdisk_offset  0x11100000  -> ramdisk 0x51100000
tags_offset     0x07C80000
dtb_offset      0x07C80000
cmdline         bootopt=64S3,32N2,64N2
```

不要随意改变这些几何参数。当前 `Resources/Scripts/mkbootimg-camellia.py` 已用原厂
boot image 做回归：数据段、头部和重复打包结果都经过逐字节比较。

## 6. 源码位置

```text
build-boot-image.sh
BootImage/init.c
BootImage/Makefile
module/drm_linear/drm_linear.c
module/uefi_jump/
Resources/Scripts/mkbootimg-camellia.py
Resources/Scripts/bootimg-info.py
```

`BASE_BOOT_IMG` 必须是设备可启动且带有 `jump2` 能力的 Linux boot image。
本仓库不附带原厂 boot image、Magisk 补丁产物或用户设备备份。

## 7. 构建 boot image

先构建热跳 FD：

```bash
./setup_env.sh -p apt
./build-camellia-hotjump.sh
```

`setup_env.sh` 会安装 boot image 工具需要的 `cpio`、`libdrm-dev` 和
`aarch64-linux-gnu-gcc`。

准备：

```bash
export BASE_BOOT_IMG=/path/to/compatible-jump2-boot.img
export UEFI_KO=/path/to/uefi_jump.ko
export UEFI_FD="$PWD/dist/SILICIUM_UEFI-hotjump.fd"
export PAYLOAD_IMG=/path/to/64MiB-payload.img
```

`UEFI_KO` 和 `UEFI_FD` 必须匹配。`PAYLOAD_IMG` 必须是恰好 64 MiB 的原始
镜像，最终会写入 `vendor_boot_a`，不会嵌入 boot image。

这里的 `UEFI_KO` 和 `UEFI_FD` 是构建脚本变量，不是文件格式：

```text
UEFI_KO -> uefi_jump-known-good.ko
UEFI_FD -> SILICIUM_UEFI-hotjump-known-good.fd
```

脚本会把它们复制成 initramfs 内的 `/uefi_jump.ko` 和 `/uefi.fd`。直接刷
已经构建好的 `camellia-preboot-uefi-v2026.10.26.img` 时，不需要设置这些变量。
完整对照见 [ARTIFACTS.zh-CN.md](ARTIFACTS.zh-CN.md)。

构建：

```bash
./build-boot-image.sh
```

输出：

```text
dist/bootimage/camellia-preboot-uefi-diag.img
dist/bootimage/initramfs.cpio.gz
dist/bootimage/SHA256SUMS.txt
```

## 8. 开发者可选：Android 内 Root + 热跳测试

这条路线只用于开发者在 Android 用户态快速迭代，不用于公开分发 boot
镜像，也不替代已经验证的 boot image 冷启动路线。

不要公开提供带 Magisk 的完整 boot 二进制。应让使用者使用自己固件版本的
原厂 boot，并提供内核源码/补丁、模块源码和操作步骤。

### 8.1 Root 与原厂 boot

1. 从当前设备固件中取得与系统版本完全匹配的原厂 `boot.img`。
2. 先保存 `boot_a` 和 `vendor_boot_a` 回滚镜像。
3. 在 Magisk App 中选择“安装 -> 选择并修补一个文件”，修补自己的原厂
   `boot.img`。
4. 只刷入修补后的 `boot_a`，确认 Android 和 `su` 正常。

Magisk 只解决 Root，不会自动提供 Camellia 的 `jump2` 内核能力。

### 8.2 内核对 jump2 的要求

仅打 Magisk 的原厂内核通常不能完成热跳。内核还需要：

- 与运行版本完全匹配的模块构建环境；
- `KALLSYMS` / `KALLSYMS_ALL`，供模块解析内部符号；
- 可用的 `smp_send_stop`、`memblock_reserve`、`memremap`、
  `ioremap_cache` 等符号；
- Camellia 的 `camellia_jump_to()` 跳转入口；
- 适合接管 MTK DRM framebuffer 的内核状态。

这里的 `camellia_jump_to()` 是内核侧实现，不是 Magisk 功能。若内核没有
该入口，`uefi_jump.ko` 会出现未解析符号，无法跳转。

公开分享时应提供对应内核补丁或源码，不应公开预 Root 的成品 boot。

### 8.3 构建模块

模块必须针对当前实际运行的内核构建：

```bash
make -C module/uefi_jump \
  KDIR=/path/to/matching-kernel-build \
  CLANG_DIR=/path/to/clang/bin
```

优先使用与设备内核完全相同的源码和配置。`module/uefi_jump/patch_versions.py`
只能作为最后手段处理 `__versions` CRC 不匹配；它不能修复内核 ABI、符号或
结构体差异。

### 8.4 构建 framebuffer 工具和模块

```bash
make -C BootImage clean all
```

准备 Hot-jump FD、匹配 KO、`drm_linear` 和可选 payload：

```bash
./build-camellia-hotjump.sh

adb push dist/SILICIUM_UEFI-hotjump.fd /data/local/tmp/uefi.fd
adb push module/uefi_jump/uefi_jump.ko /data/local/tmp/uefi_jump.ko
adb push BootImage/build/drm_linear /data/local/tmp/drm_linear
adb push /path/to/64MiB-payload.img /data/local/tmp/payload.img
adb shell 'chmod 755 /data/local/tmp/drm_linear'
```

从 Android 内执行：

```bash
adb shell "su -c '
  stop vendor.hwcomposer-2-1
  stop surfaceflinger
  pkill -x drm_linear 2>/dev/null
  nohup /data/local/tmp/drm_linear /dev/dri/card0 0xFFFF0000 \
    >/data/local/tmp/drm_linear.log 2>&1 &
  sleep 3
  insmod /data/local/tmp/uefi_jump.ko \
    fd_path=/data/local/tmp/uefi.fd \
    payload_path=/data/local/tmp/payload.img \
    payload_bytes=67108864 \
    hb=0 jump=1
'"
```

只测试进入 UEFI、不接 UniXNU 时，可以省略 `payload_path` 和
`payload_bytes`。要完整验证 UEFI -> UniXNU 链路，则必须提供正确 payload。

热跳后 Android 不会继续运行；测试结束通常需要长按电源键或使用设备既有的
fastboot 按键组合重启。

### 8.5 分发边界

- 不公开带 Magisk 的完整 boot。
- 不把 Root 当作 UEFI 运行前提；最终 boot image 已不依赖 Magisk。
- 需要可复现时，提供内核源码/补丁、模块源码、工具源码和固定版本信息。
- 每个使用者必须使用自己固件版本对应的原厂 boot 和回滚文件。

## 9. 刷写

先确认设备处于 fastboot 模式，并备份原厂分区：

```bash
fastboot getvar current-slot
fastboot flash boot_a dist/bootimage/camellia-preboot-uefi-diag.img
fastboot flash vendor_boot_a /path/to/64MiB-payload.img
fastboot reboot
```

注意：

- `vendor_boot_a` 在这里被当作 64 MiB 原始 payload 区使用，不是标准
  vendor_boot 镜像。
- `sdc31` 是当前 camellia 的分区号。其他批次或其他机型必须重新确认。
- 构建前必须确认 `BootImage/init.c` 中的 payload block device 与实际设备
  一致。
- 不建议在本设备使用 `fastboot boot`。

## 10. 屏幕阶段

当前 initramfs 状态色：

```text
红       drm_linear 已建立线性 framebuffer
绿       framebuffer page map 预检开始
青       page map 预检通过，准备最终热跳
洋红     预检、模块加载或最终跳转失败
UEFI 日志/四色带   已进入 UEFI
```

如果不出现青色，不要继续修改 UEFI DXE。先检查 DRM、OVL、IOMMU、
framebuffer page map 和 KO/FD 地址配对。

## 11. 回滚

必须提前保存原厂分区，然后执行：

```bash
fastboot flash boot_a /path/to/original-boot_a.img
fastboot flash vendor_boot_a /path/to/original-vendor_boot_a.img
fastboot reboot
```

如果设备不能进入 fastboot，使用设备既有的 LK/fastboot 按键组合恢复。
本仓库不提供分区备份。

## 12. 后续建议

1. 保留已验证的 FD/KO 对和原厂 boot 分区。
2. 优先验证最小 initramfs 链路，再接 XNU payload。
3. 不要恢复“LK 直接执行 BootShim/UEFI”路线。
4. 不要把 `fastboot boot` 失败误判为镜像格式错误。
5. 其他机型或分区布局变化时，先重新确认 `sdc31`、DRM 节点和 UEFI FD 地址。
