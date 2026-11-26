# BC250-DXEv3-ACPI-Driver

DXE driver that injects ACPI SSDTs matching the currently enabled physical-core
count, so the OS sees a consistent P-state/C-state topology after a core-unlock
configuration. It targets the BC-250 / BIOS 3.00 platform.

## At a glance

| Item | Value |
| --- | --- |
| Package / module | `BC250DXEv3ACPIAutoInjectPkg` -> `MeiMeiDXEv3_ACPI_AutoInject.efi` |
| Depex | `gEfiVariableArchProtocolGuid AND gEfiVariableWriteArchProtocolGuid AND gEfiAcpiTableProtocolGuid` |
| Produces | no MeiMeiDXEv3 protocol |
| Consumes | `MeiMeiDXEv3AcpiVar`, `EFI_ACPI_TABLE_PROTOCOL`, SMN `0x0115A870` |
| Configuration | `MeiMeiDXEv3AcpiVar`, 1 byte |
| Compliance | [COMPLIANCE.md](COMPLIANCE.md) |

## Dependencies

The ACPI table protocol and variable write services must exist; both are in the
Depex. This driver does not require the SMU-unlock drivers, because it only
reads the core-presence register.

## How it works

1. Read `MeiMeiDXEv3AcpiVar` through the shared configuration ABI.
2. If `AcpiPatch` is zero, or the variable is missing, undersized, or
   wrong-attribute, install nothing (fail-closed) and continue the boot.
3. Read SMN `0x0115A870` through PCI `00:00.0` offsets `0xB8`/`0xBC`.
4. Popcount the low byte to get 1-8 enabled physical cores.
5. Firmware compacts enabled SMT threads into `P000`..`P(2N-1)`.
6. Install one P-state and one C-state SSDT per compacted SMT pair.

## Injected tables

- One shared P-state table with `_PCT`, `_PSS`, and `_PSD`.
- One shared two-state C-state table for partial-core configurations.
- One shared three-state C-state table when all eight cores are enabled.
- One P-state and one C-state table per compacted SMT pair.
- `SSDT-PTSWAK` for partial-core configurations.

The ASL sources are compiled by `prepare_acpi_blobs.py` into embedded blobs, so
the driver does not need a precompiled table per mask.

## Configuration

- Header: `BC250-DXEv3-Shared/MeiMeiDXEv3SharedPkg/Include/Config/MeiMeiDXEv3Config.h`
- Variable: `MEIMEIDXEV3_ACPI_VAR_NAME` (`MeiMeiDXEv3AcpiVar`)
- Field: `AcpiPatch` at offset 0

Any read failure, size mismatch, or attribute mismatch means no injection and a
normal boot.

## Protocols

Also publishes `gMeiMeiDXEv3DriverInfoProtocolGuid` with its name, version, module GUID, config schema, and payload version for the `BC250-DXEv3-Driver-Info` telemetry driver.

No MeiMeiDXEv3 protocol is produced. The driver consumes the standard
`gEfiAcpiTableProtocolGuid` to install and later uninstall the tables.

## Build

`iasl` is required to compile the ASL blobs.

```bash
./build_ffs.sh                                  # container build
NO_CONTAINER=1 EDK2_DIR=/path/to/edk2 ./build_ffs.sh   # host build
```

Output:

- `Build/Output/MeiMeiDXEv3_ACPI_AutoInject.efi`
- `Build/Output/MeiMeiDXEv3_ACPI_AutoInject.ffs`

## Validation

2026-11-25, nine-driver stack, configured ceiling 2100 MHz: `DEPLOY_OK`,
actual clock 2100 MHz sustained, 82 C peak, no ring/C2P timeouts, board
reachable after the workload, and the OS booted with 8 cores / 16 threads.
See [COMPLIANCE.md](COMPLIANCE.md).
