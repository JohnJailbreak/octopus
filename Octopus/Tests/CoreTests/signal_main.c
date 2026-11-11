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
#include <stdbool.h>
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

    /* The USB transport heartbeat, on the real Darwin code path. This is what the USB Connection
     * screen reads, and the case that matters most is the last one: a state left behind after the
     * poster stops must not read as a live session. */
    oct_native_runtime live = { .mode = 2, .flags = OCT_NATIVE_RUNTIME_HOOKED | OCT_NATIVE_RUNTIME_SERVING,
                                .commands = 213, .responses = 212, .provider_events = 115 };
    /* A ptpd that has since exited left a state behind, posted before the app first looked. That
     * must not count as a heartbeat: on Apple's platforms notify_check reports a post on its first
     * call unconditionally, and trusting it showed a live session on an unplugged phone. */
    EXPECT(oct_native_runtime_publish(&live) == OCT_OK);
    bool posted = true;
    EXPECT(oct_native_runtime_check(&posted) == OCT_OK);
    EXPECT(!posted);                                               /* the leftover is not a heartbeat */
    EXPECT(oct_native_runtime_publish(&live) == OCT_OK);           /* ptpd's poll publishes */
    EXPECT(oct_native_runtime_check(&posted) == OCT_OK);
    EXPECT(posted);                                                /* heard */
    EXPECT(oct_native_runtime_check(&posted) == OCT_OK);
    EXPECT(!posted);                                               /* and not again without a new post */
    EXPECT(oct_native_runtime_publish(&live) == OCT_OK);
    EXPECT(oct_native_runtime_publish(&live) == OCT_OK);
    EXPECT(oct_native_runtime_check(&posted) == OCT_OK);
    EXPECT(posted);                                                /* two posts read as one heartbeat */
    EXPECT(oct_native_runtime_check(NULL) == OCT_E_INVALID_ARGUMENT);

    /* What the screen shows. A live serving companion is a session; the same counters once the
     * companion has fallen silent are no session at all, because ptpd has exited. */
    oct_native_runtime read_back;
    EXPECT(oct_native_runtime_read(&read_back) == OCT_OK);
    EXPECT(read_back.commands == 213 && read_back.responses == 212);
    const uint64_t heard = 1000000;
    EXPECT(oct_native_transport_stage_of(&read_back, oct_native_transport_fresh(heard + 1000, heard, 5000))
           == OCT_NATIVE_TRANSPORT_SERVING);
    EXPECT(oct_native_transport_stage_of(&read_back, oct_native_transport_fresh(heard + 6000, heard, 5000))
           == OCT_NATIVE_TRANSPORT_ABSENT);                        /* the state survived; the session did not */
    EXPECT(strcmp(oct_native_transport_stage_name(OCT_NATIVE_TRANSPORT_SERVING), "Session active") == 0);

    printf("Native startup signal (Darwin transport, stand-in notifyd): %d failures\n", failures);
    return failures != 0;
}
