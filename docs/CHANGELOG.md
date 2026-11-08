# Changelog

Newest first. Every push to `main` is a release, numbered from `project.yml`'s `MARKETING_VERSION` and the build's
run number (0.2.42 is a push of 0.2), so the versions below are the ones where something changed. Each release's own
notes, its commit messages, are on its [release page](https://github.com/m9z2w4d8vw-blip/octopus/releases) and in
the app's changelog.

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
