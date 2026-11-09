# Changelog

Newest first. Every push to `main` is a release, numbered from `project.yml`'s `MARKETING_VERSION` and the build's
run number (0.2.42 is a push of 0.2), so the versions below are the ones where something changed. Each release's own
notes, its commit messages, are on its [release page](https://github.com/m9z2w4d8vw-blip/octopus/releases) and in
the app's changelog.

## v0.5

The bridge transport now runs in the real app, the diagnostics report its state, and the
experimental native component is cleaner and ships with a CI job that proves it parses.

- **Enable USB Access starts the bridge socket** at `/var/mobile/Library/Caches/Octopus/octopus-bridge.sock`.
  `ServiceController` creates a `BridgeTransport` wrapper (a Swift class owning `oct_bridge *` and
  `oct_link *`) and attaches it to the C service before starting. Disabling USB Access stops the
  transport, closes the socket, and destroys the C objects. Repeated enable/disable cycles don't
  leak resources. A `UIBackgroundTaskIdentifier` buys ~30 s of extra time when the app goes to
  the background while USB Access is enabled.
- **New diagnostics**, measured against the running app and shown in Run Full Diagnostics:
  - `bridge.app_socket` (native observation) — tries to connect to the socket and send a PING.
    Reports PASS when the app has enabled USB Access; BLOCKED when nothing is listening.
  - `bridge.app_active` (local) — whether the bridge socket server is active in this app process.
  - `bridge.client_connected` (local) — whether a native integration client (the tweak or the
    diagnostic probe) is currently connected.
  A PING to the app socket is never reported as evidence that the tweak is loaded or that
  real USB traffic is flowing.
- **`OctopusBridge.m` rewritten** (single persistent connection, serial dispatch queue):
  - `HOOKS_ENABLED=0` by default: identity is checked, the socket is probed at startup, and
    `transportActivated` / `transportDeactivated` are logged — but ptpd's PTP behaviour is
    unchanged. Set to 1 only after Stage 3 runtime observation confirms the PTPStorage selector
    surface (the `forwardInvocation:` log in the tweak records every unknown call).
  - A single `oct_bridge_client *` is held open for the tweak's lifetime; all socket operations
    are serialised on a private queue to prevent frame interleaving.
  - `OctopusPTPStorage.forwardInvocation:` absorbs any selector ptpd calls that we haven't
    implemented yet, logging it instead of crashing ptpd and Photos import.
- **Theos CI job** (`tweak` in `.github/workflows/build.yml`): syntax-checks every C translation
  unit the tweak links and parses `OctopusBridge.m` as Objective-C. A full arm64e DEB requires a
  macOS host with Xcode; the CI job proves the sources are correct independently of the toolchain.
- **Docs**: `docs/usb-research/runtime-validation.md` documents all five integration stages, their
  current status, and the exact device-testing sequence required for Stage 3.
- **Tests**: 3,646 C checks, 0 failures. ASan/UBSan/LeakSanitizer and TSan clean. Fuzzer: 300,000
  iterations, no faults. Static analysis: no findings.

## v0.4

### Unreleased USB engineering

- Validate complete USB control requests and PTP endpoint descriptors before forwarding to the transport adapter; add simulated diagnostic coverage.
- Collect read-only PTP registry discovery and ptpd binary identity through existing diagnostics; export observations and timeline.
- Correct connection claims based only on process state or generic transaction counts.
- Preserve the current app, updater and IPA/TIPA packaging; native USB ownership and Explorer storage remain blocked.
- Record implementation-level analysis of five supplied Apple binaries under docs/usb-research.

One button runs every safe check and makes one archive to send; the adapter learns the rest of the USB class's rules.

- **Run Full Diagnostics**, in Settings > Diagnostics: 29 checks in one run, each PASS, FAIL, SKIPPED or BLOCKED with
  its reason, kind, checks and time. The responder's checks are the self-test's own phases, run one at a time, so one
  failure no longer hides the rest; the phases after it say what they needed. Simulated checks are labelled as
  simulated, and the external ones say why they can't run: no USB transport, no request from a computer, and Windows
  needs manual steps.
- **Export Diagnostics** makes one ZIP: the results as JSON and text, the failures with the log's recent warnings,
  build information with the commit CI built, the settings, the transport's state and the iPhone's full USB and PTP
  description (all 403 values, where Debug Log > Report stops at 150), and the debug log files. Container IDs are
  redacted. The ZIP writer is C (`oct_zip`), checked by Python's `zipfile`.
- **Export Diagnostics works on the iPhone.** On 0.4.7 every check ran, but the archive couldn't be written to the
  app's temporary folder, so there was nothing to export. The archive now goes to the first folder that takes it: the
  temporary folder, then `/var/mobile/Library/Caches/Octopus/Diagnostics`, `/var/tmp/Octopus/Diagnostics`, or the
  debug log's own folder. Each is created if missing and proven writable with a probe file first; the archive is
  synced, renamed into place and read back with every entry's CRC checked. A refused folder is logged with the
  system's error and its path. If none takes it, the screen says why for each one and offers **Retry Export** and
  **Share Results as Text**, which needs no file. Debug Log > Export uses the same folders; before, it failed
  silently.
- **Compatibility checks** (`oct_compat`): an MTP host's enumeration of the read-only synthetic storage at USB 2.0
  packet sizes, from DeviceInfo to `Documents/example.txt` downloaded whole and in part and matched to its SHA-256; the
  Cancel, Get Device Status and Device Reset class requests; events; and the USB lifecycle.
- **The adapter** decodes the class requests' control-pipe bytes (`oct_link_class_request`), sends event containers on
  the interrupt pipe (`oct_link_send_event`), and handles a USB bus reset and suspend and resume.
- **The debug log** remembers the folder that took it and tries it first, records why a folder was refused (on iOS
  17 the app's own Library/Logs can't be created; `/var/mobile/Library/Caches/Octopus` is used), logs that as
  information rather than a warning, reaches storage when Octopus goes to the background, and reopens its file if
  something removes it or its handle goes bad.
- **Windows diagnostics** no longer stops on properties a device lacks, which also left earlier values in later lines,
  and lists only what the iPhone presents on USB, not a Bluetooth mouse or the PPTP network miniport.
- **Tests.** 2,460 C checks (228 new): SHA-256 and CRC-32 vectors, the ZIP writer, the compatibility checks, the
  diagnostics with a failure injected, the process lookup and the log's recovery. XCTest covers a full run, the
  archive and redaction.

Windows still can't see Octopus: there is no USB driver yet.

## v0.3

A debug log, the transport adapter, and connection states taken from what Octopus observes.

- **The debug log.** Settings > Diagnostics > Debug Log shows `octopus-debug.log`, shared by the app and its C core:
  millisecond UTC timestamps, four levels, and a component and an event on every line. It records the service, the
  storage, the self-test, update checks and installs, and every transport state change and failure with its reason;
  with **Detailed logging** on, every request too, with its operation code, transaction ID, response code, sizes and
  duration. Filter it by severity, copy what's shown, export it through the share sheet, or clear it. It persists
  across launches, rotates at 512 KB keeping three older files, uses a fixed amount of memory, and keeps working in
  memory when no folder takes the file. It never records names or contents of files.
- **The transport adapter.** `oct_link` carries PTP containers between a byte-level driver and the responder, as the
  USB Still Image Capture Device Definition lays out: fragmented input, separate data and response transfers with
  zero-length-packet flags, the session and the transaction in progress, disconnects, stalls, and the Cancel, Device
  Reset and Get Device Status requests. A loopback driver exercises it with the real responder, and the self-test now
  drives it end to end.
- **Connection states.** USB Connection lists the service's states (disabled, initializing, responder ready, error)
  and the transport's (unavailable, initializing, waiting for a connection, host connected, session active, error,
  disconnected), marking only those Octopus has observed, and shows the last transport failure.
