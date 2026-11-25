#!/bin/sh
# 在 pomelo-build 容器裡編主機缺開發檔的檔案找重疊 snprintf（掛載唯讀）：docker run --rm -v ~/wm:/src:ro -v 清單:/list:ro pomelo-build:24.04 sh /list.sh
cd /src
export LC_ALL=C
PK="cairo x11 xext xrandr xft wayland-client wayland-server xkbcommon pixman-1 libinput gtk4 webkitgtk-6.0 libchewing xcb xcb-icccm dbus-1 libsecret-1 libarchive libpipewire-0.3 libpng libjpeg poppler-glib gio-2.0 glib-2.0 libssl libcrypto"
INC="-Ilibpomelo -Ilibpomelo/protocol -Ipomelo-compositor -Ipomelo-compositor/protocol -Ipomelo-doctor -Ipomelo-system -Ipomelo-viewer -Ipomelo-screensaver -Ipomelo-input -Ipomelo-input/protocol -I/usr/include/libdrm $(for p in $PK; do pkg-config --cflags $p 2>/dev/null; done) $WLR"
for f in $(cat /list); do
  d=$(dirname $f)
  out=$(gcc -O2 -D_FORTIFY_SOURCE=2 -D_GNU_SOURCE -DWLR_USE_UNSTABLE -Wrestrict -c $INC -I$d -I$d/.. $f -o /dev/null 2>&1)
  echo "$out" | grep -E "overlaps|restrict" | grep warning | sed "s|^|$f: |"
  echo "$out" | grep -q "fatal error" && echo "編不過: $f: $(echo "$out" | grep -m1 'fatal error')" >&2
done
