# Storage and object-provider boundary

Identity/convention: ptpd UUID FFD54FC7-0508-3AB8-8D17-2F698B611E10; arm64e/iOS17; base 0x100000000. [Inventory](binary-inventory.md).

## Verified through static analysis

[PTPVirtualCamera initWithName:andPath: 0x10000bf70](evidence/ptpd/10000bf70.txt) initializes camera/provider state. [PTPStorage initWithParent:andOptionalID:forSessionID: 0x1000160c4](evidence/ptpd/1000160c4.txt) creates storage tied to parent and session. These are concrete object lifecycles, not a discovered third-party registration protocol.

[addStorage: 0x10000daf0](evidence/ptpd/10000daf0.txt), encoding `v24@0:8@16`, checks existing storage dictionary count; when it equals 1 it logs “Two storages cannot co-exist, should quit”, then still inserts. This is evidence of an assumption, not proof that the second call immediately terminates. Blindly calling addStorage is not safe evidence of supported coexistence.

[storageWithID: 0x1000105f4](evidence/ptpd/1000105f4.txt), [objectForObjectHandle: 0x1000106fc](evidence/ptpd/1000106fc.txt) and [objectHandlesInStorage:forObjectFormatCode:inAssociation:responseCode: 0x1000108cc](evidence/ptpd/1000108cc.txt) route dictionary/object lookups. [storageInfoDatasetForStorageID: 0x100011690](evidence/ptpd/100011690.txt) and [objectInfoDatasetForObjectHandle: 0x1000116d8](evidence/ptpd/1000116d8.txt) bridge datasets into responder methods.

[dataSourceForObjectHandle:responseCode:size: 0x10001179c](evidence/ptpd/10001179c.txt), exact metadata `@36@0:8I16^S20^I28`:

```text
object = objectForObjectHandle(NSNumber(unsigned handle))
if object isKindOfClass PTPFile:
    source = object.dataSource
    *size = object.objectInfoDataset.objectCompressedSize
    *response = 0x2001
    return source
*response = 0x2009
return nil
```

x0/self, x1/selector, w2/handle, x3/uint16 response output, x4/uint32 size output are supported by metadata and stores. This does not authorize invoking the method on guessed objects. [PTPFile initWithPLPTPAsset:supportedFormats:andParent: 0x1000257c8](evidence/ptpd/1000257c8.txt) and [dataSource 0x1000266c4](evidence/ptpd/1000266c4.txt) connect files to Photos assets. Octopus's existing oct_store cannot simply be cast to this class.

[MTP object property list 0x100013430](evidence/ptpd/100013430.txt) is another routing surface that a future provider must preserve, alongside handles, folders, info, data source and session lifecycle. Storage/object identifiers must be disjoint from Photos and stable within a session.

Confidence: high for inspected instructions and metadata; medium for proposed extension strategy. Verified runtime: none. Unresolved: authorization lifetime, provider replacement/addition safety, Photos change notifications, exact data-source protocol, thread/queue ownership, event invalidation and error recovery. Next step is matching on-device identity plus a read-only runtime class/method/lifecycle observation tool, before any provider mutation. This release collects identity/registry evidence only; it does not contain that in-process observation tool.
