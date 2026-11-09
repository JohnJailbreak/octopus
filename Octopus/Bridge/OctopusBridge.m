/*
 * OctopusBridge.m — rootless Logos tweak loaded into ptpd on iOS 17 / Dopamine.
 *
 * Before installing any hooks, this code verifies the running ptpd binary's UUID
 * against the analysed reference copy. Any mismatch aborts silently, leaving ptpd and
 * Photos unaffected.
 *
 * When identity is confirmed, the tweak:
 *
 *  1.  Calls [PTPVirtualCamera addStorage:] with an OctopusPTPStorage proxy object on
 *      the first PTPVirtualCamera init. This registers Octopus storage ID 0x00020001
 *      with ptpd's internal routing table so that ptpd includes it in GetStorageIDs
 *      without further hooks.
 *
 *  2.  Hooks [PTPVirtualCamera storageInfoDatasetForStorageID:] to return Octopus's
 *      StorageInfo (read-only, 4 KiB, synthetic) when called with our ID.
 *
 *  3.  Hooks [PTPVirtualCamera objectHandlesInStorage:forObjectFormatCode:
 *      inAssociation:responseCode:] to return Octopus handle numbers when called for
 *      our storage.
 *
 *  4.  Hooks [PTPVirtualCamera dataSourceForObjectHandle:responseCode:size:] to return
 *      an NSData-backed NSInputStream for Octopus handles, fetching file bytes from
 *      the Octopus app via the bridge socket.
 *
 *  5.  Hooks [PTPResponder transportActivated] and [PTPResponder transportDeactivated]
 *      to send HOST_ATTACHED and HOST_DETACHED to the bridge so Octopus can track
 *      Windows connection state.
 *
 * Data is fetched synchronously from the Octopus app's unix socket. For the synthetic
 * read-only storage the largest file is a few KB, so synchronous I/O is acceptable.
 *
 * Octopus handle range: 0x80000001–0x8FFFFFFF.
 * Octopus storage ID:   0x00020001.
 *
 * Requirements:
 *   • Dopamine rootless jailbreak, iOS 17.0 on iPhone14,5.
 *   • CydiaSubstrate or libhooker installed.
 *   • Octopus.app running and its bridge socket open at OCT_BRIDGE_DEFAULT_SOCKET.
 *   • ptpd UUID FFD54FC7-0508-3AB8-8D17-2F698B611E10 (iOS 17.0 reference build).
 *
 * This file is a Logos source for the Theos build system. Build with:
 *   make -C Octopus/Bridge THEOS=/var/jb/opt/theos
 */

#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

/* The bridge socket API and PTP buffer helpers — compiled into the dylib. */
#include "../Core/oct_bridge.h"
#include "../Core/oct_log.h"
#include "../Protocol/oct_ptp.h"

#import <Foundation/Foundation.h>
#import <objc/runtime.h>

#define OCTOPUS_STORAGE_ID   0x00020001u
#define OCTOPUS_HANDLE_BASE  0x80000001u
#define OCTOPUS_HANDLE_MAX   0x8FFFFFFFu
#define BRIDGE_COMPONENT     "bridge-tweak"
#define PTPD_UUID            "FFD54FC7-0508-3AB8-8D17-2F698B611E10"

/* The analysed ptpd build. Identity is verified before any hook fires. */
static const char * const k_reference_uuid = PTPD_UUID;
static bool g_identity_ok = false;

/* ---- Identity verification --------------------------------------------------------------- */

