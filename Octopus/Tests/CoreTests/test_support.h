#ifndef OCT_TEST_SUPPORT_H
#define OCT_TEST_SUPPORT_H

#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#elif !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../Core/oct_service.h"

extern int test_failures;
extern int test_checks;
extern const char *fixture_directory;

#define CHECK(condition)                                                                   \
    do {                                                                                   \
        test_checks++;                                                                     \
        if (!(condition)) {                                                                \
            test_failures++;                                                               \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #condition); \
        }                                                                                  \
    } while (0)

#define CHECK_EQ(actual, expected)                                                         \
    do {                                                                                   \
        long long actual_value_ = (long long)(actual);                                     \
        long long expected_value_ = (long long)(expected);                                 \
        test_checks++;                                                                     \
        if (actual_value_ != expected_value_) {                                            \
            test_failures++;                                                               \
            fprintf(stderr, "%s:%d: %s == %s failed: got %lld (0x%llX), expected %lld (0x%llX)\n", \
                    __FILE__, __LINE__, #actual, #expected, actual_value_,                 \
                    (unsigned long long)actual_value_, expected_value_,                    \
                    (unsigned long long)expected_value_);                                  \
        }                                                                                  \
    } while (0)

#define CHECK_STR(actual, expected)                                                        \
    do {                                                                                   \
        const char *actual_text_ = (actual);                                               \
        const char *expected_text_ = (expected);                                           \
        test_checks++;                                                                     \
        if (!actual_text_ || strcmp(actual_text_, expected_text_) != 0) {                  \
            test_failures++;                                                               \
            fprintf(stderr, "%s:%d: %s == \"%s\" failed: got \"%s\"\n", __FILE__, __LINE__,  \
                    #actual, expected_text_, actual_text_ ? actual_text_ : "(null)");      \
        }                                                                                  \
    } while (0)

#define CHECK_OK(expression) CHECK_EQ((expression), OCT_OK)

#define SEED_TIME 1794009600 /* 2026-11-07T00:00:00Z */

void run_store_tests(void);
void run_protocol_tests(void);
void run_responder_tests(void);
void run_service_tests(void);
void run_log_tests(void);
void run_link_tests(void);
void run_usb_tests(void);
void run_diag_tests(void);

oct_store *open_synthetic(bool scratch);

/* Minimal initiator used by the responder tests. */
typedef struct client {
    oct_store     *store;
    oct_responder *responder;
    oct_buf        scratch;
    uint32_t       transaction;
    bool           session;
} client;

void         client_open(client *c, bool scratch);
void         client_close(client *c);
void         client_open_session(client *c);
oct_exchange client_call(client *c, uint16_t operation, int count, ...);
oct_exchange client_send(client *c, uint16_t operation, const void *data, size_t length, int count, ...);
uint32_t     client_upload(client *c, uint32_t parent, const char *name, bool directory, const void *data,
                           size_t length, uint16_t *code);
size_t       object_info_dataset(oct_buf *buffer, uint32_t storage, const char *name, uint16_t format,
                                 uint32_t size);

#endif
