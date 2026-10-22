/** @file
  Minimal camellia UEFI handoff beacon.

  This driver deliberately does not use display, storage, PMIC, keypad or BDS
  drivers. If DXE dispatch reaches this entry point, it waits five seconds and
  powers the device off through PSCI. A successful handoff is therefore
  observable as the boot logo disappearing without a reboot loop.
**/

#include <Uefi.h>

#define PSCI_SYSTEM_OFF  0x84000008ULL
#define FALLBACK_HZ      13000000ULL
#define BEACON_SECONDS   5ULL

STATIC
UINT64
ReadCounter (
  VOID
  )
{
  UINT64 Value;

  __asm__ volatile ("mrs %0, cntpct_el0" : "=r" (Value));
  return Value;
}

STATIC
UINT64
ReadCounterFrequency (
  VOID
  )
{
  UINT64 Value;

  __asm__ volatile ("mrs %0, cntfrq_el0" : "=r" (Value));
  return Value;
}

STATIC
VOID
PsciSystemOff (
  VOID
  )
{
  register UINT64 X0 __asm__ ("x0") = PSCI_SYSTEM_OFF;
  register UINT64 X1 __asm__ ("x1") = 0;
  register UINT64 X2 __asm__ ("x2") = 0;
  register UINT64 X3 __asm__ ("x3") = 0;

  __asm__ volatile (
    "smc #0"
    : "+r" (X0)
    : "r" (X1), "r" (X2), "r" (X3)
    : "memory"
    );
}

EFI_STATUS
EFIAPI
HandoffBeaconEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  UINT64 Frequency;
  UINT64 Start;
  UINT64 Timeout;

  (VOID)ImageHandle;
  (VOID)SystemTable;

  Frequency = ReadCounterFrequency ();
  if (Frequency == 0) {
    Frequency = FALLBACK_HZ;
  }

  Start   = ReadCounter ();
  Timeout = Frequency * BEACON_SECONDS;
  while ((ReadCounter () - Start) < Timeout) {
    __asm__ volatile ("yield");
  }

  PsciSystemOff ();

  for (;;) {
    __asm__ volatile ("wfi");
  }
}