/* Returns the LC_UUID string from the running binary's Mach-O header, or NULL. */
static NSString *running_ptpd_uuid(void)
{
    const struct mach_header *header = NULL;
    uint32_t image_count = _dyld_image_count();
    for (uint32_t i = 0; i < image_count; i++) {
        const char *name = _dyld_get_image_name(i);
        if (name && strstr(name, "ptpd")) {
            header = _dyld_get_image_header(i);
            break;
        }
    }
    if (!header) return nil;
    bool is64 = (header->magic == MH_MAGIC_64 || header->magic == MH_CIGAM_64);
    const uint8_t *commands = is64 ? ((const uint8_t *)header + sizeof(struct mach_header_64))
                                   : ((const uint8_t *)header + sizeof(struct mach_header));
    uint32_t ncmds = header->ncmds;
    for (uint32_t i = 0; i < ncmds; i++) {
        const struct load_command *lc = (const struct load_command *)commands;
        if (lc->cmd == LC_UUID) {
            const struct uuid_command *uc = (const struct uuid_command *)lc;
            const uint8_t *u = uc->uuid;
            return [NSString stringWithFormat:
                @"%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7],
                u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]];
        }
        commands += lc->cmdsize;
    }
    return nil;
}

/* ---- Bridge access ----------------------------------------------------------------------- */

static oct_bridge_client *g_client = nil;
static dispatch_queue_t g_bridge_queue = nil;

static void bridge_connect_async(void)
{
    dispatch_async(g_bridge_queue, ^{
        if (g_client) return;
        oct_bridge_client *client = NULL;
        oct_status status = oct_bridge_client_connect(OCT_BRIDGE_DEFAULT_SOCKET, 1000, &client);
        if (status == OCT_OK) {
            g_client = client;
            oct_logf(OCT_LOG_INFO, BRIDGE_COMPONENT, "connected",
                     "OctopusBridge connected to the app socket");
        } else {
            oct_logf(OCT_LOG_WARN, BRIDGE_COMPONENT, "connect_failed",
                     "Couldn't connect to %s: %s (the Octopus app may not be open)",
                     OCT_BRIDGE_DEFAULT_SOCKET, oct_status_name(status));
        }
    });
}

/* Sends a message to the bridge; returns true when the client is connected and the write succeeds. */
static bool bridge_send(uint8_t type, const void *payload, size_t length)
{
    if (!g_client) return false;
    uint8_t *buf = oct_bridge_alloc(type, payload, length, NULL);
    if (!buf) return false;
    size_t buf_len = OCT_BRIDGE_HEADER_SIZE + length;
    bool ok = write(g_client->fd, buf, buf_len) == (ssize_t)buf_len;
    free(buf);
    return ok;
}

