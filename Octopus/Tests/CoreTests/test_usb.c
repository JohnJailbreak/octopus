#include "test_support.h"
#include "../../Core/oct_device.h"
#include "../../Core/oct_usb.h"
#include "../../Core/oct_compat.h"

/* Specification-derived fixtures, not a captured iPhone configuration. */
static const uint8_t ptp[] = {
    9,2,39,0,1,1,0,0x80,50,
    9,4,0,0,3,6,1,1,0,
    7,5,0x81,2,0,2,0,
    7,5,0x02,2,0,2,0,
    7,5,0x83,3,64,0,4
};

static void test_descriptors(void)
{
    oct_usb_interface result;
    CHECK_OK(oct_usb_parse_interface(ptp, sizeof ptp, 0, 0, &result));
    CHECK_EQ(result.configuration, 1);
    CHECK_EQ(result.bulk_in, 0x81);
    CHECK_EQ(result.bulk_out, 2);
    CHECK_EQ(result.interrupt_in, 0x83);
    CHECK_EQ(result.bulk_in_packet, 512);
    CHECK_EQ(result.bulk_out_packet, 512);
    CHECK_EQ(result.interrupt_packet, 64);
    CHECK_EQ(result.interrupt_interval, 4);
    CHECK_EQ(oct_usb_parse_interface(ptp, sizeof ptp, 1, 0, &result), OCT_E_UNSUPPORTED);
    CHECK_EQ(result.configuration, 0);
    CHECK_EQ(oct_usb_parse_interface(ptp, sizeof ptp, 0, 1, &result), OCT_E_UNSUPPORTED);
    CHECK_EQ(oct_usb_parse_interface(NULL, 0, 0, 0, &result), OCT_E_INVALID_ARGUMENT);
    CHECK_EQ(oct_usb_parse_interface(ptp, sizeof ptp, 0, 0, NULL), OCT_E_INVALID_ARGUMENT);
    for (size_t n = 0; n < sizeof ptp; n++) {
        memset(&result, 0xFF, sizeof result);
        CHECK_EQ(oct_usb_parse_interface(ptp, n, 0, 0, &result), OCT_E_MALFORMED);
        CHECK_EQ(result.bulk_in, 0);
    }
    /* Each descriptor must make progress and stay within the bounded buffer. */
    const size_t starts[] = { 0,9,18,25,32 };
    for (size_t n = 0; n < sizeof starts / sizeof starts[0]; n++) {
        uint8_t bad[sizeof ptp];
        memcpy(bad, ptp, sizeof bad);
        bad[starts[n]] = 0;
        CHECK_EQ(oct_usb_parse_interface(bad, sizeof bad, 0, 0, &result), OCT_E_MALFORMED);
        bad[starts[n]] = 255;
        CHECK_EQ(oct_usb_parse_interface(bad, sizeof bad, 0, 0, &result), OCT_E_MALFORMED);
    }
    uint8_t changed[sizeof ptp];
    memcpy(changed, ptp, sizeof changed);
    changed[27] = 0x81; /* duplicate endpoint address */
    CHECK_EQ(oct_usb_parse_interface(changed, sizeof changed, 0, 0, &result), OCT_E_MALFORMED);
    memcpy(changed, ptp, sizeof changed);
    changed[20] = 0x80; /* endpoint zero */
    CHECK_EQ(oct_usb_parse_interface(changed, sizeof changed, 0, 0, &result), OCT_E_MALFORMED);
    memcpy(changed, ptp, sizeof changed);
    changed[20] = 0x91; /* reserved address bits */
    CHECK_EQ(oct_usb_parse_interface(changed, sizeof changed, 0, 0, &result), OCT_E_MALFORMED);
    memcpy(changed, ptp, sizeof changed);
    changed[23] = 0x0A; /* high-bandwidth packet encoding outside supported USB2 profile */
    CHECK_EQ(oct_usb_parse_interface(changed, sizeof changed, 0, 0, &result), OCT_E_MALFORMED);
    memcpy(changed, ptp, sizeof changed);
    changed[38] = 0; /* interrupt interval must be nonzero */
    CHECK_EQ(oct_usb_parse_interface(changed, sizeof changed, 0, 0, &result), OCT_E_UNSUPPORTED);
    memcpy(changed, ptp, sizeof changed);
    changed[14] = 0xFF; /* vendor-specific interface must not be commandeered */
    CHECK_EQ(oct_usb_parse_interface(changed, sizeof changed, 0, 0, &result), OCT_E_UNSUPPORTED);
    memcpy(changed, ptp, sizeof changed);
    changed[13] = 2;
    CHECK_EQ(oct_usb_parse_interface(changed, sizeof changed, 0, 0, &result), OCT_E_UNSUPPORTED);
    memcpy(changed, ptp, sizeof changed);
    changed[22] = 64; changed[23] = 0;
    changed[29] = 64; changed[30] = 0;
    CHECK_OK(oct_usb_parse_interface(changed, sizeof changed, 0, 0, &result));
    CHECK_EQ(result.bulk_in_packet, 64);
    /* A second, unrelated interface and endpoint do not become PTP pipes. */
    uint8_t composite[55];
    memcpy(composite, ptp, sizeof ptp);
    static const uint8_t vendor[] = { 9,4,1,0,1,0xFF,0,0,0, 7,5,0x84,2,64,0,0 };
    memcpy(composite + sizeof ptp, vendor, sizeof vendor);
    composite[2] = sizeof composite; composite[4] = 2;
    CHECK_OK(oct_usb_parse_interface(composite, sizeof composite, 0, 0, &result));
    CHECK_EQ(result.bulk_in, 0x81);
    CHECK_EQ(oct_usb_parse_interface(composite, sizeof composite, 1, 0, &result), OCT_E_UNSUPPORTED);
    /* Same selected alternate twice is ambiguous and must not choose the last. */
    composite[41] = 0;
    CHECK_EQ(oct_usb_parse_interface(composite, sizeof composite, 0, 0, &result), OCT_E_MALFORMED);
}

