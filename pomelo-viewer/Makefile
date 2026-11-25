# SPDX-License-Identifier: GPL-3.0-or-later
# pomelo-viewer — Pomelo 看圖
PREFIX ?= /usr/local
CFLAGS ?= -Os -pipe -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation
# 編譯防護（Ubuntu 的 gcc 預設就有，Fedora／Rocky 要自己加）：堆疊保護、PIE、FORTIFY、完整 RELRO
override CFLAGS += -fstack-protector-strong -fstack-clash-protection -D_FORTIFY_SOURCE=3 -fPIE -pie
override LDFLAGS += -Wl,-z,relro,-z,now
POMELO_CFLAGS ?= $(shell pkg-config --cflags pomelo)
POMELO_LIBS   ?= $(shell pkg-config --libs pomelo)
# giflib 在有些發行版沒有 .pc 檔，直接用 -lgif
IMG_CFLAGS ?= $(shell pkg-config --cflags libwebp libwebpdemux)
IMG_LIBS   ?= -lgif $(shell pkg-config --libs libwebp libwebpdemux)

all: pomelo-viewer pomelo-viewer.png

pomelo-viewer: pomelo-viewer.c formats.c
	$(CC) $(CFLAGS) $(POMELO_CFLAGS) $(IMG_CFLAGS) -pthread -o $@ pomelo-viewer.c $(POMELO_LIBS) $(IMG_LIBS) -lm $(LDFLAGS)

pomelo-viewer.png: pomelo-viewer
	LD_LIBRARY_PATH="$(LD_LIBRARY_PATH)" ./pomelo-viewer --icon $@

install: all
	install -Dm755 pomelo-viewer $(DESTDIR)$(PREFIX)/bin/pomelo-viewer
	install -Dm644 pomelo-viewer.desktop $(DESTDIR)$(PREFIX)/share/applications/pomelo-viewer.desktop
	install -Dm644 pomelo-viewer.png $(DESTDIR)$(PREFIX)/share/icons/hicolor/256x256/apps/pomelo-viewer.png

clean:
	rm -f pomelo-viewer pomelo-viewer.png

.PHONY: all install clean
