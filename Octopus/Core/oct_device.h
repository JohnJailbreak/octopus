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
#ifdef __cplusplus
}
#endif
#endif
