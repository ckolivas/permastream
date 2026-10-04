CC ?= cc
PKG_CONFIG ?= pkg-config
PREFIX ?= /usr/local
FFMPEG_PACKAGES = libavformat libavcodec libavutil libswresample
CPPFLAGS += -D_GNU_SOURCE -Isrc -Ivendor $(shell $(PKG_CONFIG) --cflags $(FFMPEG_PACKAGES))
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -pthread
LDLIBS += $(shell $(PKG_CONFIG) --libs $(FFMPEG_PACKAGES)) -lm -pthread
SOURCES = src/main.c src/config.c src/source.c src/output.c src/server.c vendor/toml.c
OBJECTS = $(SOURCES:.c=.o)

.PHONY: all clean test install uninstall
all: permastream

permastream: $(OBJECTS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

-include $(OBJECTS:.o=.d)

test: permastream
	python3 tests/integration.py
	python3 tests/hls.py

install: permastream
	install -Dm755 permastream $(DESTDIR)$(PREFIX)/bin/permastream

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/permastream

clean:
	rm -f permastream $(OBJECTS) $(OBJECTS:.o=.d)
