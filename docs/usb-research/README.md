# Octopus USB engineering evidence

Baseline: v0.4.8 build 8, commit cdca0dac47981898f93edeab80a51f46ea1abb4d. Supplied report: 29 checks, 26 PASS, 3 BLOCKED, zero external transactions. Working source/history preserved. **This delivery does not yet make synthetic storage appear in File Explorer.**

- [Status and validation](implementation-status.md)
- [Next iPhone/Windows experiment and build instructions](runtime-validation.md)
- [Exact identities, provenance, architecture and PAC conventions](binary-inventory.md)
- [ptpd function map](ptpd.md), [request lifecycle](ptp-request-lifecycle.md), [provider boundary](storage-provider-interface.md)
- [USB configuration bundle](USBDeviceConfiguration.md), [lockdownd](lockdownd.md), [afcd](afcd.md), [ptpcamerad](ptpcamerad.md)
- [Architecture](usb-architecture.md), [candidate decisions](integration-options.md), [Windows criteria](windows-compatibility.md)
- [Implemented/tested boundary](usb-boundary.md), [reproducing analysis](tools/README.md)

Evidence categories: **verified static** means inspected supplied bytes/instructions/metadata; **verified runtime** means actual device observations (only original diagnostics here); **strong inference** is supported interpretation; **unresolved hypothesis** needs further evidence. Decompiled C is explanatory, not source or a certified ABI. Every major negative result and missing shared-cache boundary remains explicit.

Evidence directory contains inventories, selected disassembly/decompilation/xrefs and function indexes. No Apple executables are included. Raw original diagnostic logs are not committed; their important nonpersonal baseline values are summarized here.
