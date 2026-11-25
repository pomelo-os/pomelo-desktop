#!/bin/sh
# pomelo-terminal X11 後端（Xvfb :97）：同樣的外觀內容，截圖跟原生版對照
TD=$(cd "$(dirname "$0")" && pwd)
. "$(dirname "$0")/../../libpomelo/tests/wl-apps/lib.sh"
fresh; xstart; H=$P/home; mkdir -p $H
export SHELL=/bin/bash TERM=xterm-256color
printf 'PS1="$ "\n' > $H/.bashrc
cd $H
$POMELO_ROOT/pomelo-terminal/pomelo-terminal -e bash --noprofile --rcfile $H/.bashrc > $P/termx.log 2>&1 &
FP=$!
sleep 2; xfocus pomelo-terminal
xdotool type --delay 30 "sh $TD/term-colors.sh"; xdotool key Return; sleep 1
xsnap term-x11-colors
kill $FP; sleep 0.3; cat $P/termx.log; xstop
