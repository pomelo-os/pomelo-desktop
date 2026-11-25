#!/bin/bash
# 小柚瀏覽器、Firefox ESR、Chromium 在同一個環境比較：啟動時間與記憶體（PSS）
# 在容器裡以 root 執行（dbus-run-session 底下），Xvfb 1280×800、沒有顯示卡；三個瀏覽器的沙箱都關掉（容器裡開不了）
# 用法見同一個資料夾的 README.md（專案根目錄掛在容器的 /src，先編好 libpomelo 與 pomelo-browser）
export LD_LIBRARY_PATH=/src/libpomelo POMELO_I18N_DIR=/src/libpomelo/i18n LANGUAGE=zh_TW
export WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1 POMELO_BROWSER_NO_GPU=1 MOZ_DISABLE_CONTENT_SANDBOX=1
Xvfb :5 -screen 0 1280x800x24 >/dev/null 2>&1 &
sleep 1; export DISPLAY=:5
mkdir -p /tmp/www
printf '<!doctype html><meta charset="utf-8"><title>PBREADY</title><h1>測試頁</h1><p>hello</p>' > /tmp/www/ready.html
LOCAL=file:///tmp/www/ready.html
WIKI='https://zh.wikipedia.org/wiki/%E8%87%BA%E7%81%A3'
WIKI2='https://en.wikipedia.org/wiki/Taiwan'
EX='https://example.com/'
mkdir -p /tmp/h-pomelo /tmp/h-chromium /tmp/h-firefox/prof
cat > /tmp/h-firefox/prof/user.js <<'P'
user_pref("browser.aboutwelcome.enabled", false);
user_pref("browser.startup.homepage_override.mstone", "ignore");
user_pref("startup.homepage_welcome_url", "");
user_pref("startup.homepage_welcome_url.additional", "");
user_pref("browser.shell.checkDefaultBrowser", false);
user_pref("datareporting.policy.dataSubmissionEnabled", false);
user_pref("toolkit.telemetry.reportingpolicy.firstRun", false);
user_pref("browser.sessionstore.resume_from_crash", false);
user_pref("trailhead.firstrun.didSeeAboutWelcome", true);
user_pref("toolkit.startup.max_resumed_crashes", -1);
P
launch() {   # 背景執行瀏覽器（setsid：自成一個 session，之後用 session 找它所有的行程）
	local b=$1; shift
	case $b in
	pomelo)   HOME=/tmp/h-pomelo setsid /src/pomelo-browser/pomelo-browser "$@" >/dev/null 2>&1 & ;;
	firefox)  HOME=/tmp/h-firefox setsid firefox-esr --no-remote -profile /tmp/h-firefox/prof "$@" >/dev/null 2>&1 & ;;
	chromium) HOME=/tmp/h-chromium setsid chromium --no-sandbox --no-first-run --no-default-browser-check \
	              --password-store=basic --user-data-dir=/tmp/h-chromium/data "$@" >/dev/null 2>&1 & ;;
	esac
}
findsid() {   # 瀏覽器主行程的 session（＝它自己的 PID）
	SID=$(ps -e -o pid=,sid=,comm= | awk '$1==$2 && ($3 ~ /^pomelo-browser|^firefox|^chromium/) {print $1}' | head -1)
}
stop() {   # 這個測試容器裡的瀏覽器全部關掉，確定視窗都消失
	[ -n "$SID" ] && kill -TERM -- -$SID 2>/dev/null; sleep 2
	pkill -KILL -f 'pomelo-browser|firefox|chromium|WebKit' 2>/dev/null; sleep 1
	rm -f /tmp/h-firefox/prof/lock /tmp/h-firefox/prof/.parentlock /tmp/h-chromium/data/Singleton*
	while ready pomelo-browser || ready firefox || ready chromium; do sleep 0.2; done
}
pss() {   # 同一個 session 所有行程的 PSS 總和（MB）與行程數
	local t=0 n=0
	for p in $(ps -e -o pid=,sid= | awk -v s=$SID '$2==s {print $1}'); do
		v=$(awk '/^Pss:/{print $2}' /proc/$p/smaps_rollup 2>/dev/null); [ -n "$v" ] || continue
		t=$((t + v)); n=$((n + 1))
	done
	echo "pss=$((t / 1024)) procs=$n"
}
ready() {   # 這個瀏覽器的視窗標題（_NET_WM_NAME）出現測試頁的標題了沒（Firefox 的 WM_NAME 放不下「—」，只能看 _NET_WM_NAME）
	local w
	for w in $(xdotool search --class "$1" 2>/dev/null); do
		xdotool getwindowname $w 2>/dev/null | grep -q PBREADY && return 0
	done
	return 1
}
ms() { echo $(( ($(date +%s%N) - $1) / 1000000 )); }
cls() { case $1 in pomelo) echo pomelo-browser ;; firefox) echo firefox ;; chromium) echo chromium ;; esac; }
for b in ${BROWSERS:-pomelo firefox chromium}; do
	launch $b $LOCAL; for i in $(seq 1 600); do ready "$(cls $b)" && break; sleep 0.05; done
	sleep 4; findsid; stop   # 第一次：建立設定檔，不算
	for r in 1 2 3; do
		s=$(date +%s%N); launch $b $LOCAL
		tw=""; tr=""
		for i in $(seq 1 3000); do
			[ -z "$tw" ] && xdotool search --onlyvisible --class "$(cls $b)" >/dev/null 2>&1 && tw=$(ms $s)
			ready "$(cls $b)" && { tr=$(ms $s); break; }
			sleep 0.01
		done
		sleep 5; findsid; echo "RESULT $b start $r window=$tw ready=$tr $(pss)"; stop
	done
	for r in 1 2 3; do launch $b "$WIKI"; sleep 15; findsid; echo "RESULT $b wiki $r $(pss)"; stop; done
	for r in 1 2 3; do
		launch $b "$WIKI" "$WIKI2" "$EX"; sleep 20; findsid; echo "RESULT $b tabs3 $r $(pss)"
		stop
	done
done
