#include "test_support.h"
int test_failures;
int test_checks;
const char *fixture_directory = "Octopus/Tests/CoreTests/fixtures";
int main(void)
{
    run_usb_tests();
    printf("USB and identity boundary: %d checks, %d failures\n", test_checks, test_failures);
    return test_failures ? 1 : 0;
}
