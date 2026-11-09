/*
 * OctopusBridge.m — rootless Logos tweak (arm64e, Dopamine, iOS 17) loaded into ptpd.
 *
 * STATUS: Stage 2. Identity verification and socket connection are implemented and active.
 *         The PTP storage hooks are written but gated OFF at run time (see g_hooks_enabled):
 *         with the gate closed every hook calls straight through to Apple's implementation,
 *         so Photos is untouched. Stage 3 (runtime observation) opens the gate.
 *
 * WHY THE GATE IS A RUNTIME FLAG, NOT #if
 *   Logos rewrites %hook/%orig/%end lexically, before the C preprocessor runs. Putting two
 *   %hook blocks for the same class in #if/#else branches makes Logos emit both groups and
 *   the build fails on duplicate symbols. So there is exactly one %hook group per class here,
 *   and the gate is checked inside each hook.
 *
 * HOW TO DEPLOY
 *   1. Install Octopus.tipa and open the app; tap Enable USB Access.
 *   2. Confirm Settings → Diagnostics → Run Full Diagnostics shows bridge.app_socket PASS.
 *   3. make package THEOS=/var/jb/opt/theos, then install the .deb with Sileo.
 *   4. Connect the iPhone to Windows over USB and trust the PC.
 *   5. Export Diagnostics and read the bridge-tweak log lines.
 *
 * THREADING
 *   ptpd calls these hooks from its own threads. Every socket operation is serialised on one
 *   private queue, because the bridge wire protocol requires whole frames and one in-flight
 *   request at a time. Hooks never run on that queue themselves, so dispatch_sync is safe.
 *
 * SAFETY
 *   - The ptpd UUID must match the analysed reference build or no hook is installed at all.
 *   - Every hooked method is one whose class is confirmed in docs/usb-research/ evidence.
 *   - Any selector ptpd sends to OctopusPTPStorage that isn't implemented is absorbed by
 *     forwardInvocation: and logged, rather than raising inside ptpd.
 *   - If the socket is unavailable, every hook falls through to %orig.
 */

#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "../Core/oct_bridge.h"
#include "../Core/oct_log.h"
#include "../Protocol/oct_ptp.h"

#import <Foundation/Foundation.h>
#import <objc/runtime.h>

/* ---- Configuration ------------------------------------------------------------------ */

/* The storage and handle range Octopus claims inside ptpd's tables. */
#define OCTOPUS_STORAGE_ID   0x00020001u
#define OCTOPUS_HANDLE_BASE  0x80000001u
#define OCTOPUS_HANDLE_MAX   0x8FFFFFFFu

#define BRIDGE_COMPONENT     "bridge-tweak"
#define PTPD_REFERENCE_UUID  "FFD54FC7-0508-3AB8-8D17-2F698B611E10"

/* PTP operation and response codes used over the bridge. */
#define PTP_OP_OPEN_SESSION   0x1002u
#define PTP_OP_CLOSE_SESSION  0x1003u
#define PTP_OP_GET_HANDLES    0x1007u
#define PTP_OP_GET_OBJECT     0x1009u
#define PTP_RC_OK             0x2001u
#define PTP_RC_INVALID_HANDLE 0x2009u

/*
 * The storage hooks are off until Stage 3 observation is done. Build with
 * -DOCTOPUS_BRIDGE_HOOKS=1 to open the gate. Read, never written after %ctor.
 */
#ifndef OCTOPUS_BRIDGE_HOOKS
#define OCTOPUS_BRIDGE_HOOKS 0
#endif

static bool g_hooks_enabled;        /* OCTOPUS_BRIDGE_HOOKS, set once in %ctor */
static dispatch_queue_t g_queue;    /* serialises every socket operation */
static oct_bridge_client *g_client; /* touched only on g_queue */

/* ---- Identity verification ---------------------------------------------------------- */

/* The LC_UUID of the loaded ptpd image, or nil when it can't be read. */
static NSString *running_ptpd_uuid(void)
{
    uint32_t images = _dyld_image_count();
    for (uint32_t i = 0; i < images; i++) {
        const char *name = _dyld_get_image_name(i);
        if (!name || !strstr(name, "ptpd")) continue;
        const struct mach_header *header = _dyld_get_image_header(i);
        if (!header) continue;
        bool is64 = header->magic == MH_MAGIC_64 || header->magic == MH_CIGAM_64;
        const uint8_t *commands = (const uint8_t *)header +
                                  (is64 ? sizeof(struct mach_header_64) : sizeof(struct mach_header));
        for (uint32_t command = 0; command < header->ncmds; command++) {
            const struct load_command *lc = (const struct load_command *)commands;
            if (lc->cmd == LC_UUID) {
                const uint8_t *u = ((const struct uuid_command *)lc)->uuid;
                return [NSString stringWithFormat:
                        @"%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                        u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7],
                        u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]];
            }
            commands += lc->cmdsize;
        }
    }
    return nil;
}

