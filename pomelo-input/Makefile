# SPDX-License-Identifier: GPL-3.0-or-later
# pomelo-input — Pomelo 輸入法（XIM 伺服器＋給 Qt 程式的 IBus 協定；注音用 libchewing，其他引擎執行時才 dlopen）
PREFIX ?= /usr/local
CFLAGS ?= -Os -pipe -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation
# 編譯防護（Ubuntu 的 gcc 預設就有，Fedora／Rocky 要自己加）：堆疊保護、PIE、FORTIFY、完整 RELRO
override CFLAGS += -fstack-protector-strong -fstack-clash-protection -D_FORTIFY_SOURCE=3 -fPIE -pie
override LDFLAGS += -Wl,-z,relro,-z,now
POMELO_CFLAGS ?= $(shell pkg-config --cflags pomelo)
POMELO_LIBS   ?= $(shell pkg-config --libs pomelo)
PKGS = xcb xcb-shape xcb-keysyms xcb-imdkit cairo-xcb chewing dbus-1 wayland-client xkbcommon
WAYLAND_SCANNER ?= $(shell pkg-config --variable=wayland_scanner wayland-scanner)
DEP_CFLAGS ?= $(shell pkg-config --cflags $(PKGS))
DEP_LIBS   ?= $(shell pkg-config --libs $(PKGS))
CHEWING_VER ?= $(shell pkg-config --modversion chewing 2>/dev/null || echo ?)
VER_FLAGS = -DCHEWING_VERSION=\"$(CHEWING_VER)\"

all: pomelo-input

SRCS = pomelo-input.c ibus.c wl.c eng-chewing.c eng-hangul.c eng-rime.c eng-anthy.c eng-viet.c
# 原生 Wayland 程式用的兩個協定（wlroots 原始碼附的 XML，放在 protocol/，建置時產生程式碼）
PROTOS = input-method-unstable-v2 virtual-keyboard-unstable-v1
PROTO_HDR = $(PROTOS:%=protocol/%-client-protocol.h)
PROTO_SRC = $(PROTOS:%=protocol/%-protocol.c)

pomelo-input: $(SRCS) engine.h ic.h third-party/rime_api.h $(PROTO_HDR) $(PROTO_SRC)
	$(CC) $(CFLAGS) $(VER_FLAGS) $(POMELO_CFLAGS) $(DEP_CFLAGS) -Iprotocol -o $@ $(SRCS) $(PROTO_SRC) $(POMELO_LIBS) $(DEP_LIBS) -ldl -lm $(LDFLAGS)

protocol/%-client-protocol.h: protocol/%.xml
	$(WAYLAND_SCANNER) client-header $< $@

protocol/%-protocol.c: protocol/%.xml
	$(WAYLAND_SCANNER) private-code $< $@

install: all
	install -Dm755 pomelo-input $(DESTDIR)$(PREFIX)/bin/pomelo-input
	install -Dm755 ibus-daemon.stub $(DESTDIR)$(PREFIX)/lib/pomelo-input/qt-ibus/ibus-daemon

clean:
	rm -f pomelo-input $(PROTO_HDR) $(PROTO_SRC)

.PHONY: all install clean
