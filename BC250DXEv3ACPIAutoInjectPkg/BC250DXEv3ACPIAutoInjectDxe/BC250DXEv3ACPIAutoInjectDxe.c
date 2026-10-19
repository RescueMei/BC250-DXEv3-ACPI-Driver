#include <Uefi.h>

#include <Guid/Acpi.h>
#include <Guid/EventGroup.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/PciSegmentLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/AcpiTable.h>

#include "AcpiTableBlobs.h"

//
// This DXE driver injects ACPI tables for the active number of physical cores.
//
// Driver execution is controlled by the shared "ACPI Patch" option (the
// AcpiPatch field of MeiMeiDXEv3AcpiVar) exposed by the BC250-DXEv3-Menu-Driver.
//
// Operational flow to unlock cores:
//   1. Read the menu driver's MeiMeiDXEv3AcpiVar variable and require the
//      AcpiPatch field to be non-zero. Exit if disabled or unavailable.
//   2. Read SMN 0x0115A870 through the host bridge SMN index/data window.
//   3. Popcount the low byte. Firmware compacts enabled SMT threads into P000..Pxxx.
//   4. Install the common tables and one pair table for each enabled core.
//

// Host bridge PCI location used to access AMD's SMN index/data window.
#define BC250_HOST_PCI_SEGMENT      0
#define BC250_HOST_PCI_BUS          0
#define BC250_HOST_PCI_DEVICE       0
#define BC250_HOST_PCI_FUNCTION     0

// PCI configuration-space offsets that expose the SMN indirect access window.
#define SMN_INDEX_OFFSET            0xB8
#define SMN_DATA_OFFSET             0xBC

// Core-mask register containing one enable bit per physical core.
#define MASK_REG                    0x0115A870U

//
// Shared DXEv3 configuration variable owned by the BC250-DXEv3-Menu-Driver.
// Only the single-byte AcpiPatch field (offset 0) is consumed.
//
#define MEIMEIDXEV3_ACPI_VAR_NAME   L"MeiMeiDXEv3AcpiVar"

typedef struct {
  UINT8  AcpiPatch;
} ACPI_CONFIG;

STATIC EFI_GUID mMeiMeiDXEv3ConfigVarGuid = {
  0x49CC168D, 0xE8B0, 0x4613, { 0xA8, 0x07, 0x16, 0x96, 0x99, 0x86, 0x72, 0x6F }
};

#define BC250_PCI_SEGMENT_ADDRESS(Offset) \
  PCI_SEGMENT_LIB_ADDRESS (BC250_HOST_PCI_SEGMENT, BC250_HOST_PCI_BUS, BC250_HOST_PCI_DEVICE, BC250_HOST_PCI_FUNCTION, (Offset))

// Tracks the protocol notification event and remembers whether this boot has
// already had its ACPI override tables installed.
typedef struct {
  EFI_EVENT   Event;
  VOID        *Registration;
  BOOLEAN     TablesInstalled;
  UINTN       InstalledKeyCount;
  UINTN       InstalledKeys[BC250_MAX_ACPI_TABLES];
} BC250_ACPI_AUTO_INJECT_CONTEXT;

// Single global context for this one-shot DXE driver instance.
STATIC BC250_ACPI_AUTO_INJECT_CONTEXT mAcpiInjectContext = {
  NULL,
  NULL,
  FALSE,
  0,
  { 0 }
};

/**
  Read a 32-bit value from an SMN register through the host bridge PCI config
  index/data pair.

  @param[in] Register  SMN register address.

  @return 32-bit value read from the addressed SMN register.
**/
STATIC
UINT32
SmnRead32 (
  IN UINT32  Register
  )
{
  PciSegmentWrite32 (BC250_PCI_SEGMENT_ADDRESS (SMN_INDEX_OFFSET), Register);
  return PciSegmentRead32 (BC250_PCI_SEGMENT_ADDRESS (SMN_DATA_OFFSET));
}

STATIC
UINT8
CountEnabledCores (
  IN UINT8  Mask
  )
{
  UINT8 Count;

  Count = 0;
  while (Mask != 0) {
    Count++;
    Mask = (UINT8)(Mask & (UINT8)(Mask - 1));
  }
  return Count;
}

