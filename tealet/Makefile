
all: tealet.so

tealet.so: tealet.c tealet.h
	gcc -fPIC -shared -O2 -o $@ tealet.c

tealet_g.so: tealet.c tealet.h
	gcc -fPIC -shared -g -o $@ tealet.c

clean:
	rm -fr tealet.so tealet_g.so
	rm -fr run_tests_*_[go]


DEBUG = #-DDEBUG_DUMP

tests: tests-static-g tests-static-o tests-dynamic-g tests-dynamic-o
	@echo "*** All test suites passed ***"

tests-static-g: tealet.c tealet.h tests.c
	gcc -g -o run_tests_static_g tealet.c tests.c ${DEBUG}
	run_tests_static_g

tests-static-o: tealet.c tealet.h tests.c
	gcc -g -O2 -o run_tests_static_o tealet.c tests.c ${DEBUG}
	run_tests_static_o

tests-dynamic-g: tealet_g.so tests.c
	gcc -g -o run_tests_dynamic_g tealet_g.so tests.c ${DEBUG}
	LD_LIBRARY_PATH=. run_tests_dynamic_g

tests-dynamic-o: tealet.so tests.c
	gcc -g -O2 -o run_tests_dynamic_o tealet.so tests.c ${DEBUG}
	LD_LIBRARY_PATH=. run_tests_dynamic_o
