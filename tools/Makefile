# SPDX-License-Identifier: GPL-2.0
CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
LDLIBS   = $(shell pkg-config --libs libusb-1.0)
CPPFLAGS = $(shell pkg-config --cflags libusb-1.0)

# The BT.656 assembler is compiled from driver/ into BOTH this tool and the
# kernel module, from one source, deliberately -- see driver/gb-bt656.h. Every
# change to it can then be replayed against a saved capture with `gbcap decode`
# and checked against tools/bt656-decode.py for byte-identical output, with no
# device attached and no risk of an oops in softirq context.
ASM = ../driver/gb-bt656.c
ASMH = ../driver/gb-bt656.h

all: gbfw gbcap gbloop gblat gbtest gbcxreg gbstop

gbfw: gbfw.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $< $(LDLIBS)

gbcap: gbcap.c $(ASM) $(ASMH)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ gbcap.c $(ASM) $(LDLIBS)

# gblat measures how old a frame is by the time userspace sees it -- the number
# that decides whether a console is playable through this device.
gblat: gblat.c
	$(CC) $(CFLAGS) -o $@ $<

# gbloop touches no USB: it is a pipe from gbcap's stdout to a v4l2loopback
# node, so it needs neither libusb nor its cflags.
gbloop: gbloop.c
	$(CC) $(CFLAGS) -o $@ $<

# Rig tools, 2026-10-11. None needs libusb: gbtest speaks V4L2, gbcxreg reads the
# decoder through the bound driver's I2C adapter (i2c-dev), and gbstop sends the
# one byte 0x37 through usbfs with the driver unbound.
gbtest: gbtest.c
	$(CC) $(CFLAGS) -o $@ $<

gbcxreg: gbcxreg.c
	$(CC) $(CFLAGS) -o $@ $<

gbstop: gbstop.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f gbfw gbcap gbloop gblat gbtest gbcxreg gbstop

.PHONY: all clean
