# Implementation status and the staged device procedure

What is built, what is proven, and what is still unknown. Written against the branch
`engineering/native-ptp-observer`. Nothing here has run on a phone or against Windows.

## Evidence levels

Each claim below carries one of these. They are not interchangeable.

| Level | Meaning |
|---|---|
| **Tested** | Exercised by the portable C suites on Linux, under the sanitizers |
| **Type-checked** | Its C calls compile against the real headers; the Objective-C around them has not been compiled |
| **Compiled for iOS** | Built by the Apple toolchain for arm64e |
| **Verified in ptpd** | Observed running inside the real process on the device |
| **Verified with Windows** | Observed serving a real File Explorer session |

As of this commit **nothing is Compiled for iOS, Verified in ptpd, or Verified with Windows.**
There is no Swift, Objective-C or Theos toolchain in the environment this work was done in, so
the macOS CI job is the first real compile of the native component.

## What the portable core can do

**Tested.** The C core has a real filesystem backend (`oct_backend_directory.c`) with
`pread`-based offset reads, `O_NOFOLLOW` on every path component, and create, write, truncate,
remove and rename. Directory listing is lazy: one directory's children load on demand, which is
exactly how PTP enumerates, so recursive browsing needs no eager index.

### Two blockers to pointing that at a real filesystem, now fixed

**1. Unbounded enumeration (tested).** PTP's `GetObjectHandles` means "the whole hierarchy" when
the host passes association 0, and the responder mapped that onto a recursive walk from the root.
Over the six-object synthetic tree that is free. Over `/` it never finishes: the handle list and
the object table grow until the allocator refuses, inside the process that also serves Photos.

`oct_store_options.max_objects` (default 65536) now caps both the handles one enumeration returns
and the objects the store tracks at once. Exceeding it yields `OCT_E_TOO_LARGE`, which the
responder reports as `0xA808` specification-by-depth-unsupported — the MTP code for a depth the
device will not serve, which leads a host to enumerate one directory at a time. Per-directory
listing is unaffected.

**2. A 32 MiB ceiling on downloads (tested, and type-checked in the provider).** The native data
source held an entire file in an `NSData` before Apple's writer read a byte of it, so a download
cost the file's size in memory and could not exceed `OCT_PTP_MAX_TRANSFER`. On a real filesystem
that rules out most media.

`PTPDataPacket::copyToBuffer:numBytes:` already asks the source for one chunk at a time through
`-read:length:offset:`, so the source now fetches each chunk with `oct_store_read` at that offset
and holds nothing. A file above 4 GiB is refused with `OBJECT_TOO_LARGE` rather than having its
32-bit size truncated, which would make Apple send a short data phase and the host write a corrupt
file. Authorization is rechecked per chunk, so a download stops at the next chunk after a lease
expires.

The streaming contract is tested against the directory backend over a file larger than the old
ceiling: every byte matches through chunked offset reads, an oversized request is clamped to what
remains, the end of the file reads nothing and succeeds, and past the end is an error rather than
silent zeros.

## What the native component does

**Type-checked only.** It observes and overlays; it does not take ownership of the protocol.

- Before any hook, `OctopusRuntime.m` checks the method's runtime type encoding and that its
  implementation lives in `/usr/libexec/ptpd`, and refuses on mismatch or on a concurrent change.
- `OctopusObserver.m` gates on iOS 17.0.0, the exact ptpd UUID, arm64e and the on-disk SHA-256.
  It opens a thread-local scope around `handleBulkData:result:`, so a `handleRequest:` nested
  inside it is known to come from real host traffic rather than any internal call.
- `OctopusProvider.m` appends a synthetic storage to Apple's own provider results instead of
  calling `addStorage:`. Apple keeps sessions, transaction numbers, USB buffers, asynchronous
  writes and final responses.

Handle ownership is now a store lookup rather than a fixed list, so objects discovered as the host
browses are recognized. The collision check still runs on every call, as it must, since Apple can
allocate a handle in our range at any time; it now asks about one handle instead of every one we
own. Apple's handles come from `FUN_100004224`, which only increments a global counter and is called
only by the `PTPFolder` and `PTPFile` initializers, so Apple cannot hold a handle in our range
without first having allocated its base. That makes the check one message into Apple's code per call
rather than one per owned object per call, which a filesystem would make unbounded.

Directory listings were also capped at six entries, a sanity bound for the synthetic tree that would
have refused every larger real directory. They are now bounded by the store's enumeration budget.

