#include <Library/MemoryMapLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>

#define FB_RUN_MAP_ADDR      0xA4000100ULL
#define FB_RUN_MAP_MAGIC     0x314D504649465545ULL
#define FB_RUN_MAP_MAX_RUNS  100U
#define CONTIG_FB_BASE       0x9C000000ULL
#define KERNEL_WINDOW_BASE   0x200000000ULL
#define KERNEL_WINDOW_SIZE   0x03FFF0000ULL
#define HIGH_DDR_TAIL_BASE   0x23FFF0000ULL
#define HIGH_DDR_TAIL_SIZE   0x000010000ULL

#pragma pack(1)
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
  FB_RUN Runs[FB_RUN_MAP_MAX_RUNS];
} FB_RUN_MAP;
#pragma pack()

STATIC
EFI_MEMORY_REGION_DESCRIPTOR
gMemoryDescriptor[MAX_ARM_MEMORY_REGION_DESCRIPTOR_COUNT];

STATIC
EFI_MEMORY_REGION_DESCRIPTOR
gStaticMemoryDescriptor[] = {
  // Name, Address, Length, HobOption, ResourceType, ResourceAttribute, MemoryType, ArmAttribute

  // DDR Regions
  {"UEFI Stack",         0x40000000, 0x00040000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK},
  {"UEFI FD",            0x40080000, 0x00200000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK},
  {"DXE Heap",           0x40280000, 0x03C00000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},
  {"FB Handoff",         0xA4000000, 0x00001000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_THROUGH},
  {"Boot DTB",           0x47C80000, 0x00030000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"Boot Ramdisk",       0x51100000, 0x04000000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"ATF Reserved",       0x4CE00000, 0x00060000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"TEE Reserved",       0x70000000, 0x05000000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"AP MD C SMEM",       0x76000000, 0x00C00000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"SPM Reserved",       0x77FF0000, 0x00010000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"AP MD NC SMEM",      0x78000000, 0x01000000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"XNU Fixed Window",   0x60000000, 0x05000000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"LK Framebuffer",     0x7BA10000, 0x01DF0000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_THROUGH},
  {"Display Reserved",   0x7D9B0000, 0x02250000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_THROUGH},
  {"TEE SecMem",         0x7FC00000, 0x00200000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"SSPM Reserved",      0x7FF00000, 0x000C0000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"SCP Reserved",       0x9F900000, 0x00600000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"CCCI Tag Mem",       0x9FFEF000, 0x00010000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"MD Mem Usage",       0xAE000000, 0x10000000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"TEE Log Reserved",   0xBFC00000, 0x00020000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"ATF Log Reserved",   0xBFE00000, 0x00040000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"ATF Ramdump",        0xBFF80000, 0x00080000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"Payload RAM",        0x98000000, 0x04000000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"Scanout RAM",        0x9C000000, 0x01000000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_THROUGH},
  // Register Regions
  {"GIC Distributor",    0x0C000000, 0x00040000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"GIC Redistributors", 0x0C040000, 0x00200000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"Top Ck Gen",         0x10000000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"Infra Cfg AO",       0x10001000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"Pinctrl",            0x10005000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"WatchDog Timer",     0x10007000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"AP Mixed",           0x1000C000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"PMIC Wrapper",       0x1000D000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"EMI",                0x10219000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"MSDC-0",             0x11230000, 0x00010000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"MSDC-1",             0x11240000, 0x00010000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"MSDC Top-0",         0x11C90000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"MSDC Top-1",         0x11CD0000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"Display OVL",        0x1400B000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"High DDR",           0x140000000, 0x100000000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_THROUGH}
};

