#!/bin/bash
# pomelo-archive 介面測試：用 Xvfb（:93）＋xdotool 實際點過一遍，檢查產生的檔案；截圖放在 $SHOTS（預設不存）。
# 用法：PA=/path/to/pomelo-archive SHOTS=/some/dir bash test-gui.sh
cd "$(dirname "$0")" || exit 1
HERE=$PWD
. ./lib.sh
PA=${PA:-pomelo-archive}
export DISPLAY=:93 POMELO_BACKEND=x11 LANG=zh_TW.UTF-8 GNUPGHOME=$(mktemp -d)
unset LC_ALL
[ -d /usr/share/pomelo/i18n ] && export POMELO_I18N_DIR=/usr/share/pomelo/i18n
W=$(mktemp -d /tmp/pagui.XXXXXX); export HOME=$W/home; mkdir -p $HOME
XV=
cleanup() { [ -n "$XV" ] && kill $XV 2>/dev/null; rm -rf "$W" "$GNUPGHOME"; }
trap cleanup EXIT
have Xvfb && have xdotool && have import || { echo "略過介面測試：缺 Xvfb／xdotool／imagemagick"; exit 0; }
Xvfb :93 -screen 0 1280x900x24 >/dev/null 2>&1 & XV=$!
sleep 1.2
cd "$W" || exit 1
bash "$HERE/make-data.sh" "$W/data"
[ -n "$SHOTS" ] && mkdir -p "$SHOTS"
shot() { [ -n "$SHOTS" ] && import -window root "$SHOTS/$1.png" 2>/dev/null; true; }
c()  { xdotool mousemove "$1" "$2"; sleep 0.25; xdotool click "${3:-1}"; sleep "${4:-0.7}"; }
dc() { xdotool mousemove "$1" "$2"; sleep 0.3; xdotool click --repeat 2 --delay 90 1; sleep "${3:-1.2}"; }
waitfor() { local n=0; while ! eval "$1" 2>/dev/null; do sleep 0.3; n=$((n+1)); [ $n -gt ${2:-40} ] && return 1; done; return 0; }
winname() { xdotool search --onlyvisible --name "$1" 2>/dev/null | head -1; }

echo "== 建立壓縮檔視窗"
$PA --compress "$W/data" >/dev/null 2>&1 & P=$!
sleep 2.5
shot gui-compress-form
kill -0 $P 2>/dev/null && ok "建立壓縮檔視窗出現（程式在運作）" || bad "建立壓縮檔視窗沒有出現"
c 858 554 1 0.4           # 建立（預設 zip）
waitfor "[ -f '$W/data.zip' ]" 30 && ok "按『建立』後產生了 data.zip" || bad "沒有產生 data.zip"
sleep 1.5; shot gui-compress-done
$PA --test "$W/data.zip" >/dev/null 2>&1 && ok "產生的 zip 可以通過測試" || bad "產生的 zip 沒通過測試"
kill $P 2>/dev/null; wait $P 2>/dev/null

echo "== 加密建立（AES-256 zip）"
rm -f data.zip
$PA --compress "$W/data" >/dev/null 2>&1 & P=$!
sleep 2.5
c 860 492                                   # 加密開關
c 752 536 1 0.3; xdotool type --delay 25 "secret123"
c 752 580 1 0.3; xdotool type --delay 25 "secret123"
shot gui-compress-enc
c 858 656 1 0.4
waitfor "[ -f '$W/data.zip' ]" 30 && ok "加密建立：產生了 data.zip" || bad "加密建立：沒有產生"
sleep 1; kill $P 2>/dev/null; wait $P 2>/dev/null
$PA --test data.zip >/dev/null 2>&1; [ $? = 2 ] && ok "加密的 zip：沒給密碼 → 要密碼（2）" || bad "加密的 zip 沒有要求密碼"
printf 'secret123\n' | $PA --test data.zip --password-stdin >/dev/null 2>&1 && ok "加密的 zip：密碼 secret123 可以通過" || bad "加密的 zip：密碼"
printf 'secret123\n' | $PA --list data.zip --password-stdin 2>/dev/null | grep -q "你好 世界.txt" && ok "加密的 zip：清單裡有中文檔名" || bad "加密的 zip：清單"

echo "== 解壓縮視窗：要密碼、密碼錯、密碼對、同名衝突"
mkdir -p out1 && cp data.zip out1/
$PA --extract-here "$W/out1/data.zip" >/dev/null 2>&1 & P=$!
sleep 2.5
shot gui-extract-password
kill -0 $P 2>/dev/null && ok "解壓縮視窗出現（要密碼，程式在等輸入）" || bad "解壓縮視窗沒出現"
c 742 417 1 0.3; xdotool type --delay 25 "wrongpw"; xdotool key Return; sleep 2
shot gui-extract-wrong
[ ! -d out1/data ] && ok "密碼錯：沒有解出任何東西" || bad "密碼錯卻解出了東西"
c 742 417 1 0.3; xdotool key ctrl+a BackSpace; xdotool type --delay 25 "secret123"; xdotool key Return
waitfor "[ -f '$W/out1/data/dir/numbers.txt' ]" 40 && ok "密碼對：解壓縮完成" || bad "密碼對卻沒有解壓縮"
sleep 1; shot gui-extract-done
cmp -s data/dir/numbers.txt out1/data/dir/numbers.txt && ok "解出來的內容一致" || bad "解出來的內容不一致"
kill $P 2>/dev/null; wait $P 2>/dev/null
# 再解一次：同名衝突 → 兩個都留著
printf 'secret123\n' | $PA --extract-here "$W/out1/data.zip" --password-stdin >/dev/null 2>&1 & P=$!
sleep 3.5
shot gui-extract-conflict
c 651 427 1 0.5                              # 兩個都留著
waitfor "[ -d '$W/out1/data (2)' ]" 40 && ok "同名衝突：選『兩個都留著』→ 產生 data (2)" || bad "同名衝突的處理"
kill $P 2>/dev/null; wait $P 2>/dev/null

