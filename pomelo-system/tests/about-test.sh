. "$(dirname "$0")/../../libpomelo/tests/wl-apps/lib.sh"
start; fresh
env -u DISPLAY $POMELO_ROOT/pomelo-system/pomelo-system --about > $P/about.log 2>&1 &
AP=$!
sleep 2; snap about-wl-open
env -u DISPLAY $POMELO_ROOT/pomelo-system/pomelo-system --about >/dev/null 2>&1; echo "second rc=$? (應立即結束)"
sleep 0.5; echo "procs: $(pgrep -u $(id -u) -f '^$POMELO_ROOT/pomelo-system/pomelo-system --about' | wc -l)"
click 640 400 0 ; sleep 0.3
keyk Escape; sleep 0.8
if kill -0 $AP 2>/dev/null; then echo STILL; else echo closed; fi
cat $P/about.log; stop
