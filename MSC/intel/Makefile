#############################################################################################
#                                              BUILD                                        #
#############################################################################################

SRC=copy.c mov.c flops.c MSC.c validation.c
OBJ=$(patsubst %.c, %.o, $(SRC))
CFLAGS+=-march=native
#CFLAGS+=-D__FMA__ -D __AVX512__

%.o: %.c
	$(CC) $(CFLAGS) -fPIC -c -o $@ $^

%.s: %.c
	$(CC) $(CFLAGS) -S -o $@ $^

MSC.a: $(OBJ)
	ar rcs $@ $(OBJ)

clean:
	rm -rf $(OBJ) *.s MSC.a

