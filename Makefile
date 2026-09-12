CC = cc
AR = ar
CFLAGS = -std=c99 -Wall -Wextra -Wpedantic -Werror -O0 -g
CPPFLAGS =
LDFLAGS =
LDLIBS =

.PHONY: all check clean

all: build/libtimelite.a build/basic

build:
	mkdir -p build

build/timelite.o: timelite.c timelite.h file_io.h Makefile | build
	$(CC) $(CPPFLAGS) -I. $(CFLAGS) -c timelite.c -o $@

build/file_io.o: file_io.c file_io.h Makefile | build
	$(CC) $(CPPFLAGS) -I. $(CFLAGS) -c file_io.c -o $@

build/libtimelite.a: build/timelite.o build/file_io.o
	$(AR) rcs $@ $^

build/basic.o: examples/basic.c timelite.h Makefile | build
	$(CC) $(CPPFLAGS) -I. $(CFLAGS) -c examples/basic.c -o $@

build/basic: build/basic.o build/libtimelite.a
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ build/basic.o build/libtimelite.a $(LDLIBS)

build/file_io_test: tests/file_io_test.c file_io.h build/libtimelite.a Makefile
	$(CC) $(CPPFLAGS) -I. $(CFLAGS) $(LDFLAGS) tests/file_io_test.c build/libtimelite.a $(LDLIBS) -o $@

build/file_io_fault_test: tests/file_io_fault_test.c tests/file_io_calls.h file_io.c file_io.h Makefile | build
	$(CC) $(CPPFLAGS) -DTIMELITE_IO_TEST -I. $(CFLAGS) $(LDFLAGS) file_io.c tests/file_io_fault_test.c $(LDLIBS) -o $@

build/lifecycle_test: tests/lifecycle_test.c timelite.h file_io.h build/libtimelite.a Makefile
	$(CC) $(CPPFLAGS) -I. $(CFLAGS) $(LDFLAGS) tests/lifecycle_test.c build/libtimelite.a $(LDLIBS) -o $@

build/lifecycle_fault_test: tests/lifecycle_fault_test.c timelite.c timelite.h file_io.h Makefile | build
	$(CC) $(CPPFLAGS) -I. $(CFLAGS) $(LDFLAGS) timelite.c tests/lifecycle_fault_test.c $(LDLIBS) -o $@

check: all build/file_io_test build/file_io_fault_test build/lifecycle_test build/lifecycle_fault_test
	./build/basic
	./build/file_io_test
	./build/file_io_fault_test
	./build/lifecycle_test
	./build/lifecycle_fault_test

clean:
	rm -rf build
