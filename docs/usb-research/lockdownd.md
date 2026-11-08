# lockdownd: host authorization and service activation

UUID **87AE1730-F0C0-39FC-84CD-24ED0EB4E48A**, arm64e/iOS17; unslid VA base 0x100000000. [Inventory](binary-inventory.md). Names below are inferred from function-local logging plus inspected control flow, not recovered exports. Evidence files include instruction xrefs. No broad unrelated subsystem analysis is claimed.

## Verified through static analysis

| Address / inferred name | Implementation and data flow |
|---|---|
| [0x100020f54 handle_start_session](evidence/lockdownd/100020f54.txt) | x0 connection/context pointer; x1 message dictionary (roles inferred). Builds StartSession reply; rejects remote-service connection path; validates HostID, obtains pairing record, checks time, creates session ID and EnableSessionSSL=true, retrieves certificates/key; error paths return protocol errors or disconnect. Context offsets observed, not a declared struct ABI. |
| [0x100021fb4 handle_start_service_with_socket](evidence/lockdownd/100021fb4.txt) | Reads Service from dictionary; checks unlocked-since-boot and recent-unlock policy. Normal path requires active session and pair record, with special service exceptions. AFC names are explicitly compared during SSL-material selection; they do not bypass all authorization. Calls 0x1000215fc, emits Port and possibly EnableServiceSSL. |
| [0x1000215fc spawn_and_handle_checkin](evidence/lockdownd/1000215fc.txt) | Called above with service and connection metadata; builds check-in dictionary with HostID/HostConnectionType and other policy fields, coordinates service startup/check-in. Decompiler's many arguments are not a verified calling signature. |
| [0x10001ab94 ask_user_to_trust_ptp block](evidence/lockdownd/10001ab94.txt) | Checks global PTP allow/deny state, current connection identity and special InterfaceNamer handling; routes trust policy. Proves PTP has a policy path separate from merely discovering the USB interface. |

Confidence: high for branches and referenced values in these bodies; medium for inferred role names and opaque argument identities. Callers/callees and precise references are in each evidence file. Complete dispatch/certificate validation and service catalogue remain partially unresolved; no public third-party registration ABI was found by this targeted pass.

## Runtime observation

None for these internal functions. The old Octopus report's running ptpd is not proof of trust authorization, host attachment, or executed StartService.

## Strong inference / unresolved hypothesis

Lockdown service activation could support a companion-client service, but is not the USB PTP bulk endpoint that File Explorer opens. Existing trust must remain intact. A future Octopus provider needs its own explicit user enable/revoke policy in addition to Apple's host trust. Exact launchd registrations, entitlement checks and in-process provider lifetime must be measured before code calls any of these internals. No lockdown messages or mutations were sent.
