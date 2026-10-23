#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/MemoryMapHelperLib.h>
#include <Library/SerialPortLib.h>
#include <Library/TimerLib.h>
#include <Library/FrameBufferMap.h>

#include "FrameBuffer.h"

STATIC UINT32 gFbWidth = 1088;
STATIC UINT32 gFbHeight = 2400;
STATIC UINT32 gFbPitch = 1088 * 4;
STATIC EFI_FRAME_BUFFER_POSITION mCurrentPosition;
STATIC FB_PAGE_MAP *mPageMap;

#include "Font.h"

inline
STATIC
UINT8*
DrawPixel (
  IN UINT8  *Pixels,
  IN UINT32  PixelColor,
  IN UINT32  FontScale)
{
  // Repeat Action
  for (UINT8 i = 0; i < FontScale; i++) {
    // Save Pixel Color
    UINT32 Color = PixelColor;

    // Go theu each Byte
    for (UINT8 j = 0; j < FB_BPP; j++) {
      // Update Pixel Color
      *Pixels++ = (UINT8)Color;

      // Shift Pixel Color
      Color >>= 8;
    }
  }

  return Pixels;
}

inline
STATIC
UINT8*
DrawRow (
  IN UINT8  *Pixels,
  IN UINT32  RowData,
  IN UINT32  FontScale,
  IN UINTN   Stride)
{
  // Repeat Action
  for (UINT8 i = 0; i < FontScale; i++) {
    // Save Row Data
    UINT32 Data = RowData;

    // Go thru each Pixel
    for (UINT8 j = 0; j < FONT_WIDTH; j++) {
      // Set Pixel Color
      UINT32 PixelColor = (Data & 1) ? 0xFFFFFFFF : 0x00000000;
      
      // Draw Pixel Color
      Pixels = DrawPixel (Pixels, PixelColor, FontScale);

      // Shift Row
      Data >>= 1;
    }

    // Append Stride
    Pixels += Stride;
  }
  
  return Pixels;
}

VOID
DrawGlyph (
  IN UINT8  *Pixels,
  IN UINT64  Glyph,
  IN UINT32  FontScale,
  IN UINTN   RowPitch)
{
  // Calculate Stride
  UINTN Stride = RowPitch - (FONT_WIDTH * FontScale * FB_BPP);

  // Split Glyph
  UINT32 TopGlyph    = (UINT32)(Glyph >> 32);
  UINT32 BottomGlyph = (UINT32)Glyph;

  // Set Inital Row Data
  UINT32 RowData = TopGlyph;

  // Go thru each Row
  for (UINT8 Row = 0; Row < 12; Row++) {
    // Switch Row Data
    if (Row == 6) {
      RowData = BottomGlyph;
    }

    // Draw Row
    Pixels = DrawRow (Pixels, RowData, FontScale, Stride);

    // Shift to next Row
    RowData >>= FONT_WIDTH;
  }
}

VOID
AdvanceNewLine (
  IN EFI_PHYSICAL_ADDRESS       FbBase,
  IN UINT64                     FbLength,
  IN EFI_FRAME_BUFFER_POSITION *CurrentPosition,
  IN EFI_FRAME_BUFFER_POSITION  MaxPosition)
{
  // Wait
  MicroSecondDelay (FB_DELAY_US);

  // Update Position
  CurrentPosition->XPos  = 0;
  CurrentPosition->YPos += 1;

  // Verify Y Position
  if (CurrentPosition->YPos >= MaxPosition.YPos) {
    // Clear Frame Buffer
    FbPageMapFill32 (
      mPageMap,
      0,
      0,
      (UINTN)((UINT64)gFbPitch * gFbHeight / sizeof (UINT32))
      );
    FbPageMapFlush (mPageMap, 0, FbLength);

    // Reset X & Y Position
    CurrentPosition->XPos = 0;
    CurrentPosition->YPos = 0;
  }
}

