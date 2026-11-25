#!/bin/sh
# pomelo-terminal X11 後端回歸（Xvfb :97，沒有視窗管理員）：鍵盤、選取貼上、滑鼠回報、捲動、分頁、右鍵、vim
. "$(dirname "$0")/../../libpomelo/tests/wl-apps/lib.sh"
fresh; xstart; H=$P/home; mkdir -p $H
export SHELL=/bin/bash TERM=xterm-256color
printf 'PS1="$ "\n' > $H/.bashrc; cd $H
printf 'int main(void)\n{\n\treturn 0; /* 中文註解 */\n}\n' > $H/a.c
$POMELO_ROOT/pomelo-terminal/pomelo-terminal -e bash --noprofile --rcfile $H/.bashrc > $P/termx.log 2>&1 &
FP=$!; sleep 2; xfocus pomelo-terminal
xdotool type --delay 40 'cat -v'; xdotool key Return; sleep 0.3
xdotool key Up Down Left Right Home End Delete Prior Next F1 F5; xdotool key alt+b; xdotool key ctrl+a; xdotool key shift+Up
xdotool type --delay 40 'é'; xdotool key Return; sleep 0.3; xdotool key ctrl+d; sleep 0.3
xdotool type --delay 30 'echo hello world'; xdotool key Return; sleep 0.4
xdotool mousemove 95 109 click --repeat 2 --delay 100 1; sleep 0.4
xsnap term-x11-sel
xdotool key ctrl+shift+c; sleep 0.3; xdotool mousemove 300 300 click 2; sleep 0.4
xdotool type --delay 40 'stty size'; xdotool key Return; sleep 0.4
xsnap term-x11-keys
xdotool type --delay 30 "printf '\\033[?1000h\\033[?1006h'; cat -v"; xdotool key Return; sleep 0.3
xdotool mousemove 100 200 click 1; xdotool mousemove 120 220 click 3; xdotool click 4; sleep 0.4
xsnap term-x11-mouse
xdotool key ctrl+c; xdotool type --delay 30 "printf '\\033[?1000l\\033[?1006l'; clear; seq 1 120"; xdotool key Return; sleep 0.6
xdotool mousemove 300 300 click 4 click 4 click 4; sleep 0.4
xsnap term-x11-scroll
xdotool key shift+End; xdotool type --delay 30 'clear; vim -u NONE -c "syntax on" a.c'; xdotool key Return; sleep 1; xsnap term-x11-vim
xdotool type ':q'; xdotool key Return; sleep 0.4
xdotool mousemove 800 14 click 1; sleep 0.8; xdotool type 'echo tab2'; xdotool key Return; sleep 0.4
xdotool mousemove 300 300 click 3; sleep 0.5; xsnap term-x11-menu; xdotool key Escape
kill $FP; cat $P/termx.log; xstop