## Why the design should survive contact with Apple's code

**Traced from the disassembly, not assumed.** The risk with an overlay is that a handler validates
a handle or storage before reaching the provider, so synthetic IDs are rejected before any hook
runs. `objectForObjectHandle:` is called only from inside the provider methods themselves, each of
which returns early for synthetic handles, and no handler validates storage through
`storageWithID:`. So the early-return design holds for the paths covered by the evidence.

## What is still unknown

- **Whether Windows accepts a second storage at all.** Appending a storage ID does not mean
  Explorer shows a volume. It may ignore it, show it empty, or reject the device. Not answerable
  from static analysis.
- **Three unhooked provider methods** have no callers in the collected evidence, but the evidence
  is a subset of the binary: `deviceInfoDataset` (which tells the host which operations and
  formats exist), `visibleStorage:` (which may filter storages), and the bulk
  `objectInfoForObjectsInStorage:forObjectFormatCode:inAssociation:withContentType:responseCode:`.
  If Explorer shows the storage but no files, these are the first places to look.
- **`hostIsWindows` exists in ptpd**, so there is Windows-specific behaviour in there that has not
  been traced.
- **Whether ptpd can read the paths we would serve.** Its launchd job runs as `UserName = mobile`,
  recorded in the v0.4.9 device capture, which settles that it shares a uid with the app and can
  therefore reach the lease directory. It does not establish what that uid can read under `/`.
- **Write operations.** The core has create, write, truncate, remove and rename, and the responder
  accepts `SendObjectInfo`, `SendObject` and `SetObjectPropValue`. None of that is wired into the
  native provider, and no write path has been exercised against Apple's responder.

## The v0.4.20 device run, and what the next run decides

**Observed.** Three diagnostic exports (app only, after installing the DEB, after a reboot) each gave 29 pass,
0 fail, 5 blocked. The companion was installed at `/var/jb/usr/lib/TweakInject/OctopusNative.dylib` with a
`ptpd` executable filter, Dopamine's `systemhook.dylib` was in ptpd's environment, the app wrote its lease as
uid 501 and ptpd ran as uid 501. The status file never appeared: the app read it as `OCT_E_IO` with `ENOENT`.
Windows kept showing Apple's Photos folders. Nothing shows the companion loaded, a native USB callback, or a
synthetic object.

**Leading hypothesis, not proven.** `launchctl procinfo` lists ptpd's entitlements, and they include
`seatbelt-profiles = ("ptpd")`: ptpd runs under Apple's ptpd sandbox. Its other entitlements cover Photos
storage, its own temporary space and some preference domains; nothing suggests access to
`/var/mobile/Library/Caches/Octopus/native`. The same uid does not give the same sandbox rights. If the sandbox
refuses that folder, then the lease is never read, nothing is hooked, Photos is unaffected and no status is
written, which is everything observed. The profile's rules are not in the evidence, and whether the dylib
loaded at all is still unknown.

**Not causes.** zsh's `log` builtin shadowed Apple's tool and `/usr/bin/log` is absent, so the attempted log
query never ran; the `zsh/watch.so` signature message came from the shell, not from Octopus; the `plutil -lint`
attempt used an unsupported syntax; `launchctl procinfo` reporting `status = stopped` is a moment's scheduling
state, and `ps` showed ptpd alive throughout.

**What changed so one run can tell.** The status file was the only channel, so it could not report its own
failure. There are now two channels that need no files:

- **A startup signal** posted through Darwin notifications: how far startup got, which identity check failed,
  and the exact stage and errno of the last lease read and status write. Run Full Diagnostics shows it as
  **Companion startup signal**, with one verdict: not loaded, a stale ptpd, rejected identity, sandbox refusal,
  waiting for a lease, status write failed, hooks refused, or running.
- **An os_log marker** under subsystem `com.parv.octopus.native` the moment the constructor runs in ptpd.
  Readable from Windows with `idevicesyslog`, not on the phone.

Stage and errno are captured at the failing call. A refusal of `open()` by the kernel or sandbox is reported
separately from a folder that opened but is not private, which used to look identical.

## The next device run

One run, observer-only, answers the open question. Nothing in it exposes a synthetic storage.

1. Install the prerelease's Octopus.tipa with TrollStore and its paired DEB with Sileo. Leave the provider off.
2. Open Octopus, enable USB Access, keep it in the foreground, and connect the cable to Windows.
3. **Photos regression first:** in File Explorer, copy one ordinary photo from DCIM. If that fails, stop and
   roll back.
