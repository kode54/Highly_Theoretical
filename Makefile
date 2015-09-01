CFLAGS = -c -DEMU_COMPILE -DEMU_LITTLE_ENDIAN -DUSE_M68K -DLSB_FIRST

OBJS = ssf2vgm.o vgmwrite.o Core/arm.o Core/dcsound.o Core/m68k/m68kcpu.o\
	Core/m68k/m68kops.o Core/satsound.o Core/sega.o Core/yam.o

OPTS = -O2

all: ssf2vgm

ssf2vgm: $(OBJS) libpsflib.a
	$(CC) $(OPTS) -o $@ $^ -lz

%.o: %.c
	$(CC) $(CFLAGS) $(OPTS) -o $@ $^

clean:
	rm -f $(OBJS) ssf2vgm > /dev/null
