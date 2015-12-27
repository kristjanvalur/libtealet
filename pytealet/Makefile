
all: _tealet.so _tealet_d.so

tests: _tealet.so
	python test_tealet.py
	python test_greenlet.py

INC = -I/usr/include/python2.7 -I ../tealet

_tealet.so: _tealet.c ../tealet/tealet.c ../tealet/tealet.h
	gcc $(INC) -fPIC -shared -O2 -o $@ _tealet.c ../tealet/tealet.c

_tealet_d.so: _tealet.c ../tealet/tealet.c ../tealet/tealet.h
	gcc $(INC) -fPIC -shared -g -o $@ _tealet.c ../tealet/tealet.c

clean:
	rm -fr _tealet.so _tealet_d.so


DEBUG = #-DDEBUG_DUMP

