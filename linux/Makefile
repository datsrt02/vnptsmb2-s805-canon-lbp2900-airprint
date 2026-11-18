CC ?= cc
ZIG ?= zig
CFLAGS ?= -O2 -g -std=gnu11 -Wall -Wextra -Wformat=2 -Wshadow -Wpointer-arith
CPPFLAGS += -D_GNU_SOURCE \
	-I../components/airprint/include -I../components/airprint \
	-I../components/canon_bjl/include
LDFLAGS ?=
LDLIBS += -pthread -lm

AIRPRINT = ../components/airprint/airprint.c ../components/airprint/ipp.c \
	../components/airprint/raster.c ../components/airprint/events.c \
	../components/airprint/printer_driver.c
PRINTERS = ../components/printers/printers.c \
	../components/printers/canon_pixma.c ../components/printers/canon_lbp2900.c
BJL = ../components/canon_bjl/canon_bjl.c \
	../components/canon_bjl/testpage.c ../components/canon_bjl/bjl_render.c
SOURCES = main.c usbfs_backend.c $(AIRPRINT) $(PRINTERS) $(BJL)

.PHONY: all armhf-static clean install

all: lbp2900-airprint

lbp2900-airprint: $(SOURCES)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SOURCES) $(LDFLAGS) $(LDLIBS) -o $@

# Reproducible static binary for Debian ARMHF. Zig 0.17 was used for the
# release artifact; override ZIG=/path/to/zig when it is not on PATH.
armhf-static:
	$(ZIG) cc -target arm-linux-musleabihf -static $(CPPFLAGS) -O2 -std=gnu11 \
		-Wall -Wextra -Wformat=2 -Wshadow -Wpointer-arith $(SOURCES) \
		-pthread -lm -o lbp2900-airprint-armhf.bin

install: lbp2900-airprint
	install -D -m 0755 lbp2900-airprint $(DESTDIR)/usr/local/sbin/lbp2900-airprint

clean:
	rm -f lbp2900-airprint lbp2900-airprint-armhf.bin
