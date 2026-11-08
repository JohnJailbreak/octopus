# Octopus

Octopus is an iOS app for moving files between an iPhone and Windows through the portable-device (PTP/MTP) support Windows already has. The goal is an organised filesystem in File Explorer with browsing, downloads, uploads, renaming and deletion where permitted.

This first phase builds the app, a transport-independent file-object provider and the PTP/MTP protocol layer. **There is no live USB transport yet, so Windows cannot see Octopus in this build.** The app says so rather than pretending otherwise.

## Status

| Area | State |
| --- | --- |
| SwiftUI app, iOS 17+ | Implemented. Built and tested only by CI; no Swift toolchain was available while writing it. |
| File-object provider (C) | Implemented and tested. |
| PTP/MTP responder (C) | 20 operations, tested against an independently written encoder. |
| Service state machine | Disabled, Starting, Ready and Error are reachable. Waiting for connection and Connected are implemented and tested with a loopback transport, but need a USB transport to occur in the app. |
| USB transport | Not implemented. |
| Windows Explorer compatibility | Not observed on hardware. |

## The app

The main screen follows the Caz UI specification: the Sunshine Sweep backdrop, the translucent menu, the Fonarto wordmark, and the same spacing, springs, opacity transitions and haptics.

- **Enable USB Access** morphs into the activity sheet. It builds the synthetic storage, starts the responder, runs the protocol self-test (optional), reports that this build has no USB transport, and stops at Ready. The button then reads **Disable USB Access**, which releases everything.
- **USB Connection** shows the service state, the transport and session, which states this build can reach, and responder counters.
- **Filesystem** shows the storage, the object tree and, per object, its metadata and contents. Contents are read through the provider.
- **Activity** shows the operation log. **Run Self-Test** replays a recorded PTP/MTP session through the responder and checks every reply.
- **Settings** has haptics, self-test while enabling, the writable Scratch folder, version information and a link to this repository.

## Storage

```text
Octopus                (storage 0x00010001)
├── README.txt
├── Documents
│   ├── example.txt
│   └── sample.bin     4096 bytes of a fixed pattern
└── Information
    └── version.txt
```

Every object is generated in memory. Nothing from the phone's filesystem is exposed.

- **Read-only seed:** all seed objects are read-only. The storage reports itself as read-only.
- **Scratch:** with **Writable Scratch folder**, an empty `Scratch` folder is added. It is the only place a host may create, rename or delete objects.
- **Disposable directory backend:** a second backend serves a real directory. It refuses any directory that lacks an `.octopus-disposable` marker file, and it never follows symlinks out of it. The tests use it, and it is the starting point for a real filesystem backend.

## Protocol

Operations: GetDeviceInfo, OpenSession, CloseSession, GetStorageIDs, GetStorageInfo, GetNumObjects, GetObjectHandles, GetObjectInfo, GetObject, GetPartialObject, DeleteObject, SendObjectInfo, SendObject, GetDevicePropDesc and GetDevicePropValue (DeviceFriendlyName), GetObjectPropsSupported, GetObjectPropDesc, GetObjectPropValue, SetObjectPropValue (rename) and GetObjectPropList.

Object properties: StorageID, ObjectFormat, ProtectionStatus, ObjectSize, ObjectFileName, DateCreated, DateModified, ParentObject, PersistentUniqueObjectIdentifier and Name.

Behaviour:

- **Parsing:** containers are parsed with bounds checks. Malformed, truncated or unexpected input is answered with an error, or refused, and never read out of bounds.
- **Sessions:** transaction IDs are validated. An OpenSession that arrives during a session keeps the session and follows the host's numbering.
- **Uploads:** an upload reservation survives unrelated commands. A replacement SendObjectInfo, a missing data phase or a closed session releases it.
- **Limits:** data phases are buffered, up to 32 MiB each. Larger transfers need streaming, which is later work.
- **Events:** the responder sends no events.

## Repository

```text
Octopus/
  App/          SwiftUI app and the Swift bridge to the core (ServiceController.swift)
  Core/         provider, memory and directory backends, service state machine (C11)
  Protocol/     containers, datasets, MTP properties, responder (C11)
  Resources/    Fonarto Regular and its licence, asset catalog
  Tests/
    CoreTests/  C tests, recorded transcript, fuzzer
    AppTests/   XCTest for the bridge, controller state machine and layout
  module.modulemap   exposes the C core to Swift as OctopusCore
Makefile        native build and tests of the core
project.yml     XcodeGen project (the .xcodeproj is generated, not committed)
.github/workflows/build.yml
```

The core is C so it can be verified without Xcode, and so the same code can later link into a `ptpd` extension or a root helper. That is the route the earlier research recommends.

## Building and testing

The core builds with GCC or Clang:

```sh
make test       # unit tests and the recorded-transcript replay
make sanitize   # the same under AddressSanitizer, UndefinedBehaviorSanitizer and LeakSanitizer
make fuzz       # randomised and mutated PTP/MTP input against the responder
make analyze    # GCC static analyzer
```

On every push, GitHub Actions does the following:

1. Runs the core suite on Linux with GCC, and again with Apple Clang.
2. Generates the Xcode project with XcodeGen.
3. Runs the XCTest suite on an iOS Simulator.
4. Builds for device.
5. Uploads `Octopus.tipa` and `Octopus.ipa`. These are ad-hoc signed, for TrollStore or a jailbroken device.

Versions are `0.1.<run number>`.

On a Mac: `brew install xcodegen && xcodegen generate && open Octopus.xcodeproj`.

## Verification in this phase

- **C tests:** 1,671 checks pass, and they also pass cleanly under the sanitizers.
- **Fuzzing:** 300,000 fuzzing iterations ran without faults. The GCC analyzer reports nothing.
- **Transcript replay:** a 22-exchange enumeration session replays byte for byte. The expected bytes were computed by a separate encoder written from the PIMA 15740 and MTP 1.0 layouts. The session is modelled on the documented MTP flow; it is not a capture from Windows.
- **Swift:** the Swift sources were never compiled while being written. The first CI run is their first build.

## Design notes and differences from Caz

- **Backdrop:** the Sunshine Sweep image is not in the reference material.
  - `Backdrop.swift` reconstructs it from the specification's screenshots by inverting the documented panel and dimming layers.
  - The reconstruction is fitted with one gradient and four soft glows; it stays within 3.3 of 255 levels on average.
  - It is drawn natively and aspect-filled, as the original image was.
- **Wordmark:** the wordmark is live text in the bundled Fonarto Regular, sized from the font file to match Caz's capital height. Caz used an image.
- **Panels:** menu panels are a flat translucent fill. The specification's pixel samples show no blur.
- **Not adopted from Caz:** the update, changelog and installer screens are out of scope.

## Next milestone

Serve this synthetic storage over the iPhone's real USB PTP function and record the exact request sequence Windows sends. The recording then becomes a replay fixture for the responder.

## Credits

Fonarto by Locomotype, released as freeware for personal and commercial use. See `Octopus/Resources/Fonts/Fonarto-License.txt`.