4. Run Full Diagnostics and export. Read **Companion startup signal**:
   - *No startup signal*: the dylib never ran its constructor in ptpd. Next: injection or code signing, not the
     sandbox. Optionally run `idevicesyslog | findstr OctopusNative` on Windows while reconnecting the cable.
   - *Sandbox refused the shared folder*, with a stage and errno: the hypothesis holds; the lease and status
     must move somewhere ptpd's profile allows.
   - *Rejected identity*: a different ptpd build; nothing was hooked, by design.
   - *Running*: then check **Native observer** for commands paired with Apple's responses during the copy.
5. Only after a *running* verdict with paired callbacks: authorize the synthetic provider, reconnect, and look
   for a second volume. Download `Documents/example.txt` and confirm 76 bytes and SHA-256
   `aeeb6cba90d7d6adfcd42d673f64897f26cfcc479c5cf489a86146e09a9d2bd2`.

The read-only filesystem backend is not connected to the live provider and stays that way until step 5
succeeds. Writes are not implemented.

**Roll back** at any point: uninstall the companion in Sileo and `killall -9 ptpd`, or reboot without
re-running Dopamine; rootless tweaks load only while jailbroken, so that boot runs stock ptpd.

## The staged device procedure

Each stage answers one question and stops. Do not skip ahead: a later stage's failure is
uninterpretable without the earlier one's result.

**Stage 1 — the app alone.** Touches nothing in ptpd, so there is nothing to recover from.
Install the IPA/TIPA, open Octopus, enable USB Access with no cable attached, run Run Full
Diagnostics, then disable and run them again. Send both exports. This confirms the app builds,
the authorization lease is written and withdrawn, and the diagnostics capture it.

**Stage 2 — the component loads.** Install the DEB from the `tweak`/`native` CI artifact for this
exact commit. With Octopus open and USB Access enabled, attach the cable, then `killall -9 ptpd`
so it reloads. Export diagnostics. The `loaded` line records the reference UUID and the uid and
gid ptpd runs as. A UUID mismatch means no hooks were installed at all, which is the safe outcome
on any other build.

**Stage 3 — real traffic, read-only.** Connect to Windows 11, trust the PC, open the iPhone in
File Explorer and copy one ordinary photo. Export diagnostics. The journal should show commands
inside native USB receive callbacks paired with Apple's successful response writes. Confirm the
photo copied and Photos import still works. This proves the observation path without the overlay
having changed anything.

**Stage 4 — the synthetic storage.** Only after Stage 3. Authorize the provider, reconnect, and
look for a second volume in Explorer. If it appears, open `Documents/example.txt` and download it,
then verify 76 bytes and SHA-256
`aeeb6cba90d7d6adfcd42d673f64897f26cfcc479c5cf489a86146e09a9d2bd2`. If the volume appears but is
empty, capture the diagnostics: that is the signature of the unhooked enumeration paths above.

**Stage 5 — the real filesystem, read-only.** Only after Stage 4 downloads a file correctly. Point
the store at a real directory, not `/`, and confirm recursive browsing and a download. `/` comes
after a budget has been chosen from what Stage 5 shows about Explorer's enumeration behaviour.

**Stage 6 — writes.** Only in a disposable directory, and only after Stage 5.

## Recovery

The filter is `ptpd` alone, so nothing else is injected and the phone stays usable. The worst case
is that USB photo import stops working until ptpd is clean.

1. `killall -9 ptpd` — launchd starts it again on the next connection.
2. Uninstall the package in Sileo, then `killall -9 ptpd`.
3. Remove the files by hand, from both locations a rootless tweak can occupy:
   `/var/jb/Library/MobileSubstrate/DynamicLibraries/` and `/var/jb/usr/lib/TweakInject/`.
   The CI job prints `dpkg-deb --contents`, which names the real paths.
4. Reboot without re-running Dopamine. Rootless tweaks load only while jailbroken, so an
   unjailbroken boot runs stock ptpd with nothing injected. This needs no SSH, Sileo or
   filesystem access and is the guaranteed way out.

Before trusting photo import again, reconnect and copy one ordinary photo.

## Writes, and what stands between here and uploading (11 November 2026)

