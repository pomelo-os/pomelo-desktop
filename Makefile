# SPDX-License-Identifier: GPL-3.0-or-later
# Pomelo — 輕量、省資源的 X11 桌面
#
#   make              編譯全部（使用這個資料夾裡的 libpomelo）
#   make test         在 Xephyr 巢狀視窗中試用整個桌面
#   make deb          產生各套件的 .deb（放在 dist/<發行版>/；在 Ubuntu 的建置環境裡執行）
#   make rpm          產生各套件的 .rpm（放在 dist/<發行版>/；在 Fedora、Rocky 的建置環境裡執行）
#   make dist         用 Docker 一次建好 Ubuntu 24.04／26.04、Debian 13、Fedora 44／45、Rocky 9.8 的套件與安裝包
#   sudo make install 直接安裝全部到 /usr/local
#
# 每個子資料夾都是獨立套件，也可以單獨用 pkg-config pomelo 編譯。

VERSION ?= 0.42
PREFIX  ?= /usr/local
APPS     = pomelo-wm pomelo-compositor pomelo-panel pomelo-session pomelo-terminal pomelo-files \
           pomelo-theme pomelo-sound pomelo-network pomelo-system pomelo-control-center \
           pomelo-lock pomelo-notify pomelo-power pomelo-screensaver pomelo-language \
           pomelo-launcher pomelo-software pomelo-display pomelo-bluetooth pomelo-monitor pomelo-storage pomelo-polkit pomelo-assistant pomelo-keyring pomelo-edit pomelo-input pomelo-greeter pomelo-dm pomelo-screenshot pomelo-calc pomelo-calendar pomelo-viewer pomelo-player pomelo-pdf pomelo-paint pomelo-remote pomelo-writer pomelo-sheet pomelo-slides pomelo-vm pomelo-docker pomelo-cluster pomelo-archive pomelo-packets pomelo-doctor pomelo-firewall pomelo-netscan pomelo-gpu pomelo-backup pomelo-vpn
# 小柚瀏覽器要 WebKitGTK 6.0（Rocky 9 沒有）：這台電腦沒有就不編
OPTAPPS := $(shell pkg-config --exists webkitgtk-6.0 2>/dev/null && echo pomelo-browser)
LIB      = $(CURDIR)/libpomelo
UIPKGS   = x11 xext xrandr cairo
INTREE   = POMELO_CFLAGS="-I$(LIB) $$(pkg-config --cflags $(UIPKGS))" \
           POMELO_LIBS="-L$(LIB) -lpomelo -Wl,-rpath-link,$(LIB) $$(pkg-config --libs $(UIPKGS))" \
           LD_LIBRARY_PATH="$(LIB)"

all:
	$(MAKE) -C libpomelo VERSION=$(VERSION) PREFIX=$(PREFIX)
	for d in $(APPS) $(OPTAPPS); do $(MAKE) -C $$d $(INTREE) || exit 1; done

install: all
	$(MAKE) -C libpomelo install PREFIX=$(PREFIX)
	for d in $(APPS) $(OPTAPPS); do $(MAKE) -C $$d install $(INTREE) PREFIX=$(PREFIX) || exit 1; done

test: all
	Xephyr -br -ac -noreset -screen 1280x800 :9 & sleep 1; \
	PATH="$(foreach d,$(APPS) $(OPTAPPS),$(CURDIR)/$(d):)$$PATH" LD_LIBRARY_PATH="$(LIB)" \
	POMELO_I18N_DIR="$(LIB)/i18n" DISPLAY=:9 POMELO_NESTED=1 ./pomelo-session/pomelo-session

deb:
	VERSION=$(VERSION) ./packaging/build-deb.sh

rpm:
	VERSION=$(VERSION) ./packaging/build-rpm.sh

dist:
	VERSION=$(VERSION) ./packaging/build-all.sh

clean:
	$(MAKE) -C libpomelo clean
	for d in $(APPS) pomelo-browser; do $(MAKE) -C $$d clean; done
	rm -rf dist build

.PHONY: rpm dist all install test deb clean
