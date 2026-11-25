# SPDX-License-Identifier: GPL-3.0-or-later
# pomelo-wm — Pomelo 視窗管理器
PREFIX ?= /usr/local
CFLAGS ?= -Os -pipe -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation
# 編譯防護（Ubuntu 的 gcc 預設就有，Fedora／Rocky 要自己加）：堆疊保護、PIE、FORTIFY、完整 RELRO
override CFLAGS += -fstack-protector-strong -fstack-clash-protection -D_FORTIFY_SOURCE=3 -fPIE -pie
override LDFLAGS += -Wl,-z,relro,-z,now
POMELO_CFLAGS ?= $(shell pkg-config --cflags pomelo)
POMELO_LIBS   ?= $(shell pkg-config --libs pomelo)

pomelo: pomelo.c config.h
	$(CC) $(CFLAGS) $(POMELO_CFLAGS) $$(pkg-config --cflags xcomposite) -o $@ pomelo.c $(POMELO_LIBS) $$(pkg-config --libs xcomposite) -lm $(LDFLAGS)

install: pomelo
	install -Dm755 pomelo $(DESTDIR)$(PREFIX)/bin/pomelo
	install -Dm644 picom.conf $(DESTDIR)$(PREFIX)/share/pomelo/picom.conf

clean:
	rm -f pomelo

.PHONY: install clean