- **The device report.** Debug Log > Report reads, without changing anything, the iPhone's USB device configurations,
  the PTP capabilities `ptpd` reports and `ptpd`'s launch description, and adds a summary to the log, with serial
  numbers redacted.
- **Windows diagnostics.** `tools/windows/octopus-usb-diagnostics.ps1` reports, read-only, how Windows sees the
  iPhone: devices, instance IDs, interface classes, drivers, services, the storages Explorer shows, recent Plug and
  Play events, and optional capture tools.
- **Tests.** 561 new C checks, 2,232 in all: the log's persistence, rotation, concurrency and failures, and the
  adapter's fragmentation, framing, malformed input, sessions, interruptions, failures and concurrency. CI now also
  runs the C suite under ThreadSanitizer (`make tsan`), which found a race in the log during development. XCTest
  covers the Swift side of the log, the device report and the connection stages.

Windows still can't see Octopus: there is no USB driver yet.

## v0.2

Updates install from the app, and Octopus has a new look.

- **In-app updates.** When a newer release is out, **Update Available** appears above the button. Tapping it shows
  the changelog, the notes of every release newer than yours, and **Update** downloads `Octopus.tipa` and installs it
  with TrollStore through a small root helper, `octopushelper`. If the helper can't install it, TrollStore is asked to
  install the same release from its link; without TrollStore, the releases page opens. The updater and its screens
  are Speaka's, which are Caz's, drawn like Dopamine's.
