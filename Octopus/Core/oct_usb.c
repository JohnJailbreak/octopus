#include "oct_usb.h"
#include <string.h>

static uint16_t read16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static bool bulk_packet(uint16_t packet)
{
    return packet == 8 || packet == 16 || packet == 32 || packet == 64 || packet == 512;
}

oct_status oct_usb_parse_interface(const uint8_t *bytes, size_t length,
                                   uint8_t number, uint8_t alternate, oct_usb_interface *out)
{
    if (!out) return OCT_E_INVALID_ARGUMENT;
    memset(out, 0, sizeof *out);
    if (!bytes) return OCT_E_INVALID_ARGUMENT;
    if (length < 9 || bytes[0] != 9 || bytes[1] != 2 || read16(bytes + 2) != length || !bytes[5])
        return OCT_E_MALFORMED;
    oct_usb_interface result = { 0 };
    result.configuration = bytes[5];
    result.number = number;
    result.alternate = alternate;
    bool selected = false, found = false;
    unsigned endpoints = 0;
    for (size_t pos = 9; pos < length;) {
        if (length - pos < 2) return OCT_E_MALFORMED;
        const uint8_t *d = bytes + pos;
        size_t size = d[0];
        if (size < 2 || size > length - pos) return OCT_E_MALFORMED;
        if (d[1] == 4) {
            if (size != 9) return OCT_E_MALFORMED;
            selected = d[2] == number && d[3] == alternate;
            if (selected) {
                if (found) return OCT_E_MALFORMED;
                found = true;
                if (d[4] != 3 || d[5] != 6 || d[6] != 1 || d[7] != 1) return OCT_E_UNSUPPORTED;
            }
        } else if (d[1] == 5 && selected) {
            if (size != 7) return OCT_E_MALFORMED;
            uint8_t address = d[2];
            uint8_t attributes = d[3];
            uint16_t packet = read16(d + 4);
            if (!(address & 0x0Fu) || (address & 0x70u) || !packet || packet > 1024)
                return OCT_E_MALFORMED;
            if (address == result.bulk_in || address == result.bulk_out || address == result.interrupt_in)
                return OCT_E_MALFORMED;
            endpoints++;
            if (attributes == 2 && bulk_packet(packet)) {
                if (address & 0x80u) {
                    if (result.bulk_in) return OCT_E_MALFORMED;
                    result.bulk_in = address;
                    result.bulk_in_packet = packet;
                } else {
                    if (result.bulk_out) return OCT_E_MALFORMED;
                    result.bulk_out = address;
                    result.bulk_out_packet = packet;
                }
            } else if (attributes == 3 && (address & 0x80u) && d[6]) {
                if (result.interrupt_in) return OCT_E_MALFORMED;
                result.interrupt_in = address;
                result.interrupt_packet = packet;
                result.interrupt_interval = d[6];
            } else {
                return OCT_E_UNSUPPORTED;
            }
        } else if (selected && d[1] == 48) {
            return OCT_E_UNSUPPORTED; /* SuperSpeed endpoint companion */
        }
        pos += size;
    }
    if (!found) return OCT_E_UNSUPPORTED;
    if (endpoints != 3 || !result.bulk_in || !result.bulk_out || !result.interrupt_in) return OCT_E_MALFORMED;
    *out = result;
    return OCT_OK;
}

oct_status oct_usb_control(oct_link *link, uint8_t interface_number,
                           const uint8_t *setup, size_t setup_length,
                           const uint8_t *out_data, size_t out_length,
                           uint8_t *reply, size_t reply_capacity, size_t *reply_length)
{
    if (reply_length) *reply_length = 0;
    if (!link || !setup || (!out_data && out_length) || !reply_length) return OCT_E_INVALID_ARGUMENT;
    if (setup_length != 8) return OCT_E_MALFORMED;
    uint8_t type = setup[0], request = setup[1];
    uint16_t value = read16(setup + 2), index = read16(setup + 4), length = read16(setup + 6);
    if (value || index != interface_number) return OCT_E_MALFORMED;
    switch (request) {
    case OCT_USB_CANCEL_REQUEST:
        if (type != 0x21 || length != 6 || out_length != 6) return OCT_E_MALFORMED;
        break;
    case OCT_USB_DEVICE_RESET_REQUEST:
        if (type != 0x21 || length || out_length) return OCT_E_MALFORMED;
        break;
    case OCT_USB_GET_DEVICE_STATUS:
        if (type != 0xA1 || out_length) return OCT_E_MALFORMED;
        break;
    default:
        return OCT_E_UNSUPPORTED;
    }
    oct_link_status status;
    oct_link_get_status(link, &status);
    if (!status.host_attached || status.suspended) return OCT_E_STATE;
    if (request != OCT_USB_GET_DEVICE_STATUS)
        return oct_link_class_request(link, request, out_data, out_length, NULL, 0, reply_length);
    size_t send_length = length < 4 ? length : 4;
    if (send_length > reply_capacity || (send_length && !reply)) return OCT_E_INVALID_ARGUMENT;
    uint8_t data[4];
    size_t got = 0;
    oct_status result = oct_link_class_request(link, request, NULL, 0, data, sizeof data, &got);
    if (result != OCT_OK) return result;
    if (got != sizeof data) return OCT_E_IO;
    if (send_length) memcpy(reply, data, send_length);
    *reply_length = send_length;
    return OCT_OK;
}