/**
  Read the ACPI Patch gate from the shared MeiMeiDXEv3AcpiVar variable.

  The variable is owned by the BC250-DXEv3-Menu-Driver.  When the variable is
  absent the menu driver's defaults (all zeros) apply, so this driver treats a
  missing variable as disabled and fails closed.

  @param[out] AcpiPatchEnabled  TRUE when the AcpiPatch field is non-zero.

  @retval EFI_SUCCESS  The gate was read (AcpiPatchEnabled reflects the setting).
  @retval others       The variable is absent or malformed (caller treats as disabled).
**/
STATIC
EFI_STATUS
GetAcpiPatchEnabled (
  OUT BOOLEAN  *AcpiPatchEnabled,
  OUT UINT8    *AcpiPatch
  )
{
  EFI_STATUS   Status;
  UINTN        DataSize;
  ACPI_CONFIG  Config;

  *AcpiPatchEnabled = FALSE;
  if (AcpiPatch != NULL) {
    *AcpiPatch = 0;
  }
  ZeroMem (&Config, sizeof (Config));

  DataSize = sizeof (Config);
  Status = gRT->GetVariable (
                  MEIMEIDXEV3_ACPI_VAR_NAME,
                  &mMeiMeiDXEv3ConfigVarGuid,
                  NULL,
                  &DataSize,
                  &Config
                  );
  if (Status == EFI_NOT_FOUND) {
    DEBUG ((DEBUG_WARN, "BC250DXEv3ACPIAutoInjectDxe: %s not found, ACPI patch disabled\n",
            MEIMEIDXEV3_ACPI_VAR_NAME));
    return EFI_NOT_FOUND;
  }

  if (Status == EFI_BUFFER_TOO_SMALL) {
    DEBUG ((DEBUG_WARN, "BC250DXEv3ACPIAutoInjectDxe: %s smaller than ACPI_CONFIG, ACPI patch disabled\n",
            MEIMEIDXEV3_ACPI_VAR_NAME));
    return EFI_BAD_BUFFER_SIZE;
  }

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "BC250DXEv3ACPIAutoInjectDxe: failed to read %s: %r\n",
            MEIMEIDXEV3_ACPI_VAR_NAME, Status));
    return Status;
  }

  if (AcpiPatch != NULL) {
    *AcpiPatch = Config.AcpiPatch;
  }
  *AcpiPatchEnabled = (BOOLEAN)(Config.AcpiPatch != 0U);
  DEBUG ((DEBUG_INFO, "BC250DXEv3ACPIAutoInjectDxe: AcpiPatch=%u\n", Config.AcpiPatch));
  return EFI_SUCCESS;
}

/**
  Install a selected set of embedded ACPI table blobs.

  Each blob is handed directly to EFI_ACPI_TABLE_PROTOCOL and, on success, the
  returned table key is saved in the driver context for diagnostic visibility.

  @param[in]     AcpiTable   Located ACPI table protocol instance.
  @param[in]     Blobs       Array of ACPI blobs to install.
  @param[in]     BlobCount   Number of entries in Blobs.
  @param[in,out] Context     Driver state used to record installed table keys.

  @retval EFI_SUCCESS  All tables were installed successfully.
  @retval others       A table installation failed.
**/
STATIC
EFI_STATUS
InstallAcpiBlobSet (
  IN EFI_ACPI_TABLE_PROTOCOL   *AcpiTable,
  IN CONST BC250_ACPI_BLOB     *Blobs,
  IN UINTN                     BlobCount,
  IN BC250_ACPI_AUTO_INJECT_CONTEXT *Context
  )
{
  EFI_STATUS Status;
  UINTN      Index;
  UINTN      TableKey;

  for (Index = 0; Index < BlobCount; ++Index) {
    TableKey = 0;
    Status = AcpiTable->InstallAcpiTable (
                          AcpiTable,
                          (VOID *)Blobs[Index].Data,
                          Blobs[Index].Size,
                          &TableKey
                          );
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "BC250DXEv3ACPIAutoInjectDxe: failed to install %a: %r\n", Blobs[Index].Name, Status));
      return Status;
    }

    DEBUG ((DEBUG_INFO, "BC250DXEv3ACPIAutoInjectDxe: installed %a (%u bytes), key=%u\n", Blobs[Index].Name, (UINT32)Blobs[Index].Size, (UINT32)TableKey));
    if (Context->InstalledKeyCount < ARRAY_SIZE (Context->InstalledKeys)) {
      Context->InstalledKeys[Context->InstalledKeyCount++] = TableKey;
    }
  }

  return EFI_SUCCESS;
}

