CPPFLAGS = -Isrc
all: libtealet.so

SOSRC = src/tealet.c $(ASM)
SOSRCALL = src/tealet.h $(SOSRC)
libtealet.so: platform $(SOSRCALL)
	$(CC) $(LDFLAGS) -fPIC -shared -O2 -o $@ $(SOSRC)

libtealet_g.so: platform $(SOSRCALL)
	$(CC) $(LDFLAGS) -fPIC -shared -g -o $@ $(SOSRC)

clean:
	rm -fr libtealet.so libtealet_g.so
	rm -fr platform.out
	rm -fr run_tests_*_[go].out

DEBUG = #-DDEBUG_DUMP

# platform selection
platform.out: src/platf_tealet/tealet_platformselect.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $^

.PHONY: platform
platform: platform.out
	# print platform intofmation
	./platform.out arg
	
# get extra assembler file to use
ASM = $(shell ./platform.out)

.PHONY: test
test: tests-static-g.out tests-static-o.out tests-dynamic-g.out tests-dynamic-o.out
test: export LD_LIBRARY_PATH = .
test: 
	./tests-static-g.out
	./tests-static-o.out
	./tests-dynamic-g.out
	./tests-dynamic-o.out
	@echo "*** All test suites passed ***"

TSSRC = $(SOSRC) tests/tests.c src/tools.c
TSDEPS = $(SOSRCALL) src/tools.h platform
tests-static-g.out: $(TSDEPS)
	$(CC) $(CFLAGS) $(CPPFLAGS) -g -o $@ $(TSSRC) ${DEBUG}

tests-static-o.out: $(TSDEPS)
	$(CC) $(CFLAGS) $(CPPFLAGS) -g -O2 -o $@ $(TSSRC) ${DEBUG}

TDSRC = tests/tests.c src/tools.c
TDDEPS = $(TDSRC) src/tools.h platform
tests-dynamic-g.out: libtealet_g.so $(TDDEPS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -L. -g -o $@ $(TDSRC) ${DEBUG} -ltealet_g

tests-dynamic-o.out: libtealet.so $(TDDEPS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -L. -g -O2 -o $@ $(TDSRC) ${DEBUG} -ltealet
