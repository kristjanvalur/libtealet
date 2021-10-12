# The following can be controlled from the shell
# to run a debug build you can execute
# `CFLAGS=-g make test`
# or to test with full optimization
# `CFLAGS="-O3 -flto" LDFLAGS="-O3 -flto" make test
#

CPPFLAGS += -Isrc -Istackman/stackman
CFLAGS += -fPIC -Wall
LDFLAGS += -Lbin

# Add the path to the correct stackman libs
ABI := $(shell sh stackman/tools/abiname.sh "$(CC)" "$(CFLAGS)")
ifndef ABI
$(error Could not determine platform)
endif
LIB := stackman/lib/$(ABI)
LDFLAGS += -L$(LIB)

.PHONY: all
all: bin/libtealet.so bin/libtealet.a

coreobj = src/tealet.o #src/switch_S.o src/switch_c.o
allobj = $(coreobj) src/tools.o

src/tealet.o: src/tealet.c src/tealet.h
src/tools.o: src/tools.c src/tools.h

bin/libtealet.so: $(allobj)
	$(CC) $(LDFLAGS) -shared -o $@ $^

bin/libtealet.a: $(allobj)
	$(AR) $(ARFLAGS) -s $@ $^

clean:
	rm -f src/*.o tests/*.o *.out *.so
	rm -f bin/*

DEBUG = #-DDEBUG_DUMP

.PHONY: test tests

tests: bin/test-static bin/test-dynamic
tests: bin/test-setcontext
LDLIBS := -ltealet -lstackman
tests: export LD_RUN_PATH := bin

test: tests
	bin/test-static > /dev/null
	bin/test-dynamic > /dev/null
	bin/test-setcontext > /dev/null
	@echo "*** All test suites passed ***"


bin/test-setcontext: tests/setcontext.o bin/libtealet.so
	$(CC) $(LDFLAGS) -static -o $@ $< ${DEBUG} $(LDLIBS)

bin/test-static: tests/tests.o bin/libtealet.a
	$(CC) $(LDFLAGS) -static -o $@ $< ${DEBUG} $(LDLIBS)

bin/test-dynamic: tests/tests.o bin/libtealet.so
	$(CC) $(LDFLAGS) -g -o $@ $< ${DEBUG} $(LDLIBS)
