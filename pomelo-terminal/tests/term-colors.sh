#!/bin/sh
# 終端機外觀測試內容：16 色、256 色、真彩色、粗體／斜體／底線／反白／刪除線、方框字元、寬字元、emoji
printf '\033[1mBold\033[0m \033[3mItalic\033[0m \033[4mUnder\033[0m \033[7mRev\033[0m \033[9mStrike\033[0m \033[2mDim\033[0m \033[1;3;4mAll\033[0m\n'
for i in 0 1 2 3 4 5 6 7; do printf '\033[4%dm  \033[0m' $i; done; printf ' '
for i in 0 1 2 3 4 5 6 7; do printf '\033[10%dm  \033[0m' $i; done; echo
for i in 0 1 2 3 4 5 6 7; do printf '\033[3%dmfg%d\033[0m ' $i $i; done; echo
i=16; while [ $i -lt 232 ]; do printf '\033[48;5;%dm \033[0m' $i; i=$((i+1)); done; echo
i=232; while [ $i -lt 256 ]; do printf '\033[48;5;%dm  \033[0m' $i; i=$((i+1)); done; echo
i=0; while [ $i -lt 72 ]; do r=$((i*255/71)); g=$((255-i*255/71)); printf '\033[48;2;%d;%d;128m \033[0m' $r $g; i=$((i+1)); done; echo
printf '\033[38;2;255;128;0mTrue color orange\033[0m \033[38;5;99mIndex 99\033[0m\n'
echo '┌──────┬──────┐ ╔══╗ ▀▄█▌▐░▒▓'
echo '│ box  │ 方框 │ ╚══╝ ⣿⠿'
echo '└──────┴──────┘'
echo '中文寬字元 CJK 測試：你好，世界！ ABC abc 123'
echo '日本語テスト 한국어 😀🎉 ❤ ✓ ★ é à ü'
echo 'Mixed: aあb中c😀d'
