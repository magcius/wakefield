
CLEANFILES =
PKGS = gtk+-3.0 wayland-server wayland-client
CFLAGS = $(shell pkg-config --cflags $(PKGS)) -Wall -Werror -g -O0 -Wno-deprecated-declarations -D_GNU_SOURCE
LDFLAGS = $(shell pkg-config --libs $(PKGS))

FILES = wakefield-compositor.c wakefield-compositor.h

all: libwakefield.so test-compositor test-client test-client-2

INTROSPECTION_GIRS = Wakefield-1.0.gir
INTROSPECTION_SCANNER_ARGS = --warn-all --warn-error --no-libtool

Wakefield-1.0.gir: libwakefield.so
Wakefield_1_0_gir_INCLUDES = Gtk-3.0
Wakefield_1_0_gir_CFLAGS = $(CFLAGS)
Wakefield_1_0_gir_LIBS = wakefield
Wakefield_1_0_gir_FILES = $(FILES)
CLEANFILES += Wakefield-1.0.gir Wakefield-1.0.typelib

include Makefile.introspection

libwakefield.so: CFLAGS += -fPIC -shared
libwakefield.so: wakefield-compositor.o wakefield-surface.c wakefield-seat.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
CLEANFILES += libwakefield.so wakefield-compositor.o

test-compositor: LDFLAGS += -L. -lwakefield

clean:
	rm -f $(CLEANFILES)