//
// DSDT namespace repair.
//
// Firmware SSDT1 ("AMD CPU") scopes \_PR.C000..C00F as Devices and attaches
// _CST/_CSD, but the DSDT only defines Processor(P000..P00F), so the SSDT fails
// to load with AE_NOT_FOUND.  Append inert Device stubs to a copy of the DSDT
// and point the FADT at the copy, so SSDT1 resolves.  These stubs have a custom
// HID (no driver binds, no extra CPUs); the functional P/C-state tables still
// come from the P-processor SSDTs installed below.
//
#define BC250_ACPI_HEADER_SIZE       36U
#define BC250_FADT_DSDT_OFFSET       40U
#define BC250_FADT_X_DSDT_OFFSET     140U
#define BC250_FADT_X_DSDT_MIN_LEN    148U
#define BC250_DSDT_MAX_SIZE          (1024U * 1024U)

STATIC BOOLEAN mDsdtCstubPatched = FALSE;

STATIC
VOID *
FindAcpiRsdp (
  VOID
  )
{
  UINTN Index;
  VOID  *Rsdp10;

  Rsdp10 = NULL;
  for (Index = 0; Index < gST->NumberOfTableEntries; ++Index) {
    //
    // The OS consumes the ACPI 2.0+ RSDP (XSDT).  The configuration table can
    // also carry the legacy 1.0 entry, which may point at a different table
    // copy, so prefer 2.0 and only fall back to 1.0.
    //
    if (CompareGuid (&gST->ConfigurationTable[Index].VendorGuid, &gEfiAcpi20TableGuid)) {
      return gST->ConfigurationTable[Index].VendorTable;
    }
    if (CompareGuid (&gST->ConfigurationTable[Index].VendorGuid, &gEfiAcpiTableGuid)) {
      Rsdp10 = gST->ConfigurationTable[Index].VendorTable;
    }
  }
  return Rsdp10;
}

STATIC
UINT8 *
FindFadt (
  IN VOID  *Rsdp
  )
{
  UINT8   *Rsdp8;
  UINT8   *Table;
  UINT32  Length;
  UINTN   Entries;
  UINTN   Index;
  UINT64  Entry;

  Rsdp8 = (UINT8 *)Rsdp;
  if (CompareMem (Rsdp8, "RSD PTR ", 8) != 0) {
    return NULL;
  }

  if (Rsdp8[15] >= 2) {
    Entry = ReadUnaligned64 (Rsdp8 + 24);
    if (Entry != 0) {
      Table = (UINT8 *)(UINTN)Entry;
      if (CompareMem (Table, "XSDT", 4) == 0) {
        Length = ReadUnaligned32 (Table + 4);
        if (Length >= BC250_ACPI_HEADER_SIZE) {
          Entries = (Length - BC250_ACPI_HEADER_SIZE) / 8;
          for (Index = 0; Index < Entries; ++Index) {
            Entry = ReadUnaligned64 (Table + BC250_ACPI_HEADER_SIZE + Index * 8);
            if (Entry != 0 && CompareMem ((VOID *)(UINTN)Entry, "FACP", 4) == 0) {
              return (UINT8 *)(UINTN)Entry;
            }
          }
        }
      }
    }
  }

  Entry = ReadUnaligned32 (Rsdp8 + 16);
  if (Entry != 0) {
    Table = (UINT8 *)(UINTN)Entry;
    if (CompareMem (Table, "RSDT", 4) == 0) {
      Length = ReadUnaligned32 (Table + 4);
      if (Length >= BC250_ACPI_HEADER_SIZE) {
        Entries = (Length - BC250_ACPI_HEADER_SIZE) / 4;
        for (Index = 0; Index < Entries; ++Index) {
          Entry = ReadUnaligned32 (Table + BC250_ACPI_HEADER_SIZE + Index * 4);
          if (Entry != 0 && CompareMem ((VOID *)(UINTN)Entry, "FACP", 4) == 0) {
            return (UINT8 *)(UINTN)Entry;
          }
        }
      }
    }
  }

  return NULL;
}

