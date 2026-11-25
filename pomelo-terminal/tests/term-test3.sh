#!/bin/sh
# pomelo-terminal 原生測試（滑鼠回報、捲動、全螢幕程式、標題、分頁、右鍵、縮放字型）
. "$(dirname "$0")/term-lib.sh"
term_start
echo "== 滑鼠回報（1000＋SGR 1006）"
key "printf '\\033[?1000h\\033[?1006h'; cat -v"; keyk Return; sleep 0.4
tclick 10 6; tclick 20 8 1; vp abs $(tcx 5) $(tcy 10) wait 80 scroll 2 wait 80 scroll -1; sleep 0.4
snap term-wl-mouse
wtype -k Shift_L -d 30 -M ctrl -k c -m ctrl; sleep 0.3
key "printf '\\033[?1000l\\033[?1006l'"; keyk Return; sleep 0.3
echo "== 捲動：seq 200，滾輪與觸控板"
key 'clear; seq 1 200'; keyk Return; sleep 0.8
vp abs $(tcx 20) $(tcy 8) wait 100 scroll -5 wait 100 scroll -5; sleep 0.5; snap term-wl-scroll1
vp abs $(tcx 20) $(tcy 8) wait 50 smooth -150; sleep 0.5; snap term-wl-scroll2
wtype -k Shift_L -d 30 -M shift -k Prior -m shift; sleep 0.4; snap term-wl-scroll3
wtype -k Shift_L -d 30 -M shift -k End -m shift; sleep 0.3
echo "== less（替代畫面，滾輪＝方向鍵）"
key 'seq 1 300 | less'; keyk Return; sleep 0.7
vp abs $(tcx 20) $(tcy 8) wait 100 scroll 3; sleep 0.5; snap term-wl-less
key 'q'; sleep 0.4
echo "== vim"
printf 'int main(void)\n{\n\treturn 0; /* 中文註解 */\n}\n' > $H/a.c
key 'vim -u NONE -c "syntax on" a.c'; keyk Return; sleep 1; snap term-wl-vim
key ':q'; keyk Return; sleep 0.5
echo "== OSC 標題"
key "printf '\\033]0;我的標題 title\\007'"; keyk Return; sleep 0.6; snap term-wl-title
echo "== 寬字元／游標形狀"
key "printf '\\033[6 q游標＿\\n'"; keyk Return; sleep 0.4
echo "== 分頁：＋、輸入、切換、右鍵選單"
click $((TX + TW - 18)) $((TY + 28 + 15)); sleep 1
key 'echo tab two'; keyk Return; sleep 0.4; snap term-wl-tab2
click $((TX + 60)) $((TY + 28 + 15)); sleep 0.5; snap term-wl-tab1
tclick 20 5 1; snap term-wl-menu; keyk Escape; sleep 0.3
echo "== 字型縮放（Ctrl+Shift++ ）"
wtype -k Shift_L -d 40 -M ctrl -M shift -k plus -m shift -m ctrl; sleep 0.6; snap term-wl-zoom
kill $TP; sleep 0.3; cat $P/term.log; stop
