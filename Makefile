CC = gcc
CPPFLAGS = -Isrc
ASM = $(shell cc src/platf_tealet/tealet_platformselect.c && ./a.out)

all: libtealet.so

SOSRC = src/tealet.c $(ASM)
SOSRCALL = src/tealet.h $(SOSRC)
libtealet.so: $(SOSRCALL)
	$(CC) $(LDFLAGS) -fPIC -shared -O2 -o $@ $(SOSRC)

libtealet_g.so: $(SOSRCALL)
	$(CC) $(LDFLAGS) -fPIC -shared -g -o $@ $(SOSRC)

clean:
	rm -fr libtealet.so libtealet_g.so
	rm -fr run_tests_*_[go].out


DEBUG = #-DDEBUG_DUMP

tests: tests-static-g.out tests-static-o.out tests-dynamic-g.out tests-dynamic-o.out
	@echo "*** All test suites passed ***"

TSSRC = $(SOSRC) tests/tests.c src/tools.c
TSDEPS = $(SOSRCALL) src/tools.h
tests-static-g.out: $(TSDEPS)
	$(CC) $(CFLAGS) $(CPPFLAGS) -g -o run_tests_static_g.out $(TSSRC) ${DEBUG}
	./run_tests_static_g.out

tests-static-o.out: $(TSDEPS)
	$(CC) $(CFLAGS) $(CPPFLAGS) -g -O2 -o run_tests_static_o.out $(TSSRC) ${DEBUG}
	./run_tests_static_o.out

TDSRC = tests/tests.c src/tools.c
TDDEPS = $(TDSRC) src/tools.h
tests-dynamic-g.out: libtealet_g.so $(TDDEPS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -L. -g -o run_tests_dynamic_g.out $(TDSRC) ${DEBUG} -ltealet_g
	LD_LIBRARY_PATH=. ./run_tests_dynamic_g.out

tests-dynamic-o.out: libtealet.so $(TDDEPS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -L. -g -O2 -o run_tests_dynamic_o.out $(TDSRC) ${DEBUG} -ltealet
	LD_LIBRARY_PATH=. ./run_tests_dynamic_o.out
