# ptpd: device-side USB responder

UUID **FFD54FC7-0508-3AB8-8D17-2F698B611E10**, arm64e, iOS min/SDK 17.0. All addresses are unslid VAs, base 0x100000000. [Inventory](binary-inventory.md) defines provenance, signing and PAC limits. This page is based on actual Ghidra instructions, xrefs and decompilation, not string presence alone. [Function index](evidence/ptpd/functions.tsv) and [method encodings](evidence/ptpd/method-encodings.json) navigate the image.

## Verified through static analysis

| Implementation | Data flow / significance | Confidence |
|---|---|---|
| [0x100020694 initWithName:path:andTransportType:](evidence/ptpd/100020694.txt) | Responder initialization and selected transport ownership. Objective-C self/selector x0/x1; explicit name/path/type parameters follow. | High dispatch; decompiler parameter types require runtime verification. |
| [0x1000241a0 PTPResponder start](evidence/ptpd/1000241a0.txt) | Starts responder/provider/transport relationship. | High static; launch timing unobserved. |
| [0x100004a64 PTPDeviceUSBTransport startResponder](evidence/ptpd/100004a64.txt) | IOServiceMatching(IOUSBDeviceInterface); IOPropertyMatch USBDeviceFunction=PTP, iterator and plug-in acquisition. Exact match now used by Octopus's read-only probe. | High. Discovery does not grant ownership. |
| [0x10000a8a8 setupDeviceInterface](evidence/ptpd/10000a8a8.txt) | Calls private table to set PTP name, class/subclass/protocol 6/1/1, three endpoints, demand/state functions. | High offsets/arguments; slot API names inferred. |
| [0x100008eac readBulkData](evidence/ptpd/100008eac.txt) / [0x10000906c callback](evidence/ptpd/10000906c.txt) | Private table +0xa0 submits asynchronous input; completion reaches handleBulkData. | High call chain; exact imported ABI unavailable. |
| [0x1000093bc handleBulkData:result:](evidence/ptpd/1000093bc.txt) | Decodes completed input and routes packet/transaction state into delegate handling; handles error paths and subsequent reads. Encoding B28@0:8Q16i24. | High; no real transfer observed. |
| [0x100023384 PTPResponder handleRequest:](evidence/ptpd/100023384.txt) | operationCode switch routes OpenSession 0x1002, CloseSession 0x1003, GetStorageIDs 0x1004, GetStorageInfo 0x1005, handles 0x1007, info 0x1008, object 0x1009 plus extensions. Encoding @24@0:8@16. | High static dispatch. |
| [0x100020964 openSession:](evidence/ptpd/100020964.txt), [0x100020b04 closeSession:](evidence/ptpd/100020b04.txt) | Provider/session state and response packets; close behavior also depends on transport type in handleRequest. | High; runtime ordering unverified. |
| [0x100020dc8 getStorageIDs:](evidence/ptpd/100020dc8.txt), [0x100020f18 getStorageInfo:](evidence/ptpd/100020f18.txt) | Provider storage list/info become PTP data and response. | High. |
| [0x100021634 getObjectHandles:](evidence/ptpd/100021634.txt), [0x1000217a4 getObjectInfo:](evidence/ptpd/1000217a4.txt) | Request parameters select provider object enumeration and info datasets. | High. |
| [0x1000218c0 getObject:](evidence/ptpd/1000218c0.txt) | Resolves camera data source and size, builds data packet, routes to transport. | High; Photos-backed source behavior imported. |
| [0x100007ebc sendResponse:](evidence/ptpd/100007ebc.txt), [0x100006fd0 sendDataPackets:](evidence/ptpd/100006fd0.txt), [0x1000075f0 split variant](evidence/ptpd/1000075f0.txt) | Container serialization/data streaming converge on bulk writer. | High; packet termination behavior needs physical trace. |
| [0x100008414 writeBulkData:](evidence/ptpd/100008414.txt) | Private interface table +0xa8 writes bulk data. | High offset; guessed signature must not be used. |
| [0x100008054 sendEvent:](evidence/ptpd/100008054.txt) | Event serialization/queue toward interrupt transfer completion. | High; host handling unobserved. |
| [0x10000b848 handleDeviceRequest:data:dataSize:status:](evidence/ptpd/10000b848.txt) | Control request dispatch separate from bulk commands. | High body; native callback contract incomplete. |
| [0x10000574c stop](evidence/ptpd/10000574c.txt) | Transport shutdown releases native resources. | High body; races with callbacks need observation. |

Each evidence file includes direct caller xrefs and disassembly followed by pseudocode. Objective-C callers often use selector stubs, so absence of a direct xref is not absence of calls. x0=self, x1=_cmd, x2 onward explicit arguments for Objective-C calls; do not use decompiler temporaries as complete declarations.

Private setup table evidence: +0x20 open-like operation; +0x50 PTP name; +0x58/+0x60/+0x68 class/subclass/protocol; +0x80 called with bulk OUT type 2 direction 0, bulk IN type 2 direction 1, interrupt IN type 3 direction 1; +0x88 commit-like operation; +0xe0 demand name `com.apple.usbptpd.demand`; +0x120 state query. These are offsets within this binary's expected interface, not a verified public vtable definition. Missing matching IOKit implementation blocks a safe native driver.

## Runtime observation / inference / unresolved

The supplied diagnostics observe ptpd running, not requests inside it. No runtime observation in this session proves these reference images execute on the phone. Strong inference: preserving Apple's transport and extending request/provider routing is less disruptive than competing for the same PTP interface. However, the provider is tightly Photos-coupled and explicitly warns about multiple storages: see [provider analysis](storage-provider-interface.md). A working additional provider remains an unresolved hypothesis. No hook, vtable call, injected dylib, USB seizure or configuration mutator was shipped.