/* Reads from the bridge socket and returns the bytes for a known Octopus handle, or nil. */
static NSData *fetch_object_data(uint32_t oct_handle)
{
    if (!g_client) return nil;
    /* Use a one-shot client connection (the main client may be in use on another thread). */
    oct_bridge_client *c = NULL;
    if (oct_bridge_client_connect(OCT_BRIDGE_DEFAULT_SOCKET, 500, &c) != OCT_OK) return nil;
    /* Attach, OpenSession, GetObject, CloseSession, Detach. */
    if (oct_bridge_client_host_event(c, true, NULL) != OCT_OK) {
        oct_bridge_client_destroy(c);
        return nil;
    }
    /* Build containers manually using oct_bridge wire protocol. */
    /* OpenSession */
    uint8_t cmd[32];
    uint32_t session = 0xBEEF;
    oct_buf b;
    oct_buf_init(&b, 0);
    oct_container_command(&b, 0x1002 /* OpenSession */, 0, &session, 1);
    oct_bridge_client_send_bulk(c, b.data, b.length);
    oct_buf_free(&b);
    uint8_t type;
    uint8_t *resp = NULL;
    size_t resp_len = 0;
    if (oct_bridge_client_receive(c, &type, &resp, &resp_len, 2000) != OCT_OK || resp_len < 12) {
        free(resp);
        oct_bridge_client_destroy(c);
        return nil;
    }
    free(resp);
    /* GetObject with the original Octopus handle (remove the 0x80000000 offset) */
    uint32_t inner_handle = oct_handle - OCTOPUS_HANDLE_BASE + 1;
    oct_buf_init(&b, 0);
    oct_container_command(&b, 0x1009 /* GetObject */, 1, &inner_handle, 1);
    oct_bridge_client_send_bulk(c, b.data, b.length);
    oct_buf_free(&b);
    /* Data phase */
    if (oct_bridge_client_receive(c, &type, &resp, &resp_len, 5000) != OCT_OK || resp_len < 12) {
        free(resp);
        oct_bridge_client_destroy(c);
        return nil;
    }
    /* resp is a PTP data container: 12-byte header, then payload. */
    uint32_t container_len = resp_len >= 4 ? (uint32_t)resp[0] | ((uint32_t)resp[1] << 8) |
                                              ((uint32_t)resp[2] << 16) | ((uint32_t)resp[3] << 24) : 0;
    uint16_t container_type = resp_len >= 6 ? (uint16_t)(resp[4] | (resp[5] << 8)) : 0;
    NSData *result = nil;
    if (container_type == 0x0002 /* data */ && container_len >= 12 && container_len <= resp_len) {
        result = [NSData dataWithBytes:resp + 12 length:container_len - 12];
    }
    free(resp);
    /* Response phase */
    if (oct_bridge_client_receive(c, &type, &resp, &resp_len, 1000) == OCT_OK) free(resp);
    /* CloseSession */
    oct_buf_init(&b, 0);
    oct_container_command(&b, 0x1003 /* CloseSession */, 2, NULL, 0);
    oct_bridge_client_send_bulk(c, b.data, b.length);
    oct_buf_free(&b);
    if (oct_bridge_client_receive(c, &type, &resp, &resp_len, 1000) == OCT_OK) free(resp);
    oct_bridge_client_host_event(c, false, "data fetch done");
    oct_bridge_client_destroy(c);
    return result;
}

/* ---- OctopusPTPStorage proxy ------------------------------------------------------------- */

/*
 * This is a minimal NSObject subclass that occupies the slot in ptpd's storages dictionary.
 * Its properties return values consistent with Octopus's read-only synthetic storage.
 * The actual routing is done by the PTPVirtualCamera method hooks below; ptpd calls those
 * on the camera object, not directly on the storage object, for the operations we intercept.
 */
@interface OctopusPTPStorage : NSObject
@property (nonatomic, assign) uint32_t storageID;
@property (nonatomic, assign) uint32_t sessionID;
@end

@implementation OctopusPTPStorage

- (instancetype)init {
    if ((self = [super init])) {
        _storageID = OCTOPUS_STORAGE_ID;
        _sessionID = 0;
    }
    return self;
}

/* ptpd calls this to get the storage's numeric ID. */
- (uint32_t)storageID { return OCTOPUS_STORAGE_ID; }
- (uint32_t)sessionID { return _sessionID; }

/* Returns false so ptpd doesn't treat this as the Photos storage. */
- (BOOL)isPhotoStorage { return NO; }
- (BOOL)hasObjects { return YES; }

/* PTP StorageType=0x0003 (Fixed RAM), FilesystemType=0x0002 (Generic hierarchical),
 * AccessCapability=0x0001 (read-only), MaxCapacity=0x1200, FreeSpaceInBytes=0,
 * FreeSpaceInImages=0xFFFFFFFF, StorageDescription="Octopus", VolumeIdentifier="octopus". */
- (NSData *)storageInfoDataset {
    uint8_t buf[256];
    size_t pos = 0;
#define W16(v) do { buf[pos++] = (v) & 0xFF; buf[pos++] = ((v) >> 8) & 0xFF; } while (0)
#define W32(v) do { W16(v); W16((v) >> 16); } while (0)
#define W64(v) do { W32(v); W32((uint64_t)(v) >> 32); } while (0)
#define WSTR(s) do { \
    const char *_s = (s); \
    size_t _n = strlen(_s); \
    buf[pos++] = (uint8_t)(_n + 1); \
    for (size_t _i = 0; _i < _n; _i++) { buf[pos++] = _s[_i]; buf[pos++] = 0; } \
    buf[pos++] = 0; buf[pos++] = 0; } while (0)
    W16(0x0003); /* StorageType: Fixed RAM */
    W16(0x0002); /* FilesystemType: Generic hierarchical */
    W16(0x0001); /* AccessCapability: read-only */
    W64(4669);   /* MaxCapacity: synthetic storage size */
    W64(0);      /* FreeSpaceInBytes */
    W32(0xFFFFFFFFu); /* FreeSpaceInImages */
    WSTR("Octopus"); /* StorageDescription */
    WSTR("octopus"); /* VolumeIdentifier */