VOID
GetMemoryMap (
  OUT EFI_MEMORY_REGION_DESCRIPTOR **MemoryDescriptor,
  OUT UINT8                         *MemoryDescriptorCount)
{
  FB_RUN_MAP  *RunMap;
  UINTN        Count;
  UINTN        Index;
  UINTN        Existing;
  BOOLEAN      ContiguousFb;

  Count = ARRAY_SIZE (gStaticMemoryDescriptor);
  CopyMem (
    gMemoryDescriptor,
    gStaticMemoryDescriptor,
    sizeof (gStaticMemoryDescriptor)
    );

  RunMap = (FB_RUN_MAP *)(UINTN)FB_RUN_MAP_ADDR;
  ContiguousFb =
    (RunMap->Magic == FB_RUN_MAP_MAGIC) &&
    (RunMap->RunCount == 1) &&
    (RunMap->Runs[0].Physical == CONTIG_FB_BASE);

  if (ContiguousFb) {
    //
    // The scanout is no longer in high DDR. Expose a conventional window for
    // DarwinBoot/XNU and keep the remaining high DDR reserved. With scattered
    // scanout, retain the original full high-DDR reservation below. Scattered
    // mode publishes the same window later from DXE.
    //
    for (Existing = 0; Existing < Count; Existing++) {
      if (AsciiStrCmp (gMemoryDescriptor[Existing].Name, "High DDR") == 0) {
        gMemoryDescriptor[Existing].Length = KERNEL_WINDOW_BASE -
                                            gMemoryDescriptor[Existing].Address;
        break;
      }
    }
    if ((Existing < Count) && (Count + 2 <= MAX_ARM_MEMORY_REGION_DESCRIPTOR_COUNT)) {
      EFI_MEMORY_REGION_DESCRIPTOR *Descriptor;

      Descriptor = &gMemoryDescriptor[Count++];
      ZeroMem (Descriptor, sizeof (*Descriptor));
      CopyMem (Descriptor->Name, "Kernel RAM", sizeof ("Kernel RAM"));
      Descriptor->Address           = KERNEL_WINDOW_BASE;
      Descriptor->Length            = KERNEL_WINDOW_SIZE;
      Descriptor->HobOption         = AddMem;
      Descriptor->ResourceType      = SYS_MEM;
      Descriptor->ResourceAttribute = SYS_MEM_CAP;
      Descriptor->MemoryType        = Conv;
      Descriptor->ArmAttributes     = WRITE_BACK;

      Descriptor = &gMemoryDescriptor[Count++];
      ZeroMem (Descriptor, sizeof (*Descriptor));
      CopyMem (Descriptor->Name, "High DDR Tail", sizeof ("High DDR Tail"));
      Descriptor->Address           = HIGH_DDR_TAIL_BASE;
      Descriptor->Length            = HIGH_DDR_TAIL_SIZE;
      Descriptor->HobOption         = AddMem;
      Descriptor->ResourceType      = MEM_RES;
      Descriptor->ResourceAttribute = SYS_MEM_CAP;
      Descriptor->MemoryType        = Reserv;
      Descriptor->ArmAttributes     = WRITE_THROUGH;
    }
  }

  if ((RunMap->Magic == FB_RUN_MAP_MAGIC) &&
      (RunMap->PageSize != 0) &&
      (RunMap->RunCount <= FB_RUN_MAP_MAX_RUNS)) {
    for (Index = 0;
         (Index < RunMap->RunCount) &&
         (Count < MAX_ARM_MEMORY_REGION_DESCRIPTOR_COUNT);
         Index++) {
      EFI_PHYSICAL_ADDRESS RunStart;
      EFI_PHYSICAL_ADDRESS RunEnd;
      BOOLEAN              Overlaps;
      EFI_MEMORY_REGION_DESCRIPTOR *Descriptor;

      RunStart = RunMap->Runs[Index].Physical;
      RunEnd   = RunStart +
                 (UINT64)RunMap->Runs[Index].Pages * RunMap->PageSize;
      Overlaps = FALSE;
      for (Existing = 0; Existing < Count; Existing++) {
        EFI_PHYSICAL_ADDRESS ExistingStart =
          gMemoryDescriptor[Existing].Address;
        EFI_PHYSICAL_ADDRESS ExistingEnd =
          ExistingStart + gMemoryDescriptor[Existing].Length;

        if ((RunStart < ExistingEnd) && (ExistingStart < RunEnd)) {
          Overlaps = TRUE;
          break;
        }
      }
      if (Overlaps) {
        continue;
      }

      Descriptor = &gMemoryDescriptor[Count++];
      ZeroMem (Descriptor, sizeof (*Descriptor));
      CopyMem (Descriptor->Name, "FB Run", sizeof ("FB Run"));
      Descriptor->Address           = RunStart;
      Descriptor->Length            = RunEnd - RunStart;
      Descriptor->HobOption         = AddMem;
      Descriptor->ResourceType      = SYS_MEM;
      Descriptor->ResourceAttribute = SYS_MEM_CAP;
      Descriptor->MemoryType        = Reserv;
      Descriptor->ArmAttributes     = WRITE_THROUGH;
    }
  }

  // Pass Data
  *MemoryDescriptor      = gMemoryDescriptor;
  *MemoryDescriptorCount = (UINT8)Count;
}
