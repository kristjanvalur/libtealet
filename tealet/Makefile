
all: tealet.so

libtealet.so: tealet.c tealet.h
	gcc -fPIC -shared -O2 -o $@ tealet.c

libtealet_g.so: tealet.c tealet.h
	gcc -fPIC -shared -g -o $@ tealet.c

clean:
	rm -fr libtealet.so libtealet_g.so
	rm -fr run_tests_*_[go]


DEBUG = #-DDEBUG_DUMP

tests: tests-static-g tests-static-o tests-dynamic-g tests-dynamic-o
	@echo "*** All test suites passed ***"

tests-static-g: tealet.c tealet.h tests.c tools.h tools.c
	gcc -g -o run_tests_static_g tealet.c tests.c tools.c ${DEBUG}
	./run_tests_static_g

tests-static-o: tealet.c tealet.h tests.c tools.h tools.c
	gcc -g -O2 -o run_tests_static_o tealet.c tests.c tools.c ${DEBUG}
	./run_tests_static_o

tests-dynamic-g: libtealet_g.so tests.c tools.h tools.c
	gcc -L. -g -o run_tests_dynamic_g tests.c tools.c ${DEBUG} -ltealet_g
	LD_LIBRARY_PATH=. ./run_tests_dynamic_g

tests-dynamic-o: libtealet.so tests.c tools.h tools.c
	gcc -L. -g -O2 -o run_tests_dynamic_o tests.c tools.c ${DEBUG} -ltealet
	LD_LIBRARY_PATH=. ./run_tests_dynamic_o
