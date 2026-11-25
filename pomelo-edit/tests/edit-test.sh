#!/bin/sh
# pomelo-edit 原生 Wayland 測試：開檔、輸入、選單、右鍵、剪貼簿、存檔、開檔對話窗、拖放、單一視窗、關閉
. "$(dirname "$0")/../../libpomelo/tests/wl-apps/lib.sh"
start; fresh
H=$P/home; mkdir -p $H/docs
printf 'hello 世界\n第二行 line two\nthird line\n' > $H/docs/a.txt
printf 'dropped file\n' > $H/docs/b.txt
cd $H
env -u DISPLAY $POMELO_ROOT/pomelo-edit/pomelo-edit $H/docs/a.txt > $P/edit.log 2>&1 &
EP=$!
sleep 2
snap edit-wl-open
click 640 300                      # 點文字區
key 'XYZ'
ctrl a; ctrl c
echo "clip: $($P/bin/clipget)"
click 640 300
keyk End
ctrl v
snap edit-wl-typed
click 160 68 0                     # 檔案選單
snap edit-wl-menu
keyk Escape
click 640 300 1                    # 右鍵
snap edit-wl-ctx
keyk Escape
ctrl s; sleep 0.5; snap edit-wl-saved
echo "--- saved a.txt:"; cat $H/docs/a.txt
ctrl o; sleep 1
snap edit-wl-opendlg
keyk Escape
# 拖放：dnd-test 的來源視窗拖一個檔案進編輯器
printf 'from drag\n' > $H/docs/c.txt
DND_FILES=$H/docs/c.txt $P/bin/dnd-test SRC src > $P/dndsrc.log 2>&1 &
DP=$!
sleep 1.5; movewin -450 150; snap edit-wl-dndsetup
read -r sx sy sw sh <<EOT
$(topwin)
EOT
dragto $((sx + 60)) $((sy + 110)) 800 400
snap edit-wl-dnd
kill $DP; sleep 0.5; click 700 300
# 第二個行程：交給第一個視窗開成分頁
env -u DISPLAY $POMELO_ROOT/pomelo-edit/pomelo-edit $H/docs/b.txt; sleep 1
snap edit-wl-second
echo "edit procs: $(pgrep -u $(id -u) -f '^$POMELO_ROOT/pomelo-edit/pomelo-edit' | wc -l)"
ctrl w; sleep 0.3; snap edit-wl-afterw; ctrl q; sleep 1; snap edit-wl-afterq
if kill -0 $EP 2>/dev/null; then echo STILL; else echo closed; fi

cat $P/edit.log
stop
