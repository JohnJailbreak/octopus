# Native request observation and additive routing boundary

This is a focused continuation of the existing `ptpd` analysis, not a new inventory. All addresses below refer to the supplied **arm64e ptpd, UUID FFD54FC7-0508-3AB8-8D17-2F698B611E10**, min OS/SDK iOS 17.0, unslid image base `0x100000000`. An offset is the displayed VA minus that base; a runtime address is the VA plus that image's verified slide. These static findings do not establish that an integration module has loaded on the phone.

## Verified static contracts for the observation module

Signatures below come from the image's Objective-C relative method metadata, independently matched to the implementations and callers in Ghidra. These are **not** inferred from selector names alone. Encodings are recorded in [method-encodings.json](evidence/ptpd/method-encodings.json). Every Objective-C implementation receives `self` in x0, `_cmd` in x1 and its explicit arguments in subsequent argument registers. `B` is C `_Bool`, `S` is unsigned 16-bit, `I` unsigned 32-bit, and `Q` unsigned 64-bit. The encoding's stack/field offsets are Objective-C type metadata; they are not instructions to manually arrange argument registers.

| Owner / selector | VA / relative offset | Encoding | Contract and evidence |
|---|---|---|---|
| `PTPDeviceUSBTransport handleBulkData:result:` | `0x1000093bc` / `0x93bc` | `B28@0:8Q16i24` | `bool(id,SEL,uint64_t,int32_t)`. First explicit argument is completed byte count, not a buffer pointer; buffer is an instance resource. [Body](evidence/ptpd/1000093bc.txt). |
| `PTPResponder handleRequest:` | `0x100023384` / `0x23384` | `@24@0:8@16` | `id(id,SEL,id)`. Returns an NSNumber indicating whether a host DATA phase is expected, not success/failure. [Body](evidence/ptpd/100023384.txt). |
| `PTPDeviceUSBTransport sendResponse:` | `0x100007ebc` / `0x7ebc` | `B24@0:8@16` | `bool(id,SEL,id)`. Result equals zero-success from the synchronous native bulk writer. [Body](evidence/ptpd/100007ebc.txt). |
| `PTPDeviceUSBTransport sendData:` | `0x100007ca8` / `0x7ca8` | `v24@0:8@16` | `void(id,SEL,id)`. Enqueues a block to `_writeDataQueue`; return is not a transmission result. [Body](evidence/ptpd/100007ca8.txt), [block](evidence/ptpd/100007d24.txt). |
| `PTPOperationRequestPacket operationCode` | `0x1000205c8` / `0x205c8` | `S16@0:8` | `uint16_t(id,SEL)`. Read-only request accessor. [Body](evidence/ptpd/1000205c8.txt). |
| `PTPOperationRequestPacket transactionID` | `0x1000205d8` / `0x205d8` | `I16@0:8` | `uint32_t(id,SEL)`. [Body](evidence/ptpd/1000205d8.txt). |
| `PTPOperationResponsePacket responseCode` | `0x10001a6f4` / `0x1a6f4` | `S16@0:8` | `uint16_t(id,SEL)`. [Body](evidence/ptpd/10001a6f4.txt). |
| `PTPOperationResponsePacket transactionID` | `0x10001a704` / `0x1a704` | `I16@0:8` | `uint32_t(id,SEL)`. [Body](evidence/ptpd/10001a704.txt). |
| `PTPDeviceUSBTransport connected` | `0x100005900` / `0x5900` | `B16@0:8` | Read-only native state. [Body](evidence/ptpd/100005900.txt). |
| `PTPDeviceUSBTransport setConnected:` | `0x100005910` / `0x5910` | `v20@0:8B16` | `void(id,SEL,bool)`. Called on activation/deactivation and some I/O errors. Observe after original returns. [Body](evidence/ptpd/100005910.txt). |
| `PTPDeviceUSBTransport startResponder` | `0x100004a64` / `0x4a64` | `B16@0:8` | Service setup success, not host attachment. [Body](evidence/ptpd/100004a64.txt). |
| `PTPDeviceUSBTransport stop` | `0x10000574c` / `0x574c` | `v16@0:8` | Resource shutdown. [Body](evidence/ptpd/10000574c.txt). |
| `PTPResponder transportActivated` | `0x100024510` / `0x24510` | `v16@0:8` | Allocates virtual camera, resets capability flags, assigns responder as camera delegate. [Body](evidence/ptpd/100024510.txt). |
| `PTPResponder transportDeactivated` | `0x100024564` / `0x24564` | `v16@0:8` | Tail dispatch to `removeVirtualCamera`. [Body](evidence/ptpd/100024564.txt). |
| `PTPResponder handleDeviceRemoval` | `0x100023c80` / `0x23c80` | `v16@0:8` | Native teardown observation. [Body](evidence/ptpd/100023c80.txt). |
| `PTPResponder sentData:responseCode:` | `0x100023dc8` / `0x23dc8` | `v32@0:8@16@24` | Second explicit argument is NSNumber, not primitive ushort. Creates terminal response, including byte-count parameter for GetPartialObject. [Body](evidence/ptpd/100023dc8.txt). |

