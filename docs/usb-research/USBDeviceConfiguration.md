# USBDeviceConfiguration: existing configuration selection

## Identity and scope

Reference UUID **DFA3C76A-08EE-3D68-9D96-BD56F908A6FA**, arm64e, `LC_BUILD_VERSION` platform iOS, minimum/SDK 17.0.0. This is **MH_BUNDLE**, not an executable. Its signature identifier is `com.apple.configd.usbdeviceconfig`. Addresses below are unslid virtual addresses; `__TEXT` starts at zero, so these are also image-relative offsets. See [inventory](binary-inventory.md) for hashes, signing caveats and pointer-authentication conventions.

Evidence: Ghidra 12.1.4 instruction decoding, chained-fixup processing, cross-references and decompilation. [Selected functions](evidence/USBDeviceConfiguration/selected-functions.txt) preserve the relevant implementations. This analysis did not execute the bundle.

## Verified through static analysis

| Address / symbol | Actual behavior, callers and argument evidence | Confidence / remaining limit |
|---|---|---|
| `0x30e8 _start` | Registers `com.apple.mobile.lockdown` notification, checks current notify state, delays setup until lockdown arrival when needed; launches worker `0x32ec`. Calls `0x34e8` once lockdown is available. | High; host-process invocation and startup order not observed. |
| `0x2dc4 _lockdownArrivalCallback` | Calls `0x34e8`; invalidates/releases the notification run-loop source and Mach port, cancels token. | High; callback argument ABI not recovered because unused. |
| `0x34e8` (inferred `waitForController`) | Calls `IOUSBDeviceControllerRegisterArrivalCallback` with callback `0x3548`, zero context, current CFRunLoop and default mode. | High call/data flow; imported IOKit implementation unavailable. |
| `0x3548` (inferred `controllerArrived`) | Creates controller, creates description from defaults and controller, reads `UniqueDeviceID`, sets serial and UDID strings, calls `IOUSBDeviceControllerSetDescription`, releases controller/description, then applies requested-interface snapshot through `0x2f3c`. | High; descriptor contents and hardware effect unresolved. |
| `0x32ec` (inferred `configurationWorker`) | `bootstrap_check_in` for `com.apple.SystemConfiguration.USBDeviceConfig`; creates IOMIGMachPort, registers demultiplexer `0x372c`, schedules CFRunLoop; initializes CFArray at `0x8088`; on loop exit unschedules/releases port, decrements Mach right, releases array. | High; Mach service accessibility from Octopus not established. |
| `0x2e40 __usb_device_configuration_add_interface` | `x1` is UTF-8 C string; `x2` points to 32-bit result, initialized to zero. Converts string to CFString, locks mutex `0x8000`, checks array for duplicate, appends if missing, copies array, unlocks while applying configuration, writes result, releases copy/string. Called by MIG wrapper `0x37b0`. `x0` is passed the incoming port but unused in body. | High for argument roles and implementation. This is not a declaration approved for native calling. |
| `0x2fec __usb_device_configuration_remove_interface` | Same argument roles; removes existing name, copies list and reapplies it through `0x2f3c`. Caller `0x3880`. | High; removal does not prove USB disconnect behavior. |
| `0x2f3c` (inferred `applyRequestedInterfaces`) | `x0` array of requested names. Creates controller, calls `IOUSBDeviceDescriptionCreateFromController`, `IOUSBDeviceDescriptionGetMatchingConfiguration(description,names)`, and, only for nonzero match, `IOUSBDeviceControllerSetPreferredConfiguration`. Releases objects and returns comparison of final status with zero. Called by add/remove and arrival callback. | High. No-match path can return success; RPC success is **not proof of activation**. |
| `0x37b0`, `0x3880` (MIG add/remove wrappers) | Check non-complex message, size >39 and <297, string count <=256, count/size alignment and NUL via `memchr`; invoke implementations. Response status at `+0x20`, boolean result at `+0x24`, success response length 40. | High; IPC rights/trailer policy is outside these bodies. |
| `0x3774 _usb_device_configuration_server_routine`, `0x3950 _usb_device_configuration_server` | Dispatch only message IDs `0x7562` and `0x7563` via table at `0x4208` / `0x4230`; reply ID is request ID +100. Unknown messages receive MIG error. | High. These IDs are evidence, not a recommendation to send speculative requests. |
| `0x348c _stop` | Under mutex, stops saved run loop; signals passed run-loop source. | High; host ownership of source not established. |

Condensed implementation of `0x2f3c`, with role names substituted for decompiler temporaries:

```text
status = IOUSBDeviceControllerCreate(allocator, &controller)
if status == 0:
    description = IOUSBDeviceDescriptionCreateFromController(allocator, controller)
    if description:
        configuration = IOUSBDeviceDescriptionGetMatchingConfiguration(description, requestedNames)
        status = configuration ? SetPreferredConfiguration(controller, configuration) : 0
        release(description)
    release(controller)
return status == 0
```

The implementation stores a **set-like array of names**, not endpoint handlers. No provider object, bulk read callback, or new function descriptor is passed by these RPCs. Default descriptions are loaded by an imported IOKit routine; the actual catalogue of configurations and interface names is **not contained in this bundle**. No PTP-specific name can safely be selected solely from this analysis.

## Strong inference

This is the configd-side coordinator selecting among controller-supported descriptions. Adding a name requests a matching existing configuration; it does not create a third-party USB gadget. Changing preferred configuration or description may cause reconfiguration/re-enumeration, but the exact effect is in absent IOKit/controller implementation and requires observation. Therefore it is inappropriate to call either mutator merely to probe availability while preserving Photos and other USB functions.

## Verified through runtime observation

None for this bundle. Original Octopus diagnostics establish a running ptpd process and unavailable Octopus transport, not execution of these interfaces.

## Unresolved hypotheses and next evidence

1. Which defaults describe the phone's existing PTP/usbmux configuration? Acquire the matching configuration resources and read-only registry observations.
2. Which task may look up this Mach service, and which entitlement/sandbox rules govern it? The wrappers alone do not prove third-party authorization.
3. What does the controller require to instantiate another device function? Analyze matching IOKit shared-cache implementation/kernel user-client contract before designing an independent driver.
4. Do preferred configuration changes disrupt photo transfer or pairing? Only a controlled physical experiment can answer.

Octopus consequence: preserve current configuration. Prefer extending the existing PTP provider if its lifecycle is verified, and use read-only discovery/identity checks before any native integration is activated.
