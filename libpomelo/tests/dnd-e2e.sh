#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# dnd-e2e.sh — 拖放端到端測試：headless 的 pomelo-compositor ＋ 虛擬滑鼠（vptr）＋ dnd-test／xdnd-test。
# 事前準備（環境變數）：
#   WL_ENV  ：source 之後 WAYLAND_DISPLAY、DISPLAY、XDG_RUNTIME_DIR 指到「測試用」compositor 的檔（不要用使用者真正的工作階段）
#   VPTR    ：虛擬滑鼠指令（指令 abs X Y／rel DX DY／down／up／wait MS，見 scratchpad/vptr/vptr.c）
#   BIN     ：放 dnd-test、xdnd-test 的資料夾（編譯指令在各檔開頭的註解）
#   PATCHED=1：compositor 用的 wlroots 有 dnd.c 的 patch 才測「原生 → XWayland 放下」
# 用法：WL_ENV=… VPTR=… BIN=… PATCHED=1 ./dnd-e2e.sh      每項印 PASS／FAIL，有失敗結束碼 1
. "$WL_ENV"
export DND_LOG=${TMPDIR:-/tmp}/dnd-e2e.$$.log
D=${TMPDIR:-/tmp}/dnd-e2e-files.$$
mkdir -p "$D"; echo a > "$D/a b.txt"; echo b > "$D/é.txt"
FILES="$D/a b.txt:$D/é.txt"
fail=0; pids=()
check() { # 名稱 樣式（在記錄檔裡要找得到）
	if grep -aq -- "$2" "$DND_LOG"; then echo "PASS $1"; else echo "FAIL $1（找不到：$2）"; sed 's/^/    /' "$DND_LOG"; fail=1; fi
}
nocheck() { if grep -aq -- "$2" "$DND_LOG"; then echo "FAIL $1（不該出現：$2）"; sed 's/^/    /' "$DND_LOG"; fail=1; else echo "PASS $1"; fi; }
start() { # 名稱 指令…（背景執行，記下 PID）
	local n=$1; shift
	"$@" >/dev/null 2>&1 & pids+=($!); sleep 1.3
}
top() { xprop -root _POMELO_WL_STACK | sed 's/.*= //; s/,//g' | awk '{print $1, $2, $3, $4}'; }   # 最上面的視窗：框的 x y 寬 高
move() { # 把最上面的視窗用標題列拖 dx dy
	read -r x y w h <<<"$(top)"
	"$VPTR" abs $((x + w / 2 - 60)) $((y + 13)) down wait 100 rel "$1" "$2" wait 100 up wait 300
}
reset() { : > "$DND_LOG"; }
drag() { # 起點 X Y、途經、終點 X Y、放開前等多久（毫秒）
	"$VPTR" abs "$1" "$2" down wait 100 rel 20 0 wait 100 abs $(( ($1 + $3) / 2 )) $(( ($2 + $4) / 2 )) wait 100 abs "$3" "$4" wait "${5:-300}" up wait 1500
}
cleanup() { for p in "${pids[@]}"; do kill "$p" 2>/dev/null; done; rm -rf "$D" "$DND_LOG"; }
trap cleanup EXIT

# 版面：SRC 在左、DST 在右（原生）；X 視窗之後另外放
export DND_FILES="$FILES"
start SRC "$BIN/dnd-test" SRC src;  move -300 100
start DST "$BIN/dnd-test" DST dst;  move 420 100
read -r dx dy dw dh <<<"$(top)"            # DST 的框
read -r sx sy sw sh <<<"$(xprop -root _POMELO_WL_STACK | sed 's/.*= //; s/,//g' | awk '{print $5, $6, $7, $8}')"
SRC_X=$((sx + 60)); SRC_Y=$((sy + 110)); DST_X=$((dx + 100)); DST_Y=$((dy + 110))

echo "== 1 原生 → 原生：檔案清單（空格、中文檔名）"
reset; drag $SRC_X $SRC_Y $DST_X $DST_Y
check "DND_ENTER 看得到 uri-list" "DST: DND_ENTER .*text/uri-list"
check "放下、動作＝複製" "DST: DND_DROP .* action=1"
check "檔案 1（含空格）" "file\[0\]=.*/a b.txt"
check "檔案 2（中文）" "file\[1\]=.*/é.txt"
check "來源收到完成" "SRC: DND_END action=1"