Confidence is high for these metadata and call-site facts. Runtime availability, object classes, image identity and matching encoding must still be checked before installing each hook. A method named the same way on another class is not this contract.

## Proven synchronous input provenance

The completed USB read callback [0x10000906c](evidence/ptpd/10000906c.txt) calls `handleBulkData:result:` after a successful read. Within that function, container type 1 creates `PTPOperationRequestPacket`, obtains selector `handleRequest:`, then performs it synchronously on the transport delegate. The returned object's `boolValue` becomes `_delegateNeedsData`. Relevant decompiled data flow is:

```text
read completion(context, result, byteCount):
    if result == 0:
        if context.handleBulkData(byteCount, 0): context.readBulkData()

handleBulkData(byteCount, result):
    append completed bytes from this transport's read buffer
    if container.type == COMMAND:
        packet = PTPOperationRequestPacket.initWithUSBBuffer(container)
        answer = delegate.performSelector(handleRequest:, packet)
        delegateNeedsData = answer.boolValue
```

A hook can therefore establish a thread-local **scoped native input context** around its original `handleBulkData:result:` call and recognize `handleRequest:` calls nested synchronously in that scope. Preserve the prior context to handle nesting and restore it even if an Objective-C exception leaves the original call. A bare `handleRequest:` invocation outside this scope is only a native method observation, not proof of external USB input. Record the transport identity, request operation/transaction ID, timestamp and provenance together. Do not persist raw object addresses as portable identities; use per-process tokens or bounded correlation records.

`handleBulkData:` returns the connection flag from its entry, not a count of parsed requests and not a status saying all received bytes produced a response. Count requests only when the request hook actually executes.

## Transmission meaning and asynchronous completion

The actual `sendResponse:` implementation checks `_connected`, serializes the response packet into its own `_writeBuffer` with `contentForUSBUsingBuffer:capacity:`, calls `writeBulkData:`, and returns `writeResult == 0`. The latter function calls the private interface's bulk writer. This supports a precise observation: **native response write returned success**. It cannot establish that Explorer consumed the response or wrote a downloaded file correctly.

Data is different: `sendData:` dispatches asynchronously to `_writeDataQueue`. The copied block captures transport and packet. [sendDataPackets:](evidence/ptpd/100006fd0.txt) copies/streams payload into Apple's own write buffer, tracks cancellation and transferred bytes, handles final short packet/ZLP behavior and calls the responder's `sentData:responseCode:`. The latter produces the terminal response. An observer must not label the void `sendData:` return as success. It should correlate the later response write or data completion, preserving error cases and partial downloads. The normal Photos path stays entirely inside Apple's original implementations.

Do not call `writeBulkData:` with arbitrary NSData: its implementation writes the transport's `_writeDataRef` resource and takes length from the wrapper. Its object argument is not an arbitrary external byte buffer. Using it as a raw socket-like writer would send the wrong buffer.

## Additive synthetic routing: supported boundary and remaining work

The promising boundary is the Objective-C request/provider layer, where a version-gated module could reuse Octopus's portable responder while leaving Apple's USB ownership intact. Static analysis supports constructing Apple's ordinary packet objects and using its ordinary send methods, without making any guessed USB vtable calls.

