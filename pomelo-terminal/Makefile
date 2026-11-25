# SPDX-License-Identifier: GPL-3.0-or-later
# pomelo-terminal — Pomelo 終端機
PREFIX ?= /usr/local
CFLAGS ?= -Os -pipe -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation
# 編譯防護（Ubuntu 的 gcc 預設就有，Fedora／Rocky 要自己加）：堆疊保護、PIE、FORTIFY、完整 RELRO
override CFLAGS += -fstack-protector-strong -fstack-clash-protection -D_FORTIFY_SOURCE=3 -fPIE -pie
override LDFLAGS += -Wl,-z,relro,-z,now
POMELO_CFLAGS ?= $(shell pkg-config --cflags pomelo)
POMELO_LIBS   ?= $(shell pkg-config --libs pomelo)
TPKGS = x11 xft fontconfig cairo

all: pomelo-terminal pomelo-terminal.png

pomelo-terminal: pomelo-terminal.c terminal-config.h
	$(CC) $(CFLAGS) $(POMELO_CFLAGS) $$(pkg-config --cflags $(TPKGS)) -o $@ pomelo-terminal.c $(POMELO_LIBS) $$(pkg-config --libs $(TPKGS)) -lutil -lm $(LDFLAGS)

pomelo-terminal.png: pomelo-terminal
	LD_LIBRARY_PATH="$(LD_LIBRARY_PATH)" ./pomelo-terminal --icon $@

install: all
	install -Dm755 pomelo-terminal $(DESTDIR)$(PREFIX)/bin/pomelo-terminal
	install -Dm644 pomelo-terminal.desktop $(DESTDIR)$(PREFIX)/share/applications/pomelo-terminal.desktop
	install -Dm644 pomelo-terminal.png $(DESTDIR)$(PREFIX)/share/icons/hicolor/256x256/apps/pomelo-terminal.png

clean:
	rm -f pomelo-terminal pomelo-terminal.png

.PHONY: all install clean
