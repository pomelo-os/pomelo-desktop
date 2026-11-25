# SPDX-License-Identifier: GPL-3.0-or-later
# lib.sh — 原生 Wayland 程式的端到端測試共用函式（階段 4）。各程式的 tests/*.sh 用 `. "$(dirname "$0")/<相對路徑>/libpomelo/tests/wl-apps/lib.sh"` 載入。
#
# 事前準備：sh libpomelo/tests/wl-apps/setup.sh   （編 vptr、clipget、dnd-test、xdnd-test，放進 $POMELO_TEST_DIR/bin）
# 環境變數：
#   POMELO_ROOT       原始碼樹（預設 ~/wm）；程式用 make POMELO_CFLAGS=… 指向這裡的 libpomelo 編好，放在各自資料夾
#   POMELO_TEST_DIR   測試工作目錄（預設 /tmp/pomelo-wl-tests；放假 HOME、截圖、記錄、bin）
# 注意：只在私有的 XDG_RUNTIME_DIR 開 headless compositor，只結束那裡的行程（絕對不要用 pkill 名稱）；測試用假 HOME。
# 測試工具：vptr（虛擬滑鼠）、wtype（鍵盤）、grim（截圖）、wlrctl、xdotool／Xvfb :97（X11 對照）、ImageMagick。
: "${POMELO_ROOT:=$HOME/wm}"
: "${POMELO_TEST_DIR:=/tmp/pomelo-wl-tests}"
S=$POMELO_TEST_DIR            # 相容舊腳本的名稱
P=$S; WL=$S/wl; RUN=$S/r; WM=$POMELO_ROOT   # RUN：私有 XDG_RUNTIME_DIR（Unix socket 路徑上限 108 字元，所以名稱要短）
TL=$POMELO_ROOT/libpomelo/tests/wl-apps
mkdir -p $P/shots $P/bin $WL $RUN; chmod 700 $RUN
# 開 headless compositor（私有 XDG_RUNTIME_DIR）。PATCHED=1：用有 patch 的 wlroots（POMELO_WLROOTS_BUILD 指到它的 build 資料夾），
# 原生拖到 XWayland 程式要這個
wl_start() {
  sh $TL/killmine.sh $RUN >/dev/null 2>&1
  export XDG_RUNTIME_DIR=$RUN WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1
  unset DISPLAY WAYLAND_DISPLAY
  ( cd $POMELO_ROOT/pomelo-compositor; [ "${PATCHED:-0}" = 1 ] && export LD_LIBRARY_PATH=$POMELO_WLROOTS_BUILD
    setsid ./pomelo-compositor > $WL/comp.log 2>&1 & )
  for i in $(seq 50); do grep -aq "XWayland 就緒" $WL/comp.log && break; sleep 0.1; done
  export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[^ ]*" $WL/comp.log | head -1 | cut -d= -f2)
  export DISPLAY=$(grep -ao "DISPLAY=:[0-9]*" $WL/comp.log | head -1 | cut -d= -f2)
  export NO_AT_BRIDGE=1 GDK_BACKEND=wayland
}
start() { wl_start
  export LD_LIBRARY_PATH=$POMELO_ROOT/libpomelo XDG_CONFIG_HOME=$P/cfg XDG_DATA_HOME=$P/data HOME=$P/home GTK_IM_MODULE=gtk-im-context-simple
  export POMELO_I18N_DIR=$POMELO_ROOT/libpomelo/i18n; export PATH=$P/bin:$PATH; }
startp() { PATCHED=1 start; }
snap() { kill -0 $FP 2>/dev/null || echo "!! 程式已結束（$1 之前）"; grim "$P/shots/$1.png" && echo "$P/shots/$1.png"; }
vp() { VW=1280 VH=720 $P/bin/vptr "$@"; }
click() { vp abs $1 $2 wait 120 click ${3:-0}; sleep 0.3; }
key() { wtype -k Shift_L -d 30 "$@"; sleep 0.3; }
stop() { sh $TL/killmine.sh $RUN >/dev/null 2>&1; }
keyk() { wtype -k Shift_L -d 30 $(for k; do printf -- "-k %s " $k; done); sleep 0.3; }   # 具名按鍵：keyk Return Tab
# Ctrl+某鍵（第一個 wtype 讓虛擬鍵盤的 keymap 先換好，不然修飾鍵會掉）
ctrl() { wtype -k Shift_L -d 30; sleep 0.3; wtype -d 40 -M ctrl -k "$1" -m ctrl; sleep 0.4; }
fresh() { rm -rf $P/home $P/cfg $P/data; mkdir -p $P/home/.config/pomelo $P/home/.local/share/pomelo $P/cfg/pomelo $P/data; }
# X11 後端對照：Xvfb :97（-dpi 96 跟原生一樣的字型大小；只記自己的 PID）
xstart() { Xvfb :97 -dpi 96 -screen 0 1280x720x24 >/dev/null 2>&1 & echo $! > $P/xvfb97.pid; sleep 1; export DISPLAY=:97 POMELO_BACKEND=x11 HOME=$P/home XDG_CONFIG_HOME=$P/cfg XDG_DATA_HOME=$P/data LD_LIBRARY_PATH=$POMELO_ROOT/libpomelo POMELO_I18N_DIR=$POMELO_ROOT/libpomelo/i18n GTK_IM_MODULE=gtk-im-context-simple; unset WAYLAND_DISPLAY; }
xstop() { kill $(cat $P/xvfb97.pid) 2>/dev/null; rm -f $P/xvfb97.pid; }
xsnap() { import -window root "$P/shots/$1.png" && echo "$P/shots/$1.png"; }
xfocus() { xdotool search --class "$1" | head -1 | xargs -I{} xdotool windowfocus {}; }
xclick() { xdotool mousemove $1 $2 click ${3:-1}; sleep 0.3; }
# 最上面的原生視窗的框：x y 寬 高（要在 start 之後；用 compositor 的 XWayland 根視窗屬性）
topwin() { xprop -root _POMELO_WL_STACK | sed 's/.*= //; s/,//g' | awk '{print $1, $2, $3, $4}'; }
# 用標題列把最上面的視窗拖 dx dy
movewin() { set -- "$1" "$2"; read -r x y w h <<EOT
$(topwin)
EOT
  vp abs $((x + w / 2 - 60)) $((y + 13)) down wait 100 rel "$1" "$2" wait 100 up wait 300; }
# 從 (x1,y1) 拖到 (x2,y2)（拖放用）
dragto() { vp abs $1 $2 down wait 100 rel 20 0 wait 100 abs $(( ($1 + $3) / 2 )) $(( ($2 + $4) / 2 )) wait 100 abs $3 $4 wait 300 up wait 1200; }
