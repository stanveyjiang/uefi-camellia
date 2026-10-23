/** @file
  Publish the loader-provided payload image as a read-only RAM block device.
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>
#include <Protocol/DiskIo.h>

#define PAYLOAD_INFO_ADDR  0xA4000780ULL
#define PAYLOAD_MAGIC      0x314D415249464555ULL
#define RAM_BLOCK_SIZE     512

#pragma pack(1)
typedef struct {
  UINT64 Magic;
  UINT64 Base;
  UINT64 Size;
  UINT32 BlockSize;
  UINT32 Reserved;
} PAYLOAD_HANDOFF;

typedef struct {
  VENDOR_DEVICE_PATH        Vendor;
  HARDDRIVE_DEVICE_PATH     Partition;
  EFI_DEVICE_PATH_PROTOCOL  End;
} RAM_BLOCK_DEVICE_PATH;
#pragma pack()

STATIC RAM_BLOCK_DEVICE_PATH  mDevicePath = {
  {
    {
      HARDWARE_DEVICE_PATH,
      HW_VENDOR_DP,
      {
        (UINT8)sizeof (VENDOR_DEVICE_PATH),
        (UINT8)(sizeof (VENDOR_DEVICE_PATH) >> 8)
      }
    },
    {
      0x2C5779A0,
      0x56A4,
      0x4C57,
      { 0x8E, 0xB2, 0x4A, 0x20, 0xE6, 0xF8, 0x7D, 0x11 }
    }
  },
  {
    {
      MEDIA_DEVICE_PATH,
      MEDIA_HARDDRIVE_DP,
      {
        (UINT8)sizeof (HARDDRIVE_DEVICE_PATH),
        (UINT8)(sizeof (HARDDRIVE_DEVICE_PATH) >> 8)
      }
    },
    0,
    0,
    0,
    { 0 },
    MBR_TYPE_PCAT,
    NO_DISK_SIGNATURE
  },
  {
    END_DEVICE_PATH_TYPE,
    END_ENTIRE_DEVICE_PATH_SUBTYPE,
    { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 }
  }
};

STATIC UINT8               *mRamBase;
STATIC UINT64               mRamSize;
STATIC EFI_BLOCK_IO_MEDIA   mMedia;

STATIC
EFI_STATUS
EFIAPI
RamBlockReset (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN BOOLEAN                ExtendedVerification
  )
{
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
RamBlockRead (
  IN  EFI_BLOCK_IO_PROTOCOL  *This,
  IN  UINT32                 MediaId,
  IN  EFI_LBA                Lba,
  IN  UINTN                  BufferSize,
  OUT VOID                   *Buffer
  )
{
  if ((This == NULL) || (Buffer == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if (MediaId != mMedia.MediaId) {
    return EFI_MEDIA_CHANGED;
  }

  if ((BufferSize == 0) || ((BufferSize % RAM_BLOCK_SIZE) != 0)) {
    return EFI_BAD_BUFFER_SIZE;
  }

  if ((Lba > mMedia.LastBlock) ||
      ((BufferSize / RAM_BLOCK_SIZE) > (mMedia.LastBlock + 1 - Lba))) {
    return EFI_INVALID_PARAMETER;
  }

  CopyMem (Buffer, mRamBase + Lba * RAM_BLOCK_SIZE, BufferSize);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
RamBlockWrite (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN UINT32                 MediaId,
  IN EFI_LBA                Lba,
  IN UINTN                  BufferSize,
  IN VOID                   *Buffer
  )
{
  return EFI_WRITE_PROTECTED;
}

STATIC
EFI_STATUS
EFIAPI
RamBlockFlush (
  IN EFI_BLOCK_IO_PROTOCOL  *This
  )
{
  return EFI_SUCCESS;
}

STATIC EFI_BLOCK_IO_PROTOCOL  mBlockIo = {
  EFI_BLOCK_IO_PROTOCOL_REVISION,
  &mMedia,
  RamBlockReset,
  RamBlockRead,
  RamBlockWrite,
  RamBlockFlush
};

STATIC
EFI_STATUS
EFIAPI
RamDiskRead (
  IN  EFI_DISK_IO_PROTOCOL  *This,
  IN  UINT32                MediaId,
  IN  UINT64                Offset,
  IN  UINTN                 BufferSize,
  OUT VOID                  *Buffer
  )
{
  if ((This == NULL) || (Buffer == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  if (MediaId != mMedia.MediaId) {
    return EFI_MEDIA_CHANGED;
  }
  if ((Offset > mRamSize) || (BufferSize > mRamSize - Offset)) {
    return EFI_INVALID_PARAMETER;
  }

  CopyMem (Buffer, mRamBase + (UINTN)Offset, BufferSize);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
RamDiskWrite (
  IN EFI_DISK_IO_PROTOCOL  *This,
  IN UINT32                MediaId,
  IN UINT64                Offset,
  IN UINTN                 BufferSize,
  IN VOID                  *Buffer
  )
{
  return EFI_WRITE_PROTECTED;
}

STATIC EFI_DISK_IO_PROTOCOL  mDiskIo = {
  EFI_DISK_IO_PROTOCOL_REVISION,
  RamDiskRead,
  RamDiskWrite
};

EFI_STATUS
EFIAPI
CamelliaRamDiskEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  PAYLOAD_HANDOFF  *Handoff;
  EFI_STATUS        Status;
  EFI_HANDLE        Handle;
  UINT8             *PayloadBase;
  UINT64            PayloadSize;
  UINT32            PartitionStart;
  UINT32            PartitionSectors;

  Handoff = (PAYLOAD_HANDOFF *)(UINTN)PAYLOAD_INFO_ADDR;
  DEBUG ((DEBUG_ERROR, "CAMRAM: entry\n"));
  DEBUG ((
    DEBUG_ERROR,
    "CAMRAM: handoff magic=0x%lx base=0x%lx size=0x%lx\n",
    Handoff->Magic,
    Handoff->Base,
    Handoff->Size
    ));
  if (Handoff->Magic != PAYLOAD_MAGIC) {
    DEBUG ((DEBUG_WARN, "CAMRAM: no payload handoff\n"));
    return EFI_SUCCESS;
  }

  if ((Handoff->Base == 0) || (Handoff->Size == 0) ||
      ((Handoff->Size % RAM_BLOCK_SIZE) != 0) ||
      (Handoff->BlockSize != RAM_BLOCK_SIZE)) {
    DEBUG ((
      DEBUG_ERROR,
      "CAMRAM: invalid payload base=0x%lx size=0x%lx block=%u\n",
      Handoff->Base,
      Handoff->Size,
      Handoff->BlockSize
      ));
    return EFI_INVALID_PARAMETER;
  }

  PayloadBase      = (UINT8 *)(UINTN)Handoff->Base;
  PayloadSize      = Handoff->Size;
  PartitionStart   = 2048;
  PartitionSectors = (UINT32)(PayloadSize / RAM_BLOCK_SIZE - PartitionStart);

  //
  // The hot-jump image uses the fixed camellia MBR layout: one FAT32
  // partition at LBA 2048. Publish that partition directly and do not touch
  // payload memory in the driver entry path.
  //
  mRamBase = PayloadBase + (UINT64)PartitionStart * RAM_BLOCK_SIZE;
  mRamSize = (UINT64)PartitionSectors * RAM_BLOCK_SIZE;
  mDevicePath.Partition.PartitionNumber = 1;
  mDevicePath.Partition.PartitionStart  = PartitionStart;
  mDevicePath.Partition.PartitionSize   = PartitionSectors;
  mDevicePath.Partition.MBRType       = MBR_TYPE_PCAT;
  mDevicePath.Partition.SignatureType = NO_DISK_SIGNATURE;

  mMedia.MediaId = 1;
  mMedia.RemovableMedia   = FALSE;
  mMedia.MediaPresent     = TRUE;
  mMedia.LogicalPartition = (BOOLEAN)(PartitionSectors != 0);
  mMedia.ReadOnly         = TRUE;
  mMedia.WriteCaching     = FALSE;
  mMedia.BlockSize        = RAM_BLOCK_SIZE;
  mMedia.IoAlign          = 1;
  mMedia.LastBlock        = mRamSize / RAM_BLOCK_SIZE - 1;

  Handle = NULL;
  DEBUG ((DEBUG_ERROR, "CAMRAM: installing block device\n"));
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Handle,
                  &gEfiDevicePathProtocolGuid,
                  &mDevicePath,
                  &gEfiBlockIoProtocolGuid,
                  &mBlockIo,
                  &gEfiDiskIoProtocolGuid,
                  &mDiskIo,
                  NULL
                  );
  DEBUG ((
    DEBUG_ERROR,
    "CAMRAM: base=0x%lx size=0x%lx blocks=%lu %r\n",
    (UINT64)(UINTN)mRamBase,
    mRamSize,
    mMedia.LastBlock + 1,
    Status
    ));
  DEBUG ((
    DEBUG_ERROR,
    "CAMRAM: mbr start=%u sectors=%u direct=yes\n",
    PartitionStart,
    PartitionSectors
    ));
  DEBUG ((DEBUG_ERROR, "CAMRAM: ready\n"));
  if (!EFI_ERROR (Status)) {
    gBS->Stall (5000000);
  }
  return Status;
}
