#include <Library/DebugLib.h>
#include <Library/FrameBufferBltLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/MemoryMapHelperLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/PcdLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/FrameBufferMap.h>

#include <Configuration/BootDevices.h>

//
// Global Variables
//
STATIC FRAME_BUFFER_CONFIGURE *FrameBufferConfiguration;
STATIC UINT32 FrameBufferPitch;
STATIC FB_PAGE_MAP *FrameBufferPageMap;
STATIC VOID *FrameBufferShadow;
STATIC EFI_PHYSICAL_ADDRESS FrameBufferDirectBase;
STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL mFrameBuffer;

EFI_STATUS
FrameBufferQueryMode (
  IN  EFI_GRAPHICS_OUTPUT_PROTOCOL          *This,
  IN  UINT32                                 ModeNumber,
  OUT UINTN                                 *SizeOfInfo,
  OUT EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info)
{
  EFI_STATUS Status;

  // Allocate Memory
  Status = gBS->AllocatePool (EfiBootServicesData, sizeof (EFI_GRAPHICS_OUTPUT_MODE_INFORMATION), (VOID *)Info);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // Pass Info Size
  *SizeOfInfo = sizeof (EFI_GRAPHICS_OUTPUT_MODE_INFORMATION);

  // Pass Frame Buffer Infos
  (*Info)->Version              = This->Mode->Info->Version;
  (*Info)->HorizontalResolution = This->Mode->Info->HorizontalResolution;
  (*Info)->VerticalResolution   = This->Mode->Info->VerticalResolution;
  (*Info)->PixelFormat          = This->Mode->Info->PixelFormat;
  (*Info)->PixelsPerScanLine    = This->Mode->Info->PixelsPerScanLine;

  return EFI_SUCCESS;
}

EFI_STATUS
FrameBufferSetMode (
  IN EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
  IN UINT32                        ModeNumber)
{
  return EFI_SUCCESS;
}

EFI_STATUS
FrameBufferGopBlt (
  IN EFI_GRAPHICS_OUTPUT_PROTOCOL      *This,
  IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL     *BltBuffer,
  IN EFI_GRAPHICS_OUTPUT_BLT_OPERATION  BltOperation OPTIONAL,
  IN UINTN                              SourceX,
  IN UINTN                              SourceY,
  IN UINTN                              DestinationX,
  IN UINTN                              DestinationY,
  IN UINTN                              Width,
  IN UINTN                              Height,
  IN UINTN                              Delta OPTIONAL)
{
  EFI_STATUS Status;
  EFI_TPL    Tpl;
  UINTN      Row;
  UINTN      BytesPerPixel;
  UINTN      RowsCopied;
  UINTN      LastRowBytes;
  UINTN      DirectOffset;
  UINTN      DirectLength;

  // Raise TPL Level
  Tpl = gBS->RaiseTPL (TPL_NOTIFY);

  // Draw to Frame Buffer
  Status = FrameBufferBlt (FrameBufferConfiguration, BltBuffer, BltOperation, SourceX, SourceY, DestinationX, DestinationY, Width, Height, Delta);

  if (!EFI_ERROR (Status) && (FrameBufferPageMap != NULL) &&
      (FrameBufferShadow != NULL) &&
      ((BltOperation == EfiBltVideoFill) ||
       (BltOperation == EfiBltBufferToVideo) ||
       (BltOperation == EfiBltVideoToVideo))) {
    BytesPerPixel = 4;
    RowsCopied = 0;
    LastRowBytes = 0;
    for (Row = 0; Row < Height; Row++) {
      UINTN DestinationOffset;
      UINTN RowBytes;

      if (DestinationY + Row >= mFrameBuffer.Mode->Info->VerticalResolution ||
          DestinationX >= mFrameBuffer.Mode->Info->HorizontalResolution) {
        break;
      }
      RowBytes = MIN (
                   Width * BytesPerPixel,
                   (UINTN)(mFrameBuffer.Mode->Info->HorizontalResolution -
                           DestinationX) * BytesPerPixel
                   );
      DestinationOffset =
        (DestinationY + Row) * FrameBufferPitch + DestinationX * BytesPerPixel;
      if (FrameBufferDirectBase != 0) {
        CopyMem (
          (UINT8 *)(UINTN)FrameBufferDirectBase + DestinationOffset,
          (UINT8 *)FrameBufferShadow + DestinationOffset,
          RowBytes
          );
      } else {
        FbPageMapCopyTo (
          FrameBufferPageMap,
          DestinationOffset,
          (UINT8 *)FrameBufferShadow + DestinationOffset,
          RowBytes
          );
      }
      RowsCopied++;
      LastRowBytes = RowBytes;
    }

    if (RowsCopied != 0) {
      DirectOffset =
        DestinationY * FrameBufferPitch + DestinationX * BytesPerPixel;
      DirectLength =
        (RowsCopied - 1) * FrameBufferPitch + LastRowBytes;
      if (FrameBufferDirectBase != 0) {
        WriteBackDataCacheRange (
          (VOID *)(UINTN)(FrameBufferDirectBase + DirectOffset),
          DirectLength
          );
      } else {
        FbPageMapFlush (FrameBufferPageMap, DirectOffset, DirectLength);
      }
    }
  }

  // Restore TPL Level
  gBS->RestoreTPL (Tpl);

  return Status;
}

STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL mFrameBuffer = {
  FrameBufferQueryMode,
  FrameBufferSetMode,
  FrameBufferGopBlt,
  NULL
};

EFI_STATUS
GetFrameBufferInfos (
  OUT EFI_PHYSICAL_ADDRESS *BaseAddress,
  OUT UINT32               *Width,
  OUT UINT32               *Height,
  OUT UINT32               *ColorDepth)
{
  FB_HANDOFF *Handoff = (FB_HANDOFF *)FB_HANDOFF_ADDR;

  if (Handoff->Magic != FB_HANDOFF_MAGIC) {
    DEBUG ((EFI_D_ERROR, "FB handoff magic mismatch: 0x%llx\n", Handoff->Magic));
    return EFI_NOT_FOUND;
  }

  // Get Frame Buffer Details
  *BaseAddress = Handoff->Base;
  *Width       = Handoff->Width;
  *Height      = Handoff->Height;
  *ColorDepth  = Handoff->Depth;
  FrameBufferPitch = Handoff->Pitch;

  // Verify Frame Buffer Resolution
  if (*Width < 640 || *Height < 475) {
    DEBUG ((EFI_D_ERROR, "Frame Buffer Resolution is too Small!\n"));
    DEBUG ((EFI_D_ERROR, "Minimal Supported Resolution = 640x475\n"));

    return EFI_UNSUPPORTED;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
AllocateGopMemory ()
{
  EFI_STATUS Status;

  // Allocate Memory
  Status = gBS->AllocatePool (EfiBootServicesData, sizeof (EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE), (VOID *)&mFrameBuffer.Mode);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Failed to Allocate Memory for GOP Mode! Status = %r\n", Status));
    return Status;
  }

  // Clear Memory
  ZeroMem (mFrameBuffer.Mode, sizeof (EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE));

  // Allocate Memory
  Status = gBS->AllocatePool (EfiBootServicesData, sizeof (EFI_GRAPHICS_OUTPUT_MODE_INFORMATION), (VOID *)&mFrameBuffer.Mode->Info);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Failed to Allocate Memory for GOP Info! Status = %r\n", Status));
    return Status;
  }

  // Clear Memory
  ZeroMem (mFrameBuffer.Mode->Info, sizeof (EFI_GRAPHICS_OUTPUT_MODE_INFORMATION));

  return EFI_SUCCESS;
}