/* ---- The bridge connection ---------------------------------------------------------- */

/* Connects if needed. On g_queue only. */
static bool bridge_connect(void)
{
    if (g_client) return true;
    oct_bridge_client *client = NULL;
    oct_status status = oct_bridge_client_connect(OCT_BRIDGE_DEFAULT_SOCKET, 500, &client);
    if (status != OCT_OK) {
        oct_logf(OCT_LOG_WARN, BRIDGE_COMPONENT, "connect_failed",
                 "Can't reach %s: %s. Open Octopus and tap Enable USB Access.",
                 OCT_BRIDGE_DEFAULT_SOCKET, oct_status_name(status));
        return false;
    }
    g_client = client;
    oct_logf(OCT_LOG_INFO, BRIDGE_COMPONENT, "connected", "Connected to the Octopus bridge socket");
    return true;
}

/* On g_queue only. */
static void bridge_disconnect(const char *reason)
{
    if (!g_client) return;
    oct_bridge_client_destroy(g_client);
    g_client = NULL;
    oct_logf(OCT_LOG_INFO, BRIDGE_COMPONENT, "disconnected", "Bridge connection dropped: %.120s", reason);
}

/* Tells the app a host attached or detached. Called from the hooks, off g_queue. */
static void bridge_host_event(bool attached, const char *reason)
{
    dispatch_sync(g_queue, ^{
        if (!bridge_connect()) return;
        if (oct_bridge_client_host_event(g_client, attached, reason) != OCT_OK) {
            bridge_disconnect("the host event couldn't be written");
        }
    });
}

/* Reads one little-endian field from a PTP container. */
static uint16_t container_u16(const uint8_t *bytes, size_t offset)
{
    return (uint16_t)(bytes[offset] | (bytes[offset + 1] << 8));
}

static uint32_t container_u32(const uint8_t *bytes, size_t offset)
{
    return (uint32_t)container_u16(bytes, offset) | ((uint32_t)container_u16(bytes, offset + 2) << 16);
}

/* Sends one PTP command. On g_queue only; returns false and drops the connection on failure. */
static bool bridge_command(uint16_t operation, uint32_t transaction, const uint32_t *params, uint32_t count)
{
    oct_buf command;
    oct_buf_init(&command, 0);
    oct_container_command(&command, operation, transaction, params, count);
    bool ok = !command.failed &&
              oct_bridge_client_send_bulk(g_client, command.data, command.length) == OCT_OK;
    oct_buf_free(&command);
    if (!ok) bridge_disconnect("a command couldn't be written");
    return ok;
}

/*
 * Reads the next container from the app. On g_queue only. `out_payload` receives the bytes after
 * the 12-byte header (caller frees), or NULL when the caller passes NULL. Returns the container's
 * code, or 0 when nothing usable arrived.
 */
static uint16_t bridge_container(uint32_t timeout_ms, uint8_t **out_payload, size_t *out_length)
{
    if (out_payload) {
        *out_payload = NULL;
        *out_length = 0;
    }
    uint8_t type = 0;
    uint8_t *frame = NULL;
    size_t length = 0;
    if (oct_bridge_client_receive(g_client, &type, &frame, &length, timeout_ms) != OCT_OK) {
        free(frame);
        bridge_disconnect("no reply from the app");
        return 0;
    }
    uint16_t code = 0;
    if (type == OCT_BRIDGE_MSG_BULK_TO_HOST && length >= 12) {
        code = container_u16(frame, 6);
        if (out_payload && length > 12) {
            uint8_t *payload = malloc(length - 12);
            if (payload) {
                memcpy(payload, frame + 12, length - 12);
                *out_payload = payload;
                *out_length = length - 12;
            }
        }
    }
    free(frame);
    return code;
}

/* Opens a PTP session over the bridge. On g_queue only. */
static bool bridge_open_session(uint32_t session)
{
    if (!bridge_connect()) return false;
    if (oct_bridge_client_host_event(g_client, true, NULL) != OCT_OK) {
        bridge_disconnect("the host event couldn't be written");
        return false;
    }
    if (!bridge_command(PTP_OP_OPEN_SESSION, 0, &session, 1)) return false;
    return bridge_container(1000, NULL, NULL) == PTP_RC_OK;
}

