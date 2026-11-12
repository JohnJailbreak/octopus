#include "test_support.h"
#include "../../Core/oct_native.h"

#define TRANSPORT UINT64_C(0x12345678)

static void start(oct_native_journal *j)
{
    oct_native_init(j, 101);
    oct_native_authorize(j, true);
    oct_native_connect(j, TRANSPORT, true);
}

static bool command(oct_native_journal *j, uint16_t operation, uint32_t tid)
{
    uint64_t scope = oct_native_receive_begin(j, TRANSPORT, 0, 16);
    bool accepted = oct_native_command(j, scope, operation, tid);
    oct_native_receive_end(j, scope);
    return accepted;
}

static void test_provenance(void)
{
    oct_native_journal j;
    oct_native_init(&j, 101);
    oct_native_connect(&j, TRANSPORT, true);
    CHECK_EQ(oct_native_receive_begin(&j, TRANSPORT, 0, 16), 0);
    CHECK_EQ(j.state.event_count, 0);
    oct_native_authorize(&j, true);
    CHECK_EQ(oct_native_receive_begin(&j, TRANSPORT, 0, 16), 0);
    oct_native_connect(&j, TRANSPORT, true);
    CHECK_EQ(oct_native_receive_begin(&j, TRANSPORT + 1, 0, 16), 0);
    CHECK(!oct_native_command(&j, 0, 0x1001, 0));
    CHECK(!oct_native_command(&j, 99, 0x1001, 0));
    CHECK(!oct_native_response(&j, TRANSPORT, 0, 0x2001, true));
    CHECK_EQ(j.state.paired_responses, 0);
    CHECK_EQ(j.state.unmatched_responses, 1);
    CHECK(command(&j, 0x1001, 0));
    CHECK(!oct_native_response(&j, TRANSPORT + 1, 0, 0x2001, true));
    CHECK(!oct_native_data_submitted(&j, TRANSPORT + 1, 0, 99));
    CHECK(oct_native_data_submitted(&j, TRANSPORT, 0, 99));
    CHECK_EQ(j.state.paired_responses, 0); /* void sendData cannot prove delivery */
    CHECK(oct_native_response(&j, TRANSPORT, 0, 0x2001, true));
    CHECK_EQ(j.state.paired_responses, 1);
    CHECK(!oct_native_response(&j, TRANSPORT, 0, 0x2001, true));
    CHECK_EQ(j.state.paired_responses, 1);
    CHECK_EQ(j.state.data_submissions, 1);
    CHECK(command(&j, 0x1004, 1));
    CHECK(!oct_native_response(&j, TRANSPORT, 1, 0x2001, false));
    CHECK_EQ(j.state.failed_responses, 1);
    CHECK(!oct_native_response(&j, TRANSPORT, 1, 0x2001, true));
    CHECK_EQ(j.state.paired_responses, 1);
    oct_native_authorize(&j, false);
    CHECK(!j.state.connected);
    CHECK(!command(&j, 0x1004, 2));
    CHECK_EQ(oct_native_receive_begin(&j, TRANSPORT, 0, 16), 0);
    CHECK_EQ(j.state.paired_responses, 1); /* cumulative observations remain labelled */
    oct_native_init(&j, 0);
    oct_native_authorize(&j, true);
    CHECK(!j.state.authorized);
}

