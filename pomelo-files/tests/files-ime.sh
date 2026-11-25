. "$(dirname "$0")/../../libpomelo/tests/wl-apps/lib.sh"
start; fresh; H=$P/home; sh "$(dirname "$0")/files-setup.sh" $H
printf 'input.engines=chewing\ninput.engine=chewing\n' > $P/cfg/pomelo/settings.conf
export POMELO_INPUT_DEBUG=1 XDG_DATA_HOME=$H/.local/share
LD_LIBRARY_PATH=$POMELO_ROOT/libpomelo ${POMELO_INPUT:-$POMELO_ROOT/pomelo-input/pomelo-input} > $P/im.log 2>&1 &
sleep 1.5
env -u DISPLAY $POMELO_ROOT/pomelo-files/pomelo-files $H > $P/files.log 2>&1 &
FP=$!; sleep 2
click 1005 160; keyk F2; sleep 1.5
ctrl space; sleep 0.5
key 'su3'; sleep 0.6; snap files-ime-preedit
keyk space; sleep 0.4; snap files-ime-cand
key 'cl3'; sleep 0.5; keyk Return; sleep 0.5; snap files-ime-commit
ctrl space; sleep 0.3; keyk Return; sleep 0.8; snap files-ime-renamed
ls $H
grep -a -i "preedit\|commit" $P/im.log | head -3
kill $FP; stop
