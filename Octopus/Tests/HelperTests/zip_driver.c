/* Writes a diagnostics-style archive with oct_zip for the host tests, which read it back with Python's zipfile. */
#include "../../Core/oct_zip.h"

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    static unsigned char binary[70000];
    for (size_t i = 0; i < sizeof binary; i++) binary[i] = (unsigned char)(i * 31u + 7u);
    oct_zip *zip = NULL;
    if (argc < 2 || oct_zip_create(&zip) != OCT_OK) return 64;
    int failures = 0;
    failures += oct_zip_add(zip, "results.json", "{\"ok\": true}\n", 13, 1762632000) != OCT_OK;
    failures += oct_zip_add(zip, "logs/debug.log", "a\nb\n", 4, 1762632000) != OCT_OK;
    failures += oct_zip_add(zip, "binary.bin", binary, sizeof binary, 1762632000) != OCT_OK;
    failures += oct_zip_add(zip, "empty.txt", NULL, 0, 0) != OCT_OK;
    failures += oct_zip_add(zip, "caf\xC3\xA9.txt", "utf8", 4, 1762632000) != OCT_OK;
    if (strcmp(argv[1], "--first") == 0) { /* zip_driver --first <folder>...: the first folder that works */
        char written[4096];
        oct_status status = oct_zip_write_first(zip, (const char *const *)(argv + 2), (size_t)(argc - 2),
                                                "diagnostics.zip", written, sizeof written);
        printf("%s\n%s\n", status == OCT_OK ? written : "", oct_zip_error(zip));
        failures += status != OCT_OK;
    } else {
        failures += oct_zip_write(zip, argv[1]) != OCT_OK;
    }
    oct_zip_destroy(zip);
    return failures;
}
