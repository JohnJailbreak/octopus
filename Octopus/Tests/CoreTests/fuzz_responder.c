/* Randomised and mutated PTP/MTP input against the responder and decoders.
 * Build with AddressSanitizer and UndefinedBehaviorSanitizer (make fuzz): any
 * out-of-bounds access, leak or undefined behaviour aborts the run. */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#elif !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../Core/oct_service.h"

static uint64_t state = 0x9E3779B97F4A7C15ull;

static uint64_t next_random(void)
{
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    return state * 0x2545F4914F6CDD1Dull;
}

static uint32_t below(uint32_t bound) { return (uint32_t)(next_random() % bound); }

static uint32_t interesting(void)
{
    static const uint32_t values[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0xFF, 0x3000, 0x3001, 0x3004, 0xDC07, 0xDC44,
                                       0xD402, 0x00010001, 0x7FFFFFFF, 0xFFFFFFFE, 0xFFFFFFFF };
    return below(4) == 0 ? (uint32_t)next_random() : values[below(sizeof values / sizeof values[0])];
}

static void verify(const oct_buf *out)
{
    size_t position = 0;
    while (position < out->length) {
        if (out->length - position < 12) abort();
        const uint8_t *p = out->data + position;
        uint32_t length = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
        uint16_t type = (uint16_t)(p[4] | p[5] << 8);
        if (length < 12 || length > out->length - position) abort();
        if (type == OCT_CONTAINER_RESPONSE) {
            if (length > 32 || (length - 12) % 4 != 0 || position + length != out->length) abort();
        } else if (type != OCT_CONTAINER_DATA) {
            abort();
        }
        position += length;
    }
}

