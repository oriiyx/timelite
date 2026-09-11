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

build/timelite.o: timelite.c timelite.h Makefile | build
	$(CC) $(CPPFLAGS) -I. $(CFLAGS) -c timelite.c -o $@

build/libtimelite.a: build/timelite.o
	$(AR) rcs $@ $<

build/basic.o: examples/basic.c timelite.h Makefile | build
	$(CC) $(CPPFLAGS) -I. $(CFLAGS) -c examples/basic.c -o $@

build/basic: build/basic.o build/libtimelite.a
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ build/basic.o build/libtimelite.a $(LDLIBS)

# Run the example to check that the library builds and links.
check: all
	./build/basic

clean:
	rm -rf build
