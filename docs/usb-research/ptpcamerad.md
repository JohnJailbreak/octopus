# ptpcamerad: PTP initiator / host-camera path

UUID **245801E3-FF45-3942-8DBF-AA9F80BD4ADC**, arm64e/iOS17; unslid VA base 0x100000000. [Inventory](binary-inventory.md).

## Verified through static analysis

[PTPHostUSBTransport startInitiator 0x100002b28](evidence/ptpcamerad/100002b28.txt) calls superclass startInitiator and matches **IOUSBInterface** with **locationID**, then reads bInterfaceClass/SubClass/Protocol, obtains a host-side interface plug-in and pipe information. This differs materially from ptpd's IOUSBDeviceInterface / USBDeviceFunction=PTP match.

| Address / symbol | Role / arguments and call relationships |
|---|---|
| [0x1000027c0 initWithLocationID:delegate:](evidence/ptpcamerad/1000027c0.txt) | Stores location/transport delegate; Objective-C x0 self, x1 selector, following arguments location and delegate. |
| [0x100003ca8 sendTransaction:timeout:](evidence/ptpcamerad/100003ca8.txt) / [0x100003d6c sendRequest:](evidence/ptpcamerad/100003d6c.txt) | Outbound transaction/request path, consistent with initiator role. |
| [0x10000442c deviceStatus](evidence/ptpcamerad/10000442c.txt), [0x100004678 cancelRequest:](evidence/ptpcamerad/100004678.txt), [0x1000048e4 deviceReset](evidence/ptpcamerad/1000048e4.txt) | Issues USB class control operations to attached camera through interface table. |
| [0x1000055a4 readResponseData:withTimeout:](evidence/ptpcamerad/1000055a4.txt), [0x10000619c handleBulkData:result:forTxID:](evidence/ptpcamerad/10000619c.txt) | Reads remote responses and associates received containers with initiator transactions. |
| [0x100003a04 stop](evidence/ptpcamerad/100003a04.txt) | Releases/ends transport resources. |
| [0x10000197c listener:shouldAcceptNewConnection:](evidence/ptpcamerad/10000197c.txt) | Remote camera-manager XPC listener; not a third-party device-provider registration interface. |

Confidence high for direction, match properties and inspected call flow; private table signatures not certified. Full instruction/caller evidence accompanies each link. Dynamic selector calls may not appear as direct xrefs.

## Runtime observation / inference / unresolved

No live observation. Strong inference: this is the phone acting as USB host for a camera, not the path by which Windows requests the phone's storage. These inspected routines supply no Octopus device-side endpoint bridge. This does not prove the whole program has no other transport roles; unrelated branches were not exhaustively classified. Reusing its host-side interface ABI in ptpd would be a category error.
