# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds the crt-bridge receiver daemon, its volume helper, and
# crt-bridge-config (reads the settings put on the boot stick).
#
#   make SWITCHRES_DIR=/path/to/switchres
#   make install DESTDIR=/some/root
#
# SWITCHRES_DIR is a switchres source tree where `make libswitchres` has run:
# the daemon links its static archive, libswitchres.a. switchres is C++ with
# static initialisers, so the final link goes through the C++ driver.
# image/build-image.sh builds switchres at the pinned commit and calls this
# file; nothing here needs to run on the receiver itself.

CC      ?= gcc
CXX     ?= g++
PREFIX  ?= /usr/local

SWITCHRES_DIR ?= ../switchres
HDA_SRC       ?= ../tools/hda-volume.c

CFLAGS   = -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -O2
CPPFLAGS = -D_POSIX_C_SOURCE=200809L -I$(SWITCHRES_DIR) -I/usr/include/libdrm
LDLIBS   = $(SWITCHRES_DIR)/libswitchres.a -ldrm -llz4 -lpthread -lrt -lm -ldl

SRCS = main.c proto.c net.c blit.c safety.c monitor.c jitter.c mmio.c pcm.c \
       idle.c lz4dec.c input.c fieldphase.c
OBJS = $(SRCS:.c=.o)

BIN = crt-bridge-rcv
VOL = hda-volume
CFG = crt-bridge-config
CFG_SRCS = crt-bridge-config.c stickcfg.c

.PHONY: all install clean

all: $(BIN) $(VOL) $(CFG)

%.o: %.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(BIN): $(OBJS) $(SWITCHRES_DIR)/libswitchres.a
	$(CXX) -O2 -o $@ $(OBJS) $(LDLIBS)

$(VOL): $(HDA_SRC)
	$(CC) -std=c11 -Wall -Wextra -O2 -o $@ $(HDA_SRC)

# Plain C, no library: it does not link switchres.
$(CFG): $(CFG_SRCS) stickcfg.h
	$(CC) $(CFLAGS) -D_POSIX_C_SOURCE=200809L -o $@ $(CFG_SRCS)

install: all
	install -Dm0755 $(BIN) $(DESTDIR)$(PREFIX)/bin/$(BIN)
	install -Dm0755 $(VOL) $(DESTDIR)$(PREFIX)/bin/$(VOL)
	install -Dm0755 $(CFG) $(DESTDIR)$(PREFIX)/bin/$(CFG)

clean:
	rm -f $(BIN) $(VOL) $(CFG) $(OBJS)
