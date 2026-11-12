#include "oct_native.h"
#include <string.h>
#include <time.h>

static void poison(oct_native_journal *j, uint32_t transaction)
{
    for (size_t i = 0; i < OCT_NATIVE_TOMBSTONES; i++) {
        oct_native_tombstone *t = &j->tombstones[i];
        if (t->active && t->transport == j->state.transport && t->transaction == transaction) return;
        if (!t->active) { *t = (oct_native_tombstone){ j->state.transport, transaction, true }; return; }
    }
    j->state.pairing_disabled = true;
}

static bool poisoned(const oct_native_journal *j, uint32_t transaction)
{
    if (j->state.pairing_disabled) return true;
    for (size_t i = 0; i < OCT_NATIVE_TOMBSTONES; i++)
        if (j->tombstones[i].active && j->tombstones[i].transport == j->state.transport &&
            j->tombstones[i].transaction == transaction) return true;
    return false;
}

static void invalidate(oct_native_journal *j)
{
    for (size_t i = 0; i < OCT_NATIVE_PENDING; i++)
        if (j->pending[i].active) poison(j, j->pending[i].transaction);
    memset(j->pending, 0, sizeof j->pending);
    memset(j->scopes, 0, sizeof j->scopes);
    if (j->state.epoch != UINT64_MAX) j->state.epoch++;
    else j->state.authorized = false; /* never reuse a correlation epoch */
}

static uint64_t record(oct_native_journal *j, oct_native_event_kind kind, uint64_t scope,
                       uint16_t operation, uint32_t transaction, uint16_t response, int32_t result, uint64_t bytes)
{
    if (j->state.sequence == UINT64_MAX) {
        j->state.authorized = false;
        invalidate(j);
        return 0;
    }
    oct_native_event event = { 0 };
    event.sequence = ++j->state.sequence;
    struct timespec now;
    if (timespec_get(&now, TIME_UTC) == TIME_UTC)
        event.utc_ms = (int64_t)now.tv_sec * 1000 + (int64_t)(now.tv_nsec / 1000000);
    event.generation = j->state.generation;
    event.epoch = j->state.epoch;
    event.transport = j->state.transport;
    event.scope = scope;
    event.operation = operation;
    event.transaction = transaction;
    event.response = response;
    event.result = result;
    event.bytes = bytes;
    event.kind = kind;
    if (j->state.event_count == OCT_NATIVE_EVENTS) {
        memmove(j->state.events, j->state.events + 1, (OCT_NATIVE_EVENTS - 1) * sizeof event);
        j->state.event_count--;
        j->state.overwritten_events++;
    }
    j->state.events[j->state.event_count++] = event;
    return event.sequence;
}

static bool live(const oct_native_journal *j, uint64_t transport)
{
    return j && j->state.generation && j->state.authorized && j->state.connected && transport &&
           transport == j->state.transport;
}

static oct_native_pending *pending(oct_native_journal *j, uint32_t transaction)
{
    for (size_t i = 0; i < OCT_NATIVE_PENDING; i++) {
        oct_native_pending *p = &j->pending[i];
        if (p->active && p->epoch == j->state.epoch && p->transaction == transaction) return p;
    }
    return NULL;
}

void oct_native_init(oct_native_journal *j, uint64_t generation)
{
    if (!j) return;
    memset(j, 0, sizeof *j);
    j->state.generation = generation;
}

void oct_native_authorize(oct_native_journal *j, bool authorized)
{
    if (!j || !j->state.generation || j->state.authorized == authorized) return;
    invalidate(j);
    j->state.connected = false;
    j->state.transport = 0;
    j->state.authorized = authorized && j->state.epoch != UINT64_MAX && j->state.sequence != UINT64_MAX;
    record(j, OCT_NATIVE_AUTHORIZATION, 0, 0, 0, 0, j->state.authorized ? 1 : 0, 0);
}

void oct_native_connect(oct_native_journal *j, uint64_t transport, bool connected)
{
    if (!j || !j->state.authorized || !transport) return;
    if (!connected && transport != j->state.transport) return;
    if (connected == j->state.connected && transport == j->state.transport) return;
    invalidate(j);
    j->state.transport = transport;
    j->state.connected = connected;
    record(j, OCT_NATIVE_CONNECTION, 0, 0, 0, 0, connected ? 1 : 0, 0);
}

