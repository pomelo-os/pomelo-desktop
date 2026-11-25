# SPDX-License-Identifier: GPL-3.0-or-later
# pomelo-vm — Pomelo 虛擬機器（QEMU／KVM）
PREFIX ?= /usr/local
CFLAGS ?= -Os -pipe -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation
# 編譯防護（Ubuntu 的 gcc 預設就有，Fedora／Rocky 要自己加）：堆疊保護、PIE、FORTIFY、完整 RELRO
override CFLAGS += -fstack-protector-strong -fstack-clash-protection -D_FORTIFY_SOURCE=3 -fPIE -pie
override LDFLAGS += -Wl,-z,relro,-z,now
POMELO_CFLAGS ?= $(shell pkg-config --cflags pomelo)
POMELO_LIBS   ?= $(shell pkg-config --libs pomelo)
VM_CFLAGS ?= $(shell pkg-config --cflags xcursor libpulse-simple)
VM_LIBS   ?= $(shell pkg-config --libs xcursor libpulse-simple)

all: pomelo-vm pomelo-vm.png

pomelo-vm: pomelo-vm.c vm.c console.c json.c
	$(CC) $(CFLAGS) $(POMELO_CFLAGS) $(VM_CFLAGS) -pthread -o $@ pomelo-vm.c $(POMELO_LIBS) $(VM_LIBS) -lm $(LDFLAGS)

pomelo-vm.png: pomelo-vm
	LD_LIBRARY_PATH="$(LD_LIBRARY_PATH)" ./pomelo-vm --icon $@

install: all
	install -Dm755 pomelo-vm $(DESTDIR)$(PREFIX)/bin/pomelo-vm
	install -Dm644 pomelo-vm.desktop $(DESTDIR)$(PREFIX)/share/applications/pomelo-vm.desktop
	install -Dm644 pomelo-vm.png $(DESTDIR)$(PREFIX)/share/icons/hicolor/256x256/apps/pomelo-vm.png

clean:
	rm -f pomelo-vm pomelo-vm.png

.PHONY: all install clean
