# The following can be controlled from the shell
# to run a debug build you can execute
# `CFLAGS=-g make test`
# or to test with full optimization
# `CFLAGS="-O3 -flto" LDFLAGS="-O3 -flto" make test
#

# Version
VERSION = 0.2.0
STACKMAN_VERSION = 1.2.0

CPPFLAGS += -Isrc -Istackman/stackman $(PLATFORMFLAGS)
CFLAGS += -fPIC -Wall $(PLATFORMFLAGS)
LDFLAGS += -Lbin

# Handle cross-compilation
ifdef PLATFORM_PREFIX
CC = $(PLATFORM_PREFIX)-gcc
CXX = $(PLATFORM_PREFIX)-g++
AR = $(PLATFORM_PREFIX)-ar
endif

# Add the path to the correct stackman libs
ABI := $(shell sh stackman/tools/abiname.sh "$(CC)" "$(CFLAGS)")
ifndef ABI
$(error Could not determine platform)
endif
LIB := stackman/lib/$(ABI)
LDFLAGS += -L$(LIB)

# Debug output (only if MAKEFILE_DEBUG is set)
ifdef MAKEFILE_DEBUG
$(info Detected ABI: $(ABI))
$(info Stackman library path: $(LIB))
$(info Checking for libstackman.a: $(wildcard $(LIB)/libstackman.a))
endif

.PHONY: all
all: bin/libtealet.so bin/libtealet.a

coreobj = src/tealet.o #src/switch_S.o src/switch_c.o
allobj = $(coreobj) src/tools.o

src/tealet.o: src/tealet.c src/tealet.h
src/tools.o: src/tools.c src/tools.h

bin:
	mkdir -p bin

bin/libtealet.so: bin $(allobj)
	$(CC) $(LDFLAGS) -shared -o $@ $(allobj)

bin/libtealet.a: bin $(allobj)
	$(AR) $(ARFLAGS) -s $@ $(allobj)

clean:
	rm -f src/*.o tests/*.o *.out *.so
	rm -f bin/*

.PHONY: abiname
abiname:
	@echo $(ABI)

DEBUG = #-DDEBUG_DUMP

.PHONY: test tests

tests: bin/test-static bin/test-dynamic
tests: bin/test-setcontext
LDLIBS := -ltealet -lstackman
tests: export LD_RUN_PATH := bin

test: tests
	$(EMULATOR) bin/test-static > /dev/null
	$(EMULATOR) bin/test-dynamic > /dev/null
	$(EMULATOR) bin/test-setcontext > /dev/null
	@echo "*** All test suites passed ***"


bin/test-setcontext: bin tests/setcontext.o bin/libtealet.so
	$(CC) $(LDFLAGS) -static -o $@ tests/setcontext.o ${DEBUG} $(LDLIBS)

bin/test-static: bin tests/tests.o bin/libtealet.a
	$(CC) $(LDFLAGS) -static -o $@ tests/tests.o ${DEBUG} $(LDLIBS)

bin/test-dynamic: bin tests/tests.o bin/libtealet.so
	$(CC) $(LDFLAGS) -g -o $@ tests/tests.o ${DEBUG} $(LDLIBS)
