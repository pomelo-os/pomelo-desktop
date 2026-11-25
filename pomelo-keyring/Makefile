# SPDX-License-Identifier: GPL-3.0-or-later
# pomelo-keyring — Pomelo 鑰匙圈（Secret Service）與登入自動解鎖的 PAM 模組
# PAM 設定的寫法：debian（@include common-*）或 redhat（include system-auth、password-auth）
PAMSTYLE ?= debian
PAMSUFFIX = $(if $(filter redhat,$(PAMSTYLE)),.redhat,)
PREFIX ?= /usr/local
CFLAGS ?= -Os -pipe -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation
# 編譯防護（Ubuntu 的 gcc 預設就有，Fedora／Rocky 要自己加）：堆疊保護、PIE、FORTIFY、完整 RELRO
override CFLAGS += -fstack-protector-strong -fstack-clash-protection -D_FORTIFY_SOURCE=3 -fPIE -pie
override LDFLAGS += -Wl,-z,relro,-z,now
POMELO_CFLAGS ?= $(shell pkg-config --cflags pomelo)
POMELO_LIBS   ?= $(shell pkg-config --libs pomelo)
MULTIARCH := $(shell $(CC) -print-multiarch)
# PAM 模組不管 PREFIX 是什麼，都要放在 PAM 找得到的地方
PAMDIR ?= /usr/lib/$(MULTIARCH)/security

all: pomelo-keyring pam_pomelo_keyring.so pomelo-keyring.png

pomelo-keyring: pomelo-keyring.c service.c store.c store.h service.h
	$(CC) $(CFLAGS) $(POMELO_CFLAGS) $$(pkg-config --cflags dbus-1 libcrypto) -o $@ pomelo-keyring.c service.c store.c \
		$(POMELO_LIBS) $$(pkg-config --libs dbus-1 libcrypto) -lm $(LDFLAGS)

pam_pomelo_keyring.so: pam_pomelo_keyring.c
	$(CC) $(CFLAGS) -fPIC -shared -o $@ pam_pomelo_keyring.c -lpam $(LDFLAGS)

pomelo-keyring.png: pomelo-keyring
	LD_LIBRARY_PATH="$(LD_LIBRARY_PATH)" ./pomelo-keyring --icon $@

install: all
	install -Dm755 pomelo-keyring $(DESTDIR)$(PREFIX)/bin/pomelo-keyring
	install -Dm644 pam_pomelo_keyring.so $(DESTDIR)$(PAMDIR)/pam_pomelo_keyring.so
	# Debian／Ubuntu：交給 pam-auth-update 加進 common-auth；Fedora／RHEL 系由 pomelo-dm 的 PAM 設定直接載入
	if [ "$(PAMSTYLE)" != redhat ]; then install -Dm644 pomelo-keyring.pam-config $(DESTDIR)/usr/share/pam-configs/pomelo-keyring; fi
	install -Dm644 pomelo-keyring.desktop $(DESTDIR)$(PREFIX)/share/applications/pomelo-keyring.desktop
	install -Dm644 pomelo-keyring.png $(DESTDIR)$(PREFIX)/share/icons/hicolor/256x256/apps/pomelo-keyring.png

clean:
	rm -f pomelo-keyring pam_pomelo_keyring.so pomelo-keyring.png

.PHONY: all install clean
