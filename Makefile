CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra
CPPFLAGS ?=
LDFLAGS ?=
LIBS = -lX11 -lXfixes -lXext

PREFIX ?= /usr
BINDIR ?= $(PREFIX)/bin

all: wecom-x11-fix

wecom-x11-fix: wecom-x11-fix.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIBS)

install: wecom-x11-fix
	install -Dm755 wecom-x11-fix $(DESTDIR)$(BINDIR)/wecom-x11-fix

clean:
	rm -f wecom-x11-fix

.PHONY: all install clean
