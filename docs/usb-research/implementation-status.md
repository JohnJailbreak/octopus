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
browses are recognized; the handle-collision question is answered once, on the first provider call,
instead of costing a message into Apple's code per object per call.

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
