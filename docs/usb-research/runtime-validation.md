# Next physical experiment and build instructions

## Build before installing

This delivery is source, not a verified iPhone binary. Keep the working v0.4.8 IPA/TIPA for rollback. The original update/release mechanism is preserved. Do not publish to main merely to run CI: its existing workflow publishes releases after successful checks.

Option A: restore the delivered Git bundle, push `engineering/usb-evidence-boundary` with your authorized Git credentials, and open a draft pull request against main. The existing pull_request workflow runs Linux and macOS validation without publishing a release. This session could not push (GitHub integration HTTP403).

```sh
git clone octopus-usb-engineering.bundle octopus
cd octopus
git remote set-url origin https://github.com/m9z2w4d8vw-blip/octopus.git
git push -u origin engineering/usb-evidence-boundary
```

Option B: extract the source ZIP on a Mac with Xcode, Xcode command-line tools, XcodeGen and ldid. From the repository root:

```sh
make test sanitize
xcodegen generate
xcrun simctl list devices available
# Substitute an available iPhone simulator UDID from the preceding command.
xcodebuild test -project Octopus.xcodeproj -scheme Octopus -destination 'id=SIMULATOR-UDID' CODE_SIGNING_ALLOWED=NO
xcodebuild build -project Octopus.xcodeproj -scheme Octopus -configuration Release -destination 'generic/platform=iOS' -derivedDataPath build/DerivedData CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO CODE_SIGN_IDENTITY='' MARKETING_VERSION=0.4.9 CURRENT_PROJECT_VERSION=9 OCTOPUS_COMMIT=DELIVERED-COMMIT
bash tools/package.sh
```

Replace DELIVERED-COMMIT with the SHA in DELIVERY.txt; 0.4.9/build9 is a suggested **local engineering label**, not a published release. If that version is already in use, choose a greater engineering version to avoid updater ambiguity. Generated files: build/Octopus.tipa, build/Octopus.ipa, SHA256SUMS. Install the TIPA through your existing TrollStore workflow after successful tests. Do not install a guessed DEB, replace ptpd, change USB configuration, or enable injection for this experiment.

On Linux, preserve required `make test sanitize tsan fuzz analyze` checks. On Windows/MSYS, `make test-usb` verifies only the portable boundary; see status for inherited POSIX test failures and unavailable sanitizer runtimes.

## iPhone 13 / iOS17.0 / Dopamine experiment

1. Leave the USB cable disconnected. Launch the engineering Octopus build. Confirm its version/build/commit in diagnostics. Keep the existing self-test setting and synthetic read-only mode; do not select filesystem export or writes.
2. Tap **Enable USB Access** using the existing explicit control, then **Settings → Diagnostics → Run Full Diagnostics**. Export the ZIP and copy it to the PC or another location as `disconnected.zip` before another run. The app can remove older exports; preserve each externally.
3. Expected new results: `adapter.usb_boundary` PASS/SIMULATED; `device.ptp_registry` either PASS with match count (including zero) or BLOCKED with reason/kernel code; `device.ptpd_identity` either PASS with UUID/hash and reference-match flags or BLOCKED with exact read/parser error. A different hash is useful evidence, not authorization or proof of corruption. A UUID mismatch prohibits applying the static function map blindly.
4. Connect directly to Windows 11 using a known working data cable; unlock the phone and respond to Apple's trust/photo prompt as usual. Keep Octopus foreground. Run Full Diagnostics again and export `connected.zip` externally.
5. Open File Explorer → This PC → iPhone. Browse existing photo folders and copy one ordinary photo that you choose to a new PC folder. Do not alter or delete phone data. Record whether the copy completed; this is the Photos regression check.
6. While still connected, return to Octopus, run diagnostics again, export `after-explorer.zip`. Unplug and export `after-unplug.zip`. Compare match counts, process observations and timestamps across captures. They may not track cable attachment; that result is valuable and must not be treated as a failed transport proof.
7. Tap Disable USB Access. Retain the four ZIPs and the Windows report. Do not expect an Octopus storage or example.txt in Explorer from this build: all three external milestone checks should remain BLOCKED. A local discovery PASS is not a USB-transfer PASS.

## Windows report

