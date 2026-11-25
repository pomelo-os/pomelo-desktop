# SPDX-License-Identifier: GPL-3.0-or-later
# pomelo-greeter — Pomelo 登入畫面（由 pomelo-dm 啟動）
PREFIX ?= /usr/local
CFLAGS ?= -Os -pipe -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation
# 編譯防護（Ubuntu 的 gcc 預設就有，Fedora／Rocky 要自己加）：堆疊保護、PIE、FORTIFY、完整 RELRO
override CFLAGS += -fstack-protector-strong -fstack-clash-protection -D_FORTIFY_SOURCE=3 -fPIE -pie
override LDFLAGS += -Wl,-z,relro,-z,now
POMELO_CFLAGS ?= $(shell pkg-config --cflags pomelo)
POMELO_LIBS   ?= $(shell pkg-config --libs pomelo)

pomelo-greeter: pomelo-greeter.c
	$(CC) $(CFLAGS) $(POMELO_CFLAGS) -o $@ pomelo-greeter.c $(POMELO_LIBS) -lm $(LDFLAGS)

install: pomelo-greeter
	install -Dm755 pomelo-greeter $(DESTDIR)$(PREFIX)/bin/pomelo-greeter

clean:
	rm -f pomelo-greeter

.PHONY: install clean
