#!/bin/sh
# pomelo-terminal 原生測試（鍵盤、選取、剪貼簿、滑鼠回報、捲動、標題）
. "$(dirname "$0")/term-lib.sh"
term_start
echo "frame: $TX $TY $TW $TH"
echo "== 鍵盤：cat -v 看送出的序列"
key 'cat -v'; keyk Return; sleep 0.4
keyk Up Down Left Right Home End Delete Prior Next F1 F2 F5 F12; sleep 0.2
wtype -k Shift_L -d 30 -M alt -k b -m alt; wtype -d 30 -M ctrl -k a -m ctrl; sleep 0.3
wtype -k Shift_L -d 30 -M shift -k Up -m shift
key '你好 é'; keyk Return; sleep 0.3
snap term-wl-keys
wtype -k Shift_L -d 30 -M ctrl -k d -m ctrl; sleep 0.4
echo "== 選取與剪貼簿"
key 'echo hello world foo/bar'; keyk Return; sleep 0.4
vp abs $(tcx 7) $(tcy 7) wait 100 click 0 wait 100 click 0; sleep 0.4    # 雙擊 hello（上一行輸出在第？列）
snap term-wl-select
echo "primary: $($P/bin/clipget --primary)"
wtype -k Shift_L -d 40 -M ctrl -M shift -k c -m shift -m ctrl; sleep 0.4
echo "clipboard: $($P/bin/clipget)"
vp abs 300 600 click 0                                                # 點空白（不在終端機上）不管
tclick 30 12; wtype -k Shift_L -d 40 -M ctrl -M shift -k v -m shift -m ctrl; sleep 0.4
snap term-wl-pasted
click $(tcx 2) $(tcy 12) 2; sleep 0.5; snap term-wl-midpaste        # 中鍵貼上 primary
kill $TP; sleep 0.3; cat $P/term.log; stop
