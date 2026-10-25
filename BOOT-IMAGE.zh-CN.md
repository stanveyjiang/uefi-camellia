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

准备：

```bash
export BASE_BOOT_IMG=/path/to/compatible-jump2-boot.img
export UEFI_KO=/path/to/uefi_jump.ko
export UEFI_FD="$PWD/dist/SILICIUM_UEFI-hotjump.fd"
export PAYLOAD_IMG=/path/to/64MiB-payload.img
```

`UEFI_KO` 和 `UEFI_FD` 必须匹配。`PAYLOAD_IMG` 必须是恰好 64 MiB 的原始
镜像，最终会写入 `vendor_boot_a`，不会嵌入 boot image。

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

## 8. 刷写

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

## 9. 屏幕阶段

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

## 10. 回滚

必须提前保存原厂分区，然后执行：

```bash
fastboot flash boot_a /path/to/original-boot_a.img
fastboot flash vendor_boot_a /path/to/original-vendor_boot_a.img
fastboot reboot
```

如果设备不能进入 fastboot，使用设备既有的 LK/fastboot 按键组合恢复。
本仓库不提供分区备份。

## 11. 后续建议

1. 保留已验证的 FD/KO 对和原厂 boot 分区。
2. 优先验证最小 initramfs 链路，再接 XNU payload。
3. 不要恢复“LK 直接执行 BootShim/UEFI”路线。
4. 不要把 `fastboot boot` 失败误判为镜像格式错误。
5. 其他机型或分区布局变化时，先重新确认 `sdc31`、DRM 节点和 UEFI FD 地址。
