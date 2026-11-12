#include "oct_device.h"
#include <stdio.h>
#include <string.h>
static uint32_t word(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

oct_status oct_device_parse_identity(const uint8_t *bytes, size_t length, oct_macho_identity *out)
{
    if (!out) return OCT_E_INVALID_ARGUMENT;
    memset(out, 0, sizeof *out);
    if (!bytes) return OCT_E_INVALID_ARGUMENT;
    if (length < 32) return OCT_E_MALFORMED;
    if (word(bytes) != 0xfeedfacfu) return OCT_E_UNSUPPORTED;
    uint32_t count = word(bytes + 16), size = word(bytes + 20);
    if (size > length - 32 || count > size / 8) return OCT_E_MALFORMED;
    oct_macho_identity result = {0};
    result.cpu_type = word(bytes + 4);
    result.cpu_subtype = word(bytes + 8);
    result.file_type = word(bytes + 12);
    size_t pos = 32, end = 32 + (size_t)size;
    bool uuid = false, build = false;
    for (uint32_t i = 0; i < count; ++i) {
        if (end - pos < 8) return OCT_E_MALFORMED;
        uint32_t cmd = word(bytes + pos), len = word(bytes + pos + 4);
        if (len < 8 || len % 8 || len > end - pos) return OCT_E_MALFORMED;
        if (cmd == 0x1bu) {
            if (uuid || len != 24) return OCT_E_MALFORMED;
            uuid = true;
            const uint8_t *u = bytes + pos + 8;
            snprintf(result.uuid, sizeof result.uuid,
                     "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                     (unsigned)u[0], (unsigned)u[1], (unsigned)u[2], (unsigned)u[3],
                     (unsigned)u[4], (unsigned)u[5], (unsigned)u[6], (unsigned)u[7],
                     (unsigned)u[8], (unsigned)u[9], (unsigned)u[10], (unsigned)u[11],
                     (unsigned)u[12], (unsigned)u[13], (unsigned)u[14], (unsigned)u[15]);
        } else if (cmd == 0x32u) {
            if (build || len < 24 || word(bytes + pos + 20) != (len - 24) / 8) return OCT_E_MALFORMED;
            build = true;
            result.platform = word(bytes + pos + 8);
            result.minimum_os = word(bytes + pos + 12);
            result.sdk = word(bytes + pos + 16);
        }
        pos += len;
    }
    if (pos != end || !uuid || !build) return OCT_E_MALFORMED;
    *out = result;
    return OCT_OK;
}
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#if defined(__APPLE__) && TARGET_OS_IOS && !TARGET_OS_SIMULATOR
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <mach/mach.h>
/* Generic IOKitLib signatures, from apple-oss-distributions/IOKitUser/IOKitLib.h.
 * iOS's public SDK omits these declarations. Resolve only read-only entrypoints;
 * never guess a device-family user-client selector or a plug-in vtable. */
typedef CFMutableDictionaryRef (*matching_fn)(const char *);
typedef kern_return_t (*services_fn)(mach_port_t, CFDictionaryRef, mach_port_t *);
typedef mach_port_t (*next_fn)(mach_port_t);
typedef kern_return_t (*release_fn)(mach_port_t);
#endif

oct_status oct_device_probe_ptp(oct_device_probe *out)
{
    if (!out) return OCT_E_INVALID_ARGUMENT;
    memset(out, 0, sizeof *out);
#if defined(__APPLE__) && TARGET_OS_IOS && !TARGET_OS_SIMULATOR
    void *library = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_LOCAL | RTLD_LAZY);
    if (!library) {
        snprintf(out->detail, sizeof out->detail, "IOKit unavailable: %s", dlerror());
        return OCT_E_UNSUPPORTED;
    }
    matching_fn matching = (matching_fn)dlsym(library, "IOServiceMatching");
    services_fn services = (services_fn)dlsym(library, "IOServiceGetMatchingServices");
    next_fn next = (next_fn)dlsym(library, "IOIteratorNext");
    release_fn release = (release_fn)dlsym(library, "IOObjectRelease");
    oct_status status = OCT_E_UNSUPPORTED;
    if (!matching || !services || !next || !release) {
        snprintf(out->detail, sizeof out->detail, "Required read-only IOKit symbols unavailable");
        goto done;
    }
    CFMutableDictionaryRef match = matching("IOUSBDeviceInterface");
    const void *keys[] = { CFSTR("USBDeviceFunction") };
    const void *values[] = { CFSTR("PTP") };
    CFDictionaryRef properties = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 1,
                                                   &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (!match || !properties) {
        if (match) CFRelease(match);
        if (properties) CFRelease(properties);
        snprintf(out->detail, sizeof out->detail, "Cannot allocate PTP matching dictionary");
        status = OCT_E_NO_MEMORY;
        goto done;
    }
    CFDictionarySetValue(match, CFSTR("IOPropertyMatch"), properties);
    CFRelease(properties);
    mach_port_t iterator = MACH_PORT_NULL;
    /* Consumes match even on failure. */
    kern_return_t kr = services(MACH_PORT_NULL, match, &iterator);
    out->system_error = kr;
    if (kr != KERN_SUCCESS) {
        snprintf(out->detail, sizeof out->detail, "PTP registry query failed: kern_return_t=0x%08x", (unsigned)kr);
        status = OCT_E_IO;
    } else {
        mach_port_t entry;
        while ((entry = next(iterator)) != MACH_PORT_NULL) {
            release(entry);
            if (out->interfaces == 64) { out->truncated = true; break; }
            out->interfaces++;
        }
        snprintf(out->detail, sizeof out->detail,
                 "%u PTP IOUSBDeviceInterface matches%s. Discovery only; ownership, host attachment and USB I/O unverified.",
                 out->interfaces, out->truncated ? " (limit reached)" : "");
        status = OCT_OK;
    }
    if (iterator != MACH_PORT_NULL) release(iterator);
done:
    dlclose(library);
    return status;
#else
    snprintf(out->detail, sizeof out->detail, "PTP registry discovery requires a physical iOS device");
    return OCT_E_UNSUPPORTED;
#endif
}

