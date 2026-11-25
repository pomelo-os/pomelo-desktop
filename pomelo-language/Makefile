# SPDX-License-Identifier: GPL-3.0-or-later
# pomelo-language — Pomelo 語言與輸入法
PREFIX ?= /usr/local
CFLAGS ?= -Os -pipe -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation
# 編譯防護（Ubuntu 的 gcc 預設就有，Fedora／Rocky 要自己加）：堆疊保護、PIE、FORTIFY、完整 RELRO
override CFLAGS += -fstack-protector-strong -fstack-clash-protection -D_FORTIFY_SOURCE=3 -fPIE -pie
override LDFLAGS += -Wl,-z,relro,-z,now
POMELO_CFLAGS ?= $(shell pkg-config --cflags pomelo)
POMELO_LIBS   ?= $(shell pkg-config --libs pomelo)

all: pomelo-language pomelo-language.png

pomelo-language: pomelo-language.c
	$(CC) $(CFLAGS) $(POMELO_CFLAGS) -o $@ pomelo-language.c $(POMELO_LIBS) -ldl -lm -pthread $(LDFLAGS)

pomelo-language.png: pomelo-language
	LD_LIBRARY_PATH="$(LD_LIBRARY_PATH)" ./pomelo-language --icon $@

install: all
	install -Dm755 pomelo-language $(DESTDIR)$(PREFIX)/bin/pomelo-language
	install -Dm644 pomelo-language.desktop $(DESTDIR)$(PREFIX)/share/applications/pomelo-language.desktop
	install -Dm644 pomelo-language.png $(DESTDIR)$(PREFIX)/share/icons/hicolor/256x256/apps/pomelo-language.png

clean:
	rm -f pomelo-language pomelo-language.png

.PHONY: all install clean
