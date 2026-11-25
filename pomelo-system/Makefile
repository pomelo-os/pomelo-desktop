# SPDX-License-Identifier: GPL-3.0-or-later
# pomelo-system — Pomelo 系統設定
PREFIX ?= /usr/local
CFLAGS ?= -Os -pipe -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation
# 編譯防護（Ubuntu 的 gcc 預設就有，Fedora／Rocky 要自己加）：堆疊保護、PIE、FORTIFY、完整 RELRO
override CFLAGS += -fstack-protector-strong -fstack-clash-protection -D_FORTIFY_SOURCE=3 -fPIE -pie
override LDFLAGS += -Wl,-z,relro,-z,now
POMELO_CFLAGS ?= $(shell pkg-config --cflags pomelo)
POMELO_LIBS   ?= $(shell pkg-config --libs pomelo)

all: pomelo-system pomelo-system.png

pomelo-system: pomelo-system.c ../pomelo-screensaver/saver-idle.h
	$(CC) $(CFLAGS) $(POMELO_CFLAGS) -I../pomelo-screensaver -o $@ pomelo-system.c $(POMELO_LIBS) -lm $(LDFLAGS)

pomelo-system.png: pomelo-system
	LD_LIBRARY_PATH="$(LD_LIBRARY_PATH)" ./pomelo-system --icon $@

install: all
	install -Dm755 pomelo-system $(DESTDIR)$(PREFIX)/bin/pomelo-system
	install -Dm644 pomelo-system.desktop $(DESTDIR)$(PREFIX)/share/applications/pomelo-system.desktop
	install -Dm644 pomelo-system.png $(DESTDIR)$(PREFIX)/share/icons/hicolor/256x256/apps/pomelo-system.png
	# 健康檢查需要管理員權限的帳號檢查（只讀，經過 pkexec）
	install -Dm755 pomelo-check-admin $(DESTDIR)$(PREFIX)/libexec/pomelo/pomelo-check-admin
	install -Dm644 tw.fedora.pomelo.check.policy $(DESTDIR)$(PREFIX)/share/polkit-1/actions/tw.fedora.pomelo.check.policy

clean:
	rm -f pomelo-system pomelo-system.png

.PHONY: all install clean