- **Update notifications and Check for Updates**, in Settings. With notifications on, Octopus checks every 2 minutes
  while it's open, each time it comes to the front, and in the background when iOS allows, and announces each release
  once. Check for Updates checks at once; the line under it says whether Octopus is up to date, why a check failed, or
  when it last checked. Checks ask GitHub conditionally, so an unchanged list costs nothing against its hourly limit.
- **The update helper does one thing.** `octopushelper install-update` takes only a regular `.tipa` or `.ipa` directly
  inside Octopus's two download folders (`/var/mobile/Library/Caches/Octopus`, then `/var/tmp/Octopus`), never a link,
  finds TrollStore's installer through its bundle container, runs it in its own session, and deletes the download.
  Its exit codes name each failure. The app starts it as root through TrollStore's persona spawn
  (`Octopus/App/Spawn/oct_spawn.c`), which it looks up at run time, so the simulator build is unaffected.
- **Signed with entitlements.** `tools/package.sh` builds the helper, puts it in the app and signs both with `ldid`:
  the app with `com.apple.private.persona-mgmt`, `com.apple.private.security.no-sandbox` and `platform-application`,
  and the helper with the entitlements Speaka's and Caz's helpers use.
- **The Slate background.** The backdrop is now Slate, design 48 of the backdrop gallery: the reconstruction of
  Caz's sweep, recoloured to a muted blue-grey. The launch screen's colour matches it.
- **A new app icon.** The eye, design 13 of the icon gallery: an almond eye with an octopus's horizontal pupil, on
  Slate's icon background, in place of the O.
- **Releases.** Each release's notes are the push's commit messages, which the app shows as the changelog. A pushed
  tag like `v0.3.0` publishes that version. Publishing retries up to three times, and a re-run replaces the files and
  the notes. This series starts at 0.2, so every 0.2 release is newer than any 0.1 build.
- **Tests.** The helper and the spawn bridge are compiled and run on the host by `make test` and `make sanitize`
  (Octopus/Tests/HelperTests); XCTest covers the updater's parsing, its choice of release and its messages; and
  Octopus/Tests/RepoTests keeps the README's links and layout, this changelog and the update path's shared values
  true.
- **The README** follows Speaka's: install, requirements, using Octopus, troubleshooting, how it works, the
  repository, building, safety and limitations.

0.1 has no updater, so install 0.2 by hand once, from the
[latest release](https://github.com/m9z2w4d8vw-blip/octopus/releases/latest). Later releases install from the app.

## v0.1

The first phase: the app, the file-object provider and the PTP/MTP protocol layer, without a USB transport.

- **The app**, in Caz's design: the Fonarto wordmark, the menu, **Enable USB Access** opening into the activity sheet,
  and the USB Connection, Filesystem, Activity and Settings windows.
- **The file-object provider**, in C: stable handles that are never reused, directories listed on first use,
  pluggable backends (in memory, and a disposable directory that must carry an `.octopus-disposable` marker and is
  never left through a link), and a write policy checked at every operation.
- **The PTP/MTP responder**: 20 operations, bounds-checked parsing, sessions and transaction IDs (a repeated
  OpenSession keeps the session and follows the host's numbering), and upload reservations.
- **The synthetic storage**: README.txt, Documents and Information, read-only, plus an optional writable Scratch
  folder.
- **Tests**: 1,671 C checks, also under AddressSanitizer, UndefinedBehaviorSanitizer and LeakSanitizer; 300,000 fuzzing
  iterations; the GCC static analyzer; a 22-exchange enumeration transcript replayed byte for byte against a separately
  written encoder; and XCTest for the Swift bridge, the controller and the layout.
- **CI** builds the core with GCC and Apple Clang, runs XCTest on the iOS Simulator, builds for device, and publishes
  `Octopus.tipa`, `Octopus.ipa` and `SHA256SUMS` as a GitHub Release for every passing push to `main`.