What a host can do now: delete. `deleteObjectWithObjectHandle:responseCode:` is a
method on the class the provider already hooks, recorded from the device as
`v28@0:8I16^S20`, so DeleteObject arrives as a plain method call with the handle
and a response-code pointer. `GetDeviceInfo` advertises DeleteObject when the
served store accepts changes, because a host reads OperationsSupported to decide
what to offer.

What the backend can do now: create, write, truncate, remove and rename, inside
the served root, when the lease grants writes. All five are tested against a real
directory, including a write at an offset and a delete that really removes the
file, and all five refuse when the lease lapses mid-operation.

Two of those capabilities a host cannot yet reach, and the reason is the same for
both.

- **Uploading a file.** PTP sends SendObjectInfo then SendObject. Neither has a
  per-object receiver on the camera class: of the 786 methods recorded from
  `/usr/libexec/ptpd`, none takes an incoming object.
- **Renaming from the host.** Explorer renames over MTP with SetObjectPropValue
  on ObjectFileName. There is no `setObjectPropValue` among the recorded methods
  either, only `objectPropValueForHandle:withObjectPropertyCode:responseCode:`,
  which reads.

Both therefore need `handleRequest:` (`0x100023384`, `@24@0:8@16`), which the
observer already wraps for counting. Handling an operation there means more than
returning a different value: Apple's machinery runs the data phase after that
call, so an interception would have to produce the response itself and, for
SendObject, consume a host-to-device data phase that arrives through
`handleBulkData:result:` (also already wrapped). That is a two-phase transaction
inside a state machine we did not write, and unlike every capability added so
far, it cannot be made safe by forwarding to Apple's implementation when the
handle is not ours: by the time the operation is recognised the transaction has
begun.

So the next step is an experiment, not an implementation: record what
`handleRequest:` receives and returns for a SendObjectInfo aimed at Apple's own
storage, and what `handleBulkData:result:` sees during the data phase, before any
attempt to answer one. The observer can do that with the hooks it already has.

## Why an upload hangs, from Apple's own code (0.4.68)

Established on hardware on 0.4.68, with USB access enabled before plugging in: Windows offered
the drop ("+ Copy"), sent SendObjectInfo once, and stalled at "Copying..." with no progress. The
traffic report says ptpd never answered that request, and the write report says the incoming
ObjectInfo was never parsed (initWithData:contentType: never fired). The write probe, run inside
ptpd, says files can be created in /var/tmp, /var/mobile/Media and DCIM, so the sandbox is not the
obstacle. Windows also read DeviceInfo twice: DeviceInfoChanged sent through the transport, unlike
the responder hand-off in 0.4.67, does make it re-read its capabilities.

From the decompiled PTPResponder handleRequest: (0x100023384, 1304 bytes, in evidence/ptpd):

- It reads the operation code and dispatches through three jump tables: 0x1001-0x1023, 0x9001-0x9010
  and 0x9801-0x9805. Everything else goes to the default.
- SendObjectInfo 0x100C and SendObject 0x100D share one destination with about twenty other
  operations Apple does not implement: handleUnsupportedRequest:.
- handleRequest: returns [NSNumber numberWithBool:]. It is NO on every path except two: Apple's
  vendor operations 0x900B handleSetHostMediaCapabilities: and 0x900E
  handleSetHostSupportedFormats:, which return their handler's BOOL. Both are operations with a
  host-to-device data phase, so the returned value tells the transport whether to read one next.
  For SendObjectInfo it is NO, so the ObjectInfo Windows sends is never read, and the unsupported
  path does not answer while a data phase is outstanding: the host waits indefinitely.
- PTPResponder handleStartData: (0x100023994) is 4 bytes, one instruction: an empty stub.
  handleData: (0x100023998) is 320 bytes and is presumably where the vendor operations' data is
  taken; its body is not yet decompiled here.

The upload handler therefore follows Apple's own template for 0x900B and 0x900E:

1. In handleRequest:, for SendObjectInfo whose storage parameter is Octopus's, record the pending
   transaction (its ID, the parent in parameter 2) and return @YES without calling Apple's, so the
   transport reads the data phase. SendObject follows an accepted SendObjectInfo the same way.
2. In handleData:, for a packet whose transaction is pending and ours, take [packet data]; forward
   every other packet to Apple's untouched.
3. When the data is complete, answer through the transport's sendResponse: with a
   PTPOperationResponsePacket: for SendObjectInfo, OK with storage, parent and the new handle; for
   SendObject, OK once the bytes are written, or the store's refusal.

Encodings recorded from the device:

