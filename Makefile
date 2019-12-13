CPPFLAGS += -Isrc
CFLAGS += -fPIC
all: libtealet.so

dude.so: src/tealet.o src/switch_S.o src/switch_c.o
	$(CC) $(LDFLAGS) -fPIC -shared -o $@ $^

coreobj = src/tealet.o src/switch_S.o src/switch_c.o
libtealet.so: $(coreobj)
	$(CC) $(LDFLAGS) -shared -o $@ $^

clean:
	rm -f src/*.o tests/*.o *.out *.so
	rm -f tests/setcontext

DEBUG = #-DDEBUG_DUMP

.PHONY: test tests
tests: tests-static.out tests-dynamic.out
tests: tests/setcontext
test: export LD_LIBRARY_PATH = .
test: tests
	./tests-static.out
	./tests-dynamic.out
	./tests/setcontext
	@echo "*** All test suites passed ***"

tests/setcontext: $(coreobj)

testobj = tests/tests.o src/tools.o
tests-static.out: $(coreobj) $(testobj)
	$(CC) $(LDFLAGS) -o $@ $^ ${DEBUG}

tests-dynamic.out: $(coreobj) $(testobj) libtealet.so
	$(CC) $(LDFLAGS) -L. -g -o $@ $(coreobj) $(testobj) ${DEBUG} -ltealet
