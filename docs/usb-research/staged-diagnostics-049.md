# Four physical-device captures from v0.4.9

These findings come from the actual `results.json`, `results.txt`, `failures.txt`, `connectivity.txt`, `device-observations.json`, `connection-timeline.txt`, `build.json` and persistent logs in the supplied staged archive. Folder labels are experiment context, **not measured attachment events**. Times below are UTC on 2026-11-08; subtract five hours for the user's America/New_York local time that day.

## Chronological observations

| Supplied folder | Diagnostic start → finish (UTC) | Service / transport | PTP registry matches | ptpd process snapshot | External request count |
|---|---|---|---:|---|---:|
| `01-disconnected` | 22:47:23 → 22:47:24 | Ready / unavailable | 1 | Not found | 0 |
| `02-connected` | 22:53:33 → 22:53:34 | Ready / unavailable | 1 | PID 39376 | 0 |
| `03-after-explorer` | 22:54:00 → 22:54:00 | Ready / unavailable | 1 | PID 39376 | 0 |
| `04-after-unplug` | 22:54:28 → 22:54:28 | Ready / unavailable | 1 | PID 39376 | 0 |

All four report **32 diagnostic results: 29 PASS, zero FAIL, zero SKIPPED, three BLOCKED**. The blocked IDs are `external.usb_transport`, `external.host_requests`, and `external.windows`. The generic transport counter is zero in every capture. `transport_failure` is empty; that means no recorded driver failure, not an available driver. The external check explicitly says no native USB driver is registered.

All four identify Octopus **0.4.9, build 9**, commit **e69b28737eb26939663e47fc0ddfe6829abbf0a8**, core 0.1.0, **iOS 17.0.0 (iPhone14,5)**. Installed ptpd identity is unchanged:

* UUID `FFD54FC7-0508-3AB8-8D17-2F698B611E10`.
* SHA-256 `a1742358da3e257204a0f190211cda26a155dfe0dabb3c9fa572117460085dcb`.
* Each identity check reports metadata match and exact-byte match with the supplied reference.

The configuration/DeviceInfo/launch-plist bodies in `connectivity.txt` are identical across the four captures (excluding service/process preamble). Configuration parsing reports 403 values, including `standardMuxPTP` with PTP and the iPod USB interface. DeviceInfo reports 14 values; launch metadata reports 11, including `IOProviderClass=IOUSBDeviceInterface`, `USBDeviceFunction=PTP` and `/usr/libexec/ptpd -t`. These are configuration observations, not proof of which configuration the host selected.

## What the persistent timeline actually shows

`connection-timeline.txt` begins “Saved log observations, not proof of native USB traffic.” Every remaining line is present in that capture's persistent application log. It is a filtered application history, **not an independently captured USB event stream**. History includes v0.4.7 and v0.4.8 runs; those must not be assigned to the current stage.

The v0.4.9 history shows local service enable at 22:47:10.119, disable at 22:48:34.937, re-enable at 22:48:54.658, disable at 22:52:11.085 and re-enable at 22:52:45.103. Every enable ends “Ready · local service only”; both disables say no host session was open and zero responder requests. Thus the captures do not represent one uninterrupted enabled interval.

An additional diagnostic at **22:52:55.555** in later persistent logs still finds no ptpd. PID 39376 is first observed at **22:53:34.283**, then at 22:54:00.442 and 22:54:28.665. This narrows its first observed appearance to that interval, but does not establish exact launch time, attach time, file-copy time or termination time. In particular, the same PID in the final stage cannot confirm or refute the reported unplug.

The current runs successfully use the fallback log under `/var/mobile/Library/Caches/Octopus`, with zero reported write errors. Later logs record prior ZIP exports falling back to the same cache because creating a file in the app container's temporary directory returned **Operation not permitted**. Historical v0.4.7 export failures remain in `failures.txt`; they are not current v0.4.9 diagnostic failures. Do not remove the existing fallback.

## Evidence classification and implementation consequences

**Verified runtime observations:** four physical-device diagnostic executions; persistent service state; exact installed ptpd identity; one registry match in every snapshot; absent/present process snapshots; local/simulated checks passing; zero recorded Octopus external requests. There is no numeric IOKit failure code in the successful discovery details and no raw USB completion status in these captures.

**Strong inference:** registry existence is insufficient as an attachment detector because the count is unchanged in all supplied stages, including the disconnected-labelled ones. Process presence is also insufficient: PID 39376 persists in the final stage. Native observation must occur at the actual receive/dispatch/response boundary and distinguish session/process epochs.

**User-reported physical behavior:** normal photo copying after opening File Explorer. The supplied ZIP contains no copied-photo hash, Windows WPD trace, PTP command capture, host descriptor capture, or Explorer screenshot validating that action independently. It is useful experiment context, not verification that Octopus handled traffic.

**Unverified:** companion loading into ptpd; private-method runtime ABI matches; provider routing/coexistence; USB attach/detach callback behavior; successful transmission/host receipt; Explorer synthetic storage/folders; `example.txt` download; disable/recovery after a live native transfer. The reported example hash comes from **local/simulated** retrieval only.

The next build therefore needs explicit module status, exact compatibility gates, observed USB receive scope correlated with operation/transaction IDs, response outcomes, provider access, and expiring enable authorization. Another registry scan cannot close the gap. Keep native-observation counters distinct from portable responder self-tests and never promote an asynchronous send submission into a completed Windows download.

## Capture identities

These SHA-256 values were recomputed from the supplied extracted `results.json` files (not copied from the archive manifest):

| Folder | results.json SHA-256 |
|---|---|
| `01-disconnected` | `772135f15b5d3f61bec230faec8d81bd35b25ea8942767c2893dcbb75cb0d801` |
| `02-connected` | `7495dd7294b3f6db9d21e2adcacff832716901f27c8cdfc977cec91a4407d4e9` |
| `03-after-explorer` | `7f72f10024080adaa90727e52a0762e515bba92ca5118ae67b0d286ede96bddf` |
| `04-after-unplug` | `5849488661cc805b267f65ca4a731e3f6af75af111e2d01b1c8dfeb16c123c6f` |

Original ZIP hashes in `MANIFEST.json` are supplied provenance; original inner ZIP bytes were not available in the extracted staged tree for independent rehashing.
