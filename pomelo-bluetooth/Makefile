# SPDX-License-Identifier: GPL-3.0-or-later
# pomelo-bluetooth
PREFIX ?= /usr/local
CFLAGS ?= -Os -pipe -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation
# 編譯防護（Ubuntu 的 gcc 預設就有，Fedora／Rocky 要自己加）：堆疊保護、PIE、FORTIFY、完整 RELRO
override CFLAGS += -fstack-protector-strong -fstack-clash-protection -D_FORTIFY_SOURCE=3 -fPIE -pie
override LDFLAGS += -Wl,-z,relro,-z,now
POMELO_CFLAGS ?= $(shell pkg-config --cflags pomelo)
POMELO_LIBS   ?= $(shell pkg-config --libs pomelo)

all: pomelo-bluetooth pomelo-bluetooth.png

pomelo-bluetooth: pomelo-bluetooth.c
	$(CC) $(CFLAGS) $(POMELO_CFLAGS) -o $@ pomelo-bluetooth.c $(POMELO_LIBS) -lm $(LDFLAGS)

pomelo-bluetooth.png: pomelo-bluetooth
	LD_LIBRARY_PATH="$(LD_LIBRARY_PATH)" ./pomelo-bluetooth --icon $@

install: all
	install -Dm755 pomelo-bluetooth $(DESTDIR)$(PREFIX)/bin/pomelo-bluetooth
	install -Dm644 pomelo-bluetooth.desktop $(DESTDIR)$(PREFIX)/share/applications/pomelo-bluetooth.desktop
	install -Dm644 pomelo-bluetooth.png $(DESTDIR)$(PREFIX)/share/icons/hicolor/256x256/apps/pomelo-bluetooth.png

clean:
	rm -f pomelo-bluetooth pomelo-bluetooth.png

.PHONY: all install clean