UINT8
GetFontScale ()
{
  // Get Smaller Display Dimension
  UINT32 ShorterDimension = (gFbWidth < gFbHeight) ? gFbWidth : gFbHeight;

  // Return Calculated Font Scale
  return (ShorterDimension < 426) ? 1 : (UINT8)(ShorterDimension / 426);
}

VOID
GetFbPositions (
  IN  EFI_PHYSICAL_ADDRESS        FbBase,
  IN  UINT64                      FbLength,
  IN  UINT8                       FontScale,
  OUT EFI_FRAME_BUFFER_POSITION **CurrentPosition,
  OUT EFI_FRAME_BUFFER_POSITION  *MaxPosition)
{
  EFI_FRAME_BUFFER_POSITION LocalMaxPosition;

  (VOID)FbBase;
  (VOID)FbLength;

  // Keep console state in the driver image. The frame buffer itself may
  // contain arbitrary pixels supplied by the loader, so its tail cannot be
  // used as zero-initialized cursor storage.
  *CurrentPosition = &mCurrentPosition;

  // Calculate Max Positions
  LocalMaxPosition.XPos = gFbWidth  / ((FONT_WIDTH  + 1) * FontScale);
  LocalMaxPosition.YPos = gFbHeight / ((FONT_HEIGHT - 4) * FontScale);

  // Pass Max Position
  *MaxPosition = LocalMaxPosition;
}

VOID
WriteFrameBuffer (
  IN EFI_PHYSICAL_ADDRESS FbBase,
  IN UINT64               FbLength,
  IN UINT8                Character)
{
  EFI_FRAME_BUFFER_POSITION *CurrentPosition;
  EFI_FRAME_BUFFER_POSITION  MaxPosition;
  UINT8                      GlyphBuffer[8192];
  UINTN                      GlyphPitch;
  UINTN                      GlyphBytes;
  UINTN                      Row;

  // Set Glyph Font
  STATIC CONST UINT64 GlyphFont[] = GLYPH_FONT;

  // Set Glyph Font Scale
  UINT8 GlyphFontScale = GetFontScale ();

  // Get Frame Buffer Positions
  GetFbPositions (FbBase, FbLength, GlyphFontScale, &CurrentPosition, &MaxPosition);

  // Skip Invalid ACSII Characters
  if (Character >= 127) {
    return;
  }

  // Handle Control Characters
  if (Character < 32) {
    if (Character == '\n') {
      AdvanceNewLine (FbBase, FbLength, CurrentPosition, MaxPosition);
    } else if (Character == '\r') {
      CurrentPosition->XPos = 0;
    } else if (Character == '\f') {
      CurrentPosition->XPos = 0;
      CurrentPosition->YPos = 0;
    }

    return;
  }

  // Skip leading Spaces
  if (CurrentPosition->XPos == 0 && Character == ' ') {
    return;
  }

  // Translate ACSII Character
  UINT64 Glyph = GlyphFont[Character - 32];

  // Calculate Glyph Dimension
  UINTN GlyphWidth  = (FONT_WIDTH  + 1) * GlyphFontScale;
  UINTN GlyphHeight = (FONT_HEIGHT - 4) * GlyphFontScale;
  UINT64 FbOffset;

  GlyphPitch = (FONT_WIDTH + 1) * GlyphFontScale * FB_BPP;
  GlyphBytes = FONT_WIDTH * GlyphFontScale * FB_BPP;
  if (GlyphPitch * GlyphHeight > sizeof (GlyphBuffer)) {
    return;
  }

  ZeroMem (GlyphBuffer, sizeof (GlyphBuffer));
  DrawGlyph (GlyphBuffer, Glyph, GlyphFontScale, GlyphPitch);

  FbOffset  = (UINT64)CurrentPosition->YPos * GlyphHeight * gFbPitch;
  FbOffset += (UINT64)CurrentPosition->XPos * GlyphWidth * FB_BPP;
  for (Row = 0; Row < GlyphHeight; Row++) {
    FbPageMapCopyTo (
      mPageMap,
      FbOffset + Row * gFbPitch,
      GlyphBuffer + Row * GlyphPitch,
      GlyphBytes
      );
  }
  FbPageMapFlush (mPageMap, FbOffset, GlyphHeight * gFbPitch);

  // Increase X Position
  CurrentPosition->XPos++;

  // Check Max X Position
  if (CurrentPosition->XPos >= MaxPosition.XPos) {
    AdvanceNewLine (FbBase, FbLength, CurrentPosition, MaxPosition);
  }
}

