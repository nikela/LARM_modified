#############################################################################################
#                                      Configuration                                        #
#############################################################################################
#Number of samples per memory.
N_SAMPLES=16
#Use PAPI and compile tool for code sampling
PAPI=no
# Benchmark for intel plateform
MSC=intel

#############################################################################################
#                                              BUILD                                        #
#############################################################################################
export CC=gcc
export OMP_FLAG= -fopenmp
export CFLAGS= -g \
	-DROOFLINE_MIN_DUR=$(LAST)\
	-DROOFLINE_N_SAMPLES=$(N_SAMPLES)\
	-DCC=$(CC)\
	-DOMP_FLAG=$(OMP_FLAG) \
	-I/home/users/nikela/local/hwloc/include

LDFLAGS=-ldl -L/home/users/nikela/local/hwloc/lib -lhwloc -lm $(OMP_FLAG)

ifdef DEBUG
CFLAGS+=-Wall -Werror -Wextra -DDEBUG2 -DDEBUG1 -I/home/users/nikela/local/hwloc/include
endif

SRC=stream.c output.c types.c stats.c topology.c roofline.c list.c
OBJ=$(patsubst %.c, %.o, $(SRC)) MSC/$(MSC)/MSC.a
PREFIX=$(HOME)/local
BIN=roofline
SO=roofline.so
LIB=librfsampling.so
LIB_DEFINE=
LIB_LDFLAGS=-lrt -L/home/users/nikela/local/hwloc/lib -lhwloc $(OMP_FLAG)

.PHONY: all install uninstall clean

ifeq (yes, $(PAPI))
LIB_DEFINE=-D_PAPI_
LIB_LDFLAGS+=-lpapi
endif

all: $(BIN) $(SO) $(LIB)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $^ $(OMP_FLAG) -fPIC

MSC/$(MSC)/MSC.a: MSC/$(MSC)/*.c
	make -C MSC/$(MSC)

$(BIN): main.c $(OBJ)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(SO): $(OBJ)
	$(CC) $(CFLAGS) -fPIC -shared $^ -o $@ $(LDFLAGS)

$(LIB): sampling.c list.o
	$(CC) $(CFLAGS) $(LIB_DEFINE) -fPIC -shared $^ -o $@ $(LIB_LDFLAGS)

test: test_sampling.c $(LIB)
	$(CC) $(CFLAGS) $< -o $@ -lrfsampling
	./$@

install:
	cp sampling.h $(PREFIX)/include/
	cp $(LIB) $(PREFIX)/lib/
	cp $(BIN) $(PREFIX)/bin/

uninstall:
	rm $(PREFIX)/include/sampling.h $(PREFIX)/lib/$(LIB) $(PREFIX)/bin/$(BIN)

clean:
	rm -f *.o *.s *.so roofline
	make -C MSC/$(MSC) clean

