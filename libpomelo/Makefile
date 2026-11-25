# SPDX-License-Identifier: GPL-3.0-or-later
# libpomelo — Pomelo 共用函式庫
VERSION ?= 0.42
PREFIX  ?= /usr/local
LIBDIR  ?= $(PREFIX)/lib
CFLAGS  ?= -Os -pipe -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation
# 編譯防護（Ubuntu 的 gcc 預設就有，Fedora／Rocky 要自己加）：堆疊保護、PIE、FORTIFY、完整 RELRO
override CFLAGS += -fstack-protector-strong -fstack-clash-protection -D_FORTIFY_SOURCE=3 -fPIE -pie
override LDFLAGS += -Wl,-z,relro,-z,now
PKGS     = x11 xext xrandr cairo libjpeg wayland-client xkbcommon zlib
SOVER    = 0
WAYLAND_SCANNER ?= $(shell pkg-config --variable=wayland_scanner wayland-scanner)
# 原生 Wayland 後端（wl.c）用的協定：XML 放在 protocol/，建置時產生程式碼
# （cursor-shape 參照到 tablet 的介面，所以 tablet 也要一起編）
PROTOS   = idle-inhibit-unstable-v1 primary-selection-unstable-v1 relative-pointer-unstable-v1 pointer-constraints-unstable-v1 keyboard-shortcuts-inhibit-unstable-v1 xdg-shell xdg-decoration-unstable-v1 text-input-unstable-v3 cursor-shape-v1 tablet-unstable-v2 wlr-foreign-toplevel-management-unstable-v1 \
           wlr-layer-shell-unstable-v1 xdg-output-unstable-v1 viewporter ext-session-lock-v1 \
           wlr-screencopy-unstable-v1 wlr-output-management-unstable-v1 wlr-output-power-management-unstable-v1 pomelo-workspace-v1 ext-idle-notify-v1
PROTO_HDR = $(PROTOS:%=protocol/%-client-protocol.h)
PROTO_OBJ = $(PROTOS:%=protocol/%-protocol.o)
OBJS     = pomelo.o textfb.o settings.o image.o widgets.o apps.o disks.o logomenu.o pkg.o dnd.o open.o wl.o \
           check.o check-net.o check-sec.o check-upd.o check-sys.o check-svc.o check-perf.o check-hw.o check-bat.o sbom.o $(PROTO_OBJ)
HDRS     = pomelo.h pomelo-settings.h pomelo-widgets.h pomelo-dnd.h pomelo-wl.h pomelo-check.h internal.h check-int.h

all: libpomelo.so.$(SOVER) libpomelo.so pomelo.pc pomelo-open

%.o: %.c $(HDRS)
	$(CC) $(CFLAGS) -fPIC $$(pkg-config --cflags $(PKGS)) -Iprotocol -c $<

wl.o: $(PROTO_HDR)

protocol/%-client-protocol.h: protocol/%.xml
	$(WAYLAND_SCANNER) client-header $< $@

protocol/%-protocol.c: protocol/%.xml
	$(WAYLAND_SCANNER) private-code $< $@

protocol/%-protocol.o: protocol/%-protocol.c
	$(CC) $(CFLAGS) -fPIC $$(pkg-config --cflags wayland-client) -c $< -o $@

libpomelo.so.$(SOVER): $(OBJS)
	$(CC) -shared -Wl,-soname,libpomelo.so.$(SOVER) -Wl,--as-needed -o $@ $(OBJS) $$(pkg-config --libs $(PKGS)) -lm -ldl $(LDFLAGS)

libpomelo.so: libpomelo.so.$(SOVER)
	ln -sf $< $@

# 「用預設程式開啟」的指令版（給 shell 指令、設定裡的自訂動作用；程式裡直接呼叫 pm_open）
pomelo-open: pomelo-open.c libpomelo.so
	$(CC) $(CFLAGS) $$(pkg-config --cflags $(PKGS)) -o $@ pomelo-open.c -L. -lpomelo $(LDFLAGS)

pomelo.pc: pomelo.pc.in
	sed -e 's|@PREFIX@|$(PREFIX)|' -e 's|@LIBDIR@|$(LIBDIR)|' -e 's|@VERSION@|$(VERSION)|' $< > $@

install: all
	install -Dm644 libpomelo.so.$(SOVER) $(DESTDIR)$(LIBDIR)/libpomelo.so.$(SOVER)
	ln -sf libpomelo.so.$(SOVER) $(DESTDIR)$(LIBDIR)/libpomelo.so
	for h in $(filter-out internal.h check-int.h,$(HDRS)); do install -Dm644 $$h $(DESTDIR)$(PREFIX)/include/pomelo/$$h; done
	install -Dm644 pomelo.pc $(DESTDIR)$(LIBDIR)/pkgconfig/pomelo.pc
	install -Dm755 pomelo-open $(DESTDIR)$(PREFIX)/bin/pomelo-open
	for t in i18n/*.tsv; do install -Dm644 $$t $(DESTDIR)$(PREFIX)/share/pomelo/i18n/$$(basename $$t); done

clean:
	rm -f *.o libpomelo.so* pomelo.pc pomelo-open $(PROTO_HDR) $(PROTOS:%=protocol/%-protocol.c) $(PROTO_OBJ)

.PHONY: all install clean