/* Closes the session opened above. On g_queue only. */
static void bridge_close_session(uint32_t transaction)
{
    if (!g_client) return;
    if (bridge_command(PTP_OP_CLOSE_SESSION, transaction, NULL, 0)) {
        (void)bridge_container(1000, NULL, NULL);
    }
    if (g_client) oct_bridge_client_host_event(g_client, false, "the request finished");
}

/* ---- OctopusPTPStorage -------------------------------------------------------------- */

/*
 * The object that occupies a slot in ptpd's storages dictionary. ptpd's own routing calls the
 * camera's methods (hooked below), not this object's, for the operations we serve; this class
 * exists to hold the storage ID and to record anything else ptpd asks of a storage object.
 */
@interface OctopusPTPStorage : NSObject
@end

@implementation OctopusPTPStorage

- (uint32_t)storageID { return OCTOPUS_STORAGE_ID; }
- (uint32_t)sessionID { return 0; }
- (BOOL)isPhotoStorage { return NO; }

/* An unimplemented selector would raise inside ptpd and take Photos down with it. These two
 * methods absorb it and write the selector's name to the log: that list is the Stage 3 output. */
- (NSMethodSignature *)methodSignatureForSelector:(SEL)selector
{
    NSMethodSignature *signature = [super methodSignatureForSelector:selector];
    return signature ?: [NSMethodSignature signatureWithObjCTypes:"@@:"];
}

- (void)forwardInvocation:(NSInvocation *)invocation
{
    oct_logf(OCT_LOG_WARN, BRIDGE_COMPONENT, "unknown_storage_selector",
             "ptpd called -[PTPStorage %.80s], which OctopusPTPStorage doesn't implement; "
             "returned nil. Record this selector before enabling the hooks.",
             sel_getName(invocation.selector));
    if (invocation.methodSignature.methodReturnLength <= sizeof(void *)) {
        const void *empty = NULL;
        [invocation setReturnValue:(void *)&empty];
    }
}

@end

static OctopusPTPStorage *g_storage;

/* ---- Hooks -------------------------------------------------------------------------- */

/* ptpd's own classes. Declared for the messages sent outside %hook bodies. */
@interface PTPVirtualCamera : NSObject
- (void)addStorage:(id)storage;
@end

%hook PTPVirtualCamera

/* Register Octopus as a second storage once the camera exists. */
- (id)initWithName:(id)name andPath:(id)path
{
    id camera = %orig;
    if (!g_hooks_enabled || !camera || g_storage) return camera;
    g_storage = [[OctopusPTPStorage alloc] init];
    [camera addStorage:g_storage];
    oct_logf(OCT_LOG_INFO, BRIDGE_COMPONENT, "storage_added",
             "Added OctopusPTPStorage (ID 0x%08X) to PTPVirtualCamera", OCTOPUS_STORAGE_ID);
    return camera;
}

/* StorageInfo for Octopus's storage: fixed RAM, generic hierarchical, read-only. */
- (id)storageInfoDatasetForStorageID:(uint32_t)storageID
{
    if (!g_hooks_enabled || storageID != OCTOPUS_STORAGE_ID) return %orig;
    NSMutableData *dataset = [NSMutableData data];
    uint16_t header[3] = { 0x0003, 0x0002, 0x0001 }; /* type, filesystem, access */
    [dataset appendBytes:header length:sizeof header];
    uint64_t capacity = 4669, free_space = 0;
    [dataset appendBytes:&capacity length:sizeof capacity];
    [dataset appendBytes:&free_space length:sizeof free_space];
    uint32_t free_images = 0xFFFFFFFFu;
    [dataset appendBytes:&free_images length:sizeof free_images];
    /* PTP strings: a length in characters including the terminator, then UTF-16LE. */
    const char *strings[2] = { "Octopus", "octopus" };
    for (int i = 0; i < 2; i++) {
        size_t length = strlen(strings[i]);
        uint8_t characters = (uint8_t)(length + 1);
        [dataset appendBytes:&characters length:1];
        for (size_t c = 0; c < length; c++) {
            uint16_t unit = (uint16_t)strings[i][c];
            [dataset appendBytes:&unit length:sizeof unit];
        }
        uint16_t terminator = 0;
        [dataset appendBytes:&terminator length:sizeof terminator];
    }
    return dataset;
}

