#!/bin/sh
# pomelo-files X11 後端回歸（Xvfb :97、沒有視窗管理員，視窗在 0,0）：開啟、雙擊、縮圖、重新命名、複製貼上、右鍵、拖放（內部與 xdnd-test）、列表
. "$(dirname "$0")/../../libpomelo/tests/wl-apps/lib.sh"
fresh; xstart; H=$P/home; sh "$(dirname "$0")/files-setup.sh" $H
export XDG_DATA_HOME=$H/.local/share
$POMELO_ROOT/pomelo-files/pomelo-files $H > $P/filesx.log 2>&1 &
FP=$!; sleep 2.5; xfocus pomelo-files
xsnap files-x11-open
xdotool mousemove 413 102 click 1 click 1; sleep 0.6; xsnap files-x11-pictures
xdotool mousemove 413 102 click 1; xdotool key F2; sleep 0.3; xdotool type --delay 60 new; xdotool key Return; sleep 0.5; xsnap files-x11-renamed; ls $H/圖片
xdotool key ctrl+c; xdotool key BackSpace; sleep 0.5; xdotool mousemove 521 102 click 1 click 1; sleep 0.5; xdotool key ctrl+v; sleep 1.5; xsnap files-x11-pasted; ls $H/文件
xdotool key BackSpace; sleep 0.5
xdotool mousemove 737 102 click 3; sleep 0.5; xsnap files-x11-ctx; xdotool key Escape; sleep 0.3
echo "== 內部拖放：data.csv → 圖片"
xdotool mousemove 737 102 click 1; sleep 0.7; xdotool mousedown 1; xdotool mousemove_relative 20 0; sleep 0.2; xdotool mousemove 500 105; sleep 0.2; xdotool mousemove 413 105; sleep 0.6; xsnap files-x11-drag-hover; xdotool mouseup 1; sleep 1.5
ls $H/圖片 | tr '\n' ' '; echo
echo "== files → xdnd-test"
$P/bin/xdnd-test XDST dst > $P/xdst.log 2>&1 &
XP=$!; sleep 1; xdotool search --name XDST | head -1 | xargs -I{} xdotool windowmove {} 980 100
sleep 0.5
xdotool mousemove 845 102 click 1; sleep 0.7; xdotool mousedown 1; xdotool mousemove_relative 20 0; sleep 0.2; xdotool mousemove 950 110; sleep 0.2; xdotool mousemove 1100 200; sleep 0.8; xdotool mouseup 1; sleep 1.5
cat $P/xdst.log | tail -4; kill $XP
xdotool mousemove 500 450 click 3; sleep 0.4
xsnap files-x11-bgctx; xdotool key Escape
xdotool mousemove 893 30 click 1; sleep 0.5; xsnap files-x11-list
kill $FP; xstop
