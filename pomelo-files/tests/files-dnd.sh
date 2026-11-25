#!/bin/sh
# pomelo-files 拖放測試：files 拖到 dnd-test（原生目標）與 xdnd-test（XWayland 目標）；dnd-test 來源拖進 files
. "$(dirname "$0")/../../libpomelo/tests/wl-apps/lib.sh"
startp; fresh
H=$P/home; sh "$(dirname "$0")/files-setup.sh" $H
export XDG_DATA_HOME=$H/.local/share
export DND_LOG=$P/dnd.log; : > $DND_LOG
env -u DISPLAY $POMELO_ROOT/pomelo-files/pomelo-files $H > $P/files.log 2>&1 &
FP=$!
sleep 2
movewin -140 0
env -u DISPLAY $P/bin/dnd-test DST dst > $P/dndsrc.log 2>&1 &
DP=$!
sleep 1.5; movewin 560 0; snap dnd-setup
echo "== 1. files → 原生 dnd-test（拷貝）"
click 865 160; dragto 865 160 1120 200; snap dnd-to-native; cat $DND_LOG
ls $H
echo "== 2. files 內部：readme.txt 拖進 空資料夾（同磁碟＝移動）"
kill $DP; sleep 0.5
click 865 160; dragto 865 160 648 165; snap dnd-internal
ls $H/空資料夾; ls $H | tr '\n' ' '; echo
echo "== 3. 原生 dnd-test 來源 → files：拖進 圖片 資料夾、再拖到空白處"
printf 'from-native\n' > $P/data/nat1.txt; printf 'from-native2\n' > $P/data/nat2.txt
DND_FILES=$P/data/nat1.txt DND_ACTIONS=both env -u DISPLAY $P/bin/dnd-test SRC src > $P/dndsrc.log 2>&1 &
DP=$!
sleep 1.5; movewin 560 0
read -r sx sy sw sh <<EOT
$(topwin)
EOT
dragto $((sx + 60)) $((sy + 110)) 433 165; snap dnd-from-native
ls $H/圖片 | tr '\n' ' '; echo; ls $P/data
kill $DP; sleep 0.5
echo "== 4. files → XWayland 的 xdnd-test"
env -u WAYLAND_DISPLAY $P/bin/xdnd-test XDST dst > $P/xdst.log 2>&1 &
XP=$!
sleep 1.5; movewin 560 0; snap xdnd-setup
click 757 160; dragto 757 160 1100 200; snap dnd-to-x; cat $DND_LOG | tail -5
kill $XP; sleep 0.5
echo "== 5. XWayland xdnd-test 來源 → files 空白處（拷貝/移動到 cwd）"
DND_FILES=$P/data/nat2.txt env -u WAYLAND_DISPLAY $P/bin/xdnd-test XSRC src > $P/xsrc.log 2>&1 &
XP=$!
sleep 1.5; movewin 560 0
read -r sx sy sw sh <<EOT
$(topwin)
EOT
dragto $((sx + 60)) $((sy + 110)) 500 450; snap dnd-from-x
ls $H | tr '\n' ' '; echo; ls $P/data
kill $XP; sleep 0.5
echo "== 6. GTK4（原生 Wayland）互拖：files → GTK4 目標、GTK4 來源 → files"
printf 'g\n' > $P/data/gtk1.txt
GTK4_DND_FILES=$P/data/gtk1.txt GDK_BACKEND=wayland env -u DISPLAY python3 $POMELO_ROOT/libpomelo/tests/gtk4-dnd.py > $P/gtk4.log 2>&1 &
GP=$!
sleep 3; movewin 560 0; snap gtk4-setup
read -r gx gy gw gh <<EOT
$(topwin)
EOT
click 757 160; dragto 757 160 $((gx + gw / 2)) $((gy + 90)); snap dnd-to-gtk4; cat $P/gtk4.log
dragto $((gx + gw / 2)) $((gy + gh - 60)) 500 450; snap dnd-from-gtk4; cat $P/gtk4.log
ls $H | tr '\n' ' '; echo
kill $GP; kill $FP; sleep 0.5; cat $P/files.log
stop
