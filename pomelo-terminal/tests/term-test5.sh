#!/bin/sh
# pomelo-terminal 原生測試（調整視窗大小、行列數跟著變、最小大小）
. "$(dirname "$0")/term-lib.sh"
term_start
key 'stty size'; keyk Return; sleep 0.4
echo "frame $TX $TY $TW $TH"
for off in 0 3 6; do
vp abs $((TX + TW + off)) $((TY + TH + off)) wait 150 down wait 150 rel -200 -150 wait 150 rel -100 -60 wait 150 up wait 800
read -r a b c d <<EOT
$(topwin)
EOT
echo "off=$off -> $a $b $c $d"
done
key 'clear; stty size'; keyk Return; sleep 0.5; snap term-wl-resized
vp abs $((a + c + 3)) $((b + d + 3)) wait 150 down wait 150 rel -900 -900 wait 300 up wait 800
read -r a b c d <<EOT
$(topwin)
EOT
echo "到最小： $a $b $c $d"; key 'clear; stty size'; keyk Return; sleep 0.5; snap term-wl-minsize
kill $TP; stop
