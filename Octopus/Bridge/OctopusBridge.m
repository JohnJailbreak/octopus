/*
 * OctopusBridge.m — rootless Logos tweak (arm64e, Dopamine, iOS 17) loaded into ptpd.
 *
 * STAGE STATUS: Stage 1 (app socket) is in place. Stage 2 (this tweak) is a draft.
 *               This file compiles and injects safely; the PTP hooks are disabled
 *               behind a guarded flag until Stage 3 (runtime observation) is complete.
 *
 * HOW TO DEPLOY
 *   1. Build and install Octopus.tipa from the latest release.
 *   2. Enable USB Access in the app; confirm the bridge.app_socket diagnostic is PASS.
 *   3. Build this tweak:  make package THEOS=/var/jb/opt/theos
 *   4. Install the .deb with Sileo.
 *   5. Connect the iPhone to Windows over USB.
 *   6. Run Settings → Diagnostics → Run Full Diagnostics and send the ZIP.
 *
 * THREADING
 *   ptpd runs its PTP handlers on its own serial queue. All bridge socket operations
 *   must also be serialized to avoid partial frames. We use a dedicated serial dispatch
 *   queue; every hook that touches the socket dispatches synchronously to it.
 *
 * SAFETY CONTRACT
 *   - Identity check before any hook fires: UUID must match the analysed reference build.
 *   - Every method we hook is verified in the decompiled evidence (see docs/usb-research/).
 *   - OctopusPTPStorage forwards unknown selectors via NSObject's standard mechanism and
 *     logs them so we can observe what ptpd actually calls on a storage object.
 *   - If the bridge socket is unavailable, every hook falls through to the original.
 *   - Errors in our code never crash ptpd; at worst they return nil/0/stall.
 *
 * ARCHITECTURE CHOICE
 *   Option A (addStorage:) is used: verified in decompilation at 0x10000daf0. The camera
 *   dispatch methods (objectHandlesInStorage:..., storageInfoDatasetForStorageID:,
 *   dataSourceForObjectHandle:responseCode:size:) are hooked to serve our storage. The
 *   addStorage: coexistence warning fires but does not prevent insertion.
 *
 *   Option B (request forwarding at handleBulkData:) would require building full PTP
 *   response packets in the tweak; Option A re-uses ptpd's own response serialization.
 *   This choice may change once Stage 3 observation data is available.
 *
 * OPEN ITEMS (do not enable HOOKS_ENABLED until these are resolved)
 *   • The app doesn't start the bridge socket server automatically; the user must enable
 *     USB Access. When this tweak fires before the app is running, the socket won't exist.
 *   • What ptpd calls on a PTPStorage object beyond storageID/sessionID is unobserved.
 *     OctopusPTPStorage logs unknown selectors; collect those logs before enabling hooks.
 *   • The server accepts one client at a time; this tweak must hold its connection open
 *     and not open short-lived connections for every PTP operation (see bridge protocol).
 */

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-variable"

#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "../Core/oct_bridge.h"
#include "../Core/oct_log.h"
#include "../Protocol/oct_ptp.h"

#import <Foundation/Foundation.h>
#import <objc/runtime.h>

/* ---- Configuration ------------------------------------------------------------------ */

/*
 * When HOOKS_ENABLED is 0 (the default), the tweak verifies identity, connects to the
 * bridge socket, and logs but does NOT modify ptpd's behaviour. Set to 1 only after:
 *  - Stage 3 observation confirms the PTPStorage selector surface
 *  - The app reliably starts the bridge socket before ptpd's init fires
 */
#define HOOKS_ENABLED 0

#define OCTOPUS_STORAGE_ID   0x00020001u
#define OCTOPUS_HANDLE_BASE  0x80000001u
#define OCTOPUS_HANDLE_MAX   0x8FFFFFFFu

#define BRIDGE_COMPONENT     "bridge-tweak"
#define PTPD_REFERENCE_UUID  "FFD54FC7-0508-3AB8-8D17-2F698B611E10"

/* ---- Identity verification ---------------------------------------------------------- */

