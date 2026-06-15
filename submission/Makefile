.PHONY: all clean

all: server client

CFLAGS = -O3 -march=native -funroll-loops -DNDEBUG
LDFLAGS = -libverbs

server: server.c ../common.h
	gcc $(CFLAGS) server.c $(LDFLAGS) -o server

client: client.c ../common.h
	gcc $(CFLAGS) client.c $(LDFLAGS) -o client

clean:
	rm -f server client
