# afcd: authorized socket service, not PTP

UUID **E91A0D5B-F781-38E1-A464-720E3348B7E3**, arm64e/iOS17; unslid VA base 0x100000000. [Inventory](binary-inventory.md). High confidence for the following inspected bodies; imported library behavior is explicitly outside scope.

## Verified through static analysis

| Address / inferred role | Implementation, callers/callees, argument roles |
|---|---|
| [0x100002510 entry](evidence/afcd/100002510.txt) | Parses `-r` crash-reporter mode; ordinary root `/private/var/mobile/Media`, crash mode CrashReporter or osanalytics group directory. Checks root via lstat, uses sandbox extension `com.apple.afc.root` and sandbox profile `afcd`; sets up queues/service listener. argc/argv roles inferred from getopt_long call. |
| [0x1000029e0 listener setup](evidence/afcd/1000029e0.txt) / [0x100002abc connection handler](evidence/afcd/100002abc.txt) | XPC lifecycle feeding event block 0x100002c34. Block context and XPC object roles follow block captures; do not treat decompiler signatures as public declarations. |
| [0x100002c34 check-in event](evidence/afcd/100002c34.txt) | `lockdown_copy_checkin_info`, `secure_lockdown_checkin`, gets socket and secure context; creates AFC server context, assigns root and lockdown connection, creates AFC connection, assigns SSL context, initializes server, optionally file coordination, dispatches worker 0x1000030ac. Errors cancel XPC connection or exit. |
| [0x1000030ac worker](evidence/afcd/1000030ac.txt) | x0 AFC connection. Sets I/O timeout 0, gets server root, calls AFCServeWithRoot, disconnects lockdown, frees context, invalidates/releases connection, ends XPC transaction. |
| [0x100003158 completion](evidence/afcd/100003158.txt) | Dispatch-group completion lifecycle, linked from event handler. |

Critical boundary: file operation decoding/mapping is inside imported **AFCServeWithRoot**, not this executable. Root context and sandbox constraints are visible; exact path traversal, symlink, write/rename and AFC opcode semantics cannot be verified without the matching AFC library. Do not claim a verified full AFC implementation from these imports.

## Runtime observation

None from this session or the supplied report for afcd.

## Strong inference

An AFC-style Octopus daemon could reuse the pattern of authorized service activation, bounded export root and socket worker cleanup. It would still speak a different protocol than the WPD PTP/MTP interface; simply starting an AFC service does not expose a File Explorer storage. Windows-side AFC client/bridge software would violate the ordinary-use requirement. This is an architectural reference, not the selected USB solution.

## Unresolved

Exact service policy, effective entitlements, AFC library internals and the relation between service names `com.apple.afc` (lockdown request) and `com.apple.afcd` (daemon startup) require matching service configuration/runtime observations. No attempt was made to start an unrestricted service or bypass pairing.