- PTPResponder handleRequest: @24@0:8@16; handleStartData: and handleData: v24@0:8@16;
  handleUnsupportedRequest: v24@0:8@16; sentData:responseCode: v32@0:8@16@24;
  sendRestartSessionEvent v16@0:8, which also gives a way to make a host restart its session.
- PTPDeviceUSBTransport sendResponse: B24@0:8@16.
- PTPOperationResponsePacket initWithResponseCode:transactionID:parameter1:parameter2:parameter3:
  @36@0:8S16I20I24I28I32; initWithResponseCode:transactionID: @24@0:8S16I20.
- PTPDataPacket data @16@0:8; bytesTransferred and bufferSize q16@0:8.

Still unknown before implementing: whether handleData: is called once per data phase or once per
chunk, which decides how a large file is assembled; and what handleStartData: is given, which may
carry the total length. Decompiling handleData: from the device's ptpd answers both.

## How ptpd dispatches a packet, and what a data phase needs (from the binary, 0.4.68)

Decoded from the device's own ptpd with tools/macho_parse.py and tools/macho_disassemble.py,
written here because no disassembler in this environment reads Mach-O arm64e. The decoded
PTPResponder handleData: is kept at evidence/ptpd/100023998.txt.

PTPDeviceUSBTransport handleBulkData:result: (0x1000093bc, 2120 bytes) does not call the responder
through a compiled selector at all. It builds a packet with initWithUSBBuffer:, then:

    NSSelectorFromString(...) -> objc_opt_respondsToSelector -> performSelector:withObject: -> boolValue

The handler names are plain C strings in __cstring: "handleRequest:" at 0x100035db1,
"handleData:" at 0x100035ded, "handleResponse:" at 0x100035e22, "handleEvent:" at 0x100035e95.
Two consequences, both decisive:

- Replacing handleRequest: or handleData: on PTPResponder is picked up. performSelector: goes
  through ordinary dispatch, so an exchanged implementation is what runs. There is no cached
  function pointer to defeat it.
- The id that handleRequest: returns is read with boolValue, and that is what tells the transport
  whether to read a host-to-device data phase next. handleRequest: returns
  [NSNumber numberWithBool:], NO on every path but Apple's own 0x900B and 0x900E. So for
  SendObjectInfo the transport is told there is no data to read, the ObjectInfo Windows sends is
  never read, and because the unsupported path answers nothing while the host is still waiting to
  send, Windows hangs at "Copying...". That is 0.4.68 exactly.

handleData: (0x100023998, 320 bytes) takes ONE packet holding the WHOLE data phase: it calls
[packet data] once and passes that object on, with no loop, no offset and no accumulation. The
transport assembles every chunk first. A handler of ours therefore receives a complete file in one
call, which was the open question. handleStartData: (0x100023994) is a single ret: nothing arrives
there, so the total length does not come that way either.

handleData: dispatches on [packet operationCode], knows 0x900E and 0x900B, and answers everything
else 0x2019 DeviceBusy. So returning YES from handleRequest: without also handling handleData:
would make Apple answer DeviceBusy rather than hang, which is a safe failure to fall back to.

Apple answers a host like this, and so must we:

    resp = [[PTPOperationResponsePacket alloc] initWithResponseCode:code transactionID:tid];
    [transport sendResponse:resp];                     // transport is the responder's ivar at 0x8

with initWithResponseCode:transactionID:parameter1: for the three-parameter form, the responder
keeping its transport at ivar 0x8 and the current transaction ID at ivar 0x38. Reading another
process's ivars by offset is brittle, so the handler should take the transaction from the request
packet it was given and the transport from the observer, which already holds it for events.

### The upload handler this implies

1. handleRequest: for SendObjectInfo 0x100C whose storage parameter is Octopus's: record the
   pending transaction and return [NSNumber numberWithBool:YES] without calling Apple's, so the
   transport reads the data phase. Every other request, including an upload to Apple's own storage,
   goes to Apple's implementation untouched.
2. handleData: for a packet whose transaction is the pending one: take [packet data], parse the
   ObjectInfo with the responder's own parser, create the file, and answer OK with storage, parent
   and the new handle through sendResponse:. Everything else forwards.
3. SendObject 0x100D after an accepted SendObjectInfo: the same, with the bytes written to the file
   just created, and the store's refusal returned as the response code if it fails.

What remains unknown: whether the transport imposes a maximum data-phase size before calling
handleData:, which would matter for a large file. A small file tests the path either way.
