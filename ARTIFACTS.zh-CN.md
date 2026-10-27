# Camellia 构建产物与变量说明

本文说明 Release 文件、构建脚本变量和运行时路径之间的关系。

## 没有名为 UEFI_KO 的正式产物

官方 UniXNU 工程中没有一个称为 `UEFI_KO` 的文件或组件。

在本仓库的 `build-boot-image.sh` 中，`UEFI_KO` 是一个环境变量，用来保存
`uefi_jump.ko` 的文件路径。它不是 `.efi` 文件，也不是 UEFI 驱动。

## 文件对应关系

| 构建变量 | Release 文件 | boot image 内路径 | 用途 |
| --- | --- | --- | --- |
| `UEFI_FD` | `SILICIUM_UEFI-hotjump-known-good.fd` | `/uefi.fd` | 热跳使用的 2 MiB UEFI 固件镜像 |
| `UEFI_KO` | `uefi_jump-known-good.ko` | `/uefi_jump.ko` | Linux 内核中执行热跳的模块 |
| `PAYLOAD_IMG` | 不公开、需单独提供 | 不嵌入 boot image | 写入 `vendor_boot_a` 的 64 MiB UniXNU payload |
| `BASE_BOOT_IMG` | 不公开、需自行准备 | 只提取 kernel 和 DTB | 提供 jump2 内核能力的原始 boot image |

`UEFI_FD` 和 `UEFI_KO` 同样是变量名。对应实际文件分别是 `.fd` 和 `.ko`。

## 三种使用方式

### 1. 直接刷发布 boot image

使用：

```text
camellia-preboot-uefi-v2026.10.26.img
```

这个镜像已经把以下文件打包进 initramfs：

```text
UEFI FD
uefi_jump.ko
drm_linear
init
```

因此不需要下载或设置 `UEFI_KO`、`UEFI_FD`。

### 2. Android 内 ADB 热跳

这条路线需要把实际文件推到手机：

```text
SILICIUM_UEFI-hotjump-known-good.fd
uefi_jump-known-good.ko
drm_linear
可选 payload.img
```

此处不使用 `UEFI_KO` 变量。`uefi_jump.ko` 的模块参数是：

```text
fd_path=/data/local/tmp/uefi.fd
payload_path=/data/local/tmp/payload.img
payload_bytes=67108864
```

`fd_path` 指向 UEFI FD；`uefi_jump.ko` 本身由 `insmod` 加载。

### 3. 从源码重新构建 boot image

构建脚本需要把实际文件路径赋给变量：

```bash
export BASE_BOOT_IMG=/path/to/compatible-jump2-boot.img
export UEFI_KO=/path/to/uefi_jump-known-good.ko
export UEFI_FD=/path/to/SILICIUM_UEFI-hotjump-known-good.fd
export PAYLOAD_IMG=/path/to/payload.img

./build-boot-image.sh
```

脚本会检查两个文件，并把它们复制到 initramfs：

```text
$UEFI_FD       -> /uefi.fd
$UEFI_KO       -> /uefi_jump.ko
```

最终热跳模块使用 `fd_path=/uefi.fd` 指向固件镜像。

## 发布文件哈希

```text
SILICIUM_UEFI-hotjump-known-good.fd
68184f8a1451d08a90fafc61b1163e6e2056469bac174ebc5c71b38f23bfc406

uefi_jump-known-good.ko
bec844cf909a8032efe33b58c879ba6ff499036263c824672a47afdc9f3a71c6

camellia-preboot-uefi-v2026.10.26.img
1fc459383672b0e1c6df7687bba8342e9ec12bf230a7df98b461407190d1b939
```

## 关键区别

- `.fd` 是 UEFI 固件镜像。
- `.ko` 是 Linux 内核模块，负责从 Linux 跳入 UEFI。
- `UEFI_FD` 和 `UEFI_KO` 是构建脚本变量，不是文件格式。
- boot image 包含 FD 和 KO；单独下载的 FD/KO 用于 ADB 热跳或重新打包。
