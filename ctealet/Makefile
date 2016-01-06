ASM = $(shell cc platf_tealet/tealet_platformselect.c && ./a.out)

all: libtealet.so

SOSRC = tealet.c $(ASM)
SOSRCALL = tealet.h $(SOSRC)
libtealet.so: $(SOSRCALL)
	gcc -fPIC -shared -O2 -o $@  $(SOSRC)

libtealet_g.so: $(SOSRCALL)
	gcc -fPIC -shared -g -o $@ $(SOSRC)

clean:
	rm -fr libtealet.so libtealet_g.so
	rm -fr run_tests_*_[go]


DEBUG = #-DDEBUG_DUMP

tests: tests-static-g tests-static-o tests-dynamic-g tests-dynamic-o
	@echo "*** All test suites passed ***"

TSSRC = $(SOSRC) tests.c tools.c
TSSRCALL = $(SOSRCALL) tools.h
tests-static-g: $(TSSRCALL)
	gcc -g -o run_tests_static_g $(TSSRC) ${DEBUG}
	./run_tests_static_g

tests-static-o: $(TSSRCALL)
	gcc -g -O2 -o run_tests_static_o $(TSSRC) ${DEBUG}
	./run_tests_static_o

TDSRC = tests.c tools.c
TDSRCALL = tests.c tools.c tools.h
tests-dynamic-g: libtealet_g.so $(TDSRCALL)
	gcc -L. -g -o run_tests_dynamic_g $(TDSRC) ${DEBUG} -ltealet_g
	LD_LIBRARY_PATH=. ./run_tests_dynamic_g

tests-dynamic-o: libtealet.so $(TDSRCALL)
	gcc -L. -g -O2 -o run_tests_dynamic_o $(TDSRC) ${DEBUG} -ltealet
	LD_LIBRARY_PATH=. ./run_tests_dynamic_o
