/*
 * Compatibility checks for the transport adapter, against documented host behaviour.
 *
 * Each check builds a temporary service on a read-only synthetic storage, puts it behind a loopback
 * link at USB 2.0 high-speed packet sizes, and drives the real responder through the adapter. They
 * are simulations: no computer is involved. The enumeration follows the MTP 1.0 specification's
 * session flow, the order Windows Portable Devices is documented to use; it is not a capture of
 * Windows traffic, which no Octopus build has received yet.
 */
#ifndef OCT_COMPAT_H
#define OCT_COMPAT_H

#include "oct_service.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct oct_compat_report {
    uint32_t checks;
    char     summary[OCT_EVENT_TEXT_MAX];
    char     failure[OCT_EVENT_TEXT_MAX];
} oct_compat_report;

/* DeviceInfo, a session, the storage, the supported properties, the root, the nested Documents folder, and
 * example.txt read whole and in part, its bytes checked against oct_compat_reference_digest; then a refused
 * delete, as the storage is read-only. The summary names the file, its size and its SHA-256. */
oct_status oct_compat_mtp_enumeration(oct_compat_report *report);
/* Cancel (with good and bad data stages), Get Device Status, Device Reset and unsupported requests. */
oct_status oct_compat_class_requests(oct_compat_report *report);
/* Event containers on the interrupt pipe: their layout, the zero-length-packet rule, and when they're refused. */
oct_status oct_compat_events(oct_compat_report *report);
/* Suspend and resume, a bus reset, an unplug mid-container, and start and stop cycles. */
oct_status oct_compat_lifecycle(oct_compat_report *report);
/* Full USB setup validation and endpoint descriptor selection; simulated only. */
oct_status oct_compat_usb_boundary(oct_compat_report *report);

/* A synthetic file's size and SHA-256 read straight from a fresh synthetic storage, without the responder:
 * the reference a host's download must match. `folder` is NULL for the root. */
oct_status oct_compat_reference_digest(const char *folder, const char *name, uint64_t *size, char hex[65]);

#ifdef __cplusplus
}
#endif

#endif /* OCT_COMPAT_H */