uint64_t oct_native_receive_begin(oct_native_journal *j, uint64_t transport,
                                  int32_t io_result, uint32_t byte_count)
{
    if (!live(j, transport)) return 0;
    if (io_result || byte_count < 12) {
        j->state.failed_reads++;
        invalidate(j);
        record(j, OCT_NATIVE_USB_READ, 0, 0, 0, 0, io_result ? io_result : -1, byte_count);
        return 0;
    }
    j->state.usb_reads++;
    for (size_t i = 0; i < OCT_NATIVE_SCOPES; i++) {
        if (!j->scopes[i].active) {
            uint64_t id = record(j, OCT_NATIVE_USB_READ, 0, 0, 0, 0, 0, byte_count);
            j->scopes[i] = (oct_native_scope){ id, j->state.epoch, id != 0 };
            return id;
        }
    }
    record(j, OCT_NATIVE_OVERFLOW, 0, 0, 0, 0, -1, 0);
    return 0;
}

void oct_native_receive_end(oct_native_journal *j, uint64_t scope)
{
    if (!j || !scope) return;
    for (size_t i = 0; i < OCT_NATIVE_SCOPES; i++)
        if (j->scopes[i].id == scope) j->scopes[i].active = false;
}

bool oct_native_command(oct_native_journal *j, uint64_t scope, uint16_t operation, uint32_t transaction)
{
    if (!j || !live(j, j->state.transport)) return false;
    bool found = false;
    for (size_t i = 0; i < OCT_NATIVE_SCOPES; i++) {
        if (scope && j->scopes[i].active && j->scopes[i].id == scope && j->scopes[i].epoch == j->state.epoch) {
            found = true;
            j->scopes[i].active = false; /* one decoded command per completed USB receive */
            break;
        }
    }
    if (!found || !((operation >= 0x1000 && operation <= 0x1FFF) ||
                     (operation >= 0x9000 && operation <= 0x9FFF))) {
        j->state.rejected_commands++;
        record(j, OCT_NATIVE_REJECTED, scope, operation, transaction, 0, -1, 0);
        return false;
    }
    if (operation == 0x1002) { /* OpenSession starts a fresh transaction namespace. */
        invalidate(j);
    }
    if (poisoned(j, transaction)) {
        j->state.rejected_commands++;
        record(j, OCT_NATIVE_REJECTED, scope, operation, transaction, 0, -3, 0);
        return false;
    }
    oct_native_pending *p = pending(j, transaction);
    if (p) {
        poison(j, transaction);
        p->active = false; /* no later response can resolve this ambiguous transaction */
        j->state.rejected_commands++;
        record(j, OCT_NATIVE_REJECTED, scope, operation, transaction, 0, -2, 0);
        return false;
    }
    p = &j->pending[0];
    for (size_t i = 0; i < OCT_NATIVE_PENDING; i++) {
        if (!j->pending[i].active) { p = &j->pending[i]; break; }
        if (j->pending[i].command_sequence < p->command_sequence) p = &j->pending[i];
    }
    if (p->active) {
        poison(j, p->transaction);
        j->state.dropped_pending++;
        record(j, OCT_NATIVE_OVERFLOW, scope, p->operation, p->transaction, 0, -2, 0);
    }
    uint64_t sequence = record(j, OCT_NATIVE_COMMAND, scope, operation, transaction, 0, 0, 0);
    if (!sequence) return false;
    *p = (oct_native_pending){ sequence, j->state.epoch, transaction, operation, true };
    j->state.commands++;
    return true;
}

bool oct_native_data_submitted(oct_native_journal *j, uint64_t transport, uint32_t transaction, uint64_t bytes)
{
    if (!live(j, transport)) return false;
    oct_native_pending *p = pending(j, transaction);
    if (!p) return false;
    j->state.data_submissions++;
    record(j, OCT_NATIVE_DATA_SUBMITTED, p->command_sequence, p->operation, transaction, 0, 0, bytes);
    return true;
}

bool oct_native_response(oct_native_journal *j, uint64_t transport, uint32_t transaction,
                         uint16_t response, bool accepted)
{
    if (!live(j, transport)) return false;
    j->state.response_calls++;
    oct_native_pending *p = pending(j, transaction);
    if (!p) {
        j->state.unmatched_responses++;
        record(j, OCT_NATIVE_RESPONSE, 0, 0, transaction, response, accepted ? 1 : 0, 0);
        return false;
    }
    uint64_t sequence = p->command_sequence;
    uint16_t operation = p->operation;
    p->active = false;
    bool valid_code = (response >= 0x2000 && response <= 0x2FFF) || (response >= 0xA000 && response <= 0xAFFF);
    if (accepted && valid_code && !poisoned(j, transaction)) j->state.paired_responses++;
    else { j->state.failed_responses++; poison(j, transaction); accepted = false; }
    record(j, OCT_NATIVE_RESPONSE, sequence, operation, transaction, response, accepted ? 1 : 0, 0);
    if (accepted && operation == 0x1003 && response == 0x2001) invalidate(j);
    return accepted;
}

void oct_native_get_snapshot(const oct_native_journal *j, oct_native_snapshot *out)
{
    if (!out) return;
    if (j) *out = j->state; else memset(out, 0, sizeof *out);
}
