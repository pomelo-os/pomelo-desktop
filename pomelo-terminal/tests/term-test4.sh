#!/bin/sh
# pomelo-terminal 原生測試（中文輸入法預編輯、拖放檔案、Ctrl+C）
. "$(dirname "$0")/term-lib.sh"
start; fresh; H=$P/home; mkdir -p $H
printf 'input.engines=chewing\ninput.engine=chewing\n' > $P/cfg/pomelo/settings.conf
export POMELO_INPUT_DEBUG=1
LD_LIBRARY_PATH=$POMELO_ROOT/libpomelo ${POMELO_INPUT:-$POMELO_ROOT/pomelo-input/pomelo-input} > $P/im.log 2>&1 &
sleep 1.5
export SHELL=/bin/bash TERM=xterm-256color; printf 'PS1="$ "\n' > $H/.bashrc; cd $H
env -u DISPLAY $POMELO_ROOT/pomelo-terminal/pomelo-terminal -e bash --noprofile --rcfile $H/.bashrc > $P/term.log 2>&1 &
TP=$!; FP=$TP; sleep 2
key 'echo '; sleep 0.5
ctrl space; sleep 0.8
key 'su3'; sleep 0.7; snap term-ime-preedit
keyk space; sleep 0.5
key 'cl3'; sleep 0.5; snap term-ime-cand
keyk Return; sleep 0.5; snap term-ime-commit
ctrl space; sleep 0.3
keyk Return; sleep 0.6
snap term-ime-done
echo "== Ctrl+C"
key 'sleep 100'; keyk Return; sleep 0.5
wtype -k Shift_L -d 30 -M ctrl -k c -m ctrl; sleep 0.5; key 'echo after-ctrlc'; keyk Return; sleep 0.4
echo "== 拖放檔案（dnd-test 來源 → 終端機）"
mkdir -p "$H/my files"; echo x > "$H/my files/報告 1.txt"; echo y > $H/plain.txt
DND_FILES="$H/my files/報告 1.txt:$H/plain.txt" env -u DISPLAY $P/bin/dnd-test SRC src > $P/dndsrc.log 2>&1 &
DP=$!
sleep 1.5; movewin 420 200
read -r sx sy sw sh <<EOT2
$(topwin)
EOT2
dragto $((sx + 60)) $((sy + 110)) 400 300
snap term-dnd
kill $DP
kill $TP; sleep 0.3; cat $P/term.log; grep -a -i "commit" $P/im.log | head -3; stop
