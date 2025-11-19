# The following can be controlled from the shell
# to run a debug build you can execute
# `CFLAGS=-g make test`
# or to test with full optimization
# `CFLAGS="-O3 -flto" LDFLAGS="-O3 -flto" make test
#

# Version
VERSION = 0.2.0
STACKMAN_VERSION = 1.2.0

CPPFLAGS += -Isrc -Istackman/stackman $(PLATFORMFLAGS) -DTEALET_WITH_STATS=1
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
tests: bin/test-setcontext bin/test-chunks bin/test-stochastic bin/test-fork
tests: export LD_RUN_PATH := bin

test: tests
	$(EMULATOR) bin/test-static > /dev/null
ifndef EMULATOR
	$(EMULATOR) bin/test-dynamic > /dev/null
endif
	$(EMULATOR) bin/test-setcontext > /dev/null
	$(EMULATOR) bin/test-stochastic -n 100 > /dev/null
	$(EMULATOR) bin/test-fork
	@echo "*** All test suites passed ***"

# Multiple chunks and sharing test
bin/test-chunks: bin tests/test_chunks.o bin/libtealet.a
	$(CC) $(LDFLAGS) $(STATIC_FLAG) -o $@ tests/test_chunks.o -ltealet

tests/test_chunks.o: tests/test_chunks.c src/tealet.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ tests/test_chunks.c

# Stochastic switching test
bin/test-stochastic: bin tests/test_stochastic.o bin/libtealet.a
	$(CC) $(LDFLAGS) $(STATIC_FLAG) -o $@ tests/test_stochastic.o -ltealet

tests/test_stochastic.o: tests/test_stochastic.c src/tealet.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ tests/test_stochastic.c

# Fork test
bin/test-fork: bin tests/test_fork.o bin/libtealet.a
	$(CC) $(LDFLAGS) $(STATIC_FLAG) -o $@ tests/test_fork.o -ltealet

tests/test_fork.o: tests/test_fork.c src/tealet.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ tests/test_fork.c

# Current tealet test
bin/test-current: bin tests/test_current.o bin/libtealet.a
	$(CC) $(LDFLAGS) $(STATIC_FLAG) -o $@ tests/test_current.o -ltealet

tests/test_current.o: tests/test_current.c src/tealet.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ tests/test_current.c

bin/test-setcontext: bin tests/setcontext.o bin/libtealet.so
	$(CC) $(LDFLAGS) $(STATIC_FLAG) -o $@ tests/setcontext.o ${DEBUG} -ltealet

bin/test-static: bin tests/tests.o bin/libtealet.a
	$(CC) $(LDFLAGS) $(STATIC_FLAG) -o $@ tests/tests.o ${DEBUG} -ltealet

bin/test-dynamic: bin tests/tests.o bin/libtealet.so
	$(CC) $(LDFLAGS) -g -o $@ tests/tests.o ${DEBUG} -ltealet

# Sanitizer tests - run on single platform for sanity checking
.PHONY: test-sanitizers test-ubsan test-valgrind

# Note: AddressSanitizer is incompatible with stack-slicing.
# ASan tracks stack boundaries and reports false positives when we
# save/restore stack data to/from the heap. Use Valgrind instead for
# memory leak detection.

# UndefinedBehaviorSanitizer - detects undefined behavior
test-ubsan: clean
	@echo "=== Building with UndefinedBehaviorSanitizer ==="
	$(MAKE) bin/test-static bin/test-setcontext bin/test-stochastic \
		CFLAGS="-fPIC -Wall $(PLATFORMFLAGS) -g -fsanitize=undefined -fno-omit-frame-pointer" \
		LDFLAGS="-Lbin -L$(LIB) $(PLATFORMFLAGS) -fsanitize=undefined" \
		STATIC_FLAG=""
	@echo "=== Running tests with UBSan ==="
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 bin/test-static
	LD_LIBRARY_PATH=bin UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 bin/test-setcontext
	LD_LIBRARY_PATH=bin UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 bin/test-stochastic -n 100
	@echo "*** UBSan tests passed ***"

# Valgrind - comprehensive memory checking
test-valgrind: clean tests
	@echo "=== Running tests with Valgrind ==="
	@which valgrind > /dev/null || (echo "ERROR: valgrind not installed" && exit 1)
	valgrind --leak-check=full --show-leak-kinds=all --error-exitcode=1 \
		--errors-for-leak-kinds=all --undef-value-errors=no bin/test-static
	LD_LIBRARY_PATH=bin valgrind --leak-check=full --show-leak-kinds=all --error-exitcode=1 \
		--errors-for-leak-kinds=all --undef-value-errors=no bin/test-setcontext
	valgrind --leak-check=full --show-leak-kinds=all --error-exitcode=1 \
		--errors-for-leak-kinds=all --undef-value-errors=no bin/test-stochastic -n 100
	@echo "*** Valgrind tests passed ***"

# Run all sanitizer tests
test-sanitizers: test-ubsan test-valgrind
	@echo "*** All sanitizer tests passed ***"
