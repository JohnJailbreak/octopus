# Windows compatibility and milestone

User-provided Windows context: interface MI_00, class 06/01/01, Microsoft's WPD/MTP driver. This was not independently captured in this session. The supplied iPhone ZIP itself proves only its internal/loopback checks and blocked transport.

Microsoft documents built-in class-driver support for PTP/MTP devices and recommends a class protocol to avoid a vendor driver: [WPD overview](https://learn.microsoft.com/en-us/windows-hardware/drivers/portable/wpd-drivers-overview), [hardware guidance](https://learn.microsoft.com/en-us/windows-hardware/drivers/portable/guidance-for-the-hardware-vendor). This supports the protocol choice, not a guarantee that Explorer will expose arbitrary folders or writes with the existing iPhone descriptors and capability dataset. AFC is not made into PTP merely by being carried over USB.

The existing enumeration fixture and new descriptor/control fixtures are **synthetic/specification-derived**, not captured Windows requests. Preserve that label. Genuine captures must retain request/response bytes, endpoint and direction, transaction/session IDs, timestamps, origin and capture conditions; redact personal identifiers before committing.

| Required demonstration | Status |
|---|---|
| Real Windows PTP command reaches Octopus integration | Not verified; no native bridge |
| Actual response transmitted | Not verified |
| Synthetic storage and nested folders in Explorer | Not verified |
| Documents/example.txt download | Loopback only |
| SHA-256 aeeb6cba90d7d6adfcd42d673f64897f26cfcc479c5cf489a86146e09a9d2bd2 | Recorded simulated 76-byte file; no PC download |
| Existing photo transfer preserved physically | No mutating integration shipped; physical regression check still needed |

Do not progress to root filesystem export until all criteria pass. MTP exposes objects, not a POSIX mount; sealed system storage is not writable by assumption.
