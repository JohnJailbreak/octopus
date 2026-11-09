# Native journal and lease validation

## Results from this engineering pass

The new code was compiled with workspace-local MSYS GCC 16.2.0 on Windows 11 using the repository's strict warning flags, including `-Werror`, `-Wconversion` and `-Wsign-conversion`. These are host tests of the real C implementations. No result in this document establishes a physical USB connection or an Explorer download.

| Check | Result |
|---|---|
| `make test-native` | 816 assertions, zero failures |
| `make test-native-lease` | 25,353 assertions, zero failures |
| `make test-native-io` with ACL and symlink semantics | 91,309 assertions, zero failures |
| Full `make test` C suite | 95,475 assertions, zero failures |
| Update helper/spawn/ZIP tests | 17 tests: 16 pass, persona-spawn test skipped on this non-Apple host |
| Repository checks | 12 tests pass |
| GCC `make analyze` | No findings |
| Plain responder fuzz | 300,000 iterations, no faults; 180,340 commands, 59,823 data phases, 44,662 mutations; 6,297 successes across 15 operations; 53,388 malformed inputs |
| ASan/UBSan | Compilation attempted; link blocked by unavailable `-lasan` and `-lubsan` runtimes |
| ThreadSanitizer | Compilation attempted; link blocked by unavailable `-ltsan` runtime |
| Apple SDK, Swift Simulator, injected observer runtime | Not covered by these tests |

The assertion counts include byte-by-byte validation of a 65,536-byte IPC payload and exhaustive single-byte mutations of the lease grammar; they are not counts of distinct USB scenarios. The journal test supplies synthetic observations explicitly. Fuzzing here was **unsanitized**, not a substitute for CI sanitizer runs.

## Host environment correction

The first native IPC attempt failed at the deliberate private-directory permission check: default MSYS `noacl` mounts report `mkdtemp(0700)` and `chmod(0700)` as mode 0755. Default MSYS symlink creation also copies the target. These host behaviors are unsuitable for verifying POSIX security checks.

Tests were rerun on a dedicated workspace NTFS directory temporarily mounted at MSYS `/tmp` with `acl,binary,posix=1`, with `MSYS=winsymlinks:sys`. The tests confirmed mode 0700 and current effective ownership, actual `lstat` symlink classification, hardlink counts and refusal of aliases. The temporary mount was removed on exit. Production guards and test assertions were unchanged. This also resolved all seven pre-existing full-suite failures and the pre-existing helper symlink failure noted in the earlier implementation status.

These are MSYS filesystem semantics, not a physical APFS/iOS test. Ownership mismatch rejection is implemented but changing a file to another user's ownership was not exercised under the nonprivileged host identity. Linux/macOS CI remains necessary. MSYS's documented symlink modes are described in the [MSYS2 documentation](https://www.msys2.org/docs/symlinks/).

Reproduction: run [run-msys-native-validation.sh](tools/run-msys-native-validation.sh) from the repository root using the workspace MSYS bash. It refuses to replace an existing explicit `/tmp` mount, installs a temporary ACL mount, collects per-check logs and unmounts on exit. Missing sanitizer runtimes remain a nonzero result. On Linux/macOS, use the existing `make test`, `make sanitize`, `make tsan`, `make analyze` and `make fuzz` directly. Focused commands are additional checks, not replacements for the full suite.

## What the native journal proves

`oct_native` accepts command observations only under a successful device-side USB receive scope for the currently authorized transport. The native wrapper must surround `handleBulkData:result:` and bind the actual transport identity; generic `handleRequest:` invocation alone is insufficient. The C core cannot independently authenticate the wrapper's inputs.

A response pairs only with an outstanding command of the same current transport, transaction ID and connection epoch. The process generation is nonzero and fresh at initialization. Events carry a monotonic sequence and a UTC millisecond timestamp. UTC is useful for cross-host correlation but is not the authorization lease clock.

The tests exercise disabled authorization, wrong transport, failed/short read, expired receive scope, asynchronous response after the receive scope ends, sendData submission without delivery credit, failed and duplicate response calls, duplicate transaction IDs, OpenSession invalidation, disconnect/reconnect, wrong response codes, bounded pending/scopes/events, tombstone overflow and generation reset.

Outstanding IDs discarded during session/connection changes become conservative tombstones. A late response cannot then be mistaken for a new command reusing that ID. Tombstone overflow disables pairing until a fresh journal generation. Completed IDs may be reused. `paired_responses` records a matched successful native `sendResponse` BOOL return, **not USB completion or Windows receipt**. Counters are cumulative per process generation; consumers must check authorization, epoch and freshness rather than interpret historical nonzero counters as a live connection.

## What the lease IPC tests prove

`oct_native_io` uses a private current-owner directory and bounded regular files named only `lease` or `status`. Reads reject final-component symlinks, hardlinks, inappropriate owner/mode, nonregular files, oversize data and changing length. Writes create a new 0600 staging file with exclusive creation, flush it, then publish with rename. Replacing a target symlink/hardlink does not overwrite its referent. A pre-existing staging filename fails without replacing the last published status; removing that obstruction allows recovery. Revocation unlinks only the lease.

The parser accepts exactly `OCTN1 <deadline> <mode> <32 lowercase hex digits>\n`, mode 1 or 2, a future deadline no farther than 15 seconds, and canonical unsigned decimal numbers. Overflow, signs, leading zeros, extra whitespace, missing/extra newline, embedded NUL, trailing data, invalid modes and invalid tokens are rejected. The parser uses explicit bounded arithmetic rather than scanf unsigned conversion. The caller supplies a common monotonic clock; wall-clock timestamps are not substituted.

Both peers are the same mobile UID. File ownership/modes reduce accidental or cross-UID interference; this is not authentication against another process with that UID. `O_NOFOLLOW` applies to the final selected directory/file component. Full ancestor trust, malicious same-UID mutation races, injected runtime behavior and device-side authorization remain separate boundaries. No arbitrary paths or file contents are exported by this journal.

## Retained logs

The complete logs from this pass are in [evidence/validation/native](evidence/validation/native/). They retain warnings/failures and host limitations. Local workspace paths, when present, are normalized; no Apple executables are included.