static NSString *running_ptpd_uuid(void)
{
    uint32_t n = _dyld_image_count();
    for (uint32_t i = 0; i < n; i++) {
        const char *name = _dyld_get_image_name(i);
        if (!name || !strstr(name, "ptpd")) continue;
        const struct mach_header *hdr = _dyld_get_image_header(i);
        if (!hdr) continue;
        bool is64 = hdr->magic == MH_MAGIC_64 || hdr->magic == MH_CIGAM_64;
        const uint8_t *cmds = is64 ? ((const uint8_t *)hdr + sizeof(struct mach_header_64))
                                   : ((const uint8_t *)hdr + sizeof(struct mach_header));
        for (uint32_t j = 0; j < hdr->ncmds; j++) {
            const struct load_command *lc = (const struct load_command *)cmds;
            if (lc->cmd == LC_UUID) {
                const struct uuid_command *uc = (const struct uuid_command *)lc;
                const uint8_t *u = uc->uuid;
                return [NSString stringWithFormat:
                    @"%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                    u[0],u[1],u[2],u[3],u[4],u[5],u[6],u[7],
                    u[8],u[9],u[10],u[11],u[12],u[13],u[14],u[15]];
            }
            cmds += lc->cmdsize;
        }
    }
    return nil;
}

/* ---- Bridge connection (one persistent client, one serial queue) --------------------- */

static dispatch_queue_t g_queue;    /* serialises all socket operations */
static oct_bridge_client *g_client; /* accessed only on g_queue */
static bool g_identity_ok;
static bool g_hooks_enabled;

/*
 * Connect (or reconnect) to the app's bridge socket. Called on g_queue.
 * Returns true when the connection is established.
 */
static bool bridge_connect_locked(void)
{
    if (g_client) return true;
    oct_bridge_client *client = NULL;
    oct_status status = oct_bridge_client_connect(OCT_BRIDGE_DEFAULT_SOCKET, 500, &client);
    if (status != OCT_OK) {
        oct_logf(OCT_LOG_WARN, BRIDGE_COMPONENT, "connect_failed",
                 "Cannot connect to %s: %s (enable USB Access in Octopus)",
                 OCT_BRIDGE_DEFAULT_SOCKET, oct_status_name(status));
        return false;
    }
    g_client = client;
    oct_logf(OCT_LOG_INFO, BRIDGE_COMPONENT, "connected",
             "OctopusBridge connected to the app socket at %s", OCT_BRIDGE_DEFAULT_SOCKET);
    return true;
}

static void bridge_disconnect_locked(const char *reason)
{
    if (!g_client) return;
    oct_bridge_client_destroy(g_client);
    g_client = NULL;
    oct_logf(OCT_LOG_INFO, BRIDGE_COMPONENT, "disconnected",
             "Bridge socket disconnected: %s", reason);
}

/*
 * Sends a HOST_ATTACHED or HOST_DETACHED notification. Called from hooks.
 */
static void bridge_host_event(bool attached, const char *reason)
{
    dispatch_sync(g_queue, ^{
        if (!bridge_connect_locked()) return;
        oct_status s = oct_bridge_client_host_event(g_client, attached, reason);
        if (s != OCT_OK) {
            oct_logf(OCT_LOG_WARN, BRIDGE_COMPONENT, "host_event_failed",
                     "%s failed: %s", attached ? "HOST_ATTACHED" : "HOST_DETACHED",
                     oct_status_name(s));
            bridge_disconnect_locked("host event write failed");
        }
    });
}

/*
 * Sends a PTP command container and receives the data + response phases.
 * Returns the response payload (caller frees) or nil on error.
 * Called on g_queue.
 */
static NSData *bridge_request_locked(const oct_buf *command, uint8_t **response_out,
                                      size_t *response_len_out)
{
    if (!bridge_connect_locked()) return nil;
    oct_status s = oct_bridge_client_send_bulk(g_client, command->data, command->length);
    if (s != OCT_OK) {
        bridge_disconnect_locked("send_bulk failed");
        return nil;
    }
    uint8_t type = 0;
    uint8_t *data = NULL;
    size_t data_len = 0;
    /* data phase */
    if (oct_bridge_client_receive(g_client, &type, &data, &data_len, 2000) != OCT_OK || data_len < 12) {
        free(data);
        bridge_disconnect_locked("data phase receive failed");
        return nil;
    }
    NSData *result = nil;
    if (type == OCT_BRIDGE_MSG_BULK_TO_HOST && data_len > 12) {
        result = [NSData dataWithBytes:data + 12 length:data_len - 12];
    }
    free(data);
    /* response phase */
    uint8_t *resp = NULL;
    size_t resp_len = 0;
    if (oct_bridge_client_receive(g_client, &type, &resp, &resp_len, 1000) != OCT_OK) {
        free(resp);
        bridge_disconnect_locked("response phase receive failed");
        return nil;
    }
    if (response_out) {
        *response_out = resp;
        *response_len_out = resp_len;
    } else {
        free(resp);
    }
    return result;
}

