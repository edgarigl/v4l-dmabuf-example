CC ?= gcc
CFLAGS ?= -O2 -g -Wall -Wextra -Werror -std=gnu11
LDFLAGS ?=

BINARIES := sender receiver
COMMON_OBJS := common.o

.PHONY: all clean

all: $(BINARIES)

sender: sender.o $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

receiver: receiver.o $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(BINARIES) *.o
