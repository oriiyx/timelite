CC = cc
AR = ar
PYTHON = python3
WARNINGS = -std=c99 -Wall -Wextra -Wpedantic -Werror
CFLAGS = -O0 -g
CPPFLAGS =
LDFLAGS =
LDLIBS =

# Tests live in tools/inventory.json and run through tools/test.py, so the
# list is not repeated here. `make check` is the fast native entry point.

.PHONY: all check clean

all: build/libtimelite.a build/basic build/batches

check: all
	CC="$(CC)" AR="$(AR)" CPPFLAGS="$(CPPFLAGS)" CFLAGS="$(CFLAGS)" \
	LDFLAGS="$(LDFLAGS)" LDLIBS="$(LDLIBS)" $(PYTHON) tools/test.py run native

build:
	mkdir -p build

# build/flags records the toolchain and flags. This runs while the Makefile is
# read: when the flags change, Make's own outputs are removed before targets
# are examined, so `make CC=clang` or `make CFLAGS=-O2` rebuilds without
# `make clean` even with coarse timestamp resolution.
FLAGS_LINE = $(CC) | $(AR) | $(CPPFLAGS) | $(WARNINGS) $(CFLAGS) | $(LDFLAGS) | $(LDLIBS)
FLAGS_CHECK := $(shell mkdir -p build; printf '%s\n' '$(FLAGS_LINE)' > build/flags.new; \
	if cmp -s build/flags.new build/flags; then rm -f build/flags.new; \
	else rm -f build/*.o build/libtimelite.a build/basic build/batches; mv build/flags.new build/flags; fi)

build/timelite.o: timelite.c timelite.h file_io.h Makefile
	$(CC) $(CPPFLAGS) -I. $(WARNINGS) $(CFLAGS) -c timelite.c -o $@

build/file_io.o: file_io.c file_io.h Makefile
	$(CC) $(CPPFLAGS) -I. $(WARNINGS) $(CFLAGS) -c file_io.c -o $@

build/libtimelite.a: build/timelite.o build/file_io.o
	$(AR) rcs $@ $^

build/basic: examples/basic.c timelite.h build/libtimelite.a Makefile
	$(CC) $(CPPFLAGS) -I. $(WARNINGS) $(CFLAGS) $(LDFLAGS) examples/basic.c build/libtimelite.a $(LDLIBS) -o $@

build/batches: examples/batches.c timelite.h build/libtimelite.a Makefile
	$(CC) $(CPPFLAGS) -I. $(WARNINGS) $(CFLAGS) $(LDFLAGS) examples/batches.c build/libtimelite.a $(LDLIBS) -o $@

clean:
	rm -rf build
