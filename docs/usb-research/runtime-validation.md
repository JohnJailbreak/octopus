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

## v0.5 integration status, recovery, and the staged device procedure

### Correction to the earlier plan

An earlier draft of this document said the native component would learn the storage interface by
registering a stub `OctopusPTPStorage` and logging, through `forwardInvocation:`, the selectors
ptpd sent to it. That plan could not work. The stub was registered only when the storage hooks
were switched on, so with them off — the shipped default — the object was never created and no
selector was ever observed. The plan promised evidence that the safety gate made impossible.

It was also unsafe. Absorbing an unknown selector means inventing a return value for a signature
you do not know: a method that returns a struct, a double, or a 128-bit value would have had a
pointer-sized zero written into its return slot. Both the stub and the forwarding are gone.

### What the component does instead

It reads ptpd's own objects. ptpd passes its real storage to `-[PTPVirtualCamera addStorage:]` and
returns it from `-storageWithID:`; hooking those yields a pointer to the genuine storage object,
and the Objective-C runtime then reports its class, its superclass chain, its full instance-method
list with type encodings, and its ivar layout. The same is done for whatever `-objectForObjectHandle:`
returns (`PTPFile` in the decompilation) and for the data source behind `-dataSourceForObjectHandle:`.

Every hook calls `%orig` first and returns Apple's result unchanged. Nothing is registered,
substituted, or altered, and no object of ours is ever handed to ptpd.

This yields the authoritative interface, read out of Apple's own object rather than guessed. It
does **not** yield the call sequence a Windows transfer uses — for that the `request`,
`object_handles`, `object_info` and `data_source` log lines record each call as it happens, with a
PC attached.

### What none of this establishes

Knowing the storage interface does not mean a second storage will work. `addStorage:` continues
past its own "Two storages cannot co-exist, should quit" warning to the dictionary insert, which
shows only that the insert happens. Whether ptpd then serves two storages correctly, and whether
Windows File Explorer shows a second volume rather than ignoring it, showing it empty, or
rejecting the device, is unknown and cannot be settled by static analysis or by this observation
build. It needs a Windows host.

### Collecting the component's log

The tweak runs inside ptpd, a different process from the app, so it writes its own file:

    /var/mobile/Library/Caches/Octopus/octopus-bridge-tweak.log

rotated at 256 KB with two older generations kept. Two ways to read it:

1. **Export Diagnostics** copies it, and its rotated generations, into the ZIP under `logs/`.
   The `bridge.tweak_log` check reports how many files exist and how many class, method and
   host-attachment observations the newest one holds.
2. **Over SSH**, which also works when the app can't read the file:

   ```sh
   ssh mobile@<iphone-address> 'cat /var/mobile/Library/Caches/Octopus/octopus-bridge-tweak.log'
   ssh mobile@<iphone-address> 'grep -E " (class|method|ivar) " /var/mobile/Library/Caches/Octopus/octopus-bridge-tweak.log'
   ```

If ptpd cannot write that path, the lines stay in the in-memory ring and the file is absent;
`bridge.tweak_log` reports BLOCKED and says so, rather than implying the tweak never loaded.

### Recovery if the component destabilises ptpd

The tweak's filter is `ptpd` alone, so nothing else is injected: SpringBoard, the phone, and the
rest of the system are unaffected by anything it does. The worst case is that USB photo import
stops working until ptpd is clean again. In increasing order of severity:

1. **Restart ptpd.** Unplug the cable, then:
   ```sh
   killall -9 ptpd
   ```
   launchd starts it again on the next USB connection, which is also the fastest way to reload the
   tweak after reinstalling it.
2. **Uninstall the package** in Sileo, then `killall -9 ptpd`.
3. **Remove the files by hand**, when Sileo can't. Theos's rootless scheme installs to
   `/var/jb/Library/MobileSubstrate/DynamicLibraries/`, and Dopamine also exposes tweaks at
   `/var/jb/usr/lib/TweakInject/`; one is a link to the other. Removing from both is safe either way:
   ```sh
   rm -f /var/jb/Library/MobileSubstrate/DynamicLibraries/OctopusBridge.dylib \
         /var/jb/Library/MobileSubstrate/DynamicLibraries/OctopusBridge.plist \
         /var/jb/usr/lib/TweakInject/OctopusBridge.dylib \
         /var/jb/usr/lib/TweakInject/OctopusBridge.plist
   killall -9 ptpd
   ```
   The exact installed paths are in the `tweak` CI job's log, which prints `dpkg-deb --contents`.
4. **Boot without the jailbreak.** Rootless tweaks load only while jailbroken, so a plain reboot
   without re-running Dopamine runs stock ptpd with nothing injected. This is the guaranteed way
   out: it needs no working SSH, no Sileo, and no filesystem access.

A ptpd crash loop cannot brick the phone or block a reboot. If you want to be sure photo import is
healthy again, reconnect to Windows and copy one ordinary photo before doing anything else.

### Stage 1 — the app's socket. Implemented; needs device confirmation.

