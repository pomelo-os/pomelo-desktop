# 終端機測試共用：term_start（開終端機）、tcell 欄 列 → 螢幕座標
. "$(dirname "$0")/../../libpomelo/tests/wl-apps/lib.sh"
term_start() {
  start; fresh
  H=$P/home; mkdir -p $H
  export SHELL=/bin/bash TERM=xterm-256color
  printf 'PS1="$ "\n' > $H/.bashrc
  cd $H
  env -u DISPLAY $POMELO_ROOT/pomelo-terminal/pomelo-terminal "$@" -e bash --noprofile --rcfile $H/.bashrc > $P/term.log 2>&1 &
  TP=$!; FP=$TP
  sleep 2
  read -r TX TY TW TH <<EOT
$(topwin)
EOT
}
# 欄 列（0 起算）的中心，螢幕座標：視窗框 (TX,TY) ＋ 標題列 28 ＋ 分頁列 30 ＋ 邊距 10
tcx() { echo $((TX + 10 + $1 * 9 + 4)); }
tcy() { echo $((TY + 28 + 30 + 10 + $1 * 20 + 10)); }
tclick() { click $(tcx $1) $(tcy $2) ${3:-0}; }
