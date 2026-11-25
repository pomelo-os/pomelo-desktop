# SPDX-License-Identifier: GPL-3.0-or-later
# pomelo-theme — Pomelo 外觀
PREFIX ?= /usr/local
CFLAGS ?= -Os -pipe -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation
# 編譯防護（Ubuntu 的 gcc 預設就有，Fedora／Rocky 要自己加）：堆疊保護、PIE、FORTIFY、完整 RELRO
override CFLAGS += -fstack-protector-strong -fstack-clash-protection -D_FORTIFY_SOURCE=3 -fPIE -pie
override LDFLAGS += -Wl,-z,relro,-z,now
POMELO_CFLAGS ?= $(shell pkg-config --cflags pomelo)
POMELO_LIBS   ?= $(shell pkg-config --libs pomelo)

all: pomelo-theme pomelo-theme.png

pomelo-theme: pomelo-theme.c
	$(CC) $(CFLAGS) $(POMELO_CFLAGS) -o $@ pomelo-theme.c $(POMELO_LIBS) -lm $(LDFLAGS)

pomelo-theme.png: pomelo-theme
	LD_LIBRARY_PATH="$(LD_LIBRARY_PATH)" ./pomelo-theme --icon $@

install: all
	install -Dm755 pomelo-theme $(DESTDIR)$(PREFIX)/bin/pomelo-theme
	install -Dm644 pomelo-theme.desktop $(DESTDIR)$(PREFIX)/share/applications/pomelo-theme.desktop
	install -Dm644 pomelo-theme.png $(DESTDIR)$(PREFIX)/share/icons/hicolor/256x256/apps/pomelo-theme.png

clean:
	rm -f pomelo-theme pomelo-theme.png

.PHONY: all install clean