/* ---- OctopusPTPStorage proxy -------------------------------------------------------- */

/*
 * A minimal NSObject subclass that occupies a slot in ptpd's storages dictionary.
 * It logs any selector ptpd calls on it beyond the ones we expect; this is Stage 3
 * (runtime observation). The forwardingTargetForSelector: returning nil means unrecognised
 * messages raise NSInvalidArgumentException inside ptpd — which would crash ptpd.
 * Therefore we implement methodSignatureForSelector: and forwardInvocation: to absorb them.
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

- (uint32_t)storageID { return OCTOPUS_STORAGE_ID; }

- (NSMethodSignature *)methodSignatureForSelector:(SEL)sel {
    NSMethodSignature *sig = [super methodSignatureForSelector:sel];
    if (sig) return sig;
    /* Return a generic id-returning signature so forwardInvocation: fires instead of crashing. */
    return [NSMethodSignature signatureWithObjCTypes:"@@:"];
}

- (void)forwardInvocation:(NSInvocation *)invocation {
    /* Log the unknown selector for Stage 3 observation, return nil/0 without crashing. */
    oct_logf(OCT_LOG_INFO, BRIDGE_COMPONENT, "unknown_storage_selector",
             "ptpd called unimplemented selector '%s' on OctopusPTPStorage — "
             "note this for the runtime observation log and add an implementation",
             sel_getName(invocation.selector));
    /* Set a nil return value (safe for id, void, and numeric return types up to pointer width). */
    if (invocation.methodSignature.methodReturnLength <= sizeof(void *)) {
        const void *nil_val = NULL;
        [invocation setReturnValue:(void *)&nil_val];
    }
}

@end

/* ---- Logos hooks -------------------------------------------------------------------- */

static OctopusPTPStorage *g_octopus_storage = nil;

#if HOOKS_ENABLED

%hook PTPVirtualCamera

/*
 * After ptpd creates the camera, inject our proxy storage.
 * We do NOT subclass or replace the existing storage.
 */
- (id)initWithName:(id)name andPath:(id)path {
    id result = %orig;
    if (result && !g_octopus_storage) {
        dispatch_sync(g_queue, ^{
            g_octopus_storage = [[OctopusPTPStorage alloc] init];
        });
        [result addStorage:g_octopus_storage];
        oct_logf(OCT_LOG_INFO, BRIDGE_COMPONENT, "storage_injected",
                 "Injected OctopusPTPStorage (ID 0x%08X) into PTPVirtualCamera. "
                 "HOOKS_ENABLED=1; Stage 3 observation required.", OCTOPUS_STORAGE_ID);
    }
    return result;
}

/*
 * Serve StorageInfo for our storage ID; pass through for Photos.
 * Returns NSData containing the PTP StorageInfo dataset, as expected by storageInfoDatasetForStorageID:.
 * We build a minimal read-only fixed-RAM dataset.
 */
- (id)storageInfoDatasetForStorageID:(uint32_t)storageID {
    if (storageID != OCTOPUS_STORAGE_ID) return %orig;
    /* Build a PTP StorageInfo dataset: StorageType=Fixed RAM (3), FS=Generic (2),
     * AccessCapability=read-only (1), MaxCapacity=synthetic ~4 KB, FreeSpace=0. */
    uint8_t buf[64];
    size_t pos = 0;
#define W16(v) do { buf[pos]=(v)&0xFF; buf[pos+1]=((v)>>8)&0xFF; pos+=2; } while(0)
#define W32(v) do { W16(v); W16((v)>>16); } while(0)
#define W64(v) do { W32(v); W32((uint64_t)(v)>>32); } while(0)
#define WSTR(s) do { \
    const char *_s=(s); uint8_t _n=(uint8_t)strlen(_s); \
    buf[pos++]=_n+1; \
    for(uint8_t _i=0;_i<_n;_i++){buf[pos++]=_s[_i];buf[pos++]=0;} \
    buf[pos++]=0;buf[pos++]=0; } while(0)
    W16(0x0003); W16(0x0002); W16(0x0001);
    W64(4669); W64(0); W32(0xFFFFFFFFu);
    WSTR("Octopus"); WSTR("octopus");
