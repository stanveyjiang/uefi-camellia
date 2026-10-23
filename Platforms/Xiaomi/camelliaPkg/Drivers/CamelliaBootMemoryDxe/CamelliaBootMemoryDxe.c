/** @file
  Publish the boot.img ramdisk and LK framebuffer described by the DTB.
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/FdtLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>
#include <Protocol/DiskIo.h>

#define DTB_POINTER_ADDR    0xA40007F8ULL
#define FB_HANDOFF_ADDR     0xA4000000ULL
#define FB_PAGE_MAP_ADDR    0xA4000100ULL
#define FB_HANDOFF_MAGIC    0x3130424649464555ULL
#define FB_PAGE_MAP_MAGIC   0x314D504649465545ULL
#define FB_WIDTH            1080U
#define FB_HEIGHT           2400U
#define FB_PITCH            4320U
#define FB_DEPTH            32U
#define RAM_BLOCK_SIZE      512U

#pragma pack(1)
typedef struct {
  UINT64 Magic;
  UINT64 Base;
  UINT32 Width;
  UINT32 Height;
  UINT32 Pitch;
  UINT32 Depth;
} FB_HANDOFF;

typedef struct {
  UINT64 Physical;
  UINT32 Pages;
  UINT32 Reserved;
} FB_RUN;

typedef struct {
  UINT64 Magic;
  UINT64 LogicalSize;
  UINT32 PageSize;
  UINT32 RunCount;
  FB_RUN Runs[1];
} FB_RUN_MAP;

typedef struct {
  VENDOR_DEVICE_PATH        Vendor;
  HARDDRIVE_DEVICE_PATH     Partition;
  EFI_DEVICE_PATH_PROTOCOL  End;
} RAM_BLOCK_DEVICE_PATH;
#pragma pack()

STATIC RAM_BLOCK_DEVICE_PATH mDevicePath = {
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
      0x45C3EAB9,
      0x7D48,
      0x4AD9,
      { 0x83, 0x03, 0xAB, 0x62, 0x15, 0xF1, 0x46, 0x10 }
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
    2048,
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

STATIC UINT8              *mRamBase;
STATIC UINT64              mRamSize;
STATIC EFI_BLOCK_IO_MEDIA  mMedia;

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
  if ((BufferSize == 0) || ((BufferSize % RAM_BLOCK_SIZE) != 0) ||
      (Lba > mMedia.LastBlock) ||
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

STATIC EFI_BLOCK_IO_PROTOCOL mBlockIo = {
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

STATIC EFI_DISK_IO_PROTOCOL mDiskIo = {
  EFI_DISK_IO_PROTOCOL_REVISION,
  RamDiskRead,
  RamDiskWrite
};

STATIC
VOID
AddMemory (
  IN EFI_PHYSICAL_ADDRESS  Base,
  IN UINT64                Size
  )
{
  EFI_STATUS                       Status;
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR  Descriptor;

  Status = gDS->GetMemorySpaceDescriptor (Base, &Descriptor);
  if (EFI_ERROR (Status)) {
    Status = gDS->AddMemorySpace (
                    EfiGcdMemoryTypeSystemMemory,
                    Base,
                    (UINTN)Size,
                    EFI_MEMORY_WB | EFI_MEMORY_WT | EFI_MEMORY_WC | EFI_MEMORY_UC
                    );
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "CAMBOOT: add memory 0x%lx+0x%lx failed %r\n",
              Base, Size, Status));
      return;
    }
  }
  Status = gDS->SetMemorySpaceAttributes (Base, (UINTN)Size, EFI_MEMORY_WB);
  DEBUG ((DEBUG_ERROR, "CAMBOOT: memory 0x%lx+0x%lx %r\n", Base, Size, Status));
}

STATIC
VOID
PublishInitrd (
  IN CONST VOID  *Fdt
  )
{
  INT32        Chosen;
  CONST VOID  *StartProp;
  CONST VOID  *EndProp;
  INT32        StartLength;
  INT32        EndLength;
  UINT64       Start;
  UINT64       End;
  EFI_HANDLE   Handle;
  EFI_STATUS   Status;

  Chosen = FdtPathOffset (Fdt, "/chosen");
  if (Chosen < 0) {
    DEBUG ((DEBUG_ERROR, "CAMBOOT: no /chosen\n"));
    return;
  }

  StartProp = FdtGetProp (Fdt, Chosen, "linux,initrd-start", &StartLength);
  EndProp   = FdtGetProp (Fdt, Chosen, "linux,initrd-end", &EndLength);
  if ((StartProp == NULL) || (EndProp == NULL) ||
      ((StartLength != 4) && (StartLength != 8)) ||
      ((EndLength != 4) && (EndLength != 8))) {
    DEBUG ((DEBUG_ERROR, "CAMBOOT: no initrd properties\n"));
    return;
  }

  Start = (StartLength == 8) ?
    Fdt64ToCpu (ReadUnaligned64 (StartProp)) :
    Fdt32ToCpu (ReadUnaligned32 (StartProp));
  End = (EndLength == 8) ?
    Fdt64ToCpu (ReadUnaligned64 (EndProp)) :
    Fdt32ToCpu (ReadUnaligned32 (EndProp));
  if ((End <= Start) || (((End - Start) % RAM_BLOCK_SIZE) != 0)) {
    DEBUG ((DEBUG_ERROR, "CAMBOOT: invalid initrd 0x%lx..0x%lx\n", Start, End));
    return;
  }

  AddMemory (Start, End - Start);
  if ((End - Start) <= (UINT64)2048 * RAM_BLOCK_SIZE) {
    DEBUG ((DEBUG_ERROR, "CAMBOOT: initrd too small 0x%lx\n", End - Start));
    return;
  }

  mRamBase       = (UINT8 *)(UINTN)(Start + (UINT64)2048 * RAM_BLOCK_SIZE);
  mRamSize       = End - Start - (UINT64)2048 * RAM_BLOCK_SIZE;
  mMedia.MediaId = 1;
  mMedia.RemovableMedia   = FALSE;
  mMedia.MediaPresent     = TRUE;
  mMedia.LogicalPartition = TRUE;
  mMedia.ReadOnly         = TRUE;
  mMedia.WriteCaching     = FALSE;
  mMedia.BlockSize        = RAM_BLOCK_SIZE;
  mMedia.IoAlign          = 1;
  mMedia.LastBlock        = mRamSize / RAM_BLOCK_SIZE - 1;
  mDevicePath.Partition.PartitionSize = mRamSize / RAM_BLOCK_SIZE;

  Handle = NULL;
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
    "CAMBOOT: initrd block 0x%lx size 0x%lx boot=%02x%02x%02x%02x %r\n",
    Start,
    End - Start,
    mRamBase[0],
    mRamBase[1],
    mRamBase[2],
    mRamBase[3],
    Status
    ));
}

STATIC
VOID
PublishFramebuffer (
  IN CONST VOID  *Fdt
  )
{
  INT32        Chosen;
  CONST VOID  *Video;
  INT32        VideoLength;
  UINT64       Base;
  UINT64       LogicalSize;
  FB_HANDOFF  *Handoff;
  FB_RUN_MAP  *Map;

  Chosen = FdtPathOffset (Fdt, "/chosen");
  if (Chosen < 0) {
    return;
  }
  Video = FdtGetProp (Fdt, Chosen, "atag,videolfb", &VideoLength);
  if ((Video == NULL) || (VideoLength < 20)) {
    DEBUG ((DEBUG_WARN, "CAMBOOT: no atag,videolfb\n"));
    return;
  }

  Base = Fdt64ToCpu (ReadUnaligned64 (Video));
  if ((Base == 0) || ((Base & 0xFFF) != 0) ||
      (Base < 0x40000000ULL) || (Base >= 0x240000000ULL)) {
    DEBUG ((DEBUG_ERROR, "CAMBOOT: invalid framebuffer base 0x%lx\n", Base));
    return;
  }

  LogicalSize = (UINT64)FB_PITCH * FB_HEIGHT;
  Handoff = (FB_HANDOFF *)(UINTN)FB_HANDOFF_ADDR;
  //
  // The same page also carries the hot-jump payload handoff at +0x780.
  // Never clear the whole page here.
  //
  ZeroMem (Handoff, sizeof (*Handoff));
  Handoff->Magic  = FB_HANDOFF_MAGIC;
  Handoff->Base   = Base;
  Handoff->Width  = FB_WIDTH;
  Handoff->Height = FB_HEIGHT;
  Handoff->Pitch  = FB_PITCH;
  Handoff->Depth  = FB_DEPTH;

  Map = (FB_RUN_MAP *)(UINTN)FB_PAGE_MAP_ADDR;
  Map->Magic       = FB_PAGE_MAP_MAGIC;
  Map->LogicalSize = LogicalSize;
  Map->PageSize    = EFI_PAGE_SIZE;
  Map->RunCount    = 1;
  Map->Runs[0].Physical = Base;
  Map->Runs[0].Pages    = (UINT32)EFI_SIZE_TO_PAGES (LogicalSize);

  DEBUG ((DEBUG_ERROR, "CAMBOOT: framebuffer 0x%lx %ux%u pitch %u\n",
          Base, FB_WIDTH, FB_HEIGHT, FB_PITCH));
}

EFI_STATUS
EFIAPI
CamelliaBootMemoryEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  UINT64  Dtb;

  Dtb = *(volatile UINT64 *)(UINTN)DTB_POINTER_ADDR;
  if ((Dtb == 0) || ((Dtb & 3) != 0) ||
      (FdtCheckHeader ((VOID *)(UINTN)Dtb) != 0)) {
    DEBUG ((DEBUG_WARN, "CAMBOOT: no valid bootloader DTB at 0x%lx\n", Dtb));
    return EFI_SUCCESS;
  }

  DEBUG ((DEBUG_ERROR, "CAMBOOT: DTB 0x%lx size 0x%x\n",
          Dtb, FdtTotalSize ((VOID *)(UINTN)Dtb)));
  PublishFramebuffer ((VOID *)(UINTN)Dtb);
  PublishInitrd ((VOID *)(UINTN)Dtb);
  return EFI_SUCCESS;
}