#pragma pack(1)
typedef struct {
  UINT32  Magic;
  UINT32  Stage;
  UINT32  Status;
  UINT32  FadtLength;
  UINT32  DsdtLength;
  UINT32  NewLength;
  UINT64  Rsdp;
  UINT64  Fadt;
  UINT64  Dsdt;
  UINT64  NewDsdt;
  UINT64  XDsdtReadBack;
  UINT32  Dsdt32ReadBack;
} BC250_CSTUB_DBG;
#pragma pack()

STATIC BC250_CSTUB_DBG mCstubDbg;

STATIC
VOID
SaveCstubDbg (
  IN UINT32      Stage,
  IN EFI_STATUS  Status
  )
{
  mCstubDbg.Magic  = 0x43535431U; // 'CST1'
  mCstubDbg.Stage  = Stage;
  mCstubDbg.Status = (UINT32)Status;
  (VOID)gRT->SetVariable (
               L"MeiMeiDXEv3AcpiDbg",
               &mMeiMeiDXEv3ConfigVarGuid,
               EFI_VARIABLE_BOOTSERVICE_ACCESS |
               EFI_VARIABLE_RUNTIME_ACCESS |
               EFI_VARIABLE_NON_VOLATILE,
               sizeof (mCstubDbg),
               &mCstubDbg
               );
}