int main(int argc, char **argv)
{
    long iterations = argc > 1 ? strtol(argv[1], NULL, 10) : 100000;
    static const uint16_t operations[] = { 0x1001, 0x1002, 0x1003, 0x1004, 0x1005, 0x1006, 0x1007, 0x1008,
                                           0x1009, 0x100A, 0x100B, 0x100C, 0x100D, 0x1014, 0x1015, 0x1019,
                                           0x101B, 0x9801, 0x9802, 0x9803, 0x9804, 0x9805, 0x9810, 0x0000 };
    oct_store *store = NULL;
    oct_responder *responder = NULL;
    oct_buf out, input, previous, dataset;
    oct_buf_init(&out, 0);
    oct_buf_init(&input, 0);
    oct_buf_init(&previous, 0);
    oct_buf_init(&dataset, 0);
    uint32_t transaction = 0;
    long commands = 0, data_phases = 0, mutations = 0, malformed = 0, succeeded = 0;
    bool reached[0x10000] = { false };
    for (long i = 0; i < iterations; i++) {
        if (i % 4000 == 0) {
            oct_responder_destroy(responder);
            oct_store_close(store);
            oct_synthetic_options options = { 1794009600, true };
            oct_backend backend;
            if (oct_synthetic_backend_create(&options, &backend) != OCT_OK || oct_store_open(NULL, backend, &store) != OCT_OK ||
                oct_store_index_all(store) != OCT_OK || oct_responder_create(store, NULL, &responder) != OCT_OK) {
                return 2;
            }
            transaction = 0;
        }
        oct_buf_clear(&input);
        uint32_t roll = below(100);
        if (roll < 60) {
            uint16_t operation = operations[below(sizeof operations / sizeof operations[0])];
            uint32_t params[5];
            uint32_t count = below(6);
            for (uint32_t k = 0; k < count; k++) params[k] = interesting();
            if (below(12) == 0) operation = 0x1002, params[0] = 1 + below(3), count = 1, transaction = 0;
            oct_container_command(&input, operation, below(10) ? transaction : interesting(), params, count);
            commands++;
        } else if (roll < 80) {
            static const uint16_t data_operations[] = { 0x100C, 0x100D, 0x9804 };
            uint16_t operation = data_operations[below(3)];
            size_t start = oct_container_data_begin(&input, operation, below(4) ? transaction - 1u : interesting());
            if (below(2)) {
                oct_object_info info;
                memset(&info, 0, sizeof info);
                info.storage_id = 0x00010001;
                info.format = (uint16_t)(below(3) ? 0x3004 : 0x3001);
                info.is_directory = info.format == 0x3001;
                info.size = below(64);
                snprintf(info.name, sizeof info.name, "fuzz-%u.txt", below(50));
                oct_dataset_object_info(&input, &info);
            } else {
                uint32_t length = below(300);
                for (uint32_t k = 0; k < length; k++) oct_buf_u8(&input, (uint8_t)next_random());
            }
            oct_container_data_end(&input, start);
            data_phases++;
        } else if (roll < 95 && previous.length) {
            oct_buf_bytes(&input, previous.data, previous.length);
            uint32_t edits = 1 + below(4);
            for (uint32_t k = 0; k < edits && input.length; k++) {
                switch (below(4)) {
                case 0: input.data[below((uint32_t)input.length)] ^= (uint8_t)(1u << below(8)); break;
                case 1: input.data[below((uint32_t)input.length)] = (uint8_t)next_random(); break;
                case 2: input.length = below((uint32_t)input.length + 1); break;
                default: {
                    uint32_t extra = below(16);
                    for (uint32_t e = 0; e < extra; e++) oct_buf_u8(&input, (uint8_t)next_random());
                }
                }
            }
            mutations++;
        } else {
            uint32_t length = below(48);
            for (uint32_t k = 0; k < length; k++) oct_buf_u8(&input, (uint8_t)next_random());
        }
        oct_buf_clear(&out);
        oct_status status = oct_responder_handle(responder, input.data, input.length, &out);
        if (status == OCT_E_MALFORMED) malformed++;
        if (status != OCT_OK && status != OCT_E_MALFORMED && status != OCT_E_STATE) abort();
        verify(&out);
        if (out.length >= 12 && out.data[out.length - 8] == OCT_CONTAINER_RESPONSE) {
            uint16_t code = (uint16_t)(out.data[out.length - 6] | out.data[out.length - 5] << 8);
            if (code != OCT_RC_INVALID_TRANSACTION_ID && code != OCT_RC_SESSION_NOT_OPEN) transaction++;
            if (code == OCT_RC_OK) {
                succeeded++;
                oct_responder_stats now;
                oct_responder_get_stats(responder, &now);
                reached[now.last_operation] = true;
            }
        } else if (status == OCT_OK && out.length == 0) {
            transaction++; /* command accepted, data phase pending */
        }
        if (input.length) {
            oct_buf_clear(&previous);
            oct_buf_bytes(&previous, input.data, input.length);
        }
        /* Decoders on raw noise. */
        oct_buf_clear(&dataset);
        uint32_t noise = below(160);
        for (uint32_t k = 0; k < noise; k++) oct_buf_u8(&dataset, (uint8_t)next_random());
        oct_object_info_request request;
        (void)oct_dataset_parse_object_info(dataset.data, dataset.length, &request);
        oct_reader reader;
        char text[64];
        oct_reader_init(&reader, dataset.data, dataset.length);
        (void)oct_read_string(&reader, text, 1 + below(sizeof text));
        oct_container container;
        (void)oct_container_parse(dataset.data, dataset.length, &container);
        if (i % 1000 == 999) oct_responder_reset(responder);
    }
    oct_responder_stats stats;
    oct_responder_get_stats(responder, &stats);
    int reached_count = 0;
    for (size_t k = 0; k < sizeof reached; k++) reached_count += reached[k];
    printf("fuzz: %ld iterations (%ld commands, %ld data phases, %ld mutations); %ld succeeded across %d operations, "
           "%ld malformed; no faults\n", iterations, commands, data_phases, mutations, succeeded, reached_count, malformed);
    oct_responder_destroy(responder);
    oct_store_close(store);
    oct_buf_free(&out);
    oct_buf_free(&input);
    oct_buf_free(&previous);
    oct_buf_free(&dataset);
    return 0;
}
