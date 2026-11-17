# Host build of the TINY engine: runs the exact same C code as the watch
# (minus the PIE SIMD kernel) so answers can be checked on a PC.
#
#   make -C host test      - compare against the original DOS program
#   make -C host bench     - letters/second on this PC
CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra -std=c11 -D_POSIX_C_SOURCE=200809L
ENGINE  := ../components/tiny_engine
SRC     := $(ENGINE)/src/tiny_engine.c $(ENGINE)/src/tiny_kernels.c $(ENGINE)/src/tiny_port_host.c tiny_host.c
MODEL   := ../model/TINY.BIN
Q       := tests/questions.txt

all: tiny_host

tiny_host: $(SRC) $(ENGINE)/include/tiny_engine.h
	$(CC) $(CFLAGS) -I$(ENGINE)/include -I$(ENGINE)/src -o $@ $(SRC) -lm

# The original program, built unchanged in its plain text mode.
tiny_ref: ../legacy/dos/TINY.C
	tr -d '\r' < $< > tiny_ref.c
	$(CC) -O2 -w -o $@ tiny_ref.c -lm
	rm -f tiny_ref.c

build/ref.txt: tiny_ref $(Q)
	mkdir -p build
	cd build && cp ../$(MODEL) TINY.BIN && ../tiny_ref 0 < ../$(Q) | sed -n 's/^>   //p' > ref.txt

test: tiny_host build/ref.txt
	./tiny_host -m $(MODEL) --float < $(Q) > build/float.txt
	./tiny_host -m $(MODEL) < $(Q) > build/int8.txt
	python3 tests/compare.py $(Q) build/ref.txt build/float.txt build/int8.txt

bench: tiny_host
	./tiny_host -m $(MODEL) --stats < $(Q) > /dev/null
	./tiny_host -m $(MODEL) --float --stats < $(Q) > /dev/null

clean:
	rm -rf tiny_host tiny_ref build

.PHONY: all test bench clean