STATIC
EFI_STATUS
PatchDsdtCstub (
  VOID
  )
{
  EFI_STATUS            Status;
  UINT32                Stage;
  VOID                  *Rsdp;
  UINT8                 *Fadt;
  UINT32                FadtLength;
  UINT64                DsdtAddress;
  UINT8                 *Dsdt;
  UINT32                DsdtLength;
  UINT32                NewLength;
  UINTN                 StubLength;
  UINTN                 Pages;
  EFI_PHYSICAL_ADDRESS  NewAddress;
  UINT8                 *NewDsdt;
  UINT32                Index;
  UINT8                 Sum;
  BOOLEAN               AlreadyPresent;

  Status = EFI_SUCCESS;
  Stage  = 0;
  ZeroMem (&mCstubDbg, sizeof (mCstubDbg));

  if (mDsdtCstubPatched) {
    Stage = 20;
    goto Done;
  }
  if (sizeof (gCstub) <= BC250_ACPI_HEADER_SIZE ||
      CompareMem (gCstub, "SSDT", 4) != 0) {
    DEBUG ((DEBUG_ERROR, "ACPIAutoInject: CSTUB blob malformed\n"));
    Status = EFI_DEVICE_ERROR;
    Stage  = 21;
    goto Done;
  }

  Rsdp = FindAcpiRsdp ();
  if (Rsdp == NULL) {
    DEBUG ((DEBUG_WARN, "ACPIAutoInject: RSDP not found\n"));
    Status = EFI_NOT_FOUND;
    Stage  = 1;
    goto Done;
  }
  mCstubDbg.Rsdp = (UINT64)(UINTN)Rsdp;

  Fadt = FindFadt (Rsdp);
  if (Fadt == NULL) {
    DEBUG ((DEBUG_WARN, "ACPIAutoInject: FADT not found\n"));
    Status = EFI_NOT_FOUND;
    Stage  = 2;
    goto Done;
  }
  mCstubDbg.Fadt = (UINT64)(UINTN)Fadt;

  FadtLength = ReadUnaligned32 (Fadt + 4);
  mCstubDbg.FadtLength = FadtLength;
  if (FadtLength < 44) {
    Status = EFI_DEVICE_ERROR;
    Stage  = 3;
    goto Done;
  }
  DsdtAddress = 0;
  if (FadtLength >= BC250_FADT_X_DSDT_MIN_LEN) {
    DsdtAddress = ReadUnaligned64 (Fadt + BC250_FADT_X_DSDT_OFFSET);
  }
  if (DsdtAddress == 0) {
    DsdtAddress = ReadUnaligned32 (Fadt + BC250_FADT_DSDT_OFFSET);
  }
  if (DsdtAddress == 0) {
    Status = EFI_DEVICE_ERROR;
    Stage  = 4;
    goto Done;
  }
  mCstubDbg.Dsdt = DsdtAddress;

  Dsdt = (UINT8 *)(UINTN)DsdtAddress;
  if (CompareMem (Dsdt, "DSDT", 4) != 0) {
    Status = EFI_DEVICE_ERROR;
    Stage  = 5;
    goto Done;
  }
  DsdtLength = ReadUnaligned32 (Dsdt + 4);
  mCstubDbg.DsdtLength = DsdtLength;
  if (DsdtLength < BC250_ACPI_HEADER_SIZE || DsdtLength > BC250_DSDT_MAX_SIZE) {
    Status = EFI_DEVICE_ERROR;
    Stage  = 6;
    goto Done;
  }

  AlreadyPresent = FALSE;
  for (Index = 0; Index + 4 <= DsdtLength; ++Index) {
    if (Dsdt[Index] == 'C' && Dsdt[Index + 1] == '0' &&
        Dsdt[Index + 2] == '0' && Dsdt[Index + 3] == '0') {
      AlreadyPresent = TRUE;
      break;
    }
  }
  if (AlreadyPresent) {
    DEBUG ((DEBUG_INFO, "ACPIAutoInject: DSDT already defines C000, skipping CSTUB\n"));
    mDsdtCstubPatched = TRUE;
    Stage = 7;
    goto Done;
  }

  StubLength = sizeof (gCstub) - BC250_ACPI_HEADER_SIZE;
  NewLength  = DsdtLength + (UINT32)StubLength;
  Pages      = EFI_SIZE_TO_PAGES (NewLength);
  NewAddress = 0xFFFFFFFFULL;
  Status = gBS->AllocatePages (AllocateMaxAddress, EfiACPIMemoryNVS, Pages, &NewAddress);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "ACPIAutoInject: DSDT allocation failed: %r\n", Status));
    Stage = 8;
    goto Done;
  }
  mCstubDbg.NewDsdt   = (UINT64)NewAddress;
  mCstubDbg.NewLength = NewLength;

  NewDsdt = (UINT8 *)(UINTN)NewAddress;
  CopyMem (NewDsdt, Dsdt, DsdtLength);
  CopyMem (NewDsdt + DsdtLength, gCstub + BC250_ACPI_HEADER_SIZE, StubLength);
  WriteUnaligned32 (NewDsdt + 4, NewLength);
  NewDsdt[9] = 0;
  Sum = 0;
  for (Index = 0; Index < NewLength; ++Index) {
    Sum = (UINT8)(Sum + NewDsdt[Index]);
  }
  NewDsdt[9] = (UINT8)(0 - Sum);

  WriteUnaligned32 (Fadt + BC250_FADT_DSDT_OFFSET, (UINT32)NewAddress);
  if (FadtLength >= BC250_FADT_X_DSDT_MIN_LEN) {
    WriteUnaligned64 (Fadt + BC250_FADT_X_DSDT_OFFSET, (UINT64)NewAddress);
  }
  Fadt[9] = 0;
  Sum = 0;
  for (Index = 0; Index < FadtLength; ++Index) {
    Sum = (UINT8)(Sum + Fadt[Index]);
  }
  Fadt[9] = (UINT8)(0 - Sum);

  mCstubDbg.XDsdtReadBack = 0;
  if (FadtLength >= BC250_FADT_X_DSDT_MIN_LEN) {
    mCstubDbg.XDsdtReadBack = ReadUnaligned64 (Fadt + BC250_FADT_X_DSDT_OFFSET);
  }
  mCstubDbg.Dsdt32ReadBack = ReadUnaligned32 (Fadt + BC250_FADT_DSDT_OFFSET);
  if (mCstubDbg.XDsdtReadBack != (UINT64)NewAddress &&
      mCstubDbg.Dsdt32ReadBack != (UINT32)NewAddress) {
    Status = EFI_WRITE_PROTECTED;
    Stage  = 10;
    goto Done;
  }

  DEBUG ((DEBUG_INFO,
    "ACPIAutoInject: DSDT CSTUB appended (%u -> %u bytes) at 0x%08x, FADT rewritten\n",
    DsdtLength, NewLength, (UINT32)NewAddress));
  mDsdtCstubPatched = TRUE;
  Stage = 11;

