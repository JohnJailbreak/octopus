/* Read-only device discovery. A match is not ownership or a host connection. */
#ifndef OCT_DEVICE_H
#define OCT_DEVICE_H
#include "oct_store.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct oct_device_probe {
    uint32_t interfaces;
    int32_t system_error;
    bool truncated;
    char detail[320];
} oct_device_probe;
/* No service open, plug-in creation, control transfer, or configuration mutation. */
oct_status oct_device_probe_ptp(oct_device_probe *out);
typedef struct oct_macho_identity {
    uint32_t cpu_type, cpu_subtype, file_type, platform, minimum_os, sdk;
    char uuid[37];
} oct_macho_identity;
/* Bounded thin little-endian Mach-O 64 header parser. No code is loaded. */
oct_status oct_device_parse_identity(const uint8_t *bytes, size_t length, oct_macho_identity *out);

/* ---- Selecting one loaded image by path -----------------------------------------------------
 *
 * The companion previously took the process's image at index 0 and assumed it was the main
 * executable. A device run on 9 November 2026 reached the identity check and rejected it, with a
 * binary that is byte-identical to the reference, so that assumption is the prime suspect:
 * ptpd's environment carries Dopamine's systemhook bootstrap and, under ElleKit, the companion
 * itself, and nothing guarantees the executable comes first.
 *
 * This selects an image by its exact path instead, and reports precisely why it could not. The
 * enumeration is behind a tiny interface so the real dyld functions are used on the device while
 * the tests drive the same code with any image list they like.
 *
 * The header pointer handed back by dyld points at mapped memory the caller does not own, so the
 * amount that may be read is derived from the header itself and capped; a header claiming more
 * than the cap is refused rather than trusted.
 */
#define OCT_IMAGE_MAX_HEADER_BYTES 65536u

typedef enum oct_image_outcome {
    OCT_IMAGE_FOUND = 0,
    OCT_IMAGE_NO_ENUMERATION,   /* the image list could not be read at all */
    OCT_IMAGE_NOT_PRESENT,      /* no loaded image has that path */
    OCT_IMAGE_AMBIGUOUS,        /* more than one image claims that path */
    OCT_IMAGE_NO_HEADER,        /* the image is listed but its header pointer is null */
    OCT_IMAGE_NOT_MACHO,        /* the header is not a thin little-endian 64-bit Mach-O */
    OCT_IMAGE_HEADER_TOO_LARGE, /* sizeofcmds exceeds OCT_IMAGE_MAX_HEADER_BYTES */
    OCT_IMAGE_MALFORMED,        /* the load commands did not parse */
    OCT_IMAGE_NOT_EXECUTABLE,   /* parsed, but it is not MH_EXECUTE */
} oct_image_outcome;

const char *oct_image_outcome_name(oct_image_outcome outcome);

/* Every call is answered from the list as it stands; nothing is cached between calls. `name` may
 * return NULL for an image, which is skipped rather than treated as a match. */
typedef struct oct_image_source {
    uint32_t    (*count)(void *context);
    const char *(*name)(void *context, uint32_t index);
    const void *(*header)(void *context, uint32_t index);
    void        *context;
} oct_image_source;

/* On OCT_IMAGE_FOUND, *index is the image's position and *identity its parsed identity. On any
 * other outcome *index is the position of the matching image when one was found (so a caller can
 * report it) or UINT32_MAX, and *identity holds whatever parsed before the failure: its uuid is
 * empty unless an LC_UUID was read, which lets a caller report the UUID actually seen. */
oct_image_outcome oct_device_select_image(const oct_image_source *source, const char *path,
                                          uint32_t *index, oct_macho_identity *identity);
#ifdef __cplusplus
}
#endif
#endif