EFI_STATUS
SetFrameBufferModeDetails (
  IN EFI_PHYSICAL_ADDRESS BaseAddress,
  IN UINT32               Width,
  IN UINT32               Height,
  IN UINT32               ColorDepth)
{
  // Set Info Size
  mFrameBuffer.Mode->SizeOfInfo = sizeof (EFI_GRAPHICS_OUTPUT_MODE_INFORMATION);

  // Set Mode Infos
  mFrameBuffer.Mode->MaxMode = 1;
  mFrameBuffer.Mode->Mode    = 0;

  // Set Frame Buffer Range
  mFrameBuffer.Mode->FrameBufferBase = BaseAddress;
  mFrameBuffer.Mode->FrameBufferSize = (UINT64)FrameBufferPitch * Height;

  // Set Mode Info Version
  mFrameBuffer.Mode->Info->Version = 0;

  // Set Frame Buffer Infos
  mFrameBuffer.Mode->Info->HorizontalResolution = Width;
  mFrameBuffer.Mode->Info->VerticalResolution   = Height;
  mFrameBuffer.Mode->Info->PixelsPerScanLine    = FrameBufferPitch / (ColorDepth / 8);

  // Set Pixel Format
  switch (ColorDepth) {
    case 32:
      mFrameBuffer.Mode->Info->PixelFormat = PixelBlueGreenRedReserved8BitPerColor;
      break;

    case 24:
      mFrameBuffer.Mode->Info->PixelFormat                   = PixelBitMask;
      mFrameBuffer.Mode->Info->PixelInformation.RedMask      = 0x00FF0000;
      mFrameBuffer.Mode->Info->PixelInformation.GreenMask    = 0x0000FF00;
      mFrameBuffer.Mode->Info->PixelInformation.BlueMask     = 0x000000FF;
      mFrameBuffer.Mode->Info->PixelInformation.ReservedMask = 0x00000000;
      break;

    case 16:
      mFrameBuffer.Mode->Info->PixelFormat                   = PixelBitMask;
      mFrameBuffer.Mode->Info->PixelInformation.RedMask      = 0x0000F800;
      mFrameBuffer.Mode->Info->PixelInformation.GreenMask    = 0x000007E0;
      mFrameBuffer.Mode->Info->PixelInformation.BlueMask     = 0x0000001F;
      mFrameBuffer.Mode->Info->PixelInformation.ReservedMask = 0x00000000;
      break;

    default:
      DEBUG ((EFI_D_ERROR, "Color Depth %u is not Supported!\n", ColorDepth));
      return EFI_UNSUPPORTED;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
CreateFrameBufferConfig (IN EFI_PHYSICAL_ADDRESS BaseAddress)
{
  EFI_STATUS Status;

  // Set Configuration Size
  STATIC UINTN FrameBufferConfigurationSize = 0;

  // Get Frame Buffer Configuration Size
  FrameBufferBltConfigure ((VOID *)(UINTN)BaseAddress, mFrameBuffer.Mode->Info, FrameBufferConfiguration, &FrameBufferConfigurationSize);

  // Allocate Memory
  FrameBufferConfiguration = AllocatePool (FrameBufferConfigurationSize);
  if (FrameBufferConfiguration == NULL) {
    DEBUG ((EFI_D_ERROR, "Failed to Allocate Memory for the Frame Buffer Configuration!\n"));
    return EFI_OUT_OF_RESOURCES;
  }

  // Create FrameBufferBltLib Configuration
  Status = FrameBufferBltConfigure ((VOID *)(UINTN)BaseAddress, mFrameBuffer.Mode->Info, FrameBufferConfiguration, &FrameBufferConfigurationSize);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Failed to Create the Frame Buffer Configuration! Status = %r\n", Status));
    return Status;
  }

  return EFI_SUCCESS;
}

VOID
FreeMemoryBuffers ()
{
  if (FrameBufferShadow != NULL) {
    gBS->FreePool (FrameBufferShadow);
    FrameBufferShadow = NULL;
  }

  // Free Buffer
  if ((mFrameBuffer.Mode != NULL) && (mFrameBuffer.Mode->Info != NULL)) {
    gBS->FreePool (mFrameBuffer.Mode->Info);
  }

  // Free Buffer
  if (mFrameBuffer.Mode != NULL) {
    gBS->FreePool (mFrameBuffer.Mode);
  }

  // Free Buffer
  if (FrameBufferConfiguration != NULL) {
    FreePool (FrameBufferConfiguration);
    FrameBufferConfiguration = NULL;
  }
}

EFI_STATUS
EFIAPI
RegisterFrameBuffer (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_STATUS           Status;
  EFI_PHYSICAL_ADDRESS BaseAddress;
  UINT32               Width;
  UINT32               Height;
  UINT32               ColorDepth;
  UINTN                FrameBufferSize;
  EFI_PHYSICAL_ADDRESS DisplayBase;
  EFI_PHYSICAL_ADDRESS ContiguousBase;
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR Gcd;
  BOOLEAN              FramebufferIsContiguous;

  // Get Frame Buffer Infos
  Status = GetFrameBufferInfos (&BaseAddress, &Width, &Height, &ColorDepth);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  FrameBufferPageMap = (FB_PAGE_MAP *)(UINTN)FB_PAGE_MAP_ADDR;
  if (!FbPageMapIsValid (FrameBufferPageMap)) {
    DEBUG ((EFI_D_ERROR, "FB page map magic/size mismatch\n"));
    FrameBufferPageMap = NULL;
    return EFI_UNSUPPORTED;
  }

  // Map the (dynamically allocated) frame buffer into the GCD space so the
  // MMU makes it accessible; it is not part of the static platform memory map.
  FrameBufferSize = (UINTN)FrameBufferPitch * Height;
  FrameBufferSize = (FrameBufferSize + EFI_PAGE_SIZE - 1) & ~(EFI_PAGE_SIZE - 1);

  Status = gDS->GetMemorySpaceDescriptor (BaseAddress, &Gcd);
  if (EFI_ERROR (Status)) {
    Status = gDS->AddMemorySpace (EfiGcdMemoryTypeSystemMemory,
                                  BaseAddress, FrameBufferSize,
                                  EFI_MEMORY_WT);
    if (EFI_ERROR (Status)) {
      DEBUG ((EFI_D_ERROR, "AddMemorySpace for frame buffer failed: %r\n", Status));
      return Status;
    }
  }

  DEBUG ((
    EFI_D_WARN,
    "FB base 0x%lx size 0x%lx gcd type %u attr 0x%lx cap 0x%lx\n",
    BaseAddress,
    FrameBufferSize,
    Gcd.GcdMemoryType,
    Gcd.Attributes,
    Gcd.Capabilities
    ));

  if ((Gcd.Attributes & EFI_CACHE_ATTRIBUTE_MASK) != EFI_MEMORY_WT) {
    Status = gDS->SetMemorySpaceAttributes (BaseAddress, FrameBufferSize,
                                            EFI_MEMORY_WT);
    if (EFI_ERROR (Status)) {
      DEBUG ((EFI_D_ERROR, "SetMemorySpaceAttributes for frame buffer failed: %r\n", Status));
    }
  }

  FramebufferIsContiguous = FbPageMapGetContiguousBase (
                              FrameBufferPageMap,
                              BaseAddress,
                              FrameBufferSize,
                              &ContiguousBase
                              );

  //
  // UEFI graphics consumers draw into a linear shadow. Each completed Blt is
  // synchronized into the run map, which also covers the contiguous case.
  //
  Status = gBS->AllocatePool (EfiBootServicesData, FrameBufferSize,
                              &FrameBufferShadow);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Failed to allocate FB shadow: %r\n", Status));
    FrameBufferShadow = NULL;
    return Status;
  }
  ZeroMem (FrameBufferShadow, FrameBufferSize);
  DisplayBase = (EFI_PHYSICAL_ADDRESS)(UINTN)FrameBufferShadow;

  // Allocate Memory
  Status = AllocateGopMemory ();
  if (EFI_ERROR (Status)) {
    goto cleanup;
  }

  // Set Frame Buffer Mode Details
  Status = SetFrameBufferModeDetails (DisplayBase, Width, Height, ColorDepth);
  if (EFI_ERROR (Status)) {
    goto cleanup;
  }
  if (FramebufferIsContiguous) {
    //
    // DarwinBoot receives FrameBufferBase through GOP. Publish the real
    // contiguous scanout address while keeping Blt on the shadow above.
    //
    mFrameBuffer.Mode->FrameBufferBase = ContiguousBase;
    FrameBufferDirectBase = ContiguousBase;
    DEBUG ((EFI_D_WARN, "FB direct handoff base 0x%lx\n", ContiguousBase));
  }

  // Create Frame Buffer Config
  Status = CreateFrameBufferConfig (DisplayBase);
  if (EFI_ERROR (Status)) {
    goto cleanup;
  }

  // Register GOP Protocol
  Status = gBS->InstallMultipleProtocolInterfaces (&ImageHandle, &gEfiDevicePathProtocolGuid, &DisplayDevicePath, &gEfiGraphicsOutputProtocolGuid, &mFrameBuffer, NULL);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Failed to Register GOP Protocol!\n"));
    ASSERT_EFI_ERROR (Status);
  }

  return EFI_SUCCESS;

cleanup:
  // Free Memory Buffers
  FreeMemoryBuffers ();

  return Status;
}
