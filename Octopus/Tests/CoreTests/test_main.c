#include "test_support.h"

#include <time.h>

int test_failures;
int test_checks;
const char *fixture_directory = "Octopus/Tests/CoreTests/fixtures";

int main(int argc, char **argv)
{
    if (argc > 1) fixture_directory = argv[1];
    setenv("TZ", "UTC0", 1); /* PTP dates are local time; pin it for golden data */
    tzset();
    printf("log\n");
    run_log_tests(); /* first, while the in-memory ring holds only its own lines */
    printf("store\n");
    run_store_tests();
    puts("loaded-image selection");
    run_image_tests();
    puts("read-only backend");
    run_readonly_tests();
    printf("protocol\n");
    run_protocol_tests();
    printf("responder\n");
    run_responder_tests();
    printf("service\n");
    run_service_tests();
    printf("link\n");
    run_link_tests();
    printf("USB boundary\n");
    run_usb_tests();
    printf("Native observation journal\n");
    run_native_tests();
    printf("Native lease IPC\n");
    run_native_io_tests();
    printf("diagnostics\n");
    run_diag_tests();
    printf("%d checks, %d failures\n", test_checks, test_failures);
    return test_failures ? 1 : 0;
}
