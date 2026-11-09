/*
 * OctopusBridge.m — rootless Logos tweak (arm64e, Dopamine, iOS 17) loaded into ptpd.
 *
 * WHAT THIS BUILD DOES
 *   It observes. Every hook calls Apple's implementation first, records what went in and what
 *   came back, and returns Apple's result unchanged. No storage is registered, no object is
 *   substituted, no response is altered. ptpd behaves exactly as it would without the tweak.
 *
 * WHY IT'S BUILT THIS WAY
 *   An earlier draft planned to learn the storage interface by registering a stub storage object
 *   and logging the selectors ptpd sent to it. That was incoherent: the stub was only registered
 *   when the storage hooks were switched on, so with them off the stub never existed and no
 *   selector was ever observed. It was also unsafe, because absorbing unknown selectors means
 *   inventing a return value for a signature you don't know.
 *
 *   This build gets the same information without any of that. ptpd hands its real storage object
 *   to -addStorage: and returns it from -storageWithID:. Hooking those gives a pointer to the
 *   genuine PTPStorage, and the Objective-C runtime reports its class, its full method list with
 *   type encodings, and its ivar layout. That is the authoritative interface, read straight out of
 *   Apple's own object. The same is done for the object class that -objectForObjectHandle:
 *   returns (PTPFile in the decompilation).
 *
 * WHAT IT CANNOT TELL US
 *   A method list says which messages a storage answers, not which ones ptpd sends during a
 *   Windows transfer, nor in what order, nor what Windows does with the results. The call log
 *   covers the first; the second and third need a Windows host. Knowing the interface does not
 *   imply File Explorer will accept a second storage: Windows may ignore it, show it empty, or
 *   refuse the device. That stays an open question until it is tried.
 *
 * SAFETY
 *   - Nothing is hooked unless the running ptpd's UUID matches the analysed reference build.
 *   - Every hooked method is one whose class is confirmed in docs/usb-research/ evidence.
 *   - Hooks only read: they call %orig and return its result untouched.
 *   - Introspection is bounded and runs once per class.
 *   - No method signatures are fabricated and no invalid call is masked; there is no
 *     forwardInvocation:, and no object of ours is ever given to ptpd.
 *   - The log is this tweak's own file, written by ptpd's own uid; see LOG below.
 *
 * LOG
 *   /var/mobile/Library/Caches/Octopus/octopus-bridge-tweak.log, rotated at 256 KB, 2 kept.
 *   Export Diagnostics copies it into the ZIP under logs/. To read it directly:
 *     ssh mobile@<iphone> 'cat /var/mobile/Library/Caches/Octopus/octopus-bridge-tweak.log'
 *   If ptpd cannot write there, the lines stay in memory and the file is absent; the
 *   bridge.tweak_log diagnostic reports which happened.
 *
 * RECOVERY — if ptpd misbehaves, see docs/usb-research/runtime-validation.md. Briefly:
 *   killall -9 ptpd           (launchd restarts it clean)
 *   Uninstall OctopusBridge in Sileo, or delete the dylib and plist under
 *   /var/jb/usr/lib/TweakInject/, then: killall -9 ptpd
 *   Last resort: reboot and do not re-jailbreak. Rootless tweaks load only when jailbroken,
 *   so an unjailbroken boot runs stock ptpd with nothing injected.
 */

#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../Core/oct_bridge.h"
#include "../Core/oct_log.h"

#import <Foundation/Foundation.h>
#import <objc/runtime.h>

/* ---- Configuration ------------------------------------------------------------------ */

#define BRIDGE_COMPONENT    "bridge-tweak"
#define PTPD_REFERENCE_UUID "FFD54FC7-0508-3AB8-8D17-2F698B611E10"
#define TWEAK_LOG_PATH      "/var/mobile/Library/Caches/Octopus/octopus-bridge-tweak.log"
#define TWEAK_LOG_BYTES     (256u * 1024u)
#define TWEAK_LOG_KEEP      2u
#define MAX_METHODS_LOGGED  256   /* a class with more is reported as truncated */

static dispatch_queue_t g_queue;    /* serialises the socket and the introspection bookkeeping */
static oct_bridge_client *g_client; /* touched only on g_queue */

/* ---- Identity ----------------------------------------------------------------------- */

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

/* Tells the app a host attached or detached, so the app's own state reflects the cable. */
static void bridge_host_event(bool attached, const char *reason)
{
    dispatch_async(g_queue, ^{
        if (!bridge_connect()) return;
        if (oct_bridge_client_host_event(g_client, attached, reason) != OCT_OK) {
            bridge_disconnect("the host event couldn't be written");
        }
    });
}

