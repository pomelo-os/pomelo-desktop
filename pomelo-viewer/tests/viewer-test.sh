#!/bin/sh
# pomelo-viewer 原生 Wayland 測試
. "$(dirname "$0")/../../libpomelo/tests/wl-apps/lib.sh"
start; fresh
H=$P/home; mkdir -p $H/pics; cp $P/pics/* $H/pics/; cd $H
env -u DISPLAY $P/bin/pomelo-viewer $H/pics/one.png > $P/viewer.log 2>&1 &
VP=$!
sleep 2.5; snap viewer-wl-open
keyk Right; sleep 0.8; snap viewer-wl-next
click 640 400; key '+'; key '+'; snap viewer-wl-zoom
key r; sleep 0.5; snap viewer-wl-rot
ctrl c; sleep 0.5
echo "clip: $($P/bin/clipget image/png $P/clip.png)"; file $P/clip.png
click 640 400 1; snap viewer-wl-ctx; keyk Escape
key i; snap viewer-wl-info; key i
key f; sleep 1; snap viewer-wl-fs; key f; sleep 0.8
snap viewer-wl-unfs
# 拖放：dnd-test 拖 three.png 進視窗
DND_FILES=$H/pics/three.png $P/bin/dnd-test SRC src > $P/dndsrc.log 2>&1 &
DP=$!
sleep 1.5; movewin -450 150
read -r sx sy sw sh <<EOT2
$(topwin)
EOT2
dragto $((sx + 60)) $((sy + 110)) 800 300
kill $DP; sleep 0.5
snap viewer-wl-dnd
click 700 300
ctrl w; sleep 0.8
if kill -0 $VP 2>/dev/null; then echo STILL; else echo closed; fi
cat $P/viewer.log
stop
