# SPDX-License-Identifier: GPL-3.0-or-later
# pomelo-writer — Pomelo 文書處理器（開啟、編輯、存檔 .docx／.odt）
PREFIX ?= /usr/local
CFLAGS ?= -Os -pipe -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation
# 編譯防護（Ubuntu 的 gcc 預設就有，Fedora／Rocky 要自己加）：堆疊保護、PIE、FORTIFY、完整 RELRO
override CFLAGS += -fstack-protector-strong -fstack-clash-protection -D_FORTIFY_SOURCE=3 -fPIE -pie
override LDFLAGS += -Wl,-z,relro,-z,now
POMELO_CFLAGS ?= $(shell pkg-config --cflags pomelo)
POMELO_LIBS   ?= $(shell pkg-config --libs pomelo)
WRITER_CFLAGS ?= $(shell pkg-config --cflags zlib)
WRITER_LIBS   ?= $(shell pkg-config --libs zlib) -lm

OBJS = zip.o xml.o doc.o docx.o odt.o layout.o pomelo-writer.o

all: pomelo-writer pomelo-writer.png

%.o: %.c zip.h xml.h doc.h docx.h odt.h layout.h
	$(CC) $(CFLAGS) $(WRITER_CFLAGS) $(POMELO_CFLAGS) -c -o $@ $<

pomelo-writer: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(POMELO_LIBS) $(WRITER_LIBS) $(LDFLAGS)

pomelo-writer.png: pomelo-writer
	LD_LIBRARY_PATH="$(LD_LIBRARY_PATH)" ./pomelo-writer --icon $@

install: all
	install -Dm755 pomelo-writer $(DESTDIR)$(PREFIX)/bin/pomelo-writer
	install -Dm644 pomelo-writer.desktop $(DESTDIR)$(PREFIX)/share/applications/pomelo-writer.desktop
	install -Dm644 pomelo-writer.png $(DESTDIR)$(PREFIX)/share/icons/hicolor/256x256/apps/pomelo-writer.png

clean:
	rm -f pomelo-writer pomelo-writer.png *.o

.PHONY: all install clean
