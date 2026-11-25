# SPDX-License-Identifier: GPL-3.0-or-later
# pomelo-dm — Pomelo 登入管理員（只需要 libc 與 libpam）
# PAM 設定的寫法：debian（@include common-*）或 redhat（include system-auth、password-auth）
PAMSTYLE ?= debian
PAMSUFFIX = $(if $(filter redhat,$(PAMSTYLE)),.redhat,)
PREFIX ?= /usr/local
CFLAGS ?= -Os -pipe -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation
# 編譯防護（Ubuntu 的 gcc 預設就有，Fedora／Rocky 要自己加）：堆疊保護、PIE、FORTIFY、完整 RELRO
override CFLAGS += -fstack-protector-strong -fstack-clash-protection -D_FORTIFY_SOURCE=3 -fPIE -pie
override LDFLAGS += -Wl,-z,relro,-z,now
VERSION ?= $(shell sed -n 's/.*POMELO_VERSION "\(.*\)"/\1/p' ../libpomelo/pomelo.h)

pomelo-dm: pomelo-dm.c
	$(CC) $(CFLAGS) -DVERSION='"$(VERSION)"' -o $@ pomelo-dm.c -lpam $(LDFLAGS)

install: pomelo-dm
	install -Dm755 pomelo-dm $(DESTDIR)$(PREFIX)/sbin/pomelo-dm
	install -Dm755 pomelo-dm-switch $(DESTDIR)$(PREFIX)/sbin/pomelo-dm-switch
	install -Dm644 pomelo-dm.pam$(PAMSUFFIX) $(DESTDIR)/etc/pam.d/pomelo-dm
	install -Dm644 pomelo-dm-autologin.pam$(PAMSUFFIX) $(DESTDIR)/etc/pam.d/pomelo-dm-autologin
	install -Dm644 dm.conf $(DESTDIR)/etc/pomelo/dm.conf
	install -Dm644 pomelo-dm.service $(DESTDIR)/usr/lib/systemd/system/pomelo-dm.service
	install -Dm644 10-pomelo.conf $(DESTDIR)/usr/share/X11/xorg.conf.d/10-pomelo.conf

clean:
	rm -f pomelo-dm

.PHONY: install clean
