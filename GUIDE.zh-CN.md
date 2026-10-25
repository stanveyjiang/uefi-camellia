# Camellia UEFI 移植与维护指南

本文面向拿到源码后需要继续修改、构建或移植到相近设备的开发者。

## 1. 项目范围

目标设备：

```text
Xiaomi Redmi Note 10 5G
codename: camellia
SoC: MediaTek MT6833 / Dimensity 700
```

当前目标是完成 UEFI bring-up，并提供可被外部 XNU/UniXNU/DarwinBoot 项目
接管的 framebuffer、RAM disk 和 payload handoff。

当前仓库不包含：

- XNU、UniXNU、DarwinBoot、kernelcache 或 `payload.img`
- Project Mu、Project Silicium、OpenSSL 等上游源码
- 用户数据、设备唯一标识或原厂完整固件

## 2. 启动链

当前验证路径是 Android/Linux 热跳，不是由 LK 直接冷启动：

```text
Android Linux
  -> drm_linear 建立线性 framebuffer
  -> uefi_jump.ko 复制 FD 并建立 framebuffer handoff
  -> UEFI SEC / PEI / DXE
  -> GOP / BDS / boot menu
  -> CamelliaBootMemoryDxe 发布 framebuffer 与 payload 环境
  -> CamelliaRamDiskDxe 发布内存块设备
  -> CamelliaUniXNULoaderDxe 启动外部 loader
  -> 外部 XNU 项目
```

热跳 FD 有两个必须同时成立的地址约束：

```text
FD base       0xA0000000
UEFI stack    0xA0200000
```

只修改 `Resources/Configs/camellia.toml` 中的 FD base 会保留
`0x40000000` stack。该区域已被 Android 使用，随后会出现早期 DXE 卡死、
随机异常或重启。

始终使用：

```bash
./build-camellia-hotjump.sh
```

不要手工拼装一个只改 base 的热跳 FD。

## 3. Camellia 相关改动

### 3.1 平台包

`Platforms/Xiaomi/camelliaPkg` 是设备入口，包含：

- `camellia.dsc` / `camellia.fdf`
- DXE 调度顺序
- 内存映射
- framebuffer 与 payload handoff
- RAM disk BlockIo/DiskIo
- 外部 loader 启动逻辑
- 按键输入
- camellia DTB

### 3.2 MT6833 支持

`Silicon/MediaTek/MT6833Pkg` 和相关 MediaTek 驱动提供：

- 初始时钟树
- GPIO
- PMIC wrapper
- MSDC/MMC
- platform SEC 支持

这些实现仍属于 bring-up 等级，不等于量产驱动。

### 3.3 framebuffer handoff

Linux 和 UEFI 之间真正稳定的显示通道是启动前已经建立的线性 framebuffer。
当前源码使用 framebuffer handoff 和 scatter-page run map，让 UEFI 不依赖
完整厂商显示栈。

不建议在启动链稳定前并行启用完整厂商 DSI/DRM/GPU 栈。完整显示栈很容易
重写 scanout 状态，导致 UEFI 黑屏。

### 3.4 RAM disk 与外部 payload

`CamelliaBootMemoryDxe`、`CamelliaRamDiskDxe` 和
`CamelliaUniXNULoaderDxe` 共同完成：

1. 读取 handoff 信息。
2. 保留 payload 和显存相关区域。
3. 发布内存块设备。
4. 查找并启动外部 loader。
5. 把 framebuffer 和内存布局交给 XNU 项目继续使用。

XNU/UniXNU 二进制不属于本仓库。接入其他 XNU 项目时，优先替换 loader 层，
不要删除已经验证过的 framebuffer 与内存 handoff。

### 3.5 Mu 补丁

`Resources/MuPatches` 中的补丁处理：

- AArch64 汇编节名兼容
- 运行时变量写入保护
- boot manager 菜单与显示兼容
- timer frequency fallback
- USB TPL 调整
- GICv3 redistributor 从 Linux 状态唤醒并清理 PPI/SGI
- 热跳后只注册可用的物理 timer PPI
- 减少 DXE HOB 调试输出造成的阻塞
- 精简异常输出，避免异常路径再次触发复杂显示或串口调用

发布补丁不包含 bring-up 阶段的 `CAFE....` framebuffer 探针。

## 4. 构建环境

推荐：

