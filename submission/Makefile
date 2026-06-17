.PHONY: all clean archive

CFLAGS   = -O3 -march=native -funroll-loops -DNDEBUG
LDFLAGS  = -libverbs
INCLUDES = -I. -I..

# In-repo build: files live in parent dir (../server.c, ../common.h).
# Extracted archive: files are flat in the current directory.
# $(wildcard ...) picks whichever exists.
SERVER_SRC = $(if $(wildcard server.c),server.c,../server.c)
COMMON_H   = $(if $(wildcard common.h),common.h,../common.h)

all: server client

server: $(SERVER_SRC) $(COMMON_H)
	gcc $(CFLAGS) $(INCLUDES) $(SERVER_SRC) $(LDFLAGS) -o server

client: client.c $(COMMON_H)
	gcc $(CFLAGS) $(INCLUDES) client.c $(LDFLAGS) -o client

archive: all
	cp ../server.c ../common.h .
	tar czf ex2_submission.tgz Makefile client.c server.c common.h
	rm -f server.c common.h

clean:
	rm -f server client *.tgz
