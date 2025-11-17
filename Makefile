RPCGEN = rpcgen
CC = gcc
CFLAGS = -Wall -g $(shell pkg-config --cflags libtirpc)
LDLIBS = $(shell pkg-config --libs libtirpc) -lnsl

all: coordinator participant

commit.h commit_xdr.c commit_clnt.c commit_svc.c: commit.x
	$(RPCGEN) -C -N commit.x

coordinator: commit_clnt.c commit_xdr.c coordinator.c
	$(CC) $(CFLAGS) -o $@ coordinator.c commit_clnt.c commit_xdr.c $(LDLIBS)

participant: commit_svc.c commit_xdr.c participant.c
	$(CC) $(CFLAGS) -o $@ participant.c commit_svc.c commit_xdr.c $(LDLIBS)

clean:
	rm -f coordinator participant *.o commit.h commit_xdr.c commit_clnt.c commit_svc.c
