# SPDX-License-Identifier: GPL-3.0-or-later
# pomelo-launcher
PREFIX ?= /usr/local
CFLAGS ?= -Os -pipe -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation
# 編譯防護（Ubuntu 的 gcc 預設就有，Fedora／Rocky 要自己加）：堆疊保護、PIE、FORTIFY、完整 RELRO
override CFLAGS += -fstack-protector-strong -fstack-clash-protection -D_FORTIFY_SOURCE=3 -fPIE -pie
override LDFLAGS += -Wl,-z,relro,-z,now
POMELO_CFLAGS ?= $(shell pkg-config --cflags pomelo)
POMELO_LIBS   ?= $(shell pkg-config --libs pomelo)

all: pomelo-launcher pomelo-launcher.png

pomelo-launcher: pomelo-launcher.c
	$(CC) $(CFLAGS) $(POMELO_CFLAGS) -o $@ pomelo-launcher.c $(POMELO_LIBS) -lm $(LDFLAGS)

pomelo-launcher.png: pomelo-launcher
	LD_LIBRARY_PATH="$(LD_LIBRARY_PATH)" ./pomelo-launcher --icon $@

install: all
	install -Dm755 pomelo-launcher $(DESTDIR)$(PREFIX)/bin/pomelo-launcher
	install -Dm644 pomelo-launcher.desktop $(DESTDIR)$(PREFIX)/share/applications/pomelo-launcher.desktop
	install -Dm644 pomelo-launcher.png $(DESTDIR)$(PREFIX)/share/icons/hicolor/256x256/apps/pomelo-launcher.png

clean:
	rm -f pomelo-launcher pomelo-launcher.png

.PHONY: all install clean
