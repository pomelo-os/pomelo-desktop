#!/bin/sh
. "$(dirname "$0")/../../libpomelo/tests/wl-apps/lib.sh"
start; fresh; H=$P/home; cd $H
env -u DISPLAY $POMELO_ROOT/pomelo-writer/pomelo-writer > $P/writer.log 2>&1 &
WP=$!
sleep 2.5; snap writer-wl-open
click 640 300
key 'Hello Writer 你好'; keyk Return; key 'second paragraph'
ctrl a; ctrl b; snap writer-wl-typed
ctrl c; echo "clip: $($P/bin/clipget)"
keyk End; ctrl v; sleep 0.4
click 100 70 0; snap writer-wl-menu; keyk Escape
ctrl s; sleep 1; snap writer-wl-savedlg
key 'rt.docx'; keyk Return; sleep 1.5; snap writer-wl-saved
ls -la $H/rt.docx
unzip -p $H/rt.docx word/document.xml | head -c 600; echo
ctrl q; sleep 1
if kill -0 $WP 2>/dev/null; then echo "STILL (maybe confirm)"; snap writer-wl-quit; fi
kill $WP 2>/dev/null
# 重開讀回
env -u DISPLAY $POMELO_ROOT/pomelo-writer/pomelo-writer $H/rt.docx > $P/writer2.log 2>&1 &
WP=$!
sleep 2.5; snap writer-wl-reopen
cat $P/writer.log $P/writer2.log
kill $WP; stop
