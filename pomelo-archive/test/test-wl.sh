#!/bin/sh
# pomelo-archive 原生 Wayland 測試（階段 4）：headless compositor 開窗、雙擊進資料夾、右鍵選單、解壓到家目錄、拖檔案進視窗加入、拷貝路徑、資訊、開啟對話窗、加密檔密碼視窗
# 用法：PA=編好的執行檔 POMELO_TEST_DIR=短路徑 sh test-wl.sh   （先 sh libpomelo/tests/wl-apps/setup.sh；截圖在 $P/shots）
. "$(dirname "$0")/../../libpomelo/tests/wl-apps/lib.sh"
PA=${PA:-$POMELO_ROOT/pomelo-archive/pomelo-archive}
start; fresh; mkdir -p $P/arch
bash "$(dirname "$0")/make-data.sh" $P/arch/data >/dev/null
rm -f $P/arch/t.zip $P/arch/e.zip; (cd $P/arch/data && zip -qr ../t.zip . && zip -qr -P secret ../e.zip dir/numbers.txt)
printf 'dropme\n' > $P/arch/dropme.txt
env -u DISPLAY $PA $P/arch/t.zip > $P/arch.log 2>&1 & FP=$!; sleep 2
snap arch-wl-open
vp abs 400 187 wait 100 click 0 wait 90 click 0; sleep 0.6; snap arch-wl-dir        # 雙擊進入 dir
click 400 283 1; snap arch-wl-menu                                                  # 右鍵 numbers.txt
click 480 332 0; click 500 326 0; sleep 2; snap arch-wl-extract                     # 解壓縮選取的項目到… → 家目錄
[ -f $HOME/dir/numbers.txt ] && echo "OK 解壓到家目錄" || echo "FAIL 沒解壓出來"
click 733 267 0; keyk Escape
DND_FILES=$P/arch/dropme.txt env -u DISPLAY $P/bin/dnd-test SRC src > $P/dndsrc.log 2>&1 & DP=$!; sleep 1.2
dragto 640 232 900 500; sleep 1; snap arch-wl-dropped
$PA --list $P/arch/t.zip | grep -q dropme.txt && echo "OK 拖進視窗加入了 dropme.txt" || echo "FAIL 沒加入"
kill $DP $FP 2>/dev/null
env -u DISPLAY $PA $P/arch/e.zip > $P/arch.log 2>&1 & FP=$!; sleep 2.5; snap arch-wl-password
kill -0 $FP && echo "OK 程式還活著"; kill $FP; cat $P/arch.log; stop