Done:
  SaveCstubDbg (Stage, Status);
  return Status;
}

STATIC
EFI_STATUS
SuppressBrokenCpuSsdt (
  VOID
  )
{
  VOID    *Rsdp;
  UINT8   *Rsdp8;
  UINT8   *Table;
  UINT64  Xsdt;
  UINT32  Rsdt;
  UINT32  Length;
  UINT32  TableLength;
  UINTN   Entries;
  UINTN   Index;
  UINT64  Entry;
  UINT8   *T;
  UINT8   Sum;
  UINT32  I;

  Rsdp = FindAcpiRsdp ();
  if (Rsdp == NULL) {
    SaveCstubDbg (40, EFI_NOT_FOUND);
    return EFI_NOT_FOUND;
  }
  Rsdp8 = (UINT8 *)Rsdp;
  mCstubDbg.Rsdp = (UINT64)(UINTN)Rsdp;

  for (I = 0; I < 2; ++I) {
    if (I == 0) {
      if (Rsdp8[15] < 2) {
        continue;
      }
      Xsdt = ReadUnaligned64 (Rsdp8 + 24);
      if (Xsdt == 0) {
        continue;
      }
      Table = (UINT8 *)(UINTN)Xsdt;
      if (CompareMem (Table, "XSDT", 4) != 0) {
        continue;
      }
      Length = ReadUnaligned32 (Table + 4);
      Entries = (Length - BC250_ACPI_HEADER_SIZE) / 8;
      for (Index = 0; Index < Entries; ++Index) {
        Entry = ReadUnaligned64 (Table + BC250_ACPI_HEADER_SIZE + Index * 8);
        if (Entry == 0) {
          continue;
        }
        T = (UINT8 *)(UINTN)Entry;
        if (CompareMem (T, "SSDT", 4) == 0 &&
            T[10] == 'A' && T[11] == 'M' && T[12] == 'D' &&
            CompareMem (T + 16, "AMD CPU", 7) == 0) {
          TableLength = ReadUnaligned32 (T + 4);
          if (TableLength < BC250_ACPI_HEADER_SIZE || TableLength > 0x10000U) {
            continue;
          }
          CopyMem (T, "OEMX", 4);
          T[9] = 0;
          Sum = 0;
          for (UINT32 J = 0; J < TableLength; ++J) {
            Sum = (UINT8)(Sum + T[J]);
          }
          T[9] = (UINT8)(0 - Sum);
          mCstubDbg.NewLength = TableLength;
          mCstubDbg.Fadt = Entry;
          SaveCstubDbg (41, EFI_SUCCESS);
          return EFI_SUCCESS;
        }
      }
    } else {
      Rsdt = ReadUnaligned32 (Rsdp8 + 16);
      if (Rsdt == 0) {
        continue;
      }
      Table = (UINT8 *)(UINTN)Rsdt;
      if (CompareMem (Table, "RSDT", 4) != 0) {
        continue;
      }
      Length = ReadUnaligned32 (Table + 4);
      Entries = (Length - BC250_ACPI_HEADER_SIZE) / 4;
      for (Index = 0; Index < Entries; ++Index) {
        Entry = ReadUnaligned32 (Table + BC250_ACPI_HEADER_SIZE + Index * 4);
        if (Entry == 0) {
          continue;
        }
        T = (UINT8 *)(UINTN)Entry;
        if (CompareMem (T, "SSDT", 4) == 0 &&
            T[10] == 'A' && T[11] == 'M' && T[12] == 'D' &&
            CompareMem (T + 16, "AMD CPU", 7) == 0) {
          TableLength = ReadUnaligned32 (T + 4);
          if (TableLength < BC250_ACPI_HEADER_SIZE || TableLength > 0x10000U) {
            continue;
          }
          CopyMem (T, "OEMX", 4);
          T[9] = 0;
          Sum = 0;
          for (UINT32 J = 0; J < TableLength; ++J) {
            Sum = (UINT8)(Sum + T[J]);
          }
          T[9] = (UINT8)(0 - Sum);
          mCstubDbg.NewLength = TableLength;
          mCstubDbg.Fadt = Entry;
          SaveCstubDbg (41, EFI_SUCCESS);
          return EFI_SUCCESS;
        }
      }
    }
  }

  SaveCstubDbg (42, EFI_NOT_FOUND);
  return EFI_NOT_FOUND;
}

