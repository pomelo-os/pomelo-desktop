# SPDX-License-Identifier: GPL-3.0-or-later
# pomelo-browser — 小柚瀏覽器（網頁引擎：系統的 WebKitGTK 6.0）
PREFIX ?= /usr/local
CFLAGS ?= -Os -pipe -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -Wno-deprecated-declarations
# 編譯防護（Ubuntu 的 gcc 預設就有，Fedora／Rocky 要自己加）：堆疊保護、PIE、FORTIFY、完整 RELRO
override CFLAGS += -fstack-protector-strong -fstack-clash-protection -D_FORTIFY_SOURCE=3 -fPIE -pie
override LDFLAGS += -Wl,-z,relro,-z,now
POMELO_CFLAGS ?= $(shell pkg-config --cflags pomelo)
POMELO_LIBS   ?= $(shell pkg-config --libs pomelo)
WEBKIT_CFLAGS ?= $(shell pkg-config --cflags webkitgtk-6.0)
WEBKIT_LIBS   ?= $(shell pkg-config --libs webkitgtk-6.0)
ROBOT = ../pomelo-assistant/robot.c

all: pomelo-browser pomelo-browser.png

pomelo-browser: pomelo-browser.c $(ROBOT) ../pomelo-assistant/robot.h
	$(CC) $(CFLAGS) $(WEBKIT_CFLAGS) $(POMELO_CFLAGS) -o $@ pomelo-browser.c $(ROBOT) $(WEBKIT_LIBS) $(POMELO_LIBS) -lm $(LDFLAGS)

pomelo-browser.png: pomelo-browser
	LD_LIBRARY_PATH="$(LD_LIBRARY_PATH)" ./pomelo-browser --icon $@

install: all
	install -Dm755 pomelo-browser $(DESTDIR)$(PREFIX)/bin/pomelo-browser
	install -Dm644 pomelo-browser.desktop $(DESTDIR)$(PREFIX)/share/applications/pomelo-browser.desktop
	install -Dm644 pomelo-browser.png $(DESTDIR)$(PREFIX)/share/icons/hicolor/256x256/apps/pomelo-browser.png

clean:
	rm -f pomelo-browser pomelo-browser.png

.PHONY: all install clean