1. `PTPVirtualCamera allStorageIDs` at `0x1000108c4` (`@16@0:8`) returns keys of `_storages`. A narrowly scoped override can append a synthetic storage ID to the returned array without invoking `addStorage:`. This is a proposed additive integration, **not a verified supported provider API**. Merely adding the ID is insufficient: storage info, handle enumeration, objects and MTP properties must route consistently.
2. `PTPResponder getStorageIDs:` at `0x100020dc8` (`v24@0:8@16`) enforces native session existence and strictly increasing transaction IDs, advances `_lastTransactionID`, serializes that array and sends a data packet. Keeping this original path preserves native session validation. [Evidence](evidence/ptpd/100020dc8.txt).
3. Requests explicitly targeting a synthetic storage or reserved synthetic object handle could be handled at `handleRequest:` and return `@NO` for these read-only operations. All unrelated commands must call the original implementation. All-storage queries and wildcard MTP property lists need an explicit merge policy; forwarding them unchanged silently omits synthetic entries, while replacing them silently hides Photos.
4. A routed request must preserve native session and transaction ordering across both implementations. A separate Octopus session counter is not enough: Apple's handlers advance their own `_lastTransactionID`. Read/validate ivar names and types through Objective-C metadata before any proposed access, never hard-code the offsets from this report. Observation can establish the live object lifetime first.
5. `PTPDataPacket initWithOperationCode:transactionID:andData:` at `0x10002a188`, encoding `@32@0:8S16I20@24`, takes `uint16_t operation`, `uint32_t transaction`, and an Objective-C data object. This can carry the **body** of an Octopus data container. The implementation retains the explicit data argument: assembly saves x4 into x19 and calls `_objc_retain_x19` before storing it in `_data`. Ghidra's generic decompile incorrectly renders this retain as self in some views; assembly is authoritative. [Evidence](evidence/ptpd/10002a188.txt).
6. `PTPOperationResponsePacket initWithResponseCode:transactionID:` at `0x100019ef4`, encoding `@24@0:8S16I20`, constructs a response; `initWithUSBBuffer:` at `0x10001a35c`, encoding `@24@0:8^v16`, parses a USB response container. [Constructor](evidence/ptpd/100019ef4.txt), [parser](evidence/ptpd/10001a35c.txt). An Octopus-produced response requires validated bytes/parameters and sufficient lifetime.
7. Do **not** emit the portable responder's final response immediately after calling native `sendData:`: Apple asynchronously calls `sentData:responseCode:` and emits its own final response. Doing both sends duplicate responses and races the data phase. A bridge must deliberately assign terminal-response ownership and test cancellation/error propagation. Pure response-only requests can use native `sendResponse:` directly.

The observation component is a real callable boundary to gather the indispensable missing runtime evidence, not an assertion that the synthetic routing is already enabled. The unresolved points for routing are bounded: verified in-process loading on this jailbreak; lifecycle/serialization of the live responder and its transport; preservation of Apple's transaction ledger; collision-free handles; wildcard merge behavior; disable during queued data; Windows acceptance of added non-photo objects.

## Implemented provider overlay

The selected implementation is now `Octopus/Native/OctopusProvider.m`: **provider return-value integration**, rather than intercepting commands and updating Apple's transaction ledger. All `PTPResponder` handlers remain original. This also leaves data-phase scheduling, final responses and USB buffers with Apple. The app-controlled callback supplies a second, explicit authorization gate; disabled, incompatible, expired or colliding routes call their original provider implementations.

| Provider method / unslid VA | Verified encoding | Return / arguments used by original caller |
|---|---|---|
| [allStorageIDs / 0x1000108c4](evidence/ptpd/1000108c4.txt) | `@16@0:8` | NSArray of NSNumber uint32 IDs; hook appends only after original storage/object collision checks. |
| [storageInfoDatasetForStorageID: / 0x100011690](evidence/ptpd/100011690.txt) | `@20@0:8I16` | Storage ID to object responding to `content`; original GetStorageInfo sends that NSData body. |
| [objectInfoDatasetForObjectHandle: / 0x1000116d8](evidence/ptpd/1000116d8.txt) | `@20@0:8I16` | Handle to object responding to `content`, `filename`, `objectCompressedSize`, `objectCompressedSize64` for the inspected standard download and Apple size paths. |
| [objectHandlesInStorage:forObjectFormatCode:inAssociation:responseCode: / 0x1000108cc](evidence/ptpd/1000108cc.txt) | `@36@0:8I16S20I24^S28` | uint32 storage, uint16 format, uint32 association, uint16 response pointer; returns NSArray<NSNumber>. |
| [dataSourceForObjectHandle:responseCode:size: / 0x10001179c](evidence/ptpd/10001179c.txt) | `@36@0:8I16^S20^I28` | uint32 handle, uint16 response output, uint32 size output. Override only synthetic handles, avoiding the original PTPFile type check for those handles. |
| [objectPropValueForHandle:withObjectPropertyCode:responseCode: / 0x100012d6c](evidence/ptpd/100012d6c.txt) | `@32@0:8I16S20^S24` | uint32 handle, uint16 property, uint16 response output; NSData value body. |
| [objectPropListForHandle:withObjectFormatCode:andPropertyCode:andPropertyGroup:andDepth:responseCode: / 0x100013430](evidence/ptpd/100013430.txt) | `@44@0:8I16I20I24I28I32^S36` | Five uint32 parameters, uint16 response output; NSData list body. |
| [objectPropertiesSupportedForObjectFormatCode:responseCode: / 0x100012c38](evidence/ptpd/100012c38.txt) | `@28@0:8S16^S20` | uint16 format, uint16 response output; NSArray of NSNumber uint16 property codes. Only text format is extended. |

