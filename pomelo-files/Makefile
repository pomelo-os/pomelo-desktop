# SPDX-License-Identifier: GPL-3.0-or-later
# pomelo-files — Pomelo 檔案管理員
PREFIX ?= /usr/local
CFLAGS ?= -Os -pipe -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation
# 編譯防護（Ubuntu 的 gcc 預設就有，Fedora／Rocky 要自己加）：堆疊保護、PIE、FORTIFY、完整 RELRO
override CFLAGS += -fstack-protector-strong -fstack-clash-protection -D_FORTIFY_SOURCE=3 -fPIE -pie
override LDFLAGS += -Wl,-z,relro,-z,now
POMELO_CFLAGS ?= $(shell pkg-config --cflags pomelo)
POMELO_LIBS   ?= $(shell pkg-config --libs pomelo)

all: pomelo-files pomelo-files.png

pomelo-files: pomelo-files.c files-config.h
	$(CC) $(CFLAGS) $(POMELO_CFLAGS) -o $@ pomelo-files.c $(POMELO_LIBS) -lm $(LDFLAGS)

pomelo-files.png: pomelo-files
	LD_LIBRARY_PATH="$(LD_LIBRARY_PATH)" ./pomelo-files --icon $@

install: all
	install -Dm755 pomelo-files $(DESTDIR)$(PREFIX)/bin/pomelo-files
	install -Dm644 pomelo-files.desktop $(DESTDIR)$(PREFIX)/share/applications/pomelo-files.desktop
	install -Dm644 pomelo-files.png $(DESTDIR)$(PREFIX)/share/icons/hicolor/256x256/apps/pomelo-files.png

clean:
	rm -f pomelo-files pomelo-files.png

.PHONY: all install clean