echo "== 瀏覽視窗"
python3 - <<'PY'
import zipfile, random
with zipfile.ZipFile('big.zip','w',zipfile.ZIP_DEFLATED) as z:
    z.writestr('README.md','x')
    random.seed(2)
    for i in range(12000): z.writestr('files/批次%02d/報告_%05d.txt' % (i%40, i), 'y'*random.randint(0,500))
PY
$PA "$W/big.zip" >/dev/null 2>&1 & P=$!
sleep 3
shot gui-browse-root
xdotool search --name 'big.zip' >/dev/null 2>&1 && ok "瀏覽視窗標題有檔名" || bad "瀏覽視窗標題"
dc 300 142; sleep 0.5; dc 300 142
shot gui-browse-in
for i in 1 2 3 4 5 6 7 8; do xdotool click 5; done; sleep 0.5
shot gui-browse-scrolled
c 300 238 3 0.8; shot gui-browse-menu; xdotool key Escape; sleep 0.4
c 760 112 1 0.5; shot gui-browse-sorted
kill -0 $P 2>/dev/null && ok "瀏覽視窗操作過後還在運作" || bad "瀏覽視窗當掉了"
kill $P 2>/dev/null; wait $P 2>/dev/null
# （對話視窗會置中在螢幕上：建立視窗 560x600 在 (360,150)、解壓視窗 520x270 在 (380,315)，座標已加上位移）
# 拖放：用 test/xdnd-drop.c 做一個 XDND 來源，把檔案放到視窗上
if have gcc; then
  gcc -O1 -o "$W/xdnd-drop" "$HERE/xdnd-drop.c" -lX11 2>/dev/null
fi
if [ -x "$W/xdnd-drop" ]; then
  echo "== 拖放（XDND）"
  $PA >/dev/null 2>&1 & P=$!
  sleep 2
  WID=$(xdotool search --class pomelo-archive 2>/dev/null | head -1)
  "$W/xdnd-drop" "$WID" "$W/big.zip" 2>&1 | head -2
  sleep 3
  shot gui-drop-open
  xdotool search --name 'big.zip' >/dev/null 2>&1 && ok "把壓縮檔拖進空白視窗 → 開啟了（標題有檔名）" || bad "拖放壓縮檔沒有開啟"
  # 再拖一般檔案進去（加入 zip）
  mkdir -p "$W/dropadd" && cp "$W/big.zip" "$W/dropadd/target.zip" && echo hello > "$W/dropadd/新檔案.txt"
  kill $P 2>/dev/null; wait $P 2>/dev/null
  $PA "$W/dropadd/target.zip" >/dev/null 2>&1 & P=$!
  sleep 3
  WID=$(xdotool search --class pomelo-archive 2>/dev/null | head -1)
  "$W/xdnd-drop" "$WID" "$W/dropadd/新檔案.txt" 2>&1 | head -1
  sleep 4
  $PA --list "$W/dropadd/target.zip" 2>/dev/null | grep -q "新檔案.txt" && ok "把檔案拖進已開啟的 zip → 加入了壓縮檔" || bad "拖放加入檔案"
  kill $P 2>/dev/null; wait $P 2>/dev/null
  pkill -f "pomelo-archive --add-gui" 2>/dev/null
  # 沒開壓縮檔時拖一般檔案：開建立壓縮檔視窗
  $PA >/dev/null 2>&1 & P=$!
  sleep 2
  WID=$(xdotool search --class pomelo-archive 2>/dev/null | head -1)
  "$W/xdnd-drop" "$WID" "$W/dropadd/新檔案.txt" 2>&1 | head -1
  sleep 3
  pgrep -f "pomelo-archive --compress" >/dev/null && ok "沒開壓縮檔時拖一般檔案進來 → 開建立壓縮檔視窗" || bad "拖放建立壓縮檔"
  shot gui-drop-compress
  pkill -f "pomelo-archive --compress" 2>/dev/null; kill $P 2>/dev/null; wait $P 2>/dev/null
else skip "拖放測試" "沒有 gcc 編譯 xdnd-drop"; fi
# 空白視窗
$PA >/dev/null 2>&1 & P=$!; sleep 2; shot gui-browse-empty; kill -0 $P 2>/dev/null && ok "沒有檔案時開空白視窗" || bad "空白視窗當掉"; kill $P 2>/dev/null; wait $P 2>/dev/null
echo
summary