From PowerShell in the extracted repository root, after connecting/unlocking:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .	ools\windows\octopus-usb-diagnostics.ps1 -OutFile .\windows-connected.txt
```

This existing script reads PnP/WPD configuration and Explorer storage names; no driver install is required. Record whether MI_00/class06/01/01 is still bound to Microsoft's existing WPD driver. Send the report and four diagnostic ZIPs, including build identity. The new archives include results.json/txt, failures.txt, build/settings, connectivity, device-observations.json, connection-timeline.txt and persistent logs. Do not mark Explorer success from PnP enumeration alone.

## Exact evidence needed before a live bridge

- Installed ptpd identity matching the map (or new matching executable analysis); effective authorization and read-only IOKit result on this phone.
- Matching IOKit device plug-in/shared-cache implementation and interface definitions for setup/read/write/control/lifetime; current offsets alone are insufficient.
- Read-only in-process class/method encodings, queue/lifetime and Photos-provider creation observations. The current app does not collect these yet; no safe injection tool is bundled.
- A demonstrated reversible provider/request routing design that preserves Photos and explicit Octopus enable/revoke, or a verified independent interface ownership contract.

## Later end-to-end acceptance (not possible with this release yet)

Only after the native integration exists: correlate a real Windows command with integration logs and transmitted response; verify synthetic storage, nested Documents folder and example.txt in Explorer; copy example.txt to the PC and run:

```powershell
(Get-FileHash -Algorithm SHA256 -LiteralPath .\example.txt).Hash.ToLowerInvariant()
```

Expected 76 bytes and `aeeb6cba90d7d6adfcd42d673f64897f26cfcc479c5cf489a86146e09a9d2bd2`. Recheck photo copy, unplug/reconnect, close/reopen session and disable behavior. Capture genuine request fixtures only with provenance. Do not begin root browsing/writes until these acceptance criteria pass.

---

## Physical-device diagnostic captures — v0.4.9 results (November 2026)

Four diagnostic archives were collected from an iPhone 13 / iOS 17.0 / Dopamine device:

| Stage | Label | Clock |
|-------|-------|-------|
| 01 | disconnected | 22:47 |
| 02 | connected (cable in, trust dialog accepted, Explorer opened) | 22:53 |
| 03 | after-explorer (photo browsed and copied through Windows) | 22:54:00 |
| 04 | after-unplug | 22:54:28 |

Each ZIP contained:
- `octopus-debug.log` — cumulative (stage N includes all stage 1..N entries)
- `diagnostics-report.json` — per-run check results

### Verified runtime observations

**ptpd identity confirmed.**
UUID `FFD54FC7-0508-3AB8-8D17-2F698B611E10` and SHA-256 `a1742358…085dcb` match the
analysed reference binary across all four stages. The analysed map is valid for this device.

**ptpd lifecycle on USB connect.**
ptpd was absent (no PID) in stage 01. It appeared as PID 39376 in stage 02 and remained at
the same PID through stages 03 and 04. This confirms launchd fires ptpd on USB connect and
leaves it running after the cable is unplugged — consistent with the binary analysis.

**IOKit PTP registry match present in the disconnected state.**
The device reported 1 IOKit PTP match in stage 01 (cable disconnected, ptpd not running).
This is a USB configuration descriptor registration — the firmware declares the PTP interface
regardless of whether a cable is attached. It is **not** a signal that a cable is connected or
that the PTP interface is active. The diagnostic check `device.ptp_registry` correctly labels
this as an IOKit registry observation, not a live-connection indicator.

**Zero external USB transactions reached Octopus in all four stages.**
The transport was `OCT_TRANSPORT_UNAVAILABLE` in every stage. No PTP commands, data phases, or
response containers were handled by oct_service. This is expected: v0.4.x builds provide no
means for an external host to route USB traffic to the Octopus responder. The Windows–Photos
copy in stage 03 went through ptpd's own PTP-over-USB interface; Octopus was not involved.

### Clarified inferences (not directly observed)

- The stage labels (01 disconnected, 02 connected, 03 after-explorer, 04 after-unplug) are
  inferred from log timestamps and ptpd PID changes. No hardware telemetry distinguishes
  the stages directly.

- Photos transfer worked in stage 03 through Apple's ptpd. This is a strong inference from
  ptpd being present and no Octopus transactions being recorded — it is not a byte-level
  trace of the Photos transfer path.

### Key finding for OctopusBridge design

The `addStorage:` decompilation at offset `0x10000daf0` shows that when the storages
dictionary already contains 1 entry, ptpd logs "Two storages cannot co-exist, should quit"
but **still executes the dictionary insert** (the `b.ne` at `0x10000db40` checks count==1
but both branches converge at the actual insert). Registering a second storage does not
hard-crash ptpd at the insert. It does not show that ptpd can then serve a second storage
correctly: what ptpd calls on the inserted object afterwards is unobserved (see below).

### Remaining unknown

The exact set of Objective-C methods ptpd calls on a PTPStorage object when routing
GetObjectHandles, GetObjectInfo and GetObject is not directly observable from the binary
analysis alone. The draft OctopusBridge tweak (Octopus/Bridge, v0.5) hooks PTPVirtualCamera
methods whose class is confirmed in the decompiled evidence (initWithName:andPath:,
addStorage:, objectHandlesInStorage:..., storageInfoDatasetForStorageID:,
dataSourceForObjectHandle:responseCode:size:) rather than conforming to the whole PTPStorage
protocol. Any other selector ptpd sends to the inserted storage object would raise an
unrecognized-selector exception inside ptpd. A Frida observation inside a live ptpd process
would list those calls; it is one of the open items below.

### Open items before an end-to-end test (v0.5)

- The Octopus app does not start the bridge server yet: nothing creates the socket at
  `/var/mobile/Library/Caches/Octopus/octopus-bridge.sock` on the phone, so a client has
  nothing to connect to. The transport itself is covered by the C tests and by the simulated
  `adapter.bridge` check in Run Full Diagnostics, which runs a server and client on a private
  socket on the phone.
- The OctopusBridge tweak has never been compiled. Its draft serves requests over short-lived
  connections while also holding a long-lived one, but the server accepts one client at a time,
  so those requests would wait behind the long-lived connection. Do not install it.
- The PTPStorage calls described above.

---

## v0.5 integration status and staged testing plan

### Stage 1 — App bridge socket: IMPLEMENTED

`ServiceController.swift` creates an `oct_bridge *` transport on every `enable()` call and
tears it down on `disable()`. The service starts with the bridge transport attached, so
`/var/mobile/Library/Caches/Octopus/octopus-bridge.sock` is created the moment USB Access
is enabled. It is removed on disable or app termination.

**Backgrounding:** A `UIBackgroundTaskIdentifier` is requested when the app moves to the
background while USB Access is enabled, buying ~30 seconds for in-flight Windows operations.
iOS may still suspend the process after this. Users should keep Octopus in the foreground
while transferring files for reliable operation.

**To verify Stage 1 on the device:**
1. Enable USB Access in Octopus.
2. Settings → Diagnostics → Run Full Diagnostics.
3. The `bridge.app_socket` core check should report PASS with a PING response.
4. The `bridge.app_active` app check should report PASS.

### Stage 2 — Native component availability: IMPLEMENTED (identity only)

`OctopusBridge.m` is built as a Logos/Theos tweak. Its `%ctor` reads the running ptpd
binary's LC_UUID and aborts without installing any hooks if the UUID doesn't match the
analysed reference build (FFD54FC7-0508-3AB8-8D17-2F698B611E10). This protects Photos on
unrecognised builds.

With a matching UUID, it connects to the app socket, sends a PING, and logs the result.
`HOOKS_ENABLED` is 0 by default: no PTP behaviour is changed.

**CI status:** The tweak job in `.github/workflows/build.yml` syntax-checks the C core
and parses `OctopusBridge.m`. A full arm64e Theos build requires macOS + Xcode + iOS SDK.

### Stage 3 — Runtime observation: PENDING (physical device required)

The PTP hooks are guarded behind `HOOKS_ENABLED=0`. To complete Stage 3:

1. Install the tweak with `HOOKS_ENABLED=0` on the reference device.
2. Connect the iPhone to Windows 11 via USB.
3. Trust the PC in the iPhone prompt, open File Explorer → This PC.
4. Run Settings → Diagnostics → Export Diagnostics and send the ZIP.
5. The ZIP's `connectivity.txt` and bridge-related log lines will show:
   a. Whether `transportActivated` fired (Windows saw the PTP interface).
   b. Whether the bridge socket is reachable.
   c. What unrecognised selectors ptpd called on `OctopusPTPStorage` (from the
      `forwardInvocation:` log lines — this is the key Stage 3 output).
6. Update this document with those selector observations.
7. Implement the missing selectors in `OctopusPTPStorage`.
8. Rebuild the tweak with `HOOKS_ENABLED=1` for Stage 4.

### Open items before Stage 4

- The bridge server (app socket) must be running before ptpd calls `initWithName:andPath:`.
  With `HOOKS_ENABLED=1`, the tweak will only inject storage if the app is already open.
  A reliable solution: install a small launchd user agent that keeps the app running when
  USB is connected. (Low priority; the user can open the app manually first.)
- The session-level bridge protocol needs adjustment: for each object operation the tweak
  currently opens a new PTP session over the socket. The bridge server accepts one session
  at a time; two operations in quick succession may race. Use a persistent session or
  correlated request/response tags. This is a known issue to fix before Stage 4.
