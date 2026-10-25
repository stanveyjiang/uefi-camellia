#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
KERNEL_BOOT=${KERNEL_BOOT:-${BASE_BOOT_IMG:-}}
UEFI_KO=${UEFI_KO:-}
UEFI_FD=${UEFI_FD:-$ROOT/dist/SILICIUM_UEFI-hotjump.fd}
PAYLOAD_IMG=${PAYLOAD_IMG:-}
OUT=${OUT:-$ROOT/dist/bootimage}

need_file()
{
	if [ ! -f "$1" ]; then
		echo "missing file: $1" >&2
		exit 1
	fi
}

if [ -z "$KERNEL_BOOT" ]; then
	echo "set BASE_BOOT_IMG to a compatible camellia boot.img" >&2
	exit 1
fi
if [ -z "$UEFI_KO" ]; then
	echo "set UEFI_KO to the uefi_jump.ko matching UEFI_FD" >&2
	exit 1
fi
if [ -z "$PAYLOAD_IMG" ]; then
	echo "set PAYLOAD_IMG to the 64 MiB raw payload store" >&2
	exit 1
fi

need_file "$KERNEL_BOOT"
need_file "$UEFI_KO"
need_file "$UEFI_FD"
need_file "$PAYLOAD_IMG"

payload_size=$(stat -c %s "$PAYLOAD_IMG")
if [ "$payload_size" -ne 67108864 ]; then
	echo "payload must be exactly 64 MiB, got $payload_size" >&2
	exit 1
fi

mkdir -p "$ROOT/BootImage/build" "$OUT"
make -C "$ROOT/BootImage" clean all

DRM_LINEAR=${DRM_LINEAR:-$ROOT/BootImage/build/drm_linear}
need_file "$DRM_LINEAR"

python3 - "$KERNEL_BOOT" "$OUT" <<'PY'
import os
import struct
import sys

source, output = sys.argv[1], sys.argv[2]
data = open(source, "rb").read()

if data[:8] != b"ANDROID!":
    raise SystemExit("source boot image has no ANDROID! header")

page = struct.unpack_from("<I", data, 36)[0]
kernel_size = struct.unpack_from("<I", data, 8)[0]
ramdisk_size = struct.unpack_from("<I", data, 16)[0]
header_size = struct.unpack_from("<I", data, 1644)[0]
dtb_size = struct.unpack_from("<I", data, 1648)[0]

kernel_offset = page
ramdisk_offset = kernel_offset + ((kernel_size + page - 1) // page) * page
dtb_offset = ramdisk_offset + ((ramdisk_size + page - 1) // page) * page

segments = (
    ("kernel", kernel_offset, kernel_size),
    ("dtb", dtb_offset, dtb_size),
)
for name, offset, size in segments:
    if size <= 0:
        raise SystemExit(f"{name} segment is empty")
    path = os.path.join(output, name)
    with open(path, "wb") as handle:
        handle.write(data[offset:offset + size])
    print(f"{name}: offset={offset} size={size}")

if header_size != 1660:
    raise SystemExit(f"unexpected boot header size: {header_size}")
PY

STAGE="$OUT/rootfs"
if [ -d "$STAGE" ]; then
	find "$STAGE" -mindepth 1 -delete
fi
mkdir -p "$STAGE/dev/dri" "$STAGE/proc" "$STAGE/sys" "$STAGE/tmp"

install -m 0755 "$ROOT/BootImage/build/init" "$STAGE/init"
install -m 0755 "$DRM_LINEAR" "$STAGE/drm_linear"
install -m 0644 "$UEFI_KO" "$STAGE/uefi_jump.ko"
install -m 0644 "$UEFI_FD" "$STAGE/uefi.fd"

cat > "$STAGE/build-info.txt" <<EOF
source_boot=$KERNEL_BOOT
drm_linear=$DRM_LINEAR
uefi_jump=$UEFI_KO
uefi_fd=$UEFI_FD
payload=$PAYLOAD_IMG
loader_check_args=regs=1
copy_args=fd_path=/uefi.fd jump=0 contig_fb=0 hb=0
mapcheck_args=fd_path=/uefi.fd jump=0 contig_fb=0 hb=0 mapcheck=1
contig_copy_args=fd_path=/uefi.fd jump=0 contig_fb=1 hb=0
mapdump_args=fd_path=/uefi.fd jump=0 contig_fb=0 hb=0 mapdump=1
jump_args=fd_path=/uefi.fd payload_path=/dev/block/sdc31 payload_bytes=67108864 jump=1 contig_fb=0 hb=0
status_colors=red:drm,green:mapdump-start,cyan:mapdump-pass,magenta:error
EOF

(
	cd "$STAGE"
	find . -print0 |
		cpio --null --create --format=newc --owner=0:0 2>/dev/null |
		gzip -n -9 > "$OUT/initramfs.cpio.gz"
)

python3 "$ROOT/Resources/Scripts/mkbootimg-camellia.py" \
	--kernel "$OUT/kernel" \
	--ramdisk "$OUT/initramfs.cpio.gz" \
	--dtb "$OUT/dtb" \
	--output "$OUT/camellia-preboot-uefi-diag.img" \
	--verify

sha256sum \
	"$OUT/camellia-preboot-uefi-diag.img" \
	"$OUT/initramfs.cpio.gz" \
	"$OUT/kernel" \
	"$OUT/dtb" \
	"$STAGE/init" \
	"$STAGE/uefi.fd" \
	"$STAGE/uefi_jump.ko" \
	"$STAGE/drm_linear" \
	"$PAYLOAD_IMG" > "$OUT/SHA256SUMS.txt"

printf '\nArtifact: %s\n' "$OUT/camellia-preboot-uefi-diag.img"
stat -c 'Size: %s bytes' "$OUT/camellia-preboot-uefi-diag.img"