static void test_lifecycle(void)
{
    oct_native_journal j;
    start(&j);
    uint64_t scope = oct_native_receive_begin(&j, TRANSPORT, 0, 16);
    oct_native_receive_end(&j, scope);
    CHECK(!oct_native_command(&j, scope, 0x1001, 0));
    CHECK(command(&j, 0x1001, 0));
    CHECK(!command(&j, 0x1001, 0)); /* duplicate makes both requests ambiguous */
    CHECK(!oct_native_response(&j, TRANSPORT, 0, 0x2001, true));
    CHECK(!command(&j, 0x1001, 0)); /* ambiguity must not disappear after one rejection */
    CHECK_EQ(j.state.paired_responses, 0);
    CHECK(command(&j, 0x1004, 1));
    CHECK(command(&j, 0x1002, 2)); /* OpenSession retires old pending work */
    CHECK(!oct_native_response(&j, TRANSPORT, 1, 0x2001, true));
    CHECK(!command(&j, 0x1004, 1)); /* late old response cannot pair with reused tid */
    CHECK(oct_native_response(&j, TRANSPORT, 2, 0x2001, true));
    CHECK(command(&j, 0x1004, 3));
    oct_native_connect(&j, TRANSPORT + 1, false); /* unrelated disconnect */
    CHECK(oct_native_response(&j, TRANSPORT, 3, 0x2001, true));
    CHECK(command(&j, 0x1004, 4));
    oct_native_connect(&j, TRANSPORT, false);
    CHECK(!oct_native_response(&j, TRANSPORT, 4, 0x2001, true));
    oct_native_connect(&j, TRANSPORT, true);
    CHECK(!command(&j, 0x1004, 4));
    CHECK(command(&j, 0x1004, 5));
    CHECK_EQ(oct_native_receive_begin(&j, TRANSPORT, -536870184, 16), 0);
    CHECK(!oct_native_response(&j, TRANSPORT, 5, 0x2001, true));
    CHECK_EQ(oct_native_receive_begin(&j, TRANSPORT, 0, 11), 0);
    CHECK_EQ(j.state.failed_reads, 2);
    CHECK(command(&j, 0x1004, 6));
    CHECK(!oct_native_response(&j, TRANSPORT, 6, 0x1001, true)); /* not a response code */
    CHECK_EQ(j.state.paired_responses, 2);
    oct_native_init(&j, 102);
    oct_native_authorize(&j, true);
    oct_native_connect(&j, TRANSPORT, true);
    CHECK(command(&j, 0x1004, 4));
    CHECK(oct_native_response(&j, TRANSPORT, 4, 0x2001, true));
    CHECK_EQ(j.state.generation, 102);
    CHECK_EQ(j.state.paired_responses, 1);
}

static void test_bounds(void)
{
    oct_native_journal j;
    start(&j);
    uint64_t scopes[OCT_NATIVE_SCOPES];
    for (size_t i = 0; i < OCT_NATIVE_SCOPES; i++) {
        scopes[i] = oct_native_receive_begin(&j, TRANSPORT, 0, 16);
        CHECK(scopes[i] != 0);
    }
    CHECK_EQ(oct_native_receive_begin(&j, TRANSPORT, 0, 16), 0);
    for (size_t i = 0; i < OCT_NATIVE_SCOPES; i++) oct_native_receive_end(&j, scopes[i]);
    for (uint32_t tid = 0; tid <= OCT_NATIVE_PENDING; tid++) CHECK(command(&j, 0x1004, tid));
    CHECK_EQ(j.state.dropped_pending, 1);
    CHECK(!oct_native_response(&j, TRANSPORT, 0, 0x2001, true));
    CHECK(!command(&j, 0x1004, 0));
    CHECK(oct_native_response(&j, TRANSPORT, OCT_NATIVE_PENDING, 0x2001, true));
    start(&j);
    for (uint32_t tid = 0; tid < 200; tid++) {
        CHECK(command(&j, 0x1004, tid));
        CHECK(oct_native_response(&j, TRANSPORT, tid, 0x2001, true));
    }
    CHECK_EQ(j.state.paired_responses, 200);
    CHECK_EQ(j.state.event_count, OCT_NATIVE_EVENTS);
    CHECK(j.state.overwritten_events > 0);
    oct_native_snapshot snapshot;
    oct_native_get_snapshot(&j, &snapshot);
    for (size_t i = 1; i < snapshot.event_count; i++) {
        CHECK_EQ(snapshot.events[i].sequence, snapshot.events[i-1].sequence + 1);
        CHECK_EQ(snapshot.events[i].generation, 101);
        CHECK(snapshot.events[i].utc_ms > 0);
    }
    start(&j);
    for (uint32_t tid = 0; tid < OCT_NATIVE_TOMBSTONES; tid++) {
        CHECK(command(&j, 0x1004, tid));
        CHECK(!command(&j, 0x1004, tid));
    }
    CHECK(!j.state.pairing_disabled);
    CHECK(command(&j, 0x1004, 1000));
    CHECK(!command(&j, 0x1004, 1000));
    CHECK(j.state.pairing_disabled);
    CHECK(!command(&j, 0x1004, 2000));
    CHECK(!oct_native_response(&j, TRANSPORT, 2000, 0x2001, true));
    start(&j);
    j.state.sequence = UINT64_MAX;
    CHECK_EQ(oct_native_receive_begin(&j, TRANSPORT, 0, 16), 0);
    CHECK(!j.state.authorized);
    CHECK(!command(&j, 0x1004, 1));
}

void run_native_tests(void)
{
    test_provenance();
    test_lifecycle();
    test_bounds();
}

#ifdef OCT_NATIVE_TEST_MAIN
int test_checks;
int test_failures;
int main(void)
{
    run_native_tests();
    printf("Native observation journal: %d checks, %d failures (synthetic observations only)\n", test_checks, test_failures);
    return test_failures ? 1 : 0;
}
#endif