/**
  ACPI table protocol notification callback.

  Once EFI_ACPI_TABLE_PROTOCOL is available, this callback re-checks the
  firmware ACPI-patch gate, reads the BC250 core mask, selects the compacted
  processor-pair tables, and installs the corresponding AML tables exactly once
  for the current boot.

  @param[in] Event       Notification event being signaled.
  @param[in] ContextPtr  Pointer to BC250_ACPI_AUTO_INJECT_CONTEXT.
**/
STATIC
VOID
EFIAPI
OnAcpiTableProtocolReady (
  IN EFI_EVENT Event,
  IN VOID      *ContextPtr
  )
{
  EFI_STATUS                    Status;
  EFI_ACPI_TABLE_PROTOCOL       *AcpiTable;
  UINT32                        MaskValue;
  UINT8                         MaskLowByte;
  UINT8                         EnabledCoreCount;
  UINT8                         PairIndex;
  BC250_ACPI_BLOB               SelectedBlobs[BC250_MAX_ACPI_TABLES];
  UINTN                         SelectedCount;
  BOOLEAN                       AcpiPatchEnabled;
  UINT8                         AcpiPatch;
  BC250_ACPI_AUTO_INJECT_CONTEXT *Context;

  Context = (BC250_ACPI_AUTO_INJECT_CONTEXT *)ContextPtr;
  (VOID)Event;
  if (Context->TablesInstalled) {
    return;
  }

  Status = gBS->LocateProtocol (&gEfiAcpiTableProtocolGuid, NULL, (VOID **)&AcpiTable);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "BC250DXEv3ACPIAutoInjectDxe: ACPI table protocol not yet available: %r\n", Status));
    return;
  }

  AcpiPatchEnabled = FALSE;
  AcpiPatch = 0;
  Status = GetAcpiPatchEnabled (&AcpiPatchEnabled, &AcpiPatch);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "BC250DXEv3ACPIAutoInjectDxe: ACPI patch gate unavailable, skipping\n"));
    return;
  }

  if (!AcpiPatchEnabled) {
    DEBUG ((DEBUG_INFO, "BC250DXEv3ACPIAutoInjectDxe: ACPI patch is disabled, skipping\n"));
    return;
  }

  // Read the hardware mask only after the firmware gate allows injection.
  MaskValue = SmnRead32 (MASK_REG);
  MaskLowByte = (UINT8)(MaskValue & 0xFFU);
  DEBUG ((DEBUG_INFO, "BC250DXEv3ACPIAutoInjectDxe: core presence mask=0x%08x\n", MaskValue));

  EnabledCoreCount = CountEnabledCores (MaskLowByte);
  if ((EnabledCoreCount == 0) || (EnabledCoreCount > BC250_PAIR_TABLE_COUNT)) {
    DEBUG ((DEBUG_ERROR, "BC250DXEv3ACPIAutoInjectDxe: invalid enabled-core count=%u\n", EnabledCoreCount));
    return;
  }

  SelectedCount = 0;
  SelectedBlobs[SelectedCount++] = gPstateCommonTable;
  if (EnabledCoreCount == BC250_PAIR_TABLE_COUNT) {
    SelectedBlobs[SelectedCount++] = gFullCstateCommonTable;
  } else {
    SelectedBlobs[SelectedCount++] = gPartialCstateCommonTable;
  }

  for (PairIndex = 0; PairIndex < EnabledCoreCount; ++PairIndex) {
    SelectedBlobs[SelectedCount++] = gPstatePairs[PairIndex];
    if (EnabledCoreCount == BC250_PAIR_TABLE_COUNT) {
      SelectedBlobs[SelectedCount++] = gFullCstatePairs[PairIndex];
    } else {
      SelectedBlobs[SelectedCount++] = gPartialCstatePairs[PairIndex];
    }
  }

  if (EnabledCoreCount < BC250_PAIR_TABLE_COUNT) {
    SelectedBlobs[SelectedCount++] = gPtswakTable;
  }

  DEBUG ((DEBUG_INFO, "BC250DXEv3ACPIAutoInjectDxe: selected %u physical cores (%u tables)\n", EnabledCoreCount, (UINT32)SelectedCount));
  Status = InstallAcpiBlobSet (AcpiTable, SelectedBlobs, SelectedCount, Context);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "BC250DXEv3ACPIAutoInjectDxe: ACPI installation aborted: %r\n", Status));
    return;
  }

  Context->TablesInstalled = TRUE;

  //
  // Optional DSDT namespace repair (AcpiPatch bit 1).  Disabled by default:
  // on this firmware the ACPI table protocol owns the final DSDT/FADT copies,
  // and a post-install override was observed to break the OS boot.  Bit 0
  // alone keeps the proven P/C-state power-management injection.
  //
  if ((AcpiPatch & 0x02U) != 0) {
    Status = PatchDsdtCstub ();
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_WARN,
        "BC250DXEv3ACPIAutoInjectDxe: DSDT CSTUB patch failed: %r\n", Status));
    }
  } else {
    ZeroMem (&mCstubDbg, sizeof (mCstubDbg));
    SaveCstubDbg (30, EFI_UNSUPPORTED);
    DEBUG ((DEBUG_INFO, "BC250DXEv3ACPIAutoInjectDxe: DSDT CSTUB disabled (bit1 clear)\n"));
  }

  if ((AcpiPatch & 0x04U) != 0) {
    Status = SuppressBrokenCpuSsdt ();
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_WARN,
        "BC250DXEv3ACPIAutoInjectDxe: AMD CPU SSDT suppress failed: %r\n", Status));
    }
  }

  if (Context->Event != NULL) {
    gBS->CloseEvent (Context->Event);
    Context->Event = NULL;
    Context->Registration = NULL;
  }
}

