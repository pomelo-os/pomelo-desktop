#!/bin/sh
# pomelo-terminal 原生 Wayland 測試（外觀）：sh term-test.sh
TD=$(cd "$(dirname "$0")" && pwd)
. "$(dirname "$0")/../../libpomelo/tests/wl-apps/lib.sh"
start; fresh
H=$P/home; mkdir -p $H
export SHELL=/bin/bash PS1='$ ' TERM=xterm-256color
printf 'PS1="$ "\n' > $H/.bashrc
cd $H
FP=0
env -u DISPLAY $POMELO_ROOT/pomelo-terminal/pomelo-terminal -e bash --noprofile --rcfile $H/.bashrc > $P/term.log 2>&1 &
FP=$!
sleep 2
snap term-wl-open
key "sh $TD/term-colors.sh"; keyk Return; sleep 1
snap term-wl-colors
kill $FP; sleep 0.3; cat $P/term.log
stop