static void test_control(void)
{
    oct_service *service = NULL;
    oct_loopback *loopback = NULL;
    oct_link *link = NULL;
    oct_link_driver driver;
    oct_transport transport;
    CHECK_OK(oct_service_create(&service));
    CHECK_OK(oct_loopback_create(&loopback));
    oct_loopback_driver(loopback, &driver);
    oct_link_config config = { 512, 1000, "usbtest", true, 64 };
    CHECK_OK(oct_link_create(&driver, &config, &link));
    oct_link_transport(link, &transport);
    CHECK_OK(oct_service_set_transport(service, &transport));
    oct_service_config service_config = { SEED_TIME, false, false };
    CHECK_OK(oct_service_start(service, &service_config));
    CHECK_OK(oct_link_host_attached(link));
    uint8_t setup[] = { 0x21, OCT_USB_DEVICE_RESET_REQUEST, 0,0,7,0,0,0 };
    size_t length = 99;
    CHECK_OK(oct_usb_control(link, 7, setup, 8, NULL, 0, NULL, 0, &length));
    CHECK_EQ(length, 0);
    oct_link_status status;
    oct_link_get_status(link, &status);
    CHECK_EQ(status.resets, 1);
    for (unsigned type = 0; type < 256; type++) {
        if (type == 0x21) continue;
        setup[0] = (uint8_t)type;
        CHECK_EQ(oct_usb_control(link, 7, setup, 8, NULL, 0, NULL, 0, &length), OCT_E_MALFORMED);
        CHECK_EQ(length, 0);
    }
    setup[0] = 0x21;
    for (size_t n = 0; n < 8; n++)
        CHECK_EQ(oct_usb_control(link, 7, setup, n, NULL, 0, NULL, 0, &length), OCT_E_MALFORMED);
    CHECK_EQ(oct_usb_control(link, 7, setup, 9, NULL, 0, NULL, 0, &length), OCT_E_MALFORMED);
    CHECK_EQ(oct_usb_control(link, 0, setup, 8, NULL, 0, NULL, 0, &length), OCT_E_MALFORMED);
    setup[5] = 1; /* full 16-bit wIndex, not only the low byte */
    CHECK_EQ(oct_usb_control(link, 7, setup, 8, NULL, 0, NULL, 0, &length), OCT_E_MALFORMED);
    setup[5] = 0;
    oct_link_get_status(link, &status);
    CHECK_EQ(status.resets, 1); /* Every malformed request had no side effects. */
    uint8_t reply[4] = { 0xCC,0xCC,0xCC,0xCC };
    setup[0] = 0xA1; setup[1] = OCT_USB_GET_DEVICE_STATUS; setup[6] = 4;
    CHECK_EQ(oct_usb_control(link, 7, setup, 8, NULL, 0, reply, 3, &length), OCT_E_INVALID_ARGUMENT);
    CHECK_EQ(length, 0); CHECK_EQ(reply[0], 0xCC);
    CHECK_OK(oct_usb_control(link, 7, setup, 8, NULL, 0, reply, 4, &length));
    CHECK_EQ(length, 4);
    CHECK_EQ(reply[0], 4); CHECK_EQ(reply[1], 0); CHECK_EQ(reply[2], 1); CHECK_EQ(reply[3], 0x20);
    setup[6] = 0;
    CHECK_OK(oct_usb_control(link, 7, setup, 8, NULL, 0, NULL, 0, &length));
    CHECK_EQ(length, 0);
    setup[1] = OCT_USB_GET_EXTENDED_EVENT_DATA;
    CHECK_EQ(oct_usb_control(link, 7, setup, 8, NULL, 0, NULL, 0, &length), OCT_E_UNSUPPORTED);
    CHECK_EQ(oct_usb_control(link, 7, setup, 8, NULL, 1, NULL, 0, &length), OCT_E_INVALID_ARGUMENT);
    CHECK_EQ(oct_usb_control(link, 7, setup, 8, NULL, 0, NULL, 0, NULL), OCT_E_INVALID_ARGUMENT);
    CHECK_OK(oct_service_stop(service));
    setup[1] = OCT_USB_GET_DEVICE_STATUS;
    CHECK_EQ(oct_usb_control(link, 7, setup, 8, NULL, 0, NULL, 0, &length), OCT_E_STATE);
    oct_service_destroy(service);
    oct_link_destroy(link);
    oct_loopback_destroy(loopback);
}