/* The object handles in Octopus's storage, read from the app over the bridge. */
- (id)objectHandlesInStorage:(uint32_t)storageID
         forObjectFormatCode:(uint16_t)formatCode
               inAssociation:(uint32_t)association
                responseCode:(uint16_t *)responseCode
{
    if (!g_hooks_enabled || storageID != OCTOPUS_STORAGE_ID) return %orig;
    (void)formatCode;
    __block NSArray *handles = nil;
    dispatch_sync(g_queue, ^{
        if (!bridge_open_session(0xC0DE)) return;
        uint32_t params[3] = { 0xFFFFFFFFu, 0, association };
        uint8_t *payload = NULL;
        size_t length = 0;
        if (bridge_command(PTP_OP_GET_HANDLES, 1, params, 3) &&
            bridge_container(2000, &payload, &length) == PTP_OP_GET_HANDLES && length >= 4) {
            uint32_t count = container_u32(payload, 0);
            NSMutableArray *found = [NSMutableArray array];
            for (uint32_t i = 0; i < count && 4 + 4 * (size_t)(i + 1) <= length; i++) {
                uint32_t handle = container_u32(payload, 4 + 4 * (size_t)i);
                [found addObject:@(handle + OCTOPUS_HANDLE_BASE - 1)];
            }
            (void)bridge_container(1000, NULL, NULL); /* the response phase */
            handles = found;
        }
        free(payload);
        bridge_close_session(2);
    });
    if (responseCode) *responseCode = PTP_RC_OK;
    return handles ?: @[];
}

/* The bytes of one Octopus object, as a stream ptpd can read. */
- (id)dataSourceForObjectHandle:(uint32_t)handle
                   responseCode:(uint16_t *)responseCode
                           size:(uint32_t *)size
{
    if (!g_hooks_enabled || handle < OCTOPUS_HANDLE_BASE || handle > OCTOPUS_HANDLE_MAX) return %orig;
    uint32_t object = handle - OCTOPUS_HANDLE_BASE + 1;
    __block NSData *contents = nil;
    dispatch_sync(g_queue, ^{
        if (!bridge_open_session(0xBEEF)) return;
        uint8_t *payload = NULL;
        size_t length = 0;
        if (bridge_command(PTP_OP_GET_OBJECT, 1, &object, 1) &&
            bridge_container(5000, &payload, &length) == PTP_OP_GET_OBJECT) {
            contents = length ? [NSData dataWithBytes:payload length:length] : [NSData data];
            (void)bridge_container(1000, NULL, NULL); /* the response phase */
        }
        free(payload);
        bridge_close_session(2);
    });
    if (!contents) {
        if (responseCode) *responseCode = PTP_RC_INVALID_HANDLE;
        if (size) *size = 0;
        return nil;
    }
    if (responseCode) *responseCode = PTP_RC_OK;
    if (size) *size = (uint32_t)contents.length;
    return [NSInputStream inputStreamWithData:contents];
}

%end

%hook PTPResponder

/* Windows opened the PTP interface. */
- (void)transportActivated
{
    %orig;
    oct_logf(OCT_LOG_INFO, BRIDGE_COMPONENT, "transport_activated",
             "ptpd reports a host on the PTP interface (hooks %s)",
             g_hooks_enabled ? "on" : "off, observing only");
    bridge_host_event(true, NULL);
}

/* Windows went away. */
- (void)transportDeactivated
{
    oct_logf(OCT_LOG_INFO, BRIDGE_COMPONENT, "transport_deactivated", "ptpd reports the host has gone");
    bridge_host_event(false, "the host disconnected");
    %orig;
}

%end

/* ---- Constructor -------------------------------------------------------------------- */

%ctor {
    NSString *uuid = running_ptpd_uuid();
    if (!uuid) {
        NSLog(@"[OctopusBridge] The host binary's UUID can't be read; no hooks installed.");
        return;
    }
    if (![uuid isEqualToString:@PTPD_REFERENCE_UUID]) {
        NSLog(@"[OctopusBridge] ptpd %@ isn't the analysed build %s; no hooks installed, "
               "so Photos is untouched.", uuid, PTPD_REFERENCE_UUID);
        return;
    }
    g_hooks_enabled = OCTOPUS_BRIDGE_HOOKS != 0;
    g_queue = dispatch_queue_create("com.parv.octopus.bridge", DISPATCH_QUEUE_SERIAL);
    %init;
    oct_logf(OCT_LOG_INFO, BRIDGE_COMPONENT, "loaded",
             "OctopusBridge loaded into ptpd %s; storage hooks are %s",
             PTPD_REFERENCE_UUID, g_hooks_enabled ? "on" : "off (observation only)");
    dispatch_async(g_queue, ^{
        if (!bridge_connect()) return;
        char answered[OCT_EVENT_TEXT_MAX] = "";
        oct_status status = oct_bridge_client_ping(g_client, answered, sizeof answered, 500);
        oct_logf(status == OCT_OK ? OCT_LOG_INFO : OCT_LOG_WARN, BRIDGE_COMPONENT, "startup_ping",
                 "Ping to the app: %s%s%s", oct_status_name(status), answered[0] ? ", socket " : "", answered);
    });
}
