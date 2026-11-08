# Evidence-based USB architecture

The existing Photos service is ptpd -> PTPDeviceUSBTransport -> private IOKit device interface -> PTP USB bulk/interrupt/control endpoints. The inspected configuration bundle selects an existing device configuration. Lockdown handles host trust and authorized service connections; afcd serves a rooted filesystem over its authorized socket. ptpcamerad's inspected USB path is an initiator for external cameras.

Octopus's portable responder is already separate: oct_store -> oct_responder -> oct_service -> oct_link -> missing native driver. This release adds a full-setup/control and descriptor boundary (`oct_usb`) and read-only iOS discovery/identity (`oct_device`). The new probe uses exactly ptpd's observed matching dictionary and only generic IOKit functions; it does not load Apple's private interface plug-in.

The generic IOKit declarations and reference consumption rules are cross-checked against [Apple IOKitLib.h](https://github.com/apple-oss-distributions/IOKitUser/blob/main/IOKitLib.h). Availability and permission on this jailbreak are unverified, hence errors become exportable BLOCKED results. No new entitlements, background daemon, hook, or system write were added.

The physical baseline is the supplied diagnostics, not this static graph. It has zero external transactions. Seeing an IORegistry match, a running ptpd, or successful loopback replies establishes neither USB ownership nor Windows interoperability.