#undef W16 #undef W32 #undef W64 #undef WSTR
    return [NSData dataWithBytes:buf length:pos];
}

/*
 * Serve handles for our storage. For other storages, call through.
 */
- (id)objectHandlesInStorage:(uint32_t)storageID
       forObjectFormatCode:(uint16_t)formatCode
            inAssociation:(uint32_t)assoc
             responseCode:(uint16_t *)responseCode {
    if (storageID != OCTOPUS_STORAGE_ID) return %orig;
    if (responseCode) *responseCode = 0x2001; /* OK */
    __block NSArray *handles = @[];
    dispatch_sync(g_queue, ^{
        if (!bridge_connect_locked()) return;
        uint32_t session = 0xC0DE, params[3] = { 0xFFFFFFFFu, 0, assoc };
        oct_buf cmd; oct_buf_init(&cmd, 0);
        oct_bridge_client_host_event(g_client, true, NULL);
        oct_container_command(&cmd, 0x1002, 0, &session, 1); /* OpenSession */
        oct_bridge_client_send_bulk(g_client, cmd.data, cmd.length);
        oct_buf_free(&cmd);
        uint8_t type = 0; uint8_t *rx = NULL; size_t rxlen = 0;
        if (oct_bridge_client_receive(g_client, &type, &rx, &rxlen, 1000) != OCT_OK) {
            free(rx); bridge_disconnect_locked("open session failed"); return;
        }
        free(rx);
        oct_buf_init(&cmd, 0);
        oct_container_command(&cmd, 0x1007, 1, params, 3); /* GetObjectHandles */
        oct_bridge_client_send_bulk(g_client, cmd.data, cmd.length);
        oct_buf_free(&cmd);
        /* data phase */
        if (oct_bridge_client_receive(g_client, &type, &rx, &rxlen, 2000) != OCT_OK || rxlen < 16) {
            free(rx); bridge_disconnect_locked("get handles data failed"); return;
        }
        uint32_t count = (uint32_t)rx[12]|((uint32_t)rx[13]<<8)|((uint32_t)rx[14]<<16)|((uint32_t)rx[15]<<24);
        NSMutableArray *arr = [NSMutableArray array];
        for (uint32_t i = 0; i < count && 16 + 4*(i+1) <= rxlen; i++) {
            uint32_t h = (uint32_t)rx[16+4*i]|((uint32_t)rx[17+4*i]<<8)|
                         ((uint32_t)rx[18+4*i]<<16)|((uint32_t)rx[19+4*i]<<24);
            [arr addObject:@(h + OCTOPUS_HANDLE_BASE - 1)];
        }
        free(rx);
        if (oct_bridge_client_receive(g_client, &type, &rx, &rxlen, 500) == OCT_OK) free(rx); /* resp */
        oct_bridge_client_host_event(g_client, false, "handles fetch done");
        handles = [arr copy];
    });
    return handles;
}

/*
 * Serve object data for our handles. For other handles, call through.
 */
- (id)dataSourceForObjectHandle:(uint32_t)handle
                   responseCode:(uint16_t *)responseCode
                           size:(uint32_t *)size {
    if (handle < OCTOPUS_HANDLE_BASE || handle > OCTOPUS_HANDLE_MAX) return %orig;
    uint32_t inner = handle - OCTOPUS_HANDLE_BASE + 1;
    __block NSData *data = nil;
    dispatch_sync(g_queue, ^{
        if (!bridge_connect_locked()) return;
        oct_bridge_client_host_event(g_client, true, NULL);
        uint32_t session = 0xBEEF;
        oct_buf cmd; oct_buf_init(&cmd, 0);
        oct_container_command(&cmd, 0x1002, 0, &session, 1); /* OpenSession */
        oct_bridge_client_send_bulk(g_client, cmd.data, cmd.length);
        oct_buf_free(&cmd);
        uint8_t type = 0; uint8_t *rx = NULL; size_t rxlen = 0;
        if (oct_bridge_client_receive(g_client, &type, &rx, &rxlen, 1000) != OCT_OK) {
            free(rx); bridge_disconnect_locked("open session for data failed"); return;
        }
        free(rx);
        oct_buf_init(&cmd, 0);
        oct_container_command(&cmd, 0x1009, 1, &inner, 1); /* GetObject */
        oct_bridge_client_send_bulk(g_client, cmd.data, cmd.length);
        oct_buf_free(&cmd);
        if (oct_bridge_client_receive(g_client, &type, &rx, &rxlen, 5000) != OCT_OK || rxlen < 12) {
            free(rx); bridge_disconnect_locked("get object data failed"); return;
        }
        if (type == OCT_BRIDGE_MSG_BULK_TO_HOST) {
            data = [NSData dataWithBytes:rx + 12 length:rxlen - 12];
        }
        free(rx);
        if (oct_bridge_client_receive(g_client, &type, &rx, &rxlen, 500) == OCT_OK) free(rx);
        oct_bridge_client_host_event(g_client, false, "data fetch done");
    });
    if (!data) {
        if (responseCode) *responseCode = 0x2009; /* Invalid object handle */
        if (size) *size = 0;
        return nil;
    }
    if (responseCode) *responseCode = 0x2001; /* OK */
    if (size) *size = (uint32_t)[data length];
    return [NSInputStream inputStreamWithData:data];
}