`ServiceController` attaches a bridge transport on every Enable USB Access and tears it down on
Disable. The socket path is fixed, because the integration is a separate process and can only look
somewhere agreed in advance:

    /var/mobile/Library/Caches/Octopus/octopus-bridge.sock

If that folder can't be created or written, or the path is too long for a socket, no transport is
attached and the app behaves exactly as every build before this one: a local service, with Windows
unable to see it. If a transport is attached but fails to start — something that isn't a socket
already sits at the path, say — the app detaches it and enables once more without it; both attempts
appear in the activity sheet. The bridge can cost Windows access, but never the local service. On the Simulator, where there is no `/var/mobile`, this is the path taken,
which is why the Simulator tests still see "No USB transport in this build".

**On the device:**

1. Open Octopus and tap **Enable USB Access**. No USB cable yet.
2. The activity sheet should end at **Waiting for a host**, not **Ready**, and the USB Connection
   window should mark *Waiting for a connection*. On earlier builds it stopped at *Responder ready*.
3. **Settings → Diagnostics → Run Full Diagnostics.** Expect:
   - `bridge.app_active` PASS, naming the socket path.
   - `bridge.app_socket` PASS, having connected to the socket and received a PONG.
   - `bridge.client_connected` BLOCKED: nothing has connected yet.
   - `bridge.tweak_log` BLOCKED: the component isn't installed yet.
   - `adapter.bridge` PASS: the simulated end-to-end transport check.
4. Tap **Disable USB Access**, then run diagnostics again. `bridge.app_active` and
   `bridge.app_socket` should both go BLOCKED, and the socket file should be gone.
5. Enable and disable three or four more times and watch the debug log for `app.bridge` lines.
   Each cycle should log one `transport_set` and one `stopped`, with no error between them.

Stop here and send the two ZIPs if anything above does not match. Nothing in this stage touches
ptpd, so there is nothing to recover from.

### Stage 2 — the component loads and is identified. Needs the DEB.

1. Build the package on a machine with Theos:
   ```sh
   cd Octopus/Bridge && make package THEOS=/var/jb/opt/theos
   ```
   or take `OctopusBridge-deb` from the `tweak` job's artifacts on the CI run for this commit.
2. Install the `.deb` with Sileo. Do **not** reboot yet.
3. With Octopus open and USB Access enabled, connect the cable and unlock the phone.
4. `killall -9 ptpd` if ptpd was already running, so it reloads with the tweak.
5. Export Diagnostics. Expect `bridge.tweak_log` PASS, and in `logs/octopus-bridge-tweak.log`:
   - one `loaded` line quoting the reference UUID **and the uid and gid ptpd runs as**. Which
     user that is isn't in the static evidence, and it matters: connecting to the app's socket
     needs write permission on it, and the app creates it as `mobile`. Root or `mobile` both
     work. Any other user can neither connect nor write this log; then the file is absent, and
     the same `loaded` line is in the system log instead, readable with
     `ssh mobile@<iphone> 'log show --last 10m --predicate "process == \"ptpd\"" | grep OctopusBridge'`,
   - a `startup_ping` line showing it reached the app's socket,
   - `connected` from the app side in the app's own log.
6. If the UUID does not match, the log says so and **no hooks are installed at all**. That is the
   expected, safe outcome on any build other than the analysed one; send the line.

### Stage 3 — read-only observation of the native interface. Needs Windows.

1. With the component installed, Octopus open and USB Access enabled, connect the iPhone to
   Windows 11 and trust the PC.
2. Open File Explorer → This PC → the iPhone, and browse into the camera folders.
3. Copy one ordinary photo to the PC. This exercises the full native path: enumeration, object
   info, and a data source.
4. Export Diagnostics and send the ZIP. The useful contents are, in `logs/octopus-bridge-tweak.log`:
   - `class` and `method` lines: the real storage class, its method list with type encodings, and
     the same for the object and data-source classes. This is the interface, definitively.
   - `request` lines: the operations Windows actually sent, in order.
   - `object_handles`, `object_info`, `data_source` lines: the arguments and results of each, so
     the shape of a real transfer is on record.
   - `transport_activated` / `transport_deactivated` around the session.
5. Confirm the photo copied correctly and that Photos import still works. If anything is wrong,
   use the recovery steps above.

Only after that evidence exists is there a basis for designing Stage 4, and the Stage 4 design has
to answer the Windows question separately from the interface question.

### Still open

- Which user ptpd runs as. The component records it at load (Stage 2, step 5); until then it is
  unknown whether ptpd can reach the socket at all.

- The app must be running with USB Access enabled before ptpd starts, or the component finds no
  socket at launch. It retries on each hook, so connecting the cable after opening the app works;
  the reverse may not.
- Whether Windows will accept a second PTP storage at all. Unknown, and not answerable from here.
- Whether a single bridge connection can carry concurrent PTP operations. The server serves one
  client at a time and the client is not thread-safe; the observation build has no PTP traffic of
  its own, so this only matters from Stage 4 onward.
