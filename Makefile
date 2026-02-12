CC ?= gcc
CFLAGS ?= -O2 -g -Wall -Wextra -Werror -std=gnu11
LDFLAGS ?=
PKG_CONFIG ?= pkg-config

BINARIES := sender receiver
COMMON_OBJS := common.o
SDL2_CFLAGS := $(shell $(PKG_CONFIG) --cflags sdl2 2>/dev/null)
SDL2_LIBS := $(shell $(PKG_CONFIG) --libs sdl2 2>/dev/null)

ifneq ($(strip $(SDL2_LIBS)),)
BINARIES += receiver_sdl
HAVE_SDL2 := 1
endif

.PHONY: all clean

all: $(BINARIES)

sender: sender.o $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

receiver: receiver.o $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

receiver_sdl: receiver_sdl.o $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(SDL2_LIBS) $(LDFLAGS)

receiver_sdl.o: receiver_sdl.c
	$(CC) $(CFLAGS) $(SDL2_CFLAGS) -c -o $@ $<

clean:
	rm -f sender receiver receiver_sdl *.o
