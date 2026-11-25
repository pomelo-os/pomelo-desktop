. "$(dirname "$0")/../../libpomelo/tests/wl-apps/lib.sh"
start; fresh; H=$P/home; mkdir -p $H/docs; printf 'hello\n' > $H/docs/a.txt; cd $H
printf 'input.engines=chewing\ninput.engine=chewing\n' > $P/cfg/pomelo/settings.conf
export POMELO_INPUT_DEBUG=1
LD_LIBRARY_PATH=$POMELO_ROOT/libpomelo ${POMELO_INPUT:-$POMELO_ROOT/pomelo-input/pomelo-input} > $P/im.log 2>&1 &
sleep 1.5
env -u DISPLAY $POMELO_ROOT/pomelo-edit/pomelo-edit $H/docs/a.txt > $P/edit.log 2>&1 &
sleep 2; click 640 300; keyk End
ctrl space; sleep 0.5; snap ime1
key 'su3'; sleep 0.6; snap ime2
keyk space; sleep 0.4; snap ime3
key 'cl3'; sleep 0.5; snap ime4
keyk Return; sleep 0.5; snap ime5
grep -a -i "preedit\|commit\|engine" $P/im.log | head -5
stop
