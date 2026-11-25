#!/bin/sh
# pomelo-files 原生 Wayland 測試（基本操作）：sh files-test.sh
. "$(dirname "$0")/../../libpomelo/tests/wl-apps/lib.sh"
start; fresh
H=$P/home; sh "$(dirname "$0")/files-setup.sh" $H
export XDG_DATA_HOME=$H/.local/share
env -u DISPLAY $POMELO_ROOT/pomelo-files/pomelo-files $H > $P/files.log 2>&1 &
FP=$!
sleep 2
dbl() { vp abs $1 $2 wait 100 click 0 wait 120 click 0; sleep 0.6; }
echo "== 圖片：雙擊、縮圖、重新命名"
dbl 572 160; snap files-wl-pictures
click 572 160; keyk F2; key 'new'; keyk Return; sleep 0.5; snap files-wl-renamed; ls $H/圖片
echo "== 複製、貼上到文件（Ctrl+C／V）"
ctrl c; echo "clip: $($P/bin/clipget)"; sleep 1
keyk BackSpace; sleep 0.5; dbl 681 160; ctrl v; sleep 1.5; snap files-wl-pasted; ls $H/文件
echo "== 剪下、貼上（Ctrl+X 把 a.txt 搬到 空資料夾）"
keyk BackSpace; sleep 0.5; dbl 681 160   # 進文件；排序：a.txt b.md p2new.png 三頁.pdf 筆記.txt
click 465 160; ctrl x; keyk BackSpace; sleep 0.4; dbl 789 160; ctrl v; sleep 1.5; snap files-wl-cut; ls $H/空資料夾 $H/文件
ctrl v; sleep 1; ls $H/空資料夾    # 第二次貼上不該再有東西
echo "== 右鍵（項目、空白處）"
keyk BackSpace; sleep 0.5
click 897 160 1; snap files-wl-ctx-item; keyk Escape
click 700 450 1; snap files-wl-ctx-bg; keyk Escape
echo "== 列表檢視、排序"
click 893 84; sleep 0.4; snap files-wl-list
click 770 122; sleep 0.4; snap files-wl-list-sorted
click 859 84
echo "== 新增資料夾、搜尋"
keyk Escape
ctrl f; key 'read'; sleep 0.5; snap files-wl-search; keyk Escape; keyk Escape
echo "== 丟到垃圾桶、垃圾桶檢視、放回"
click 1005 160; keyk Delete; sleep 1; snap files-wl-trashed; ls $H/.local/share/Trash/files
click 217 197; sleep 0.6; snap files-wl-trash
click 465 160 1; snap files-wl-trash-ctx; keyk Escape
keyk Escape
click 1005 160 1; keyk Escape
echo "== 垃圾桶裡：放回原處（右鍵第一項）"
click 217 197; sleep 0.6; click 465 160 1; sleep 0.3
snap files-wl-trash-ctx2
keyk Escape
ls $H/.local/share/Trash/files
echo "== 調整視窗大小（拖右下角）"
click 217 109; sleep 0.5
vp abs 1119 657 down wait 150 rel -250 -200 wait 150 rel -150 -100 wait 150 up wait 800; snap files-wl-resized
kill $FP; sleep 0.5; cat $P/files.log
stop
