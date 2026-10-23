/** @file
  Start the UniXNU application after GOP and FAT are available.
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>
#include <Protocol/DiskIo.h>
#include <Protocol/FirmwareVolume2.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>

STATIC CONST EFI_GUID  mUniXNUApplicationGuid = {
  0x9B665204, 0xA133, 0x47A2, { 0x93, 0x96, 0x9E, 0x2B, 0x37, 0x0A, 0x4E, 0x25 }
};

STATIC CONST CHAR16  mFirmwareMarker[] = L"unixnu:firmware";

#define PAYLOAD_INFO_ADDR  0xA4000780ULL
#define PAYLOAD_MAGIC      0x314D415249464555ULL
#define RAM_BLOCK_SIZE     512
#define RAM_PARTITION_LBA  2048

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
} RAM_DISK_DEVICE_PATH;
#pragma pack()

STATIC RAM_DISK_DEVICE_PATH  mRamDevicePath = {
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
      0x7B6C2D71,
      0x3B16,
      0x4A5F,
      { 0x9A, 0x55, 0x81, 0x2D, 0x9F, 0x64, 0x71, 0xE0 }
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
    1,
    RAM_PARTITION_LBA,
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
STATIC EFI_BLOCK_IO_MEDIA   mRamMedia;

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
  if ((This == NULL) || (Buffer == NULL) ||
      (MediaId != mRamMedia.MediaId) ||
      (BufferSize == 0) || ((BufferSize % RAM_BLOCK_SIZE) != 0) ||
      (Lba > mRamMedia.LastBlock) ||
      ((BufferSize / RAM_BLOCK_SIZE) > (mRamMedia.LastBlock + 1 - Lba))) {
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

STATIC EFI_BLOCK_IO_PROTOCOL  mRamBlockIo = {
  EFI_BLOCK_IO_PROTOCOL_REVISION,
  &mRamMedia,
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
  if ((This == NULL) || (Buffer == NULL) ||
      (MediaId != mRamMedia.MediaId) ||
      (Offset > mRamSize) || (BufferSize > mRamSize - Offset)) {
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

STATIC EFI_DISK_IO_PROTOCOL  mRamDiskIo = {
  EFI_DISK_IO_PROTOCOL_REVISION,
  RamDiskRead,
  RamDiskWrite
};

STATIC
EFI_STATUS
PublishRamDisk (
  VOID
  )
{
  PAYLOAD_HANDOFF  *Handoff;
  EFI_HANDLE       Handle;
  UINT64           PartitionSectors;

  Handoff = (PAYLOAD_HANDOFF *)(UINTN)PAYLOAD_INFO_ADDR;
  DEBUG ((DEBUG_ERROR, "CAMUNIXNU: ram handoff magic=0x%lx base=0x%lx size=0x%lx\n",
          Handoff->Magic, Handoff->Base, Handoff->Size));
  if ((Handoff->Magic != PAYLOAD_MAGIC) ||
      (Handoff->Base == 0) ||
      (Handoff->Size <= (UINT64)RAM_PARTITION_LBA * RAM_BLOCK_SIZE) ||
      ((Handoff->Size % RAM_BLOCK_SIZE) != 0)) {
    return EFI_NOT_FOUND;
  }

  PartitionSectors = Handoff->Size / RAM_BLOCK_SIZE - RAM_PARTITION_LBA;
  mRamBase = (UINT8 *)(UINTN)(
    Handoff->Base + (UINT64)RAM_PARTITION_LBA * RAM_BLOCK_SIZE
    );
  mRamSize = PartitionSectors * RAM_BLOCK_SIZE;
  mRamDevicePath.Partition.PartitionSize = PartitionSectors;

  mRamMedia.MediaId         = 1;
  mRamMedia.RemovableMedia  = FALSE;
  mRamMedia.MediaPresent    = TRUE;
  mRamMedia.LogicalPartition = TRUE;
  mRamMedia.ReadOnly        = TRUE;
  mRamMedia.WriteCaching    = FALSE;
  mRamMedia.BlockSize       = RAM_BLOCK_SIZE;
  mRamMedia.IoAlign         = 1;
  mRamMedia.LastBlock       = mRamSize / RAM_BLOCK_SIZE - 1;

  Handle = NULL;
  return gBS->InstallMultipleProtocolInterfaces (
                &Handle,
                &gEfiDevicePathProtocolGuid,
                &mRamDevicePath,
                &gEfiBlockIoProtocolGuid,
                &mRamBlockIo,
                &gEfiDiskIoProtocolGuid,
                &mRamDiskIo,
                NULL
                );
}

STATIC
UINTN
CountProtocolHandles (
  IN EFI_GUID  *Protocol
  )
{
  EFI_HANDLE  *Handles;
  UINTN       Count;

  if (EFI_ERROR (gBS->LocateHandleBuffer (
                       ByProtocol,
                       Protocol,
                       NULL,
                       &Count,
                       &Handles
                       )))
  {
    return 0;
  }

  FreePool (Handles);
  return Count;
}

STATIC
VOID
ReportStorage (
  VOID
  )
{
  UINTN  BlockCount;
  UINTN  DiskCount;
  UINTN  FsCount;

  BlockCount = CountProtocolHandles (&gEfiBlockIoProtocolGuid);
  DiskCount  = CountProtocolHandles (&gEfiDiskIoProtocolGuid);
  FsCount    = CountProtocolHandles (&gEfiSimpleFileSystemProtocolGuid);

  DEBUG ((
    DEBUG_ERROR,
    "\fCAMUNIXNU: storage block=%u disk=%u fs=%u\n"
    "CAMUNIXNU: halted for storage probe\n",
    (UINT32)BlockCount,
    (UINT32)DiskCount,
    (UINT32)FsCount
    ));
  gBS->Stall (1000000);
}

STATIC
VOID
ConnectStorage (
  VOID
  )
{
  EFI_HANDLE  *Handles;
  UINTN       HandleCount;
  UINTN       Index;
  UINTN       Pass;

  //
  // PartitionDxe creates an MBR child during the first pass. FAT can only
  // bind to that child on the next pass. Repeat until the storage stack is
  // complete so UniXNU can scan \EFI\UniXNU\unixnu.toml before it starts.
  //
  for (Pass = 0; Pass < 8; Pass++) {
    if (EFI_ERROR (gBS->LocateHandleBuffer (
                         AllHandles,
                         NULL,
                         NULL,
                         &HandleCount,
                         &Handles
                         )))
    {
      return;
    }

    for (Index = 0; Index < HandleCount; Index++) {
      gBS->ConnectController (Handles[Index], NULL, NULL, TRUE);
    }
    FreePool (Handles);

    if (CountProtocolHandles (&gEfiSimpleFileSystemProtocolGuid) != 0) {
      break;
    }
  }
}

EFI_STATUS
EFIAPI
CamelliaUniXNULoaderEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_HANDLE                    *Handles;
  UINTN                         HandleCount;
  UINTN                         Index;
  EFI_STATUS                    Status;
  EFI_LOADED_IMAGE_PROTOCOL     *Loaded;
  EFI_FIRMWARE_VOLUME2_PROTOCOL *Fv;
  VOID                          *ImageData;
  UINTN                         ImageSize;
  UINT32                        AuthenticationStatus;
  EFI_HANDLE                    ApplicationHandle;
  UINTN                         ExitDataSize;
  CHAR16                        *ExitData;

  Status = PublishRamDisk ();
  DEBUG ((DEBUG_ERROR, "CAMUNIXNU: publish ram disk %r\n", Status));

  ConnectStorage ();
  ReportStorage ();

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiFirmwareVolume2ProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  for (Index = 0; Index < HandleCount; Index++) {
    Status = gBS->HandleProtocol (
                    Handles[Index],
                    &gEfiFirmwareVolume2ProtocolGuid,
                    (VOID **)&Fv
                    );
    if (EFI_ERROR (Status)) {
      continue;
    }

    ImageData = NULL;
    ImageSize = 0;
    AuthenticationStatus = 0;
    Status = Fv->ReadSection (
                   Fv,
                   &mUniXNUApplicationGuid,
                   EFI_SECTION_PE32,
                   0,
                   &ImageData,
                   &ImageSize,
                   &AuthenticationStatus
                   );
    if (EFI_ERROR (Status)) {
      continue;
    }
    break;
  }
  FreePool (Handles);
  if (Index == HandleCount) {
    DEBUG ((DEBUG_ERROR, "CAMUNIXNU: application not found in FV\n"));
    return EFI_NOT_FOUND;
  }

  DEBUG ((DEBUG_ERROR, "\fCAMUNIXNU: starting application, size=0x%lx\n", ImageSize));
  ApplicationHandle = NULL;
  Status = gBS->LoadImage (
                  FALSE,
                  ImageHandle,
                  NULL,
                  ImageData,
                  ImageSize,
                  &ApplicationHandle
                  );
  FreePool (ImageData);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "CAMUNIXNU: LoadImage failed %r\n", Status));
    return Status;
  }

  //
  // The normal BDS path starts the application through a Boot#### option
  // carrying this optional data. UniXNU uses it to connect the complete
  // driver stack before scanning volumes for unixnu.toml.
  //
  Status = gBS->HandleProtocol (
                  ApplicationHandle,
                  &gEfiLoadedImageProtocolGuid,
                  (VOID **)&Loaded
                  );
  if (!EFI_ERROR (Status)) {
    Loaded->LoadOptions     = (VOID *)mFirmwareMarker;
    Loaded->LoadOptionsSize = sizeof (mFirmwareMarker);
  } else {
    DEBUG ((DEBUG_ERROR, "CAMUNIXNU: no LoadedImage protocol %r\n", Status));
  }

  ExitDataSize = 0;
  ExitData = NULL;
  Status = gBS->StartImage (ApplicationHandle, &ExitDataSize, &ExitData);
  DEBUG ((DEBUG_ERROR, "CAMUNIXNU: StartImage returned %r\n", Status));
  return Status;
}
