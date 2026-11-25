# SPDX-License-Identifier: GPL-3.0-or-later
# pomelo-docker — Pomelo 容器（Docker 管理）
PREFIX ?= /usr/local
CFLAGS ?= -Os -pipe -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation
# 編譯防護（Ubuntu 的 gcc 預設就有，Fedora／Rocky 要自己加）：堆疊保護、PIE、FORTIFY、完整 RELRO
override CFLAGS += -fstack-protector-strong -fstack-clash-protection -D_FORTIFY_SOURCE=3 -fPIE -pie
override LDFLAGS += -Wl,-z,relro,-z,now
POMELO_CFLAGS ?= $(shell pkg-config --cflags pomelo)
POMELO_LIBS   ?= $(shell pkg-config --libs pomelo)

all: pomelo-docker pomelo-docker.png

pomelo-docker: pomelo-docker.c api.c backup.c json.c
	$(CC) $(CFLAGS) $(POMELO_CFLAGS) -o $@ pomelo-docker.c $(POMELO_LIBS) -lm $(LDFLAGS)

pomelo-docker.png: pomelo-docker
	LD_LIBRARY_PATH="$(LD_LIBRARY_PATH)" ./pomelo-docker --icon $@

install: all
	install -Dm755 pomelo-docker $(DESTDIR)$(PREFIX)/bin/pomelo-docker
	install -Dm644 pomelo-docker.desktop $(DESTDIR)$(PREFIX)/share/applications/pomelo-docker.desktop
	install -Dm644 pomelo-docker.png $(DESTDIR)$(PREFIX)/share/icons/hicolor/256x256/apps/pomelo-docker.png

clean:
	rm -f pomelo-docker pomelo-docker.png

.PHONY: all install clean
