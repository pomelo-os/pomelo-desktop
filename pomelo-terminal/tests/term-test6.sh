#!/bin/sh
# pomelo-terminal 原生測試：很多個分頁（pmwl_next_fds 的上限）、背景分頁的輸出、關閉分頁
TD=$(cd "$(dirname "$0")" && pwd)
. "$(dirname "$0")/term-lib.sh"
term_start
for i in $(seq 1 20); do wtype -k Shift_L -d 20 -M ctrl -M shift -k t -m shift -m ctrl; sleep 0.25; done
sleep 0.5
key 'echo tab21; sleep 1; echo BG-DONE'; keyk Return
wtype -k Shift_L -d 20 -M ctrl -M shift -k t -m shift -m ctrl; sleep 0.5
key 'echo last-tab'; keyk Return; sleep 2
snap term-wl-20tabs
echo "bash 行程數（每個分頁一個）： $(pgrep -u $(id -u) -P $TP | wc -l)"
kill $TP; sleep 0.3; stop
