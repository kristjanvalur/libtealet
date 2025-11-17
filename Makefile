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
LDFLAGS += -Lbin $(PLATFORMFLAGS)

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
	$(CC) $(LDFLAGS) -shared -o $@ $(allobj) -lstackman

bin/libtealet.a: bin $(allobj)
	$(AR) -rcs $@ $(allobj)
	@# Extract stackman objects and merge into libtealet.a
	@mkdir -p bin/tmp_ar
	@cd bin/tmp_ar && $(AR) -x ../../$(LIB)/libstackman.a
	@$(AR) -rs $@ bin/tmp_ar/*.o
	@rm -rf bin/tmp_ar

clean:
	rm -f src/*.o tests/*.o *.out *.so
	rm -f bin/*

.PHONY: abiname
abiname:
	@echo $(ABI)

DEBUG = #-DDEBUG_DUMP

# macOS doesn't support static linking
STATIC_FLAG := -static
ifeq ($(shell uname -s),Darwin)
	STATIC_FLAG :=
endif

.PHONY: test tests

tests: bin/test-static bin/test-dynamic
tests: bin/test-setcontext
LDLIBS := -ltealet -lstackman
tests: export LD_RUN_PATH := bin

test: tests
	$(EMULATOR) bin/test-static > /dev/null
ifndef EMULATOR
	$(EMULATOR) bin/test-dynamic > /dev/null
endif
	$(EMULATOR) bin/test-setcontext > /dev/null
	@echo "*** All test suites passed ***"


bin/test-setcontext: bin tests/setcontext.o bin/libtealet.so
	$(CC) $(LDFLAGS) $(STATIC_FLAG) -o $@ tests/setcontext.o ${DEBUG} $(LDLIBS)

bin/test-static: bin tests/tests.o bin/libtealet.a
	$(CC) $(LDFLAGS) $(STATIC_FLAG) -o $@ tests/tests.o ${DEBUG} -ltealet

bin/test-dynamic: bin tests/tests.o bin/libtealet.so
	$(CC) $(LDFLAGS) -g -o $@ tests/tests.o ${DEBUG} $(LDLIBS)