#undef W16
#undef W32
#undef W64
#undef WSTR
    return [NSData dataWithBytes:buf length:pos];
}

@end

/* ---- Logos hooks ------------------------------------------------------------------------- */

static OctopusPTPStorage *g_octopus_storage = nil;

%hook PTPVirtualCamera

/* After ptpd initialises the camera, register Octopus as a second storage. */
- (id)initWithName:(id)name andPath:(id)path {
    id result = %orig;
    if (result && !g_octopus_storage) {
        g_octopus_storage = [[OctopusPTPStorage alloc] init];
        [result addStorage:g_octopus_storage];
        oct_logf(OCT_LOG_INFO, BRIDGE_COMPONENT, "storage_registered",
                 "OctopusPTPStorage (ID 0x%08X) registered with PTPVirtualCamera; "
                 "bridge socket %s", OCTOPUS_STORAGE_ID, OCT_BRIDGE_DEFAULT_SOCKET);
        bridge_connect_async();
    }
    return result;
}

/* Return StorageInfo for Octopus's storage; call through for Photos. */
- (id)storageInfoDatasetForStorageID:(uint32_t)storageID {
    if (storageID == OCTOPUS_STORAGE_ID) {
        return g_octopus_storage ? [g_octopus_storage storageInfoDataset] : nil;
    }
    return %orig;
}

/*
 * Return the handles for objects in Octopus storage.
 * Octopus handle range: OCTOPUS_HANDLE_BASE (0x80000001) + (inner_handle - 1).
 * We ask Octopus for all handles via the bridge and remap them.
 */
- (id)objectHandlesInStorage:(uint32_t)storageID
       forObjectFormatCode:(uint16_t)formatCode
            inAssociation:(uint32_t)association
             responseCode:(uint16_t *)responseCode {
    if (storageID != OCTOPUS_STORAGE_ID) return %orig;
    if (!g_client) {
        if (responseCode) *responseCode = 0x2001; /* OK: empty list, we're not ready */
        return @[];
    }
    /* Fetch handles from Octopus via bridge. For the synthetic storage there are ≤6 handles. */
    oct_bridge_client *c = NULL;
    if (oct_bridge_client_connect(OCT_BRIDGE_DEFAULT_SOCKET, 500, &c) != OCT_OK) {
        if (responseCode) *responseCode = 0x2001;
        return @[];
    }
    oct_bridge_client_host_event(c, true, NULL);
    oct_buf b;
    oct_buf_init(&b, 0);
    uint32_t session = 0xC0DE;
    oct_container_command(&b, 0x1002, 0, &session, 1);
    oct_bridge_client_send_bulk(c, b.data, b.length);
    oct_buf_free(&b);
    uint8_t type;
    uint8_t *resp = NULL;
    size_t resp_len = 0;
    if (oct_bridge_client_receive(c, &type, &resp, &resp_len, 1000) != OCT_OK) {
        free(resp);
        oct_bridge_client_host_event(c, false, NULL);
        oct_bridge_client_destroy(c);
        if (responseCode) *responseCode = 0x2001;
        return @[];
    }
    free(resp);
    /* GetObjectHandles: params = storageID=0xFFFFFFFF (all), format=0, assoc=parent */
    uint32_t params[3] = { 0xFFFFFFFFu, 0, association };
    oct_buf_init(&b, 0);
    oct_container_command(&b, 0x1007, 1, params, 3);
    oct_bridge_client_send_bulk(c, b.data, b.length);
    oct_buf_free(&b);
    if (oct_bridge_client_receive(c, &type, &resp, &resp_len, 2000) != OCT_OK || resp_len < 16) {
        free(resp);
        oct_bridge_client_host_event(c, false, NULL);
        oct_bridge_client_destroy(c);
        if (responseCode) *responseCode = 0x2001;
        return @[];
    }
    uint32_t count = (uint32_t)resp[12] | ((uint32_t)resp[13] << 8) |
                     ((uint32_t)resp[14] << 16) | ((uint32_t)resp[15] << 24);
    NSMutableArray *handles = [NSMutableArray array];
    for (uint32_t i = 0; i < count && 16 + 4 * (i + 1) <= resp_len; i++) {
        uint32_t h = (uint32_t)resp[16 + 4 * i] | ((uint32_t)resp[17 + 4 * i] << 8) |
                     ((uint32_t)resp[18 + 4 * i] << 16) | ((uint32_t)resp[19 + 4 * i] << 24);
        [handles addObject:@(h + OCTOPUS_HANDLE_BASE - 1)];
    }
    free(resp);
    if (oct_bridge_client_receive(c, &type, &resp, &resp_len, 500) == OCT_OK) free(resp); /* response */
    oct_bridge_client_host_event(c, false, NULL);
    oct_bridge_client_destroy(c);
    if (responseCode) *responseCode = 0x2001;
    return handles;
}

