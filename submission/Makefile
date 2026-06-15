.PHONY: all clean archive

CFLAGS  = -O3 -march=native -funroll-loops -DNDEBUG
LDFLAGS = -libverbs
INCLUDES = -I..

all: server client

server: server.c common.h
	gcc $(CFLAGS) $(INCLUDES) server.c $(LDFLAGS) -o server

client: client.c common.h
	gcc $(CFLAGS) $(INCLUDES) client.c $(LDFLAGS) -o client

archive: all
	tar czf ex2_submission.tgz Makefile server.c client.c common.h

clean:
	rm -f server client *.tgz