```text
Ubuntu/Debian
Python 3.11+
Clang/LLD 18
Git
```

首次准备：

```bash
git clone https://github.com/stanveyjiang/uefi-camellia.git
cd uefi-camellia
git submodule update --init
./setup_env.sh -p apt
```

热跳构建：

```bash
./build-camellia-hotjump.sh
```

输出：

```text
dist/SILICIUM_UEFI-hotjump.fd
```

脚本会自动检查：

- FD 大小必须为 `0x200000`
- C entry 必须位于 `0xA0000000..0xA0200000`
- SEC stack 必须匹配 `0xA0200000`

冷启动构建脚本仍然保留，但当前 LK 路径没有达到与热跳相同的验证等级：

```bash
./build-camellia-coldboot.sh
```

不要用冷启动 FD 配热跳 KO，也不要反过来混用。

## 5. 建议测试顺序

顺序很重要，一次只推进一层：

1. 使用已验证 FD 与已验证 KO，确认能重新进入 UEFI。
2. 使用相同 KO 测试新构建的 UEFI-only candidate。
3. 确认 UEFI boot menu 稳定后，再接外部 XNU payload。
4. 确认 Linux -> UEFI -> XNU handoff 后，再测试原生显示、存储和输入。

出现失败时，先保存以下信息：

```text
FD SHA-256
KO SHA-256
FD C entry
SEC stack setup
最后显示的 UEFI driver/stage
是否出现同步异常、out of resources 或 watchdog 重启
```

不要只记录“黑屏”或“卡住”。同一黑屏可能来自 framebuffer 切换、DXE OOM、
异常处理路径或外部 payload 启动失败。

## 6. 已知边界

- 原生 UFS DXE 尚未完成。
- MT6359 电源、时钟和 GPIO 仍需按实际板级校准。
- ACPI 仍是继承平台的占位实现。
- 完整 DSI/DRM、触控、GPU、USB 和量产输入栈不在此基线内。
- 热跳依赖 Android 内核保留 framebuffer 和匹配的 `uefi_jump.ko`。
- Linux 热跳存在间歇性红屏或进入 UEFI 前失败，已验证 FD/KO 应成对保留。
- cold boot 仍受设备 LK 接受自制 boot image 的限制。

## 7. 主要修改入口

| 目标 | 位置 |
| --- | --- |
| DDR 保留区、设备 MMIO、显存窗口 | `Platforms/Xiaomi/camelliaPkg/Library/MemoryMapLib/` |
| framebuffer、payload handoff | `Platforms/Xiaomi/camelliaPkg/Drivers/CamelliaBootMemoryDxe/` |
| 外部 XNU/UniXNU loader | `Platforms/Xiaomi/camelliaPkg/Drivers/CamelliaUniXNULoaderDxe/` |
| RAM disk 块设备 | `Platforms/Xiaomi/camelliaPkg/Drivers/CamelliaRamDiskDxe/` |
| DXE 加载顺序 | `Platforms/Xiaomi/camelliaPkg/Include/` |
| 设备树 | `Resources/DTBs/camellia.dtb` |
| Mu/ARM 热跳修复 | `Resources/MuPatches/Camellia-Essential.patch` |
| Linux 热跳模块 | `module/uefi_jump/` |
| FD 地址和 boot image 参数 | `Resources/Configs/camellia.toml` |
| 热跳 stack 和构建检查 | `build-camellia-hotjump.sh` |

## 8. 后续建议

优先顺序建议保持为：

1. 保持可用的 FD/KO 恢复点，任何实验都不要覆盖它。
2. 先完成稳定的 Linux -> UEFI -> 外部 loader 链路。
3. 再实现 UFS，让外部 loader 可以脱离内存块设备。
4. 然后实现 MT6833/MT6359 原生显示与输入。
5. 最后处理量产 ACPI、USB、GPU、触控和 cold boot。

不要同时扩大多个高风险子系统。当前最大的风险不是缺少某个驱动，而是启动
早期地址、内存映射、中断状态和 framebuffer 状态互相影响，导致问题被错误
归因。

## 9. 许可证

仓库根目录的 `LICENSE` 和上游组件许可证适用。Project Mu、Project
Silicium、OpenSSL 等依赖仍由各自上游许可证约束。发布二进制前应同时保留
源码、构建脚本、版本固定信息和 SHA-256。
