# SPDX-License-Identifier: GPL-3.0-or-later
# pomelo-control-center — Pomelo 控制中心
PREFIX ?= /usr/local
CFLAGS ?= -Os -pipe -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation
# 編譯防護（Ubuntu 的 gcc 預設就有，Fedora／Rocky 要自己加）：堆疊保護、PIE、FORTIFY、完整 RELRO
override CFLAGS += -fstack-protector-strong -fstack-clash-protection -D_FORTIFY_SOURCE=3 -fPIE -pie
override LDFLAGS += -Wl,-z,relro,-z,now
POMELO_CFLAGS ?= $(shell pkg-config --cflags pomelo)
POMELO_LIBS   ?= $(shell pkg-config --libs pomelo)

all: pomelo-control-center pomelo-control-center.png

pomelo-control-center: pomelo-control-center.c
	$(CC) $(CFLAGS) $(POMELO_CFLAGS) -o $@ pomelo-control-center.c $(POMELO_LIBS) -lm $(LDFLAGS)

pomelo-control-center.png: pomelo-control-center
	LD_LIBRARY_PATH="$(LD_LIBRARY_PATH)" ./pomelo-control-center --icon $@

install: all
	install -Dm755 pomelo-control-center $(DESTDIR)$(PREFIX)/bin/pomelo-control-center
	install -Dm644 pomelo-control-center.desktop $(DESTDIR)$(PREFIX)/share/applications/pomelo-control-center.desktop
	install -Dm644 pomelo-control-center.png $(DESTDIR)$(PREFIX)/share/icons/hicolor/256x256/apps/pomelo-control-center.png

clean:
	rm -f pomelo-control-center pomelo-control-center.png

.PHONY: all install clean