/* ---- Read-only runtime introspection -------------------------------------------------- */

/*
 * Writes one class's interface to the log: its name, its superclass chain, how many instance
 * methods it declares, and each selector with its type encoding. Reads the Objective-C runtime
 * only; it sends no message to the class or to any instance, so it cannot change ptpd's state.
 * Each class is reported once per launch.
 */
static void describe_class_once(Class cls, const char *role)
{
    if (!cls) return;
    static NSMutableSet *seen;   /* guarded by g_queue */
    static dispatch_once_t once;
    dispatch_once(&once, ^{ seen = [NSMutableSet set]; });

    const char *name = class_getName(cls);
    NSString *key = [NSString stringWithFormat:@"%s/%s", role, name];
    __block bool first = false;
    dispatch_sync(g_queue, ^{
        if (![seen containsObject:key]) {
            [seen addObject:key];
            first = true;
        }
    });
    if (!first) return;

    NSMutableString *chain = [NSMutableString stringWithUTF8String:name];
    for (Class parent = class_getSuperclass(cls); parent; parent = class_getSuperclass(parent)) {
        [chain appendFormat:@" : %s", class_getName(parent)];
    }
    unsigned count = 0;
    Method *methods = class_copyMethodList(cls, &count);
    oct_logf(OCT_LOG_INFO, BRIDGE_COMPONENT, "class",
             "%s is %.200s, %u instance methods", role, chain.UTF8String, count);
    unsigned logged = count < MAX_METHODS_LOGGED ? count : MAX_METHODS_LOGGED;
    for (unsigned i = 0; i < logged; i++) {
        const char *encoding = method_getTypeEncoding(methods[i]);
        oct_logf(OCT_LOG_INFO, BRIDGE_COMPONENT, "method",
                 "%s -[%s %.120s] %.60s", role, name,
                 sel_getName(method_getName(methods[i])), encoding ? encoding : "?");
    }
    if (logged < count) {
        oct_logf(OCT_LOG_WARN, BRIDGE_COMPONENT, "method_truncated",
                 "%s declares %u methods; only the first %u were logged", role, count, logged);
    }
    free(methods);

    unsigned ivar_count = 0;
    Ivar *ivars = class_copyIvarList(cls, &ivar_count);
    for (unsigned i = 0; i < ivar_count && i < MAX_METHODS_LOGGED; i++) {
        const char *type = ivar_getTypeEncoding(ivars[i]);
        oct_logf(OCT_LOG_DEBUG, BRIDGE_COMPONENT, "ivar",
                 "%s %s.%.80s offset %ld type %.40s", role, name, ivar_getName(ivars[i]),
                 (long)ivar_getOffset(ivars[i]), type ? type : "?");
    }
    free(ivars);
}

/* Describes an object's class. Sends it no message. */
static void describe_object_once(id object, const char *role)
{
    if (object) describe_class_once(object_getClass(object), role);
}

/* ---- Hooks: every one calls through and returns Apple's own result ------------------- */

%hook PTPVirtualCamera

/*
 * ptpd adds its own storage here. Logging the argument's class is how the real PTPStorage
 * interface is recovered: this is Apple's object, not one of ours.
 */
- (void)addStorage:(id)storage
{
    describe_object_once(storage, "storage");
    oct_logf(OCT_LOG_INFO, BRIDGE_COMPONENT, "add_storage",
             "ptpd added a storage of class %s", storage ? class_getName(object_getClass(storage)) : "nil");
    %orig;
}

- (id)storageWithID:(uint32_t)storageID
{
    id storage = %orig;
    describe_object_once(storage, "storage");
    oct_logf(OCT_LOG_INFO, BRIDGE_COMPONENT, "storage_with_id",
             "storageWithID:0x%08X returned %s", storageID,
             storage ? class_getName(object_getClass(storage)) : "nil");
    return storage;
}

- (id)allStorageIDs
{
    id ids = %orig;
    oct_logf(OCT_LOG_INFO, BRIDGE_COMPONENT, "all_storage_ids", "allStorageIDs: %.200s",
             ids ? [[ids description] UTF8String] : "nil");
    return ids;
}

/* The dataset Windows receives for GetStorageInfo. Logging its length shows the real format. */
- (id)storageInfoDatasetForStorageID:(uint32_t)storageID
{
    id dataset = %orig;
    NSUInteger length = [dataset isKindOfClass:NSData.class] ? [(NSData *)dataset length] : 0;
    oct_logf(OCT_LOG_INFO, BRIDGE_COMPONENT, "storage_info",
             "storageInfoDatasetForStorageID:0x%08X returned %s of %lu bytes", storageID,
             dataset ? class_getName(object_getClass(dataset)) : "nil", (unsigned long)length);
    return dataset;
}

