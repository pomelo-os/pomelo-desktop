#!/bin/sh
# 在「已經裝好 Pomelo 0.35 的 Debian 13」上，各瀏覽器多佔多少：套件數、下載量、磁碟
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq >/dev/null 2>&1
for b in "pomelo:libwebkitgtk-6.0-4 ibus-gtk4" "firefox:firefox-esr" "chromium:chromium"; do
	name=${b%%:*}; pkgs=${b#*:}
	for mode in default minimal; do
		opt=""; [ $mode = minimal ] && opt="--no-install-recommends"
		n=$(apt-get install -s $opt $pkgs 2>/dev/null | grep -c '^Inst ')
		dl=$(apt-get install --assume-no $opt $pkgs 2>/dev/null | sed -n 's/^Need to get \([0-9.,]* [kMG]B\).*/\1/p' | head -1)
		echo "SIZE $name $mode packages=$n download=$dl"
	done
done
