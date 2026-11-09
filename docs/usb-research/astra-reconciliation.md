# Reconciling Astra's local handoff with sonnet

Astra's archive was a working tree whose `.git` sat at `742cca7` (engineering/native-ptp-observer), with
later deliveries of this branch copied over it and Astra's own edits on top. Copying it over sonnet would
have mixed three things, so every file was classified against every commit in this branch's history first,
with line endings normalized so CRLF did not count as a change.

| Classification | Files | Decision | Reason |
|---|---|---|---|
| Identical to sonnet | 210, including 104 evidence files | Already present | Nothing to do |
| Older copy of this branch's own work | `Protocol/oct_responder.c`, `Core/oct_service.c`, `Tests/CoreTests/test_responder.c`, `Tests/CoreTests/test_store.c` (all equal to `d229457`), `Tests/AppTests/DiagnosticsTests.swift` (equal to `3d4256a`), `docs/usb-research/implementation-status.md` | **Kept sonnet's** | Taking them would have reverted the enumeration budget, its tests and the archive-count fix |
| Astra edit, merged three-way from its true base | `Native/OctopusProvider.m`, `Native/OctopusObserver.m`, `Native/Tests/ObserverTests.m`, `Core/oct_store.c`, `Core/oct_store.h`, `Core/oct_backend_directory.c`, `Core/oct_backend_memory.c`, `App/NativeIntegration.swift`, `App/Diagnostics.swift`, `App/ServiceController.swift`, `App/Windows.swift`, `module.modulemap`, `.github/workflows/build.yml`, `.gitignore`, `docs/usb-research/native-build.md` | **Imported** | Clean merges; sonnet's changes in the same files (budget, streaming source, per-call collision check) survive |
| Astra edit, conflicting | `tools/test_provider.sh`, `tools/test_observer.sh` | **Resolved** | Kept sonnet's `xcrun --sdk macosx clang` fix; added Astra's dataset source to the provider test |
| New in Astra | `Native/oct_native_dataset.c/.h`, `Native/Tests/DatasetTests.c`, `tools/test_native_dataset.sh`, `Core/oct_backend_readonly.c`, `Core/oct_readonly.h` | **Imported after audit** | Dataset parser bounds-checked against the PTP DeviceInfo layout; read-only backend now has 149 tests of its own |
| Raw workspace extras | duplicate baselines, prebuilt tools, binary RE data, temporary files | **Rejected** | Not source; never committed |

## Verified rather than assumed

Every Apple method the imported code calls was checked against the ptpd function table and method encodings
by owning class and exact encoding at its address, because one wrong encoding disables the whole module:
`PTPVirtualCamera deviceInfoDataset` (`@16@0:8`),
`objectInfoForObjectsInStorage:forObjectFormatCode:inAssociation:withContentType:responseCode:`
(`@40@0:8I16S20I24i28^S32`), `PTPDeviceInfoDataset content` (`@16@0:8`), `PTPObjectInfoDataset initWithData:`
(`@24@0:8@16`), `setObjectHandle:` (`v20@0:8I16`), `content:` (`@20@0:8i16`), and
`PTPOperationRequestPacket numParameters` (`i16@0:8`) and `parameterAtIndex:` (`I20@0:8I16`). All matched.

## Gaps in the interrupted work, fixed here

- `tools/build_native.sh` never compiled `Octopus/Native/*.c`, so the dylib would have failed to link on the
  provider's call into the DeviceInfo parser.
- `ProviderTests.m` was never updated for the two new hooks. `OCTProviderValidate` requires every hooked
  selector to exist with its exact encoding, so the test's first assertion would have aborted the macOS CI step.
- The DeviceInfo hook returns a substitute object in place of Apple's `PTPDeviceInfoDataset`, but
  `getDeviceInfo:` (`0x100020bbc`) was never captured, so it is unknown which messages Apple sends it. The
  substitute now delegates any message it does not implement to Apple's genuine object.
- `publish()` still discarded the status write's result, as the handoff notes said.

## Astra's validation logs

The handoff's host logs report earlier totals (for example 95,475 assertions and a 300,000-iteration
unsanitized fuzz) from a Windows/MSYS environment where sanitizer runtimes were unavailable and the helper
tests were affected by symlink semantics. They describe that environment, not this branch; every suite was
rerun here, and none of those numbers is a device result.
