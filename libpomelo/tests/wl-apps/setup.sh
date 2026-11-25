#!/bin/sh
# 編測試工具：vptr（虛擬滑鼠）、clipget、dnd-test、xdnd-test、xft-metrics → $POMELO_TEST_DIR/bin
# 需要：wayland-scanner、libwayland-dev、libx11-dev、libcairo2-dev、libxft-dev；libpomelo 要先編好（make -C libpomelo）
: "${POMELO_ROOT:=$HOME/wm}"; : "${POMELO_TEST_DIR:=/tmp/pomelo-wl-tests}"
B=$POMELO_TEST_DIR/bin; mkdir -p $B $POMELO_TEST_DIR/build
T=$POMELO_ROOT/libpomelo/tests; L=$POMELO_ROOT/libpomelo
cd $POMELO_TEST_DIR/build
wayland-scanner client-header $T/wl-apps/vptr/vp.xml vp.h && wayland-scanner private-code $T/wl-apps/vptr/vp.xml vp.c
wayland-scanner client-header $T/wl-apps/vkbd/vk.xml vk.h && wayland-scanner private-code $T/wl-apps/vkbd/vk.xml vk.c
cc -I. -o $B/vkbd $T/wl-apps/vkbd/vkbd.c vk.c $(pkg-config --cflags --libs wayland-client xkbcommon)
cc -I. -o $B/vptr $T/wl-apps/vptr/vptr.c vp.c $(pkg-config --cflags --libs wayland-client)
cc -o $B/clipget $T/wl-apps/clipget.c -I$L -L$L -lpomelo $(pkg-config --cflags --libs cairo x11) -Wl,-rpath,$L
cc -o $B/dnd-test $T/dnd-test.c -I$L -L$L -lpomelo $(pkg-config --cflags --libs cairo x11) -Wl,-rpath,$L
cc -o $B/xdnd-test $T/xdnd-test.c -lX11
cc -o $B/idleprobe $T/wl-apps/idleprobe.c -I$L -L$L -lpomelo $(pkg-config --cflags --libs cairo x11) -Wl,-rpath,$L
cc -o $B/xft-metrics $T/wl-apps/xft-metrics.c $(pkg-config --cflags --libs xft fontconfig cairo x11)
mkdir -p $POMELO_TEST_DIR/pics && cp $T/wl-apps/pics/* $POMELO_TEST_DIR/pics/
echo "工具編好了：$B"
