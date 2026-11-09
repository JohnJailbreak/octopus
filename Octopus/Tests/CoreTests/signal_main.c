/*
 * signal_main.c — the startup signal's Darwin transport, run against a stand-in notifyd.
 *
 * oct_native_io.c posts the companion's startup signal through libnotify on the device. The
 * portable suite otherwise builds only the non-Apple stub, so this target builds the real branch
 * (OCT_NATIVE_FAKE_NOTIFY) against fakes/notify.h, which has Apple's prototypes, and checks what
 * the app will see: nothing before the first post, then exactly what ptpd posted.
 */
#include "../../Core/oct_native_io.h"
#include <stdio.h>
#include <string.h>

int fake_notify_posts(void);

int main(void)
{
    int failures = 0;
#define EXPECT(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s failed\n", __FILE__, __LINE__, #c); failures++; } } while (0)
    oct_native_signal out, in = { .phase = OCT_NATIVE_PHASE_TIMER, .identity = OCT_NATIVE_IDENTITY_COMPATIBLE,
                                  .lease_stage = OCT_NATIVE_IO_OPEN_DIRECTORY, .lease_error = 1, .pid = 1152 };
    EXPECT(oct_native_signal_read(&out) == OCT_E_STATE);          /* nothing posted since boot */
    EXPECT(oct_native_signal_publish(&in) == OCT_OK);
    in.phase = OCT_NATIVE_PHASE_PUBLISHED;
    EXPECT(oct_native_signal_publish(&in) == OCT_OK);              /* one token, reused */
    EXPECT(fake_notify_posts() == 2);
    EXPECT(oct_native_signal_read(&out) == OCT_OK);
    EXPECT(memcmp(&in, &out, sizeof in) == 0);                     /* the latest state, intact */
    EXPECT(oct_native_signal_publish(NULL) == OCT_E_INVALID_ARGUMENT);
    EXPECT(oct_native_signal_read(NULL) == OCT_E_INVALID_ARGUMENT);
    printf("Native startup signal (Darwin transport, stand-in notifyd): %d failures\n", failures);
    return failures != 0;
}