The synthetic storage ID is `0x4f430001`; its six handles start at `0x7f000001`. A high number is not a proof of reservation: original `allStorageIDs` and original `objectForObjectHandle:` are checked before every routed call, and any collision disables that call's overlay. The observed Apple allocator at [0x100004224](evidence/ptpd/100004224.txt) monotonically increments a uint32 counter and returns it in w0; no high-bit namespace reservation was discovered. The six handles and storage are generated by existing `oct_store` options, not native ivar manipulation.

The overlay reuses `oct_responder` with a separately serialized **local dataset-generation session**. Only its payload body becomes a native provider return value. This session is not counted as an external USB session. Native session validation and the global native transaction sequence still happen once, in original Apple handlers. [GetNumObjects at 0x100021034](evidence/ptpd/100021034.txt) calls the same handle enumeration provider then counts the returned array; it therefore includes the synthetic count without another hook.

An `OCTNativeSource` wraps immutable synthetic bytes and implements `length` (`Q16@0:8`) plus `read:length:offset:` (`q40@0:8^v16Q24q32`). It checks authorization and the captured authorization generation at every read. Partial reads, EOF and negative/out-of-range offsets have defined behavior. Revocation returns -1; [PTPDataPacket copyToBuffer:numBytes: at 0x10002aa94](evidence/ptpd/10002aa94.txt) recognizes -1 as a read error and returns zero to its caller. Re-enabling a later lease cannot revive a source created under an earlier lease. Bytes already copied into Apple's asynchronous write buffer cannot be recalled; this remains a bounded in-flight synthetic transfer, not a claim of instantaneous USB cancellation.

All-storage handle queries merge original Apple handles with synthetic handles; a query scoped to an Apple parent stays original. Synthetic nested parents are resolved through the core. PTP association 0 means recursive enumeration, while 0xffffffff means root children. For MTP wildcard property lists, this ptpd explicitly returns nil for handle 0/0xffffffff. The implementation preserves nil rather than inventing a synthetic-only list that hides Photos. If a validated original list is returned, it merges the count and payload with the core list. Explicit synthetic-handle property lists and values work independently. Whether Windows uses the unsupported wildcard path is a physical-test question.

`OCTProviderValidate` checks all required methods and original implementation provenance without installing hooks or initializing storage. `OCTProviderInstall` prepares the immutable tree, verifies all contracts before mutation, installs typed wrappers, and marks them usable only after every installation succeeds. On partial failure any installed wrapper remains pass-through. The production identity gate belongs to the observer/controller; calling the provider installer by itself does not validate iOS or the executable hash.

`tools/test_provider.sh` compiles the real provider, runtime validation and portable core against precisely typed stand-in native classes on macOS. It checks hierarchy, all-storage merging, a 76-byte download with the specified SHA-256, partial reads, read-only metadata, property values, handle/storage collisions, disabled forwarding and authorization generation revocation. `tools/test_observer.sh` separately tests the real observer wrappers with precisely typed stand-ins, including scoped input provenance, unchanged return values, async enqueue accounting, failed native write results, exception cleanup, disconnect and revocation. These are **LOCAL boundary tests**, never evidence of an actual Apple USB transfer. Their execution status must be taken from the build/test report, not inferred from the presence of these scripts.

## PAC, installation and failure boundaries

Use Objective-C runtime method discovery and implementation replacement, typed function pointers and an arm64e-capable build. Do not compute branch addresses from these VAs, strip/resign guessed IMP bits, overwrite Apple files, or construct private IOKit tables. Check the executable UUID/hash and OS build before installation of hooks; check every required method encoding before changing any. Resolve all prerequisites first so an unsupported process remains unmodified. Calls to originals must preserve arguments and return values exactly.

Static analysis does not prove Dopamine permits loading into `ptpd` or that a particular injection loader supports this executable. Packaging can target the rootless loader mechanism, but successful package creation is not successful loading. A module-loaded event, verified image identity, hook-installed event and **nested USB request observation** are distinct acceptance gates. Disabled observation should stop collecting; disabling future filesystem exposure must additionally revoke routing, retire handles and resolve queued transfers without touching Photos.

## Evidence classification

- **Verified static analysis:** method encodings, synchronous delegate dispatch, NSNumber return semantics, native sendResponse return meaning, async sendData completion, native transaction checks, constructor ownership described above.
- **Verified runtime observation in this analysis:** none. Diagnostic captures may independently establish process and file identity; they do not establish these hooks executed.
- **Strong inference:** scoped observation through these methods is substantially narrower than claiming the live USB interface; a request-level additive bridge can avoid the Photos storage constructor and USB private-table ABI.
- **Unverified hypothesis:** a full additive synthetic namespace can preserve every relevant Windows enumeration and Apple Photos lifecycle in the target process. It requires implementation tests and the physical acceptance test, not just this map.
