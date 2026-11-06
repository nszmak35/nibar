.POSIX:
.SUFFIXES:

VERSION    = 0.1
PKG_CONFIG = pkg-config
CC         = cc
CFLAGS     = -O2
PREFIX     = /usr/local

SRCDIR   = src
BUILDDIR = build
GENDIR   = $(BUILDDIR)/gen
PROTODIR = $(SRCDIR)/protocols

# wld is neuwld (comes with neuswc)
PKGS = wayland-client wld

WARN = -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
	-Werror=implicit-function-declaration -Werror=return-type \
	-Werror=incompatible-pointer-types -Werror=strict-prototypes

PYTHON_CFLAGS = `$(PKG_CONFIG) --cflags python3-embed 2>/dev/null || python3-config --includes`
PYTHON_LIBS   = `$(PKG_CONFIG) --libs python3-embed 2>/dev/null || python3-config --embed --ldflags`

WAYLAND_SCANNER = `$(PKG_CONFIG) --variable=wayland_scanner wayland-scanner`

PROTOCOLS = \
	dwl-ipc-unstable-v2 \
	ofwc-ipc-unstable-v1 \
	wlr-layer-shell-unstable-v1 \
	xdg-shell

GEN_HEADERS = $(PROTOCOLS:%=$(GENDIR)/%-client-protocol.h)
GEN_OBJS    = $(PROTOCOLS:%=$(BUILDDIR)/%-protocol.o)
HEADERS     = $(wildcard $(SRCDIR)/*.h)

all: nibar

nibar: $(BUILDDIR)/nibar.o $(GEN_OBJS)
	$(CC) $(BUILDDIR)/nibar.o $(GEN_OBJS) $(LDFLAGS) \
		`$(PKG_CONFIG) --libs $(PKGS)` $(PYTHON_LIBS) -lm -o $@

# One translation unit: every header is a dependency.
$(BUILDDIR)/nibar.o: $(SRCDIR)/nibar.c $(HEADERS) $(GENDIR)/default_config.h $(GEN_HEADERS)
	@mkdir -p $(@D)
	$(CC) -I$(SRCDIR) -I$(GENDIR) -DVERSION=\"$(VERSION)\" $(WARN) $(CFLAGS) \
		`$(PKG_CONFIG) --cflags $(PKGS)` $(PYTHON_CFLAGS) -o $@ -c $(SRCDIR)/nibar.c

$(BUILDDIR)/%-protocol.o: $(GENDIR)/%-protocol.c
	$(CC) -I$(GENDIR) $(CFLAGS) `$(PKG_CONFIG) --cflags wayland-client` -o $@ -c $<

$(GENDIR)/%-client-protocol.h: $(PROTODIR)/%.xml
	@mkdir -p $(@D)
	$(WAYLAND_SCANNER) client-header $< $@

$(GENDIR)/%-protocol.c: $(PROTODIR)/%.xml
	@mkdir -p $(@D)
	$(WAYLAND_SCANNER) private-code $< $@

# default/config.py is compiled in as the built-in default.
$(GENDIR)/default_config.h: default/config.py
	@mkdir -p $(@D)
	( echo 'static const char nibar_default_config[] ='; \
	  sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/"/' -e 's/$$/\\n"/' default/config.py; \
	  echo ';' ) > $@

clean:
	rm -rf $(BUILDDIR) nibar

install: nibar
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	rm -f $(DESTDIR)$(PREFIX)/bin/nibar
	cp -f nibar $(DESTDIR)$(PREFIX)/bin/nibar
	chmod 755 $(DESTDIR)$(PREFIX)/bin/nibar
	mkdir -p $(DESTDIR)/etc/nibar
	cp -f default/config.py $(DESTDIR)/etc/nibar/config.py

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/nibar
	rm -rf $(DESTDIR)/etc/nibar

.PHONY: all clean install uninstall