/* ---- Selecting one loaded image by path ------------------------------------------------- */

const char *oct_image_outcome_name(oct_image_outcome outcome)
{
    switch (outcome) {
    case OCT_IMAGE_FOUND: return "found";
    case OCT_IMAGE_NO_ENUMERATION: return "no-enumeration";
    case OCT_IMAGE_NOT_PRESENT: return "not-present";
    case OCT_IMAGE_AMBIGUOUS: return "ambiguous";
    case OCT_IMAGE_NO_HEADER: return "no-header";
    case OCT_IMAGE_NOT_MACHO: return "not-mach-o";
    case OCT_IMAGE_HEADER_TOO_LARGE: return "header-too-large";
    case OCT_IMAGE_MALFORMED: return "malformed";
    case OCT_IMAGE_NOT_EXECUTABLE: return "not-executable";
    }
    return "unknown";
}

oct_image_outcome oct_device_select_image(const oct_image_source *source, const char *path,
                                          uint32_t *index, oct_macho_identity *identity)
{
    if (index) *index = UINT32_MAX;
    if (identity) memset(identity, 0, sizeof *identity);
    if (!source || !source->count || !source->name || !source->header || !path || !*path) {
        return OCT_IMAGE_NO_ENUMERATION;
    }
    uint32_t total = source->count(source->context);
    if (!total) return OCT_IMAGE_NO_ENUMERATION;

    /* Scan the whole list rather than stopping at the first match: two images claiming the same
     * path is a situation to report, not to resolve by picking one. */
    uint32_t found = UINT32_MAX, matches = 0;
    for (uint32_t i = 0; i < total; i++) {
        const char *name = source->name(source->context, i);
        if (name && strcmp(name, path) == 0) {
            matches++;
            if (found == UINT32_MAX) found = i;
        }
    }
    if (!matches) return OCT_IMAGE_NOT_PRESENT;
    if (index) *index = found;
    if (matches > 1) return OCT_IMAGE_AMBIGUOUS;

    const uint8_t *header = (const uint8_t *)source->header(source->context, found);
    if (!header) return OCT_IMAGE_NO_HEADER;
    /* Read the fixed 32-byte header first; only it is known to be present. */
    if (word(header) != 0xfeedfacfu) return OCT_IMAGE_NOT_MACHO;
    uint32_t sizeofcmds = word(header + 20);
    if (sizeofcmds > OCT_IMAGE_MAX_HEADER_BYTES) return OCT_IMAGE_HEADER_TOO_LARGE;
    oct_macho_identity parsed;
    if (oct_device_parse_identity(header, 32 + (size_t)sizeofcmds, &parsed) != OCT_OK) {
        /* Nothing trustworthy was produced, but the caller still learns it was this image. */
        return OCT_IMAGE_MALFORMED;
    }
    if (identity) *identity = parsed;
    /* An injected dylib at the executable's path would be MH_DYLIB; only MH_EXECUTE is the
     * process's own program. */
    if (parsed.file_type != 2) return OCT_IMAGE_NOT_EXECUTABLE;
    return OCT_IMAGE_FOUND;
}
