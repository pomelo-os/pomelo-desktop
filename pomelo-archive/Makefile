# SPDX-License-Identifier: GPL-3.0-or-later
# pomelo-archive — Pomelo 壓縮管理員
PREFIX ?= /usr/local
CFLAGS ?= -Os -pipe -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation
# 編譯防護（Ubuntu 的 gcc 預設就有，Fedora／Rocky 要自己加）：堆疊保護、PIE、FORTIFY、完整 RELRO
override CFLAGS += -fstack-protector-strong -fstack-clash-protection -D_FORTIFY_SOURCE=3 -fPIE -pie
override LDFLAGS += -Wl,-z,relro,-z,now
POMELO_CFLAGS ?= $(shell pkg-config --cflags pomelo)
POMELO_LIBS   ?= $(shell pkg-config --libs pomelo)
ARCHIVE_CFLAGS ?= $(shell pkg-config --cflags libarchive)
ARCHIVE_LIBS   ?= $(shell pkg-config --libs libarchive)

# ac-*.c 是核心（不 #include <pomelo.h>：那邊有 dpy、T、sh 之類的巨集），cli.c 是命令列，gui-*.c 是介面
CORE = ac-util.c ac-read.c ac-write.c cli.c
GUI  = gui-common.c gui-dialogs.c gui-browse.c
HDRS = ac.h ac-int.h cli.h gui.h

all: pomelo-archive pomelo-archive.png

pomelo-archive: pomelo-archive.c $(CORE) $(GUI) $(HDRS)
	$(CC) $(CFLAGS) $(POMELO_CFLAGS) $(ARCHIVE_CFLAGS) -o $@ pomelo-archive.c $(CORE) $(GUI) $(POMELO_LIBS) $(ARCHIVE_LIBS) -lpthread -lm $(LDFLAGS)

pomelo-archive.png: pomelo-archive
	LD_LIBRARY_PATH="$(LD_LIBRARY_PATH)" ./pomelo-archive --icon $@

# 測試：核心（往返、加密、跨工具、編碼、惡意封存檔…）；PA 可以指定要測哪個執行檔
test: pomelo-archive
	PA=$(CURDIR)/pomelo-archive bash test/test-core.sh

install: all
	install -Dm755 pomelo-archive $(DESTDIR)$(PREFIX)/bin/pomelo-archive
	install -Dm644 pomelo-archive.desktop $(DESTDIR)$(PREFIX)/share/applications/pomelo-archive.desktop
	install -Dm644 pomelo-archive.png $(DESTDIR)$(PREFIX)/share/icons/hicolor/256x256/apps/pomelo-archive.png

clean:
	rm -f pomelo-archive pomelo-archive.png

.PHONY: all test install clean