%end /* PTPVirtualCamera */

%hook PTPResponder

- (void)transportActivated {
    %orig;
    bridge_host_event(true, NULL);
    oct_logf(OCT_LOG_INFO, BRIDGE_COMPONENT, "transport_activated",
             "PTPResponder transportActivated: Windows connected");
}

- (void)transportDeactivated {
    bridge_host_event(false, "Windows disconnected");
    oct_logf(OCT_LOG_INFO, BRIDGE_COMPONENT, "transport_deactivated",
             "PTPResponder transportDeactivated: Windows disconnected");
    %orig;
}

%end /* PTPResponder */

#else /* HOOKS_ENABLED == 0 */

/*
 * Observation-only hooks: log what ptpd does without modifying any behaviour.
 * These confirm our identity check works, the socket connects, and nothing crashes.
 */
%hook PTPResponder

- (void)transportActivated {
    %orig;
    oct_logf(OCT_LOG_INFO, BRIDGE_COMPONENT, "transport_activated",
             "PTPResponder transportActivated (observation only; HOOKS_ENABLED=0)");
    bridge_host_event(true, NULL);
}

- (void)transportDeactivated {
    oct_logf(OCT_LOG_INFO, BRIDGE_COMPONENT, "transport_deactivated",
             "PTPResponder transportDeactivated (observation only; HOOKS_ENABLED=0)");
    bridge_host_event(false, "Windows disconnected");
    %orig;
}

%end /* PTPResponder */

#endif /* HOOKS_ENABLED */

/* ---- Constructor -------------------------------------------------------------------- */

%ctor {
    NSString *uuid = running_ptpd_uuid();
    if (!uuid) {
        NSLog(@"[OctopusBridge] Cannot read the running binary UUID; not installing hooks.");
        return;
    }
    if (![uuid isEqualToString:@(PTPD_REFERENCE_UUID)]) {
        NSLog(@"[OctopusBridge] ptpd UUID %@ does not match the analysed reference %s; "
              "not installing hooks to protect Photos.", uuid, PTPD_REFERENCE_UUID);
        return;
    }
    g_identity_ok = true;
    g_hooks_enabled = HOOKS_ENABLED;
    g_queue = dispatch_queue_create("com.parv.octopus.bridge", DISPATCH_QUEUE_SERIAL);
    %init;
    oct_logf(OCT_LOG_INFO, BRIDGE_COMPONENT, "loaded",
             "OctopusBridge loaded (UUID confirmed, HOOKS_ENABLED=%d, arm64e/iOS17/Dopamine). "
             "Stage 2 complete: identity verified. Stage 3: connect cable and inspect logs.",
             HOOKS_ENABLED);
    /* Connect eagerly so the socket state is visible in diagnostics. */
    dispatch_async(g_queue, ^{
        if (bridge_connect_locked()) {
            char info[128] = "";
            oct_bridge_client_ping(g_client, info, sizeof info, 500);
            oct_logf(OCT_LOG_INFO, BRIDGE_COMPONENT, "startup_ping",
                     "Startup PING to app socket: %s", info[0] ? info : "(no PONG; app may not be running)");
        }
    });
}

#pragma clang diagnostic pop