/**
  DXE entry point.

  The driver registers for EFI_ACPI_TABLE_PROTOCOL notification so it can defer
  SSDT installation until the firmware ACPI table interface is ready. It also
  invokes the callback immediately to handle the common case where the protocol
  already exists.

  @param[in] ImageHandle   Standard UEFI image handle.
  @param[in] SystemTable   Standard UEFI system table pointer.

  @retval EFI_SUCCESS  Driver initialization completed successfully.
  @retval others       Event creation or protocol-notify registration failed.
**/
EFI_STATUS
EFIAPI
BC250DXEv3ACPIAutoInjectEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS Status;

  (VOID)ImageHandle;
  (VOID)SystemTable;

  DEBUG ((DEBUG_INFO, "BC250DXEv3ACPIAutoInjectDxe: entry\n"));

  Status = gBS->CreateEvent (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  OnAcpiTableProtocolReady,
                  &mAcpiInjectContext,
                  &mAcpiInjectContext.Event
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "BC250DXEv3ACPIAutoInjectDxe: failed to create notify event: %r\n", Status));
    return Status;
  }

  Status = gBS->RegisterProtocolNotify (
                  &gEfiAcpiTableProtocolGuid,
                  mAcpiInjectContext.Event,
                  &mAcpiInjectContext.Registration
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "BC250DXEv3ACPIAutoInjectDxe: failed to register ACPI notify: %r\n", Status));
    gBS->CloseEvent (mAcpiInjectContext.Event);
    mAcpiInjectContext.Event = NULL;
    return Status;
  }

  OnAcpiTableProtocolReady (mAcpiInjectContext.Event, &mAcpiInjectContext);
  return EFI_SUCCESS;
}