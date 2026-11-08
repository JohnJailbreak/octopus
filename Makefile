# Native build and tests for the Octopus C core (provider, protocol, service), the update helper and the spawn
# bridge that starts it, and the repository's checks.
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
PYTHON    ?= python3
# The update path's C code (the root helper and the spawn bridge), and the repository's own checks.
HELPER_TESTS := Octopus/Tests/HelperTests/test_helper.py
REPO_TESTS   := Octopus/Tests/RepoTests/test_repository.py

ifeq ($(shell uname -s),Linux)
SAN_ENV   := ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=print_stacktrace=1
else
SAN_ENV   := ASAN_OPTIONS=abort_on_error=1 UBSAN_OPTIONS=print_stacktrace=1
endif

PLAIN     := $(patsubst Octopus/%.c,build/plain/%.o,$(CORE))
SANITIZED := $(patsubst Octopus/%.c,build/san/%.o,$(CORE))

.PHONY: tsan all test sanitize fuzz analyze check clean

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
	CC="$(CC)" HELPER_CFLAGS="$(STD) $(CFLAGS) $(STRICT)" $(PYTHON) $(HELPER_TESTS)
	$(PYTHON) $(REPO_TESTS)

sanitize: build/core-tests-san
	TZ=UTC0 $(SAN_ENV) ./build/core-tests-san $(FIXTURES)
	CC="$(CC)" HELPER_CFLAGS="$(STD) $(CFLAGS) $(WARN) $(SAN)" $(SAN_ENV) $(PYTHON) $(HELPER_TESTS)

# ThreadSanitizer over the whole C suite: the debug log and the transport adapter are shared between threads.
tsan: $(CORE) $(TESTS) $(HEADERS)
	@mkdir -p build
	$(CC) $(STD) $(CFLAGS) $(WARN) -fsanitize=thread $(CORE) $(TESTS) $(LDLIBS) -o build/core-tests-tsan
	TZ=UTC0 TSAN_OPTIONS=halt_on_error=1 ./build/core-tests-tsan $(FIXTURES)

fuzz: build/core-fuzz
	$(SAN_ENV) ./build/core-fuzz $(FUZZ_RUNS)

# GCC's static analyzer over the core, the update helper and the spawn bridge (GCC only).
analyze:
	@for f in $(CORE); do $(CC) $(STD) $(WARN) -fanalyzer -c $$f -o /dev/null || exit 1; done
	@$(CC) $(STD) $(WARN) -fanalyzer -DOCT_HELPER_TEST -c Octopus/RootHelper/main.c -o /dev/null
	@$(CC) $(STD) $(WARN) -fanalyzer -c Octopus/App/Spawn/oct_spawn.c -o /dev/null
	@echo "analyzer: no findings"

check: test sanitize fuzz

clean:
	rm -rf build

# Focused portable boundary suite; does not replace the full POSIX/Apple CI checks.
.PHONY: test-usb
test-usb: $(PLAIN) $(HEADERS) Octopus/Tests/CoreTests/test_usb.c Octopus/Tests/CoreTests/usb_main.c
	$(CC) $(STD) $(CFLAGS) $(WARN) $(PLAIN) Octopus/Tests/CoreTests/test_usb.c Octopus/Tests/CoreTests/usb_main.c $(LDLIBS) -o build/usb-tests
	TZ=UTC0 ./build/usb-tests
