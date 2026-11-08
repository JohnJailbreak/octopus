/* Device-side USB boundary. This is not an IOKit driver or a registration API.
 * A native owner must supply actual descriptors and route its completed control
 * transfers here. No host attachment is inferred from descriptor discovery. */
#ifndef OCT_USB_H
#define OCT_USB_H
#include "oct_link.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct oct_usb_interface {
    uint8_t configuration;
    uint8_t number;
    uint8_t alternate;
    uint8_t bulk_in;
    uint8_t bulk_out;
    uint8_t interrupt_in;
    uint16_t bulk_in_packet;
    uint16_t bulk_out_packet;
    uint16_t interrupt_packet;
    uint8_t interrupt_interval;
} oct_usb_interface;

/* Parse one complete USB configuration (including its interface/endpoint
 * descriptors), selecting exactly one interface/alternate with class 06/01/01.
 * Output is zeroed on every failure. Extra bytes beyond wTotalLength, missing or
 * duplicate endpoint addresses and malformed descriptor chains are rejected.
 * This checks USB 2.0 full/high speed layouts, not SuperSpeed companions.
 * OCT_E_UNSUPPORTED: no selected PTP interface, or an unsupported layout.
 * OCT_E_MALFORMED: invalid descriptor encoding. */
oct_status oct_usb_parse_interface(const uint8_t *bytes, size_t length,
                                   uint8_t number, uint8_t alternate, oct_usb_interface *out);

/* Dispatch a COMPLETE control transfer: the eight on-wire setup bytes, followed
 * by the complete OUT stage (if any). The native owner must validate USB transfer
 * completion before calling this. It owns standard requests, endpoint stalls,
 * transfer cancellation and the control status stage.
 *
 * The selected interface number is from the active descriptors, not a constant.
 * All setup fields are validated BEFORE mutating the link. Device-to-host data
 * is truncated to wLength; reply_capacity must fit that truncated result.
 * On any error reply_length is zero and the owner must STALL this request.
 * Unsupported requests are not forwarded to another interface automatically.
 * This does not mark a host attached; the owner must do so after authorization.
 */
oct_status oct_usb_control(oct_link *link, uint8_t interface_number,
                           const uint8_t *setup, size_t setup_length,
                           const uint8_t *out_data, size_t out_length,
                           uint8_t *reply, size_t reply_capacity, size_t *reply_length);
#ifdef __cplusplus
}
#endif
#endif
