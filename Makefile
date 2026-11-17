# mcgregor-wm for Slackware 3.6 (Linux 2.0, libc5, gcc 2.7.2.3, XFree86 3.3).
# Plain assignments only: works with old GNU make. Override on the command
# line, e.g.   make CC=cc   or   make PREFIX=/usr
CC      = gcc
CFLAGS  = -O2 -Wall
XINC    = -I/usr/X11R6/include
XLIB    = -L/usr/X11R6/lib
LDLIBS  = -lX11
PREFIX  = /usr/local
MANDIR  = $(PREFIX)/man/man1

all: mcgregor-wm

mcgregor-wm: mcgregor-wm.c
	$(CC) $(CFLAGS) $(XINC) -o mcgregor-wm mcgregor-wm.c $(XLIB) $(LDLIBS)

install: mcgregor-wm
	mkdir -p $(PREFIX)/bin $(MANDIR)
	cp mcgregor-wm $(PREFIX)/bin/mcgregor-wm
	strip $(PREFIX)/bin/mcgregor-wm
	chmod 755 $(PREFIX)/bin/mcgregor-wm
	cp mcgregor-wm.1 $(MANDIR)/mcgregor-wm.1
	chmod 644 $(MANDIR)/mcgregor-wm.1

uninstall:
	rm -f $(PREFIX)/bin/mcgregor-wm $(MANDIR)/mcgregor-wm.1

clean:
	rm -f mcgregor-wm core *.o
