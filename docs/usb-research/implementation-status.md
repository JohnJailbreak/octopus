# Implementation status and validation

## Delivered code

- `oct_usb`: bounded USB2 PTP interface/endpoint selection and full control SETUP validation/routing to the existing oct_link. Integrated core tests and one-tap simulated diagnostic, not a replacement responder.
- `oct_device`: actual read-only iOS IOKit matching using ptpd's verified match dictionary; explicit errors, resource cleanup and enumeration bound. Portable Mach-O identity parser with truncation/malformed tests.
- Existing diagnostics: adds PTP registry and ptpd UUID/SHA-256 observations, preserves error codes, fixes process/transaction-count overclaims, adds device-observations.json and connection-timeline.txt to the existing ZIP export. New adapter diagnostic remains SIMULATED.
- Existing native UI, updater, IPA/TIPA packaging, entitlements and release workflows retained. No DEB/injection/driver or filesystem exposure added.

## Validation performed on Windows

Portable workspace-only MSYS GCC 16.2.0, strict existing warnings including conversion/sign-conversion and Werror. Tool packages fetched from MSYS repository and checked against database SHA-256. No toolchain committed.

| Check | Result |
|---|---|
| Strict C core/test compilation | Pass |
| `make test-usb` | **836 checks, 0 failures**, actual new implementation with existing link/service/responder |
| Full updated core suite | **3350 checks, 7 failures** |
| Unchanged baseline core suite | **2506 checks, same 7 failures** |
| Repository checks | **12 pass** |
| Helper/spawn/ZIP suite | 17 tests: 15 pass, 1 failure, 1 skipped; baseline reproduces identical failure/skip |
| GCC `make analyze` | No findings (includes core, helper and spawn; iOS-only branch excluded on Windows) |
| Responder fuzz, unsanitized | **300000 iterations, no faults**; 180340 commands, 59823 data phases, 44662 mutations |
| New identity parser against all five real binaries | Pass; UUID/CPU/platform/minimum match independent inventory |
| ASan/UBSan/TSan | Attempted, blocked by missing -lasan/-lubsan/-ltsan runtimes in MSYS toolchain |
| Swift Simulator / native iOS build | Not run: no macOS/Xcode available |
| GitHub CI | Not triggered: connector Git tree write rejected HTTP403, local push credential unavailable |
| Physical iPhone/Explorer | No new live verification |

The seven full-suite failures are the exact existing symlink/root-alias/path-replacement and chmod-based write-refusal assertions under Windows/MSYS. Helper's existing symlink rejection test also fails identically on baseline. These are not silenced or reclassified as passing. Linux/macOS checks remain required. A preliminary run also lacked MSYS /tmp; creating the workspace runtime's tmp directory resolved that setup problem. A stale expected adapter count was fixed from 5 to 6. Separate unsanitized fuzz executable was built from plain objects after sanitizer runtime failures; no sanitized pass is claimed.

See [validation logs](evidence/validation). Paths in logs are normalized to WORKSPACE. These logs are host tests, not USB traces. Swift changes were source-reviewed and tests updated but not compiled here.

## Static findings versus runtime evidence

All five binaries have identities/load commands/signatures inventoried. Ghidra implementation-level evidence retained for USB setup/read/write, provider routing, configuration selection, lockdown service/trust, AFC lifecycle and camera initiator paths. Private imported frameworks/shared-cache implementations remain missing. No public additional-storage registration interface was established.

Only the user-supplied v0.4.8 diagnostic run is physical-device evidence: 26 PASS / 3 BLOCKED / zero external transactions. Existing synthetic example digest remains aeeb6cba90d7d6adfcd42d673f64897f26cfcc479c5cf489a86146e09a9d2bd2. The immediate File Explorer synthetic-storage milestone is **not achieved**. Actual native driver, additional provider, root browsing, writes and physical Photos coexistence are unverified/unimplemented. This release deliberately does not advance filesystem exposure past the prerequisite milestone.

## Delivery and recovery

Engineering branch based on original cdca0da history; local committed changes, full source archive and Git bundle delivered. Push was attempted through authorized access but denied; main/release remains unchanged. Build and install an engineering artifact only after macOS checks below. No Apple binary or system-volume write occurred; disabling Octopus uses the existing app control. There is no native transport mutation to undo.
