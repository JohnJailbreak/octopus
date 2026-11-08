# Binary inventory and provenance

User-supplied `Octopus_Reference_Binaries.zip`, extracted read-only to the analysis workspace. Original files were not modified. Origin is user attribution; no IPSW manifest, extraction procedure, or original signature chain was supplied. These identities do not establish which image is installed on the target phone.

| Image | Bytes | LC_UUID | SHA-256 | Mach-O type |
|---|---:|---|---|---|
| afcd | 75104 | E91A0D5B-F781-38E1-A464-720E3348B7E3 | `3791245c22b66ac6d1e63b4cadf6871bae80c3b961b0a307597e48ede4e00c4d` | MH_EXECUTE |
| lockdownd | 1273248 | 87AE1730-F0C0-39FC-84CD-24ED0EB4E48A | `95fb679abbe5b0fd978b9b94835587afcbd57a1e09899415e0f5f40c1e9db240` | MH_EXECUTE |
| ptpcamerad | 405056 | 245801E3-FF45-3942-8DBF-AA9F80BD4ADC | `2c665cc76c44ff36863a7768a2321e2b5c218cc687e858c40fe325f0fd14f5f7` | MH_EXECUTE |
| ptpd | 417184 | FFD54FC7-0508-3AB8-8D17-2F698B611E10 | `a1742358da3e257204a0f190211cda26a155dfe0dabb3c9fa572117460085dcb` | MH_EXECUTE |
| USBDeviceConfiguration | 73296 | DFA3C76A-08EE-3D68-9D96-BD56F908A6FA | `68bfd2de405f44b310950e41bc3d4036cdb868a3ec4ea87d181d29407bee3863` | MH_BUNDLE |

All five: thin little-endian 64-bit Mach-O, CPU ARM64 (`0x0100000c`), subtype arm64e (`0x80000002`), LC_BUILD_VERSION iOS / minimum 17.0 / SDK 17.0. Complete segment/load-command tables, imports, symbols, signature metadata are preserved in [evidence/inventory](evidence/inventory). USBDeviceConfiguration is a bundle, not an executable process.

Each contains code, data and LINKEDIT, with linked libraries outside this archive. They are not self-contained: actual IOKit device plug-in routines, AFC wire operations and Photos framework internals reside in absent libraries, typically supplied by the dyld shared cache. Do not mistake import stubs for their implementations. All five CodeDirectories have ad-hoc flag 0x2 and an empty CMS wrapper; preserved identifiers and entitlements are declarations, not proof of an original Apple signature or usable privilege. Installed bytes may differ after re-signing even when UUID matches. No code-signature cryptographic verification was performed.

## Address convention and ABI

Executables use unslid virtual addresses based at 0x100000000; USBDeviceConfiguration __TEXT starts at 0. File offsets follow segment mapping: fileoff + (VA - vmaddr), only inside file-backed segment bytes. Runtime VA = static VA + dyld slide. Tables and instruction references below use static VA, never guessed runtime pointers.

Ghidra 12.1.4 processed chained fixups and ARM64e instructions. PAC-authenticated pointers are not ordinary integers: runtime stripping/signing and image slide must be handled by the loader/runtime. Do not cast static table values into function pointers or patch PAC branches. Ghidra's EXTERNAL pseudo-addresses, unaligned imported-thunk warnings and guessed `undefined` signatures are not callable ABI evidence. Objective-C method encodings provide additional static type evidence but do not verify runtime object ownership, queue rules or method replacement safety.

Verified through static analysis: these file/header facts. Verified through runtime observation: none for these exact files. Strong inference: extracted or re-signed standalone images, dependent on system shared-cache libraries. Unresolved: original extraction provenance, exact installed identity and runtime ABI.