/* Return an NSInputStream for Octopus handles. */
- (id)dataSourceForObjectHandle:(uint32_t)handle
                   responseCode:(uint16_t *)responseCode
                           size:(uint32_t *)size {
    if (handle < OCTOPUS_HANDLE_BASE || handle > OCTOPUS_HANDLE_MAX) return %orig;
    NSData *data = fetch_object_data(handle);
    if (!data) {
        if (responseCode) *responseCode = 0x2009; /* Invalid object handle */
        if (size) *size = 0;
        return nil;
    }
    if (responseCode) *responseCode = 0x2001; /* OK */
    if (size) *size = (uint32_t)[data length];
    return [NSInputStream inputStreamWithData:data];
}

%end

%hook PTPResponder

- (void)transportActivated {
    %orig;
    oct_logf(OCT_LOG_INFO, BRIDGE_COMPONENT, "transport_activated",
             "PTPResponder transportActivated: Windows connected the PTP interface");
    bridge_send(OCT_BRIDGE_MSG_HOST_ATTACHED, NULL, 0);
}

- (void)transportDeactivated {
    oct_logf(OCT_LOG_INFO, BRIDGE_COMPONENT, "transport_deactivated",
             "PTPResponder transportDeactivated: Windows disconnected");
    const char *reason = "Windows disconnected";
    bridge_send(OCT_BRIDGE_MSG_HOST_DETACHED, reason, strlen(reason));
    %orig;
}

%end

/* ---- Constructor -------------------------------------------------------------------------- */

%ctor {
    NSString *uuid = running_ptpd_uuid();
    if (!uuid) {
        NSLog(@"[OctopusBridge] Cannot read the running binary UUID; not installing hooks.");
        return;
    }
    if (![uuid isEqualToString:@(k_reference_uuid)]) {
        NSLog(@"[OctopusBridge] ptpd UUID %@ does not match the analysed reference %s; "
              "not installing hooks to protect Photos.", uuid, k_reference_uuid);
        return;
    }
    g_identity_ok = true;
    g_bridge_queue = dispatch_queue_create("com.parv.octopus.bridge", DISPATCH_QUEUE_SERIAL);
    %init;
    oct_logf(OCT_LOG_INFO, BRIDGE_COMPONENT, "loaded",
             "OctopusBridge loaded into ptpd (UUID %s confirmed)", k_reference_uuid);
}
