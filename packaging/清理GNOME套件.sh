#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# 移除舊版 Pomelo（0.7 以前）順便裝上的 GNOME 相關套件
#
# 安全的做法：不直接 apt remove（那會連帶移除相依它的套件，例如 gdm3 相依 zenity），
# 而是把這些套件標記成「自動安裝」，再讓 apt autoremove 判斷：
# 只要還有任何已安裝的套件需要它，就會留著。
#
# 用法：先安裝 Pomelo 0.8 以上，再執行  sh 清理GNOME套件.sh
# apt 會先列出要移除的套件，確認後才會動手。

set -e

if ! dpkg -s pomelo-polkit >/dev/null 2>&1; then
	echo "請先安裝 Pomelo 0.8 以上（sudo apt install ./*.deb），再執行這個腳本。"
	exit 1
fi

installed() { dpkg -s "$1" 2>/dev/null | grep -q "^Status: install ok installed"; }

# 1. GDM 的登入畫面本身就是 GNOME Shell，用 GDM 就一定要留著 GNOME 的元件
if installed gdm3; then
	echo "這台電腦用 GDM 登入。GDM 的登入畫面就是 GNOME Shell，需要 zenity、gnome-keyring 等元件，"
	echo "只要還在用 GDM，這些就會保留（不會被移除）。"
	echo
	echo "要完全不裝 GNOME，可以改用 LightDM（約 130 個套件，不含 GNOME）。"
	printf "要現在改用 LightDM 嗎？[y/N] "
	read ans
	case "$ans" in
	y|Y|yes)
		sudo apt-get install --no-install-recommends lightdm lightdm-gtk-greeter
		echo "lightdm shared/default-x-display-manager select lightdm" | sudo debconf-set-selections
		sudo env DEBIAN_FRONTEND=noninteractive dpkg-reconfigure lightdm
		echo "/usr/sbin/lightdm" | sudo tee /etc/X11/default-display-manager >/dev/null
		# gdm3 改成自動安裝：下面的 autoremove 會連同 GNOME Shell 一起移除
		sudo apt-mark auto gdm3 >/dev/null
		echo "已改用 LightDM，重新開機後生效。"
		;;
	*)
		echo "保留 GDM。"
		;;
	esac
	echo
fi

# 2. 正在用的輸入法、登入相關的套件標記成「手動安裝」，autoremove 不會動到
keep=""
for p in fcitx5 fcitx5-chewing fcitx5-frontend-gtk3 fcitx5-frontend-gtk4 fcitx5-table-extra fcitx5-chinese-addons \
         fcitx5-rime fcitx5-mozc fcitx5-hangul ibus ibus-chewing \
         lightdm lightdm-gtk-greeter unity-greeter slick-greeter sddm \
         xserver-xorg xserver-xorg-core xserver-xorg-input-all xserver-xorg-video-all xinit x11-xserver-utils \
         dbus-x11 dbus-user-session polkitd pkexec; do
	installed "$p" && keep="$keep $p"
done
# 還在用 GDM 的話，GDM 也要保留
if installed gdm3 && [ "$(cat /etc/X11/default-display-manager 2>/dev/null)" = "/usr/sbin/gdm3" ]; then
	keep="$keep gdm3"
fi
[ -n "$keep" ] && sudo apt-mark manual $keep >/dev/null

# 3. 舊版帶進來的 GNOME／Qt 元件標記成「自動安裝」（不直接移除）
auto=""
for p in policykit-1-gnome gnome-keyring gnome-keyring-pkcs11 libpam-gnome-keyring pinentry-gnome3 gcr \
         zenity zenity-common im-config fcitx5-frontend-all fcitx5-config-qt fcitx5-frontend-gtk2 \
         fcitx5-frontend-qt6 qt5-gtk-platformtheme qt6-gtk-platformtheme; do
	installed "$p" && auto="$auto $p"
done
[ -n "$auto" ] && sudo apt-mark auto $auto >/dev/null

# 4. autoremove 只會移除「已經沒有任何套件需要」的（會先列出清單請你確認）
echo "下面是可以安全移除的套件："
sudo apt-get autoremove --purge

echo
echo "完成。登出再登入後，需要管理員權限時會跳出 Pomelo 自己的密碼視窗。"
