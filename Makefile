# Native build and tests for the Octopus C core (provider, protocol, service).
# Works with GCC or Clang on Linux and macOS. The iOS app is built by Xcode.

CC        ?= cc
CFLAGS    ?= -O1 -g
STD       := -std=c11
WARN      := -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Wvla -Wstrict-prototypes -Wmissing-prototypes -Werror
STRICT    := $(WARN) -Wconversion -Wsign-conversion
SAN       := -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all
LDLIBS    := -lpthread

CORE      := $(wildcard Octopus/Core/*.c Octopus/Protocol/*.c)
HEADERS   := $(wildcard Octopus/Core/*.h Octopus/Protocol/*.h Octopus/Tests/CoreTests/*.h)
TESTS     := $(wildcard Octopus/Tests/CoreTests/test_*.c)
FUZZER    := Octopus/Tests/CoreTests/fuzz_responder.c
FIXTURES  := Octopus/Tests/CoreTests/fixtures
FUZZ_RUNS ?= 200000

ifeq ($(shell uname -s),Linux)
SAN_ENV   := ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=print_stacktrace=1
else
SAN_ENV   := ASAN_OPTIONS=abort_on_error=1 UBSAN_OPTIONS=print_stacktrace=1
endif

PLAIN     := $(patsubst Octopus/%.c,build/plain/%.o,$(CORE))
SANITIZED := $(patsubst Octopus/%.c,build/san/%.o,$(CORE))

.PHONY: all test sanitize fuzz analyze check clean

all: test

build/plain/%.o: Octopus/%.c $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(STD) $(CFLAGS) $(STRICT) -c $< -o $@

build/san/%.o: Octopus/%.c $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(STD) $(CFLAGS) $(STRICT) $(SAN) -c $< -o $@

build/core-tests: $(PLAIN) $(TESTS) $(HEADERS)
	$(CC) $(STD) $(CFLAGS) $(WARN) $(PLAIN) $(TESTS) $(LDLIBS) -o $@

build/core-tests-san: $(SANITIZED) $(TESTS) $(HEADERS)
	$(CC) $(STD) $(CFLAGS) $(WARN) $(SAN) $(SANITIZED) $(TESTS) $(LDLIBS) -o $@

build/core-fuzz: $(SANITIZED) $(FUZZER) $(HEADERS)
	$(CC) $(STD) $(CFLAGS) $(WARN) $(SAN) $(SANITIZED) $(FUZZER) $(LDLIBS) -o $@

test: build/core-tests
	TZ=UTC0 ./build/core-tests $(FIXTURES)

sanitize: build/core-tests-san
	TZ=UTC0 $(SAN_ENV) ./build/core-tests-san $(FIXTURES)

fuzz: build/core-fuzz
	$(SAN_ENV) ./build/core-fuzz $(FUZZ_RUNS)

# GCC's static analyzer over the core sources (GCC only).
analyze:
	@for f in $(CORE); do $(CC) $(STD) $(WARN) -fanalyzer -c $$f -o /dev/null || exit 1; done
	@echo "analyzer: no findings"

check: test sanitize fuzz

clean:
	rm -rf build
