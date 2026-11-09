# Optional native companion build and package

The existing IPA/TIPA app and updater remain the normal app distribution. The experimental rootless companion is a **separate optional package**, not yet a unified app DEB. Package construction does not install software, start a service, issue USB requests or restart ptpd.

## Build on macOS

Requires Xcode with the iPhoneOS SDK, Python 3.9+ and `ldid` (the existing build uses Homebrew's `ldid`). From the repository root:

```sh
python3 Octopus/Tests/RepoTests/test_native_package.py
bash tools/test_provider.sh
bash tools/test_observer.sh
bash tools/build_native.sh 0.4.10~experimental
```

The version here is an example package version, not a claim that a release exists. CI passes its generated release version automatically. `test_provider.sh` uses macOS Foundation to exercise the actual provider/runtime implementation against controlled native class definitions. It does not run Apple's ptpd or USB transport. `build_native.sh` compiles the current portable C core/protocol plus `OctopusRuntime.m`, `OctopusObserver.m` and `OctopusProvider.m`, using the iPhoneOS SDK, **arm64e**, minimum iOS 17.0, Objective-C ARC/blocks, and warnings as errors. It links Foundation/CoreFoundation and signs the resulting dylib with `ldid -S`.

Outputs:

* `build/native/OctopusNative.dylib` — signed companion, install name `/var/jb/usr/lib/TweakInject/OctopusNative.dylib`.
* `build/native/com.parv.octopus.native_<version>_iphoneos-arm64.deb`.
* The same DEB name followed by `.sha256` and `.json` — package hash, payload list and dylib identity.

The package architecture is Debian's rootless `iphoneos-arm64`; its **Mach-O payload is arm64e**, required for the target process. The packager masks CPU subtype capability bits before comparing subtype 2, checks `MH_DYLIB`, load-command bounds and an embedded CodeDirectory. This is structural validation, **not signature trust verification, code-page hash validation or proof iOS will accept/load it**. The build step signs the fresh output; deployment still requires physical validation.

## Exact package contents

Only two files are installed, both owned by root:root:

| Path | Mode | Purpose |
|---|---|---|
| `/var/jb/usr/lib/TweakInject/OctopusNative.dylib` | 0755 | Optional process component |
| `/var/jb/usr/lib/TweakInject/OctopusNative.plist` | 0644 | Filter: `Executables = [ptpd]` |

Directories are 0755. The package declares `firmware (>= 17.0)`, `firmware (<< 17.1)` and `ellekit`; runtime gates are stricter (iOS 17.0.0 and the verified ptpd identity). There are **no maintainer scripts, automatic daemon restarts, launchd jobs, Apple binaries, configuration replacements or pre-authorized lease files**. The existing app is not part of this DEB. Archives use explicit modes/ownership and zero timestamps for reproducibility on Windows and macOS. Debian `md5sums` are an integrity inventory, not authentication; the sidecar supplies SHA-256.

CI runs the package tests on Linux. A separate macOS `native` job runs provider and observer tests and builds/uploads `OctopusNative-experimental-<version>` as an Actions artifact. Its failure remains visible but does not block the established app build/release jobs; the companion is not silently published as a working release asset or installed. Do not use a stale companion artifact from a different source revision with the app.

## Controlled installation and removal

Keep the working v0.4.9 IPA/TIPA as rollback. Only after the actual native build succeeds, manually open the matching DEB in Sileo on the target Dopamine rootless device and install it. The package writes only the two paths above; it does not replace `/usr/libexec/ptpd` or touch the sealed volume. The loader must start a **new ptpd process** before the module can appear. Unplugging alone is not a verified restart mechanism: the staged v0.4.9 capture kept the same PID after its unplug-labelled stage. First reconnect and inspect native module status. If unavailable, use a deliberate reboot/re-jailbreak or the jailbreak's supported process lifecycle procedure; no automatic process killing is included.

Do not equate installation with activation. Use the app's explicit experimental controls and USB Access authorization. The native component requires a renewed lease, expires it after at most 15 seconds without renewal, checks identity/method compatibility, and fails closed. The shared status/lease directory is `/var/mobile/Library/Caches/Octopus/native`, owned by the participating mobile user with private-directory checks; it is not a security boundary against other code already running as that same UID.

To roll back, disable native exposure/USB Access in the app and wait for the lease to expire. Remove **Octopus Native (Experimental)** (`com.parv.octopus.native`) in Sileo. Removal deletes the two package files and prevents future injection. A currently running process can retain its already loaded module until exit; the hooks forward to Apple's original implementations when unauthorized. A deliberate reboot/re-jailbreak removes that in-memory instance. Reinstall the retained v0.4.9 IPA/TIPA if app rollback is desired. Keep diagnostic exports before removal.

## Verification limits

The Python package tests ran on the Windows engineering host: three test methods passed, including deterministic archive output, exact payload/filter/modes/ownership/checksums, malformed/unsigned/wrong-architecture rejection, and version injection rejection. Their input is a **synthetic structural Mach-O fixture**, not an Apple binary or an installable companion. Native compilation, Objective-C provider execution and iPhone installation cannot be verified by these tests; consult the actual CI result for that source revision. This document does not claim a successful native build, physical load, USB request, or Explorer download.
