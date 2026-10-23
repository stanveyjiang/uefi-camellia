#ifndef FRAME_BUFFER_MAP_H_
#define FRAME_BUFFER_MAP_H_

#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>

#define FB_HANDOFF_ADDR    0xA4000000ULL
#define FB_PAGE_MAP_ADDR       0xA4000100ULL
#define FB_PAGE_MAP_SIZE       0x00000680U
#define FB_PAGE_MAP_MAGIC      0x314D504649465545ULL
#define FB_HANDOFF_MAGIC       0x3130424649464555ULL
#define FB_PAGE_MAP_MAX_RUNS   100U

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
} FB_PAGE_MAP_RUN;

typedef struct {
  UINT64 Magic;
  UINT64 LogicalSize;
  UINT32 PageSize;
  UINT32 RunCount;
  FB_PAGE_MAP_RUN Runs[FB_PAGE_MAP_MAX_RUNS];
} FB_PAGE_MAP;
#pragma pack()

STATIC_ASSERT (
  sizeof (FB_PAGE_MAP) <= FB_PAGE_MAP_SIZE,
  "FB_PAGE_MAP does not fit in the reserved handoff region"
  );

STATIC
BOOLEAN
FbPageMapIsValid (
  IN FB_PAGE_MAP  *Map
  )
{
  return Map != NULL &&
         Map->Magic == FB_PAGE_MAP_MAGIC &&
         Map->PageSize != 0 &&
         Map->RunCount != 0 &&
         Map->RunCount <= FB_PAGE_MAP_MAX_RUNS;
}

STATIC
UINT8 *
FbPageMapTranslate (
  IN FB_PAGE_MAP  *Map,
  IN UINT64        Offset
  )
{
  UINT64 PageIndex;
  UINT64 RunEnd;
  UINT32 PageOffset;
  UINT32 Index;

  if (!FbPageMapIsValid (Map) || Offset >= Map->LogicalSize) {
    return NULL;
  }

  PageIndex  = Offset / Map->PageSize;
  PageOffset = (UINT32)(Offset % Map->PageSize);
  RunEnd     = 0;
  for (Index = 0; Index < Map->RunCount; Index++) {
    RunEnd += Map->Runs[Index].Pages;
    if (PageIndex < RunEnd) {
      return (UINT8 *)(UINTN)(
        Map->Runs[Index].Physical +
        (PageIndex - (RunEnd - Map->Runs[Index].Pages)) * Map->PageSize +
        PageOffset
        );
    }
  }

  return NULL;
}

STATIC
VOID
FbPageMapCopyTo (
  IN FB_PAGE_MAP  *Map,
  IN UINT64        Offset,
  IN VOID         *Buffer,
  IN UINTN         Length
  )
{
  UINT8  *Source;
  UINTN   Done;

  Source = Buffer;
  while ((Length != 0) && (Offset < Map->LogicalSize)) {
    UINT8 *Destination;
    UINTN PageRemaining;
    UINTN Chunk;

    Destination   = FbPageMapTranslate (Map, Offset);
    if (Destination == NULL) {
      break;
    }
    Done          = (UINTN)(Offset % Map->PageSize);
    PageRemaining = Map->PageSize - Done;
    Chunk         = MIN (Length, PageRemaining);
    CopyMem (Destination, Source, Chunk);
    Source += Chunk;
    Offset += Chunk;
    Length -= Chunk;
  }
}

STATIC
VOID
FbPageMapFill32 (
  IN FB_PAGE_MAP  *Map,
  IN UINT64        Offset,
  IN UINT32        Value,
  IN UINTN         Count
  )
{
  while ((Count != 0) && (Offset < Map->LogicalSize)) {
    UINT8  *Destination;
    UINTN   PageRemaining;
    UINTN   Chunk;

    Destination   = FbPageMapTranslate (Map, Offset);
    if (Destination == NULL) {
      break;
    }
    PageRemaining = Map->PageSize - (UINTN)(Offset % Map->PageSize);
    Chunk         = MIN (Count * sizeof (UINT32), PageRemaining);
    Chunk        &= ~(UINTN)(sizeof (UINT32) - 1);
    if (Chunk == 0) {
      Offset += PageRemaining;
      continue;
    }

    SetMem32 (Destination, Chunk, Value);
    Offset += Chunk;
    Count  -= Chunk / sizeof (UINT32);
  }
}

STATIC
VOID
FbPageMapFlush (
  IN FB_PAGE_MAP  *Map,
  IN UINT64        Offset,
  IN UINTN         Length
  )
{
  //
  // The scanout pages use a WRITE_THROUGH mapping. On this platform a
  // cache-maintenance operation over the high DDR aliases can fault after
  // ArmConfigureMmu(), so direct write-through stores are sufficient.
  //
  if (FbPageMapIsValid (Map) && (Map->RunCount == 1) &&
      (Offset + Length <=
       (UINT64)Map->Runs[0].Pages * Map->PageSize)) {
    //
    // The contiguous hot-jump scanout lives in low DDR and can be cached
    // normally. Clean only this run; scattered high-DDR aliases must not be
    // passed to cache maintenance.
    //
    WriteBackDataCacheRange (
      (VOID *)(UINTN)(Map->Runs[0].Physical + Offset),
      Length
      );
  }
}

STATIC
inline
BOOLEAN
FbPageMapGetContiguousBase (
  IN  FB_PAGE_MAP         *Map,
  IN  EFI_PHYSICAL_ADDRESS Base,
  IN  UINTN                Size,
  OUT EFI_PHYSICAL_ADDRESS *ContiguousBase
  )
{
  if (!FbPageMapIsValid (Map) || (Map->RunCount != 1) ||
      (Map->Runs[0].Physical != Base) ||
      ((UINT64)Map->Runs[0].Pages * Map->PageSize < Size)) {
    return FALSE;
  }

  *ContiguousBase = Map->Runs[0].Physical;
  return TRUE;
}

#endif