EFI_STATUS
GetFrameBufferMemory (
  OUT EFI_PHYSICAL_ADDRESS *Base,
  OUT UINT64               *Length)
{
  FB_HANDOFF *Handoff = (FB_HANDOFF *)FB_HANDOFF_ADDR;

  if (Handoff->Magic != FB_HANDOFF_MAGIC) {
    return EFI_NOT_FOUND;
  }

  gFbWidth = Handoff->Width;
  gFbHeight = Handoff->Height;
  gFbPitch = Handoff->Pitch;
  mPageMap = (FB_PAGE_MAP *)(UINTN)FB_PAGE_MAP_ADDR;
  if (!FbPageMapIsValid (mPageMap)) {
    mPageMap = NULL;
    return EFI_UNSUPPORTED;
  }

  // Pass Frame Buffer Memory
  *Base   = Handoff->Base;
  *Length = (UINT64)Handoff->Pitch * Handoff->Height;

  return EFI_SUCCESS;
}

UINTN
EFIAPI
SerialPortWrite (
  IN UINT8 *Buffer,
  IN UINTN  NumberOfBytes)
{
  EFI_STATUS           Status;
  EFI_PHYSICAL_ADDRESS FbBase;
  UINT64               FbLength;

  // Get Frame Buffer Memory
  Status = GetFrameBufferMemory (&FbBase, &FbLength);
  if (EFI_ERROR (Status)) {
    return 0;
  }

  // Get Interrupts State
  BOOLEAN InterruptState = ArmGetInterruptState ();

  // Disable Interrupts
  if (InterruptState) {
    ArmDisableInterrupts ();
  }

  // Write Debug Message to Frame Buffer
  for (UINTN i = 0; i < NumberOfBytes; i++) {
    WriteFrameBuffer (FbBase, FbLength, *Buffer++);
  }

  // Enable Interrupts
  if (InterruptState) {
    ArmEnableInterrupts ();
  }

  // Flush all physical pages backing the logical frame buffer.
  FbPageMapFlush (mPageMap, 0, FbLength);

  return NumberOfBytes;
}

UINTN
EFIAPI
SerialPortRead (
  OUT UINT8 *Buffer,
  IN  UINTN  NumberOfBytes)
{
  return 0;
}

BOOLEAN
EFIAPI
SerialPortPoll ()
{
  return FALSE;
}

EFI_STATUS
EFIAPI
SerialPortSetControl (IN UINT32 Control)
{
  return EFI_UNSUPPORTED;
}

EFI_STATUS
EFIAPI
SerialPortGetControl (OUT UINT32 *Control)
{
  return EFI_UNSUPPORTED;
}

EFI_STATUS
EFIAPI
SerialPortSetAttributes (
  IN OUT UINT64             *BaudRate,
  IN OUT UINT32             *ReceiveFifoDepth,
  IN OUT UINT32             *Timeout,
  IN OUT EFI_PARITY_TYPE    *Parity,
  IN OUT UINT8              *DataBits,
  IN OUT EFI_STOP_BITS_TYPE *StopBits)
{
  return EFI_UNSUPPORTED;
}

EFI_STATUS
EFIAPI
SerialPortInitialize ()
{
  return EFI_SUCCESS;
}