echo "== 2 Esc 取消"
reset
( "$VPTR" abs $SRC_X $SRC_Y down wait 100 rel 20 0 wait 100 abs $DST_X $DST_Y wait 2500 up wait 300 >/dev/null 2>&1 & )
sleep 1.3; wtype -k Shift_L; sleep 0.2; wtype -k Escape; sleep 3
check "目標收到 LEAVE" "DST: DND_LEAVE"
check "來源取消（action=0）" "SRC: DND_END action=0"
nocheck "沒有放下" "DND_DROP"

echo "== 3 拖到沒有目標的地方放開"
reset; drag $SRC_X $SRC_Y 640 60
check "來源取消" "SRC: DND_END action=0"
nocheck "沒有放下" "DND_DROP"

echo "== 4 同一個程式內（左半來源→右半目標），自訂格式＋移動"
DND_MIME=custom DND_ACTIONS=move start BOTH "$BIN/dnd-test" BOTH both
read -r bx by bw bh <<<"$(top)"
reset; drag $((bx + 100)) $((by + 110)) $((bx + 300)) $((by + 110)) 800
check "收到自訂格式的資料" "custom=hello-custom"
check "動作＝移動" "BOTH: DND_DROP .* action=2"
check "來源完成" "BOTH: DND_END action=2"
kill "${pids[-1]}"; unset 'pids[-1]'; sleep 0.5

echo "== 5 原生 → XWayland"
if [ "${PATCHED:-0}" = 1 ]; then
	GDK_BACKEND= start XDST "$BIN/xdnd-test" XDST dst; move 360 120
	read -r xx xy xw xh <<<"$(top)"
	reset; drag $SRC_X $SRC_Y $((xx + xw / 2)) $((xy + xh / 2 + 10))
	check "X 目標收到放下" "XDST: XdndDrop"
	check "X 目標讀到檔案" "uri=file://.*a%20b.txt"
	check "來源完成" "SRC: DND_END action=1"
	kill "${pids[-1]}"; unset 'pids[-1]'; sleep 0.5
else echo "SKIP（沒有 PATCHED=1：未打 patch 的 wlroots 放下後收不到資料）"; fi

echo "== 6 XWayland → 原生"
GDK_BACKEND= start XSRC "$BIN/xdnd-test" XSRC src; move -200 150
read -r xx xy xw xh <<<"$(top)"
reset; drag $((xx + xw / 2)) $((xy + xh / 2 + 10)) $DST_X $DST_Y 600
check "原生目標收到 ENTER" "DST: DND_ENTER .*text/uri-list"
check "X 來源收到接受" "XSRC: XdndStatus accept=1"
check "原生目標放下" "DST: DND_DROP"
check "原生目標讀到檔案" "file\[0\]=.*/a b.txt"
check "X 來源收到完成" "XSRC: XdndFinished ok=1"
echo "== 6b XWayland → 原生，中途拖回桌面放開"
reset
"$VPTR" abs $((xx + xw / 2)) $((xy + xh / 2 + 10)) down wait 100 rel 20 0 wait 100 abs $DST_X $DST_Y wait 300 abs 640 40 wait 300 up wait 1000 >/dev/null 2>&1
check "目標收到 LEAVE" "DST: DND_LEAVE"
nocheck "沒有放下" "DND_DROP"
check "X 來源知道沒放成" "XSRC: released without accepted target"
kill "${pids[-1]}"; unset 'pids[-1]'; sleep 0.5

echo "== 7 XWayland → XWayland（指標焦點留在來源視窗也不影響 X 對 X）"
GDK_BACKEND= start XSRC "$BIN/xdnd-test" XSRC src; move -250 250
read -r ax ay aw ah <<<"$(top)"
GDK_BACKEND= start XDST "$BIN/xdnd-test" XDST dst; move 300 -100
read -r bx by bw bh <<<"$(top)"
reset; drag $((ax + aw / 2)) $((ay + ah / 2 + 10)) $((bx + bw / 2)) $((by + bh / 2 + 10)) 600
check "X 目標收到放下" "XDST: XdndDrop"
check "X 來源收到完成" "XSRC: XdndFinished ok=1"

[ $fail = 0 ] && echo "全部通過" || echo "有失敗"
exit $fail
