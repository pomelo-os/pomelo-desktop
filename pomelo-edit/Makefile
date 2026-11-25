# SPDX-License-Identifier: GPL-3.0-or-later
# pomelo-edit — Pomelo 文字編輯器
PREFIX ?= /usr/local
CFLAGS ?= -Os -pipe -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation
# 編譯防護（Ubuntu 的 gcc 預設就有，Fedora／Rocky 要自己加）：堆疊保護、PIE、FORTIFY、完整 RELRO
override CFLAGS += -fstack-protector-strong -fstack-clash-protection -D_FORTIFY_SOURCE=3 -fPIE -pie
override LDFLAGS += -Wl,-z,relro,-z,now
POMELO_CFLAGS ?= $(shell pkg-config --cflags pomelo)
POMELO_LIBS   ?= $(shell pkg-config --libs pomelo)

all: pomelo-edit pomelo-edit.png

pomelo-edit: pomelo-edit.c
	$(CC) $(CFLAGS) $(POMELO_CFLAGS) -o $@ pomelo-edit.c $(POMELO_LIBS) -lm $(LDFLAGS)

pomelo-edit.png: pomelo-edit
	LD_LIBRARY_PATH="$(LD_LIBRARY_PATH)" ./pomelo-edit --icon $@

install: all
	install -Dm755 pomelo-edit $(DESTDIR)$(PREFIX)/bin/pomelo-edit
	install -Dm644 pomelo-edit.desktop $(DESTDIR)$(PREFIX)/share/applications/pomelo-edit.desktop
	install -Dm644 pomelo-edit.png $(DESTDIR)$(PREFIX)/share/icons/hicolor/256x256/apps/pomelo-edit.png
	# 純文字檔預設用這個編輯器開（xdg-open 會看 $$XDG_CURRENT_DESKTOP 小寫＋ -mimeapps.list）
	install -Dm644 pomelo-mimeapps.list $(DESTDIR)$(PREFIX)/share/applications/pomelo-mimeapps.list

clean:
	rm -f pomelo-edit pomelo-edit.png

.PHONY: all install clean
