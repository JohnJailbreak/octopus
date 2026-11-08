# USB PTP request lifecycle

Static ptpd map (UUID FFD54FC7-0508-3AB8-8D17-2F698B611E10, arm64e iOS17, base 0x100000000):

1. `startResponder` 0x100004a64 matches IOUSBDeviceInterface / USBDeviceFunction=PTP.
2. `setupDeviceInterface` 0x10000a8a8 configures native endpoints through a private plug-in table.
3. `readBulkData` 0x100008eac submits read; completion 0x10000906c calls `handleBulkData:result:` 0x1000093bc.
4. Packet/delegate path reaches `PTPResponder handleRequest:` 0x100023384 and its operation switch.
5. Session methods or storage/object methods call `PTPVirtualCamera` and concrete `PTPStorage`/`PTPFile` objects.
6. Data-source-backed download feeds sendDataPackets 0x100006fd0 or split variant 0x1000075f0; response serialization 0x100007ebc uses writeBulkData 0x100008414.
7. Events use sendEvent 0x100008054 and interrupt queue/completion; control requests use 0x10000b848 separately.
8. stop 0x10000574c releases native resources.

[ptpd evidence map](ptpd.md) links instructions, xrefs and pseudocode for each step. High-confidence static routing does not prove that any Octopus request traverses it.

Octopus currently has `oct_link_input -> oct_service -> oct_responder -> oct_store`, then driver.send callbacks. The added `oct_usb_control` validates the complete USB setup packet before class dispatch; `oct_usb_parse_interface` extracts explicit endpoint addresses/packet sizes from supplied descriptors. **No native owner currently connects Apple's endpoint callbacks to oct_link.** Loopback transactions remain simulated. Read-only `oct_device_probe_ptp` deliberately stops after matching and releasing registry entries; it does not create the plug-in, open the interface or submit I/O.
