SHELL := /bin/sh

CC ?= cc
CXX ?= c++
AR ?= ar
PREFIX ?= /usr/local
DESTDIR ?=
BUILD ?= build/release

CPPFLAGS ?=
CFLAGS ?= -O2 -g
CXXFLAGS ?= -O2 -g
WARNINGS := -Wall -Wextra -Wpedantic -Werror -Wconversion -Wshadow \
	-Wstrict-prototypes -Wmissing-prototypes -Wformat=2
COMMON_CPPFLAGS := -Iinclude -I.
COMMON_CFLAGS := -std=c11 $(WARNINGS) -pthread
COMMON_CXXFLAGS := -std=c++17 -Wall -Wextra -Wpedantic -Werror

VERSION := $(shell sed -n '1p' VERSION)
UNAME_S := $(shell uname -s)
PLATFORM_SOURCE :=
ifeq ($(UNAME_S),Linux)
PLATFORM_SOURCE := src/loop_epoll.c
endif
ifeq ($(UNAME_S),Darwin)
PLATFORM_SOURCE := src/loop_kqueue.c
endif
SOURCES := src/result.c src/clock.c src/fd.c src/socket.c src/wakeup.c src/thread.c \
	src/loop.c src/loop_poll.c $(PLATFORM_SOURCE)
OBJECTS := $(patsubst %.c,$(BUILD)/%.o,$(SOURCES))
LIB := $(BUILD)/lib/libmaelys_sys.a
TEST := $(BUILD)/tests/test_sys
CONSUMER_TEST := $(BUILD)/tests/test_consumers
STRESS_TEST := $(BUILD)/tests/test_stress
FAULT_TEST := $(BUILD)/tests/test_faults
TESTS := $(TEST) $(CONSUMER_TEST) $(STRESS_TEST) $(FAULT_TEST)
HEADER_CPP := $(BUILD)/tests/header_cpp
PC := $(BUILD)/pkgconfig/maelys-sys.pc
EXAMPLE_NAMES := tcp-relay timer-server cross-thread-wakeup
EXAMPLES := $(addprefix $(BUILD)/examples/,$(EXAMPLE_NAMES))
BENCHMARK := $(BUILD)/benchmarks/reactor-maelys

.PHONY: all check test stress fault-check consumer-check clean header-check \
	check-version audit asan ubsan asan-ubsan tsan analyze install \
	install-check uninstall dist examples examples-check benchmark \
	mutation-check package-release package-linux

all: $(LIB)

$(BUILD)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(COMMON_CPPFLAGS) $(CFLAGS) $(COMMON_CFLAGS) -c $< -o $@

$(LIB): $(OBJECTS)
	@mkdir -p $(@D)
	$(AR) rcs $@ $^

$(TEST): tests/test_sys.c $(LIB)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(COMMON_CPPFLAGS) $(CFLAGS) $(COMMON_CFLAGS) $< $(LIB) $(LDFLAGS) -o $@

$(BUILD)/tests/test_%: tests/test_%.c $(LIB)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(COMMON_CPPFLAGS) $(CFLAGS) $(COMMON_CFLAGS) $< $(LIB) $(LDFLAGS) -o $@

$(BUILD)/examples/%: examples/%.c $(LIB)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(COMMON_CPPFLAGS) $(CFLAGS) $(COMMON_CFLAGS) $< $(LIB) $(LDFLAGS) -o $@

$(BENCHMARK): benchmarks/reactor_maelys.c $(LIB)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(COMMON_CPPFLAGS) $(CFLAGS) $(COMMON_CFLAGS) $< $(LIB) $(LDFLAGS) -o $@

$(HEADER_CPP): tests/header_cpp.cpp
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(COMMON_CPPFLAGS) $(CXXFLAGS) $(COMMON_CXXFLAGS) $< -c -o $@.o
	$(CXX) $@.o $(LDFLAGS) -o $@

$(PC): pkgconfig/maelys-sys.pc.in VERSION
	@mkdir -p $(@D)
	sed -e 's|@PREFIX@|$(PREFIX)|g' -e 's|@VERSION@|$(VERSION)|g' $< > $@

test: $(TESTS)
	$(TEST)
	$(CONSUMER_TEST)
	$(FAULT_TEST)
	$(STRESS_TEST)

consumer-check: $(CONSUMER_TEST)
	$(CONSUMER_TEST)

fault-check: $(FAULT_TEST)
	$(FAULT_TEST)

stress: $(STRESS_TEST)
	$(STRESS_TEST)

header-check: $(HEADER_CPP)
	$(HEADER_CPP)

check-version:
	@test "$(VERSION)" = "$$(sed -n 's/^#define MAELYS_SYS_VERSION "\([^"]*\)"/\1/p' include/maelys/sys/version.h)"

audit:
	./scripts/audit-boundaries.sh

examples: $(EXAMPLES)

examples-check: examples
	./scripts/examples-check.sh $(BUILD)/examples

mutation-check:
	./scripts/mutation-check.sh

benchmark: $(BENCHMARK)
	./scripts/run-benchmarks.sh $(BUILD)

check: test header-check check-version audit examples-check

asan:
	$(MAKE) clean
	$(MAKE) check BUILD=build/asan CFLAGS='-O1 -g -fsanitize=address -fno-omit-frame-pointer' LDFLAGS='-fsanitize=address'

ubsan:
	$(MAKE) clean
	$(MAKE) check BUILD=build/ubsan CFLAGS='-O1 -g -fsanitize=undefined -fno-omit-frame-pointer' LDFLAGS='-fsanitize=undefined'

asan-ubsan:
	$(MAKE) clean
	$(MAKE) check BUILD=build/asan-ubsan CFLAGS='-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer' LDFLAGS='-fsanitize=address,undefined'

tsan:
	$(MAKE) clean
	$(MAKE) check BUILD=build/tsan CFLAGS='-O1 -g -fsanitize=thread -fno-omit-frame-pointer' LDFLAGS='-fsanitize=thread'

analyze:
	@for source in $(SOURCES); do \
		$(CC) --analyze -Xanalyzer -analyzer-output=text \
			$(CPPFLAGS) $(COMMON_CPPFLAGS) -std=c11 -pthread $$source || exit 1; \
	done

install: $(LIB) $(PC)
	install -d $(DESTDIR)$(PREFIX)/lib $(DESTDIR)$(PREFIX)/include/maelys/sys \
		$(DESTDIR)$(PREFIX)/lib/pkgconfig
	install -m 0644 $(LIB) $(DESTDIR)$(PREFIX)/lib/libmaelys_sys.a
	install -m 0644 include/maelys/sys.h $(DESTDIR)$(PREFIX)/include/maelys/sys.h
	install -m 0644 include/maelys/sys/*.h $(DESTDIR)$(PREFIX)/include/maelys/sys/
	install -m 0644 $(PC) $(DESTDIR)$(PREFIX)/lib/pkgconfig/maelys-sys.pc

install-check: all
	./scripts/install-check.sh

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/lib/libmaelys_sys.a \
		$(DESTDIR)$(PREFIX)/include/maelys/sys.h \
		$(DESTDIR)$(PREFIX)/lib/pkgconfig/maelys-sys.pc
	rm -rf $(DESTDIR)$(PREFIX)/include/maelys/sys

dist: check
	@mkdir -p dist
	git archive --format=tar --prefix=maelys-system-$(VERSION)/ HEAD | \
		gzip -n > dist/maelys-system-$(VERSION).tar.gz

package-release:
	./scripts/package-release.sh

package-linux:
	./scripts/package-release.sh --linux-packages

clean:
	rm -rf build