/* What Windows asks for when it enumerates a folder. */
- (id)objectHandlesInStorage:(uint32_t)storageID
         forObjectFormatCode:(uint16_t)formatCode
               inAssociation:(uint32_t)association
                responseCode:(uint16_t *)responseCode
{
    id handles = %orig;
    NSUInteger count = [handles respondsToSelector:@selector(count)] ? [handles count] : 0;
    oct_logf(OCT_LOG_INFO, BRIDGE_COMPONENT, "object_handles",
             "objectHandles storage=0x%08X format=0x%04X assoc=0x%08X -> %lu handles, rc=0x%04X",
             storageID, formatCode, association, (unsigned long)count,
             responseCode ? *responseCode : 0);
    return handles;
}

/* The object class behind a handle: PTPFile in the decompilation. */
- (id)objectForObjectHandle:(id)handle
{
    id object = %orig;
    describe_object_once(object, "object");
    return object;
}

- (id)objectInfoDatasetForObjectHandle:(uint32_t)handle
{
    id dataset = %orig;
    NSUInteger length = [dataset isKindOfClass:NSData.class] ? [(NSData *)dataset length] : 0;
    oct_logf(OCT_LOG_INFO, BRIDGE_COMPONENT, "object_info",
             "objectInfoDatasetForObjectHandle:0x%08X returned %lu bytes", handle,
             (unsigned long)length);
    return dataset;
}

/* The stream Windows reads a file from. */
- (id)dataSourceForObjectHandle:(uint32_t)handle
                   responseCode:(uint16_t *)responseCode
                           size:(uint32_t *)size
{
    id source = %orig;
    describe_object_once(source, "data_source");
    oct_logf(OCT_LOG_INFO, BRIDGE_COMPONENT, "data_source",
             "dataSourceForObjectHandle:0x%08X -> %s, size=%u, rc=0x%04X", handle,
             source ? class_getName(object_getClass(source)) : "nil",
             size ? *size : 0, responseCode ? *responseCode : 0);
    return source;
}

%end

%hook PTPResponder

/* Windows opened the PTP interface. */
- (void)transportActivated
{
    %orig;
    oct_logf(OCT_LOG_INFO, BRIDGE_COMPONENT, "transport_activated",
             "ptpd reports a host on the PTP interface");
    bridge_host_event(true, NULL);
}

/* Windows went away. */
- (void)transportDeactivated
{
    oct_logf(OCT_LOG_INFO, BRIDGE_COMPONENT, "transport_deactivated", "ptpd reports the host has gone");
    bridge_host_event(false, "the host disconnected");
    %orig;
}

/* Every PTP operation Windows sends, in order: the sequence a real transfer uses. */
- (void)handleRequest:(id)request
{
    oct_logf(OCT_LOG_INFO, BRIDGE_COMPONENT, "request", "ptpd handling %.200s",
             request ? [[request description] UTF8String] : "nil");
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
        NSLog(@"[OctopusBridge] ptpd %@ isn't the analysed build %s; no hooks installed, so "
               "Photos is untouched.", uuid, PTPD_REFERENCE_UUID);
        return;
    }
    g_queue = dispatch_queue_create("com.parv.octopus.bridge", DISPATCH_QUEUE_SERIAL);

    /* This tweak's own log file, separate from the app's, since a different process writes it. */
    oct_log_config log = { TWEAK_LOG_PATH, TWEAK_LOG_BYTES, TWEAK_LOG_KEEP, OCT_LOG_DEBUG };
    oct_status opened = oct_log_open(&log);

    %init;

    oct_logf(OCT_LOG_INFO, BRIDGE_COMPONENT, "loaded",
             "OctopusBridge loaded into ptpd %s, observation only: hooks read and call through, "
             "nothing is registered or altered", PTPD_REFERENCE_UUID);
    if (opened != OCT_OK) {
        NSLog(@"[OctopusBridge] Can't write %s (%s); lines stay in memory only.",
              TWEAK_LOG_PATH, oct_status_name(opened));
    }
    dispatch_async(g_queue, ^{
        if (bridge_connect()) {
            char answered[OCT_EVENT_TEXT_MAX] = "";
            oct_status status = oct_bridge_client_ping(g_client, answered, sizeof answered, 500);
            oct_logf(status == OCT_OK ? OCT_LOG_INFO : OCT_LOG_WARN, BRIDGE_COMPONENT, "startup_ping",
                     "Ping to the app: %s%s%.80s", oct_status_name(status),
                     answered[0] ? ", socket " : "", answered);
        }
        oct_log_flush();
    });
}