static void test_identity(void)
{
    uint8_t bytes[80] = {0xcf,0xfa,0xed,0xfe, 0x0c,0,0,1, 2,0,0,0x80, 2,0,0,0,
                         2,0,0,0, 48,0,0,0};
    bytes[32] = 0x1b; bytes[36] = 24;
    bytes[56] = 0x32; bytes[60] = 24;
    bytes[64] = 2; bytes[70] = 17; bytes[74] = 17;
    oct_macho_identity identity;
    CHECK_OK(oct_device_parse_identity(bytes, sizeof bytes, &identity));
    CHECK_EQ(identity.cpu_type, 0x0100000c);
    CHECK_EQ(identity.cpu_subtype, 0x80000002u);
    CHECK_EQ(identity.minimum_os, 0x00110000);
    CHECK_STR(identity.uuid, "00000000-0000-0000-0000-000000000000");
    for (size_t n = 0; n < sizeof bytes; ++n) {
        CHECK_EQ(oct_device_parse_identity(bytes, n, &identity), OCT_E_MALFORMED);
        CHECK_EQ(identity.uuid[0], 0);
    }
    bytes[36] = 0;
    CHECK_EQ(oct_device_parse_identity(bytes, sizeof bytes, &identity), OCT_E_MALFORMED);
    bytes[36] = 24; bytes[56] = 0x1b;
    CHECK_EQ(oct_device_parse_identity(bytes, sizeof bytes, &identity), OCT_E_MALFORMED);
    bytes[56] = 0x32; bytes[76] = 1;
    CHECK_EQ(oct_device_parse_identity(bytes, sizeof bytes, &identity), OCT_E_MALFORMED);
    bytes[76] = 0; bytes[0] = 0;
    CHECK_EQ(oct_device_parse_identity(bytes, sizeof bytes, &identity), OCT_E_UNSUPPORTED);
    CHECK_EQ(oct_device_parse_identity(NULL, 0, &identity), OCT_E_INVALID_ARGUMENT);
    CHECK_EQ(oct_device_probe_ptp(NULL), OCT_E_INVALID_ARGUMENT);
}

void run_usb_tests(void)
{
    test_identity();
    test_descriptors();
    test_control();
    oct_compat_report report;
    CHECK_OK(oct_compat_usb_boundary(&report));
    CHECK(report.checks > 70);
    CHECK(strstr(report.summary, "Synthetic USB") != NULL);
}
