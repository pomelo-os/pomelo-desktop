/* SPDX-License-Identifier: GPL-3.0-or-later */
/* pomelo-dm — Pomelo 登入管理員
 *
 * 由 systemd 以 root 啟動（pomelo-dm.service），負責：
 *   1. 啟動 pomelo-compositor（取代 Xorg；VT/DRM 權限由它自己透過 libseat 取得）
 *   2. 以沒有權限的系統帳號 pomelo-greeter 執行登入畫面（pomelo-greeter）
 *   3. 收到帳號密碼後，在「工作階段子行程」裡用 PAM 驗證、向 systemd-logind 登記，
 *      再切換成該使用者執行 /usr/share/xsessions 裡選的工作階段（預設 pomelo）
 *   4. 登出後把 compositor 的 Wayland／X11 socket 換回登入畫面的帳號，不重開 compositor
 *      （只有單一、長命的 compositor／內嵌 XWayland 貫穿登入畫面與工作階段，見 docs 的分期計畫）
 *
 * 跟 LightDM 比：不需要 GLib、D-Bus、AccountsService，常駐只有這一個小程式（約 1 MB 記憶體）；
 * 登入畫面在登入後就結束，不佔資源。
 *
 * 與登入畫面之間用 socketpair（登入畫面的 fd 3）傳訊息，每則訊息是一個 SEQPACKET，欄位用 \0 分隔：
 *   登入畫面 → 這裡：LOGIN 使用者 密碼 工作階段   POWER poweroff|reboot|suspend
 *   這裡 → 登入畫面：OK                              FAIL 訊息
 *
 * 跟 pomelo-compositor 之間也用一個 socket（ipc_socket_path）傳訊息，細節見 pomelo-compositor/ipc.c
 * 開頭的協定說明：INFO 查 Wayland／X11 socket 資訊，SETUID 在登入前後切換誰能連線。
 *
 * 測試（不需要 root）：
 *   pomelo-dm --test --display :9 --compositor "pomelo-compositor --nested" --pam-dir 目錄 --state 目錄
 *   以目前的使用者執行，不切換身分、不登記 logind、電源按鈕只印出訊息。
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <linux/vt.h>
#include <sys/ioctl.h>
#include <sys/random.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <security/pam_appl.h>

#ifndef VERSION
#define VERSION "?"
#endif
#define LEN(a) (sizeof(a) / sizeof(a)[0])
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define N_(s) s   /* 送給登入畫面的訊息：登入畫面會翻譯（libpomelo/i18n） */

/* ------------------------------------------------------------------ */
/* 設定                                                                */
/* ------------------------------------------------------------------ */

static struct {
	char display[16], vt[8], compositor[512], greeter[256], greeter_user[64];
	char autologin_user[64], autologin_session[64], default_session[64];
	char state_dir[256], run_dir[256], pam_dir[256], conf_path[256], sessions_dir[256];
	int test;
} cfg = {
	.display = ":0", .vt = "7", .compositor = "", .greeter = "pomelo-greeter", .greeter_user = "pomelo-greeter",
	.default_session = "pomelo", .state_dir = "/var/lib/pomelo-dm", .run_dir = "/run/pomelo-dm",
	.conf_path = "/etc/pomelo/dm.conf", .sessions_dir = "/usr/share/xsessions",
};

static int debug;   /* POMELO_DM_DEBUG=1：多印一些流程訊息（不會印密碼） */
static void logmsg(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fputs("pomelo-dm: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

static void trim(char *s)
{
	char *e = s + strlen(s);
	while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
	char *b = s;
	while (isspace((unsigned char)*b)) b++;
	if (b != s) memmove(s, b, strlen(b) + 1);
}

/* /etc/pomelo/dm.conf：key=value，# 開頭是註解 */
static void load_conf(void)
{
	FILE *f = fopen(cfg.conf_path, "r");
	if (!f) return;
	char line[640];
	while (fgets(line, sizeof line, f)) {
		trim(line);
		char *eq = strchr(line, '=');
		if (!line[0] || line[0] == '#' || !eq) continue;
		*eq = 0;
		char *k = line, *v = eq + 1;
		trim(k); trim(v);
		struct { const char *k; char *dst; size_t n; } map[] = {
			{ "autologin-user", cfg.autologin_user, sizeof cfg.autologin_user },
			{ "autologin-session", cfg.autologin_session, sizeof cfg.autologin_session },
			{ "default-session", cfg.default_session, sizeof cfg.default_session },
			{ "vt", cfg.vt, sizeof cfg.vt },
			{ "compositor", cfg.compositor, sizeof cfg.compositor },
		};
		for (unsigned i = 0; i < LEN(map); i++)
			if (!strcmp(k, map[i].k)) snprintf(map[i].dst, map[i].n, "%s", v);
	}
	fclose(f);
}

/* 系統語言（Debian 系的 /etc/default/locale，或 Fedora／RHEL 系的 /etc/locale.conf 裡的 LANG），登入畫面用 */
static void system_lang(char *out, size_t n)
{
	snprintf(out, n, "C.UTF-8");
	FILE *f = fopen("/etc/default/locale", "r");
	if (!f) f = fopen("/etc/locale.conf", "r");
	if (!f) { const char *l = getenv("LANG"); if (l && *l) snprintf(out, n, "%s", l); return; }
	char line[256];
	while (fgets(line, sizeof line, f)) {
		trim(line);
		if (!strncmp(line, "LANG=", 5)) {
			char *v = line + 5;
			if (*v == '"') { v++; v[strcspn(v, "\"")] = 0; }
			if (*v) snprintf(out, n, "%s", v);
		}
	}
	fclose(f);
}

/* ------------------------------------------------------------------ */
/* 狀態（上次登入的使用者與各自的工作階段）                              */
/* ------------------------------------------------------------------ */

static void state_set(const char *key, const char *val)
{
	char path[300], tmp[310], line[256], out[8192] = "";
	snprintf(path, sizeof path, "%s/state", cfg.state_dir);
	FILE *f = fopen(path, "r");
	size_t k = 0, kl = strlen(key);
	while (f && fgets(line, sizeof line, f))
		if (!(strncmp(line, key, kl) == 0 && line[kl] == '=') && k + strlen(line) < sizeof out - 1) {
			memcpy(out + k, line, strlen(line) + 1);
			k += strlen(line);
		}
	if (f) fclose(f);
	snprintf(out + k, sizeof out - k, "%s=%s\n", key, val);
	snprintf(tmp, sizeof tmp, "%s.new", path);
	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0644);
	if (fd < 0) return;
	ssize_t w = write(fd, out, strlen(out));
	close(fd);
	if (w == (ssize_t)strlen(out)) rename(tmp, path);
	else unlink(tmp);
}

/* ------------------------------------------------------------------ */
/* 工作階段清單（/usr/share/xsessions 裡的 .desktop）                        */
/* ------------------------------------------------------------------ */

static int valid_id(const char *s)
{
	if (!s || !*s || strlen(s) > 60) return 0;
	for (const char *c = s; *c; c++) if (!isalnum((unsigned char)*c) && !strchr("._-", *c)) return 0;
	return s[0] != '.';
}

/* 讀出工作階段的 Exec 與 DesktopNames；找不到回傳 0 */
static int session_info(const char *id, char *exec, size_t en, char *names, size_t nn)
{
	if (!valid_id(id)) return 0;
	char path[400], line[1024];
	snprintf(path, sizeof path, "%s/%s.desktop", cfg.sessions_dir, id);
	FILE *f = fopen(path, "r");
	if (!f) return 0;
	exec[0] = names[0] = 0;
	int in_entry = 0;
	while (fgets(line, sizeof line, f)) {
		trim(line);
		if (line[0] == '[') { in_entry = !strcmp(line, "[Desktop Entry]"); continue; }
		if (!in_entry) continue;
		if (!strncmp(line, "Exec=", 5)) snprintf(exec, en, "%s", line + 5);
		else if (!strncmp(line, "DesktopNames=", 13)) snprintf(names, nn, "%s", line + 13);
	}
	fclose(f);
	for (char *c = names; *c; c++) if (*c == ';') *c = ':';
	size_t l = strlen(names);
	if (l && names[l - 1] == ':') names[l - 1] = 0;
	return exec[0] != 0;
}

/* ------------------------------------------------------------------ */
/* pomelo-compositor                                                   */
/* ------------------------------------------------------------------ */

static pid_t x_pid = -1, greeter_pid = -1, worker_pid = -1;   /* x_pid：compositor 的 pid（沿用舊名字，少一點改動） */
static char ipc_socket_path[300];
static int sigfd;

/* 跟 pomelo-compositor 的控制 socket 對話一次：送 req，把回應整段存進 reply。
 * 協定見 pomelo-compositor/ipc.c 開頭的說明（SOCK_SEQPACKET，欄位用 \0 分隔，第一個欄位是 OK/ERR）。
 * 回傳 0 且 reply 開頭是 "OK" 才算成功。 */
static int ipc_call(const char *req, char *reply, size_t rn)
{
	int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	if (fd < 0) return -1;
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	snprintf(sa.sun_path, sizeof sa.sun_path, "%s", ipc_socket_path);
	if (connect(fd, (struct sockaddr *)&sa, sizeof sa)) { close(fd); return -1; }
	int ok = send(fd, req, strlen(req), MSG_NOSIGNAL) >= 0;
	ssize_t n = ok ? recv(fd, reply, rn - 1, 0) : -1;
	close(fd);
	if (n < 0) return -1;
	reply[n] = 0;
	return strncmp(reply, "OK", 2) == 0 ? 0 : -1;
}

/* 把 compositor 的 Wayland／X11 socket 換成只有 uid 能連線（登入前＝登入畫面的系統帳號，
 * 登入後＝剛登入的使用者）。見 pomelo-compositor/ipc.c：底層是 chown+chmod 0600 那兩個 socket 檔，
 * 不是傳統 X11 的 Xauthority cookie——wlroots 的 rootless XWayland 本來就不用 cookie。 */
static int ipc_setuid(uid_t uid)
{
	char req[64], reply[256];
	snprintf(req, sizeof req, "SETUID %ld", (long)uid);
	if (ipc_call(req, reply, sizeof reply)) {
		logmsg("無法把顯示切給 uid %ld：%s", (long)uid, reply[0] ? reply + 4 : "沒有回應");
		return -1;
	}
	return 0;
}

/* 問 compositor 要連線用的 Wayland socket 名稱與 XWayland 的 :N；成功後 cfg.display 會更新成
 * 實際的 :N（XWayland 自己選的，不是固定的 :0）。 */
static char comp_wayland[64];
static int ipc_info(void)
{
	char reply[256];
	if (ipc_call("INFO", reply, sizeof reply)) return -1;
	char *p = reply + 3;   /* 跳過 "OK\0" */
	snprintf(comp_wayland, sizeof comp_wayland, "%s", p);
	p += strlen(p) + 1;
	if (p[0] != ':') return -1;
	snprintf(cfg.display, sizeof cfg.display, "%s", p);
	return 0;
}

/* 等到某個訊號；期間收到其他訊號照常處理。逾時回傳 0 */
static int handle_signal(const struct signalfd_siginfo *si);
static long now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000;
}

/* 等 compositor 就緒：它在 Wayland socket／第一個 output／XWayland 都準備好之後，
 * 會往繼承來的 fd 寫一個 byte（取代 Xorg 的 SIGUSR1），見 pomelo-compositor 的 --ready-fd。 */
static int wait_compositor_ready(int ready_fd, int timeout_ms)
{
	long end = now_ms() + timeout_ms;
	for (;;) {
		long left = end - now_ms();
		if (left <= 0) return 0;
		struct pollfd p[2] = { { sigfd, POLLIN, 0 }, { ready_fd, POLLIN, 0 } };
		if (poll(p, 2, (int)left) <= 0) continue;
		if (p[1].revents & POLLIN) {
			char byte;
			return read(ready_fd, &byte, 1) == 1;
		}
		if (p[0].revents & POLLIN) {
			struct signalfd_siginfo si;
			if (read(sigfd, &si, sizeof si) != sizeof si) continue;
			handle_signal(&si);
			if (x_pid < 0) return 0;   /* compositor 起不來就結束了 */
		}
	}
}

/* 切到設定的 VT（預設 7）。不切的話開機時 compositor 會留在 tty1，跟 PAM 登記的 XDG_VTNR 不一樣，
 * logind 就一直把工作階段當成在背景（Active=no）：重新開機、關機變成要管理員密碼，
 * 音效、USB 等裝置權限也拿不到。Xorg 時代這件事由它的 vtN 參數處理。
 * 最多等 3 秒，切不過去照樣啟動（頂多跟沒切一樣）。 */
static void switch_vt(void)
{
	int vt = atoi(cfg.vt);
	if (cfg.test || vt <= 0) return;
	int fd = open("/dev/tty0", O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (fd < 0) return;
	struct vt_stat st;
	if (ioctl(fd, VT_GETSTATE, &st) == 0 && st.v_active != vt && ioctl(fd, VT_ACTIVATE, vt) == 0)
		for (int i = 0; i < 60 && ioctl(fd, VT_GETSTATE, &st) == 0 && st.v_active != vt; i++) usleep(50000);
	if (ioctl(fd, VT_GETSTATE, &st) == 0 && st.v_active != vt) logmsg("無法切換到 VT %d（目前在 %d）", vt, st.v_active);
	close(fd);
}

/* 開機畫面（plymouth）的指令：plymouth --ping、deactivate、quit --retain-splash…。沒裝 plymouth 或它沒在跑時
 * 指令會失敗，什麼都不做。最多等 3 秒，plymouth 卡住也不會拖住登入畫面。 */
static int plymouth(const char *a1, const char *a2)
{
	if (cfg.test) return -1;
	pid_t pid = fork();
	if (pid < 0) return -1;
	if (pid == 0) {
		sigset_t all;
		sigemptyset(&all);
		sigprocmask(SIG_SETMASK, &all, NULL);
		int nul = open("/dev/null", O_RDWR);
		if (nul >= 0) { dup2(nul, 0); dup2(nul, 1); dup2(nul, 2); }
		execlp("plymouth", "plymouth", a1, a2, (char *)NULL);
		_exit(127);
	}
	for (int i = 0; i < 60; i++) {
		int st;
		if (waitpid(pid, &st, WNOHANG) == pid) return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
		usleep(50000);
	}
	kill(pid, SIGKILL);
	waitpid(pid, NULL, 0);
	return -1;
}

static int start_compositor(void)
{
	snprintf(ipc_socket_path, sizeof ipc_socket_path, "%s/compositor.sock", cfg.run_dir);

	char cmd[700];
	snprintf(cmd, sizeof cmd, "%s", cfg.compositor[0] ? cfg.compositor : "pomelo-compositor");

	/* 指令用空白分開（設定檔是 root 才能改的，不經過 shell） */
	char *argv[24];
	int argc = 0;
	for (char *t = strtok(cmd, " \t"); t && argc < 16; t = strtok(NULL, " \t")) argv[argc++] = t;
	argv[argc++] = "--ipc-socket";
	argv[argc++] = ipc_socket_path;
	argv[argc++] = "--ready-fd";
	argv[argc++] = "3";
	argv[argc] = NULL;
	/* 開機畫面還在的話，先請它放開螢幕（畫面停在最後一格），compositor 才拿得到；compositor 準備好再叫它
	 * 結束但保留畫面。開機畫面直接接到登入畫面，中間不會閃一下黑畫面或文字（跟 GDM 同樣的做法）。
	 * pomelo-dm.service 因此用 Conflicts= 擋住 plymouth-quit.service，不讓它先把開機畫面關掉。 */
	int splash = plymouth("--ping", NULL) == 0;
	if (splash) plymouth("deactivate", NULL);
	/* VT/DRM 權限由 wlr_backend_autocreate() 透過 libseat 取得；實機上 logind 後端會因為 root
	 * 沒有工作階段而失敗，改用 libseat 內建的 seatd，它綁的是「當下」的 VT，所以要先切過去 */
	switch_vt();

	int rp[2];
	if (pipe2(rp, O_CLOEXEC)) { logmsg("無法建立就緒管線"); return -1; }
	x_pid = fork();
	if (x_pid < 0) { close(rp[0]); close(rp[1]); return -1; }
	if (x_pid == 0) {
		sigset_t all;
		sigemptyset(&all);
		sigprocmask(SIG_SETMASK, &all, NULL);
		setsid();
		close(rp[0]);
		dup2(rp[1], 3);   /* dup2 會清掉 CLOEXEC，--ready-fd 3 才讀得到 */
		if (rp[1] != 3) close(rp[1]);
		int nul = open("/dev/null", O_RDWR);
		if (nul >= 0) { dup2(nul, 0); if (!cfg.test) dup2(nul, 1); }
		setenv("XDG_RUNTIME_DIR", cfg.run_dir, 1);   /* 讓 Wayland socket 開在固定、不管誰（greeter/使用者）都能到的地方 */
		execvp(argv[0], argv);
		_exit(127);
	}
	close(rp[1]);
	int ok = wait_compositor_ready(rp[0], 20000);
	close(rp[0]);
	if (!ok) {
		logmsg("pomelo-compositor 沒有在 20 秒內準備好");
		if (x_pid > 0) kill(x_pid, SIGTERM);
		if (splash) plymouth("quit", NULL);   /* 起不來：關掉開機畫面，至少看得到文字主控台 */
		return -1;
	}
	if (ipc_info()) {
		logmsg("無法向 pomelo-compositor 查詢顯示資訊");
		kill(x_pid, SIGTERM);
		if (splash) plymouth("quit", NULL);
		return -1;
	}
	if (splash) plymouth("quit", "--retain-splash");
	return 0;
}

/* 等 ms 毫秒，期間照常處理訊號（SIGTERM 會讓 quitting 變成 1 並提早結束） */
static volatile int quitting;
static void idle_ms(int ms)
{
	long end = now_ms() + ms;
	while (!quitting) {
		long left = end - now_ms();
		if (left <= 0) return;
		struct pollfd p = { sigfd, POLLIN, 0 };
		if (poll(&p, 1, (int)left) <= 0) continue;
		struct signalfd_siginfo si;
		if (read(sigfd, &si, sizeof si) == sizeof si) handle_signal(&si);
	}
}

/* 馬上強制結束（例如還在驗證密碼、還沒開工作階段的子行程） */
static void kill_now(pid_t *pid)
{
	if (*pid <= 0) return;
	kill(*pid, SIGKILL);
	waitpid(*pid, NULL, 0);
	*pid = -1;
}

static void kill_wait(pid_t *pid, int ms)
{
	if (*pid <= 0) return;
	kill(*pid, SIGTERM);
	for (int i = 0; i < ms / 50; i++) {
		if (waitpid(*pid, NULL, WNOHANG) == *pid) { *pid = -1; return; }
		usleep(50000);
	}
	kill(*pid, SIGKILL);
	waitpid(*pid, NULL, 0);
	*pid = -1;
}

/* Xorg 時代這裡有一段「工作階段期間保持一條空連線」的技巧，因為 X 的預設行為是
 * 「最後一個連線離開就整個重設」，短命小程式（xrdb、setxkbmap…）每個結束都會讓畫面重設一次。
 * pomelo-compositor 沒有這種行為（它自己就是唯一的「顯示伺服器」，不會因為某個 client 斷線
 * 就重設自己），這整套技巧跟著 Xorg 一起不需要了。 */

static void stop_compositor(void)
{
	kill_wait(&x_pid, 5000);
	if (ipc_socket_path[0]) unlink(ipc_socket_path);
}

/* ------------------------------------------------------------------ */
/* 登入畫面                                                            */
/* ------------------------------------------------------------------ */

static int gfd = -1;   /* 跟登入畫面之間的 socket */
static int greeter_safe;   /* 登入畫面連續當掉：叫它不要載入使用者的圖片 */

static void send_greeter(const char *a, const char *b)
{
	if (gfd < 0) return;
	char buf[600];
	size_t la = strlen(a), lb = b ? strlen(b) : 0;
	if (la + lb + 2 > sizeof buf) lb = sizeof buf - la - 2;
	memcpy(buf, a, la + 1);
	if (b) memcpy(buf + la + 1, b, lb), buf[la + 1 + lb] = 0;
	if (send(gfd, buf, la + 1 + (b ? lb + 1 : 0), MSG_NOSIGNAL) < 0) logmsg("送訊息給登入畫面失敗：%s", strerror(errno));
}

static int start_greeter(void)
{
	int sv[2];
	if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv)) return -1;
	struct passwd *gp = cfg.test ? getpwuid(getuid()) : getpwnam(cfg.greeter_user);
	if (!gp) { logmsg("找不到登入畫面的系統帳號 %s", cfg.greeter_user); close(sv[0]); close(sv[1]); return -1; }
	if (ipc_setuid(gp->pw_uid)) { close(sv[0]); close(sv[1]); return -1; }
	char lang[64];
	system_lang(lang, sizeof lang);
	greeter_pid = fork();
	if (greeter_pid < 0) { close(sv[0]); close(sv[1]); return -1; }
	if (greeter_pid == 0) {
		sigset_t all;
		sigemptyset(&all);
		sigprocmask(SIG_SETMASK, &all, NULL);
		setsid();
		dup2(sv[1], 3);   /* dup2 會清掉 CLOEXEC */
		int nul = open("/dev/null", O_RDWR);
		if (nul >= 0) dup2(nul, 0);
		if (!cfg.test) {
			if (initgroups(gp->pw_name, gp->pw_gid) || setgid(gp->pw_gid) || setuid(gp->pw_uid)) _exit(126);
			if (setuid(0) == 0) _exit(126);   /* 確認真的丟掉 root 了 */
		}
		const char *home = cfg.test ? getenv("HOME") : cfg.state_dir;
		char conf_home[320];
		snprintf(conf_home, sizeof conf_home, "%s/greeter", cfg.state_dir);
		const char *keep_path = getenv("PATH");
		char path_env[1024];
		snprintf(path_env, sizeof path_env, "%s", cfg.test && keep_path ? keep_path : "/usr/local/bin:/usr/bin:/bin");
		const char *keep[] = { "POMELO_I18N_DIR", "LD_LIBRARY_PATH", "NO_AT_BRIDGE" };
		char keepv[LEN(keep)][512];
		for (unsigned i = 0; i < LEN(keep); i++) snprintf(keepv[i], sizeof keepv[i], "%s", cfg.test && getenv(keep[i]) ? getenv(keep[i]) : "");
		clearenv();
		setenv("PATH", path_env, 1);
		setenv("HOME", home ? home : "/", 1);
		setenv("XDG_CONFIG_HOME", conf_home, 1);
		setenv("DISPLAY", cfg.display, 1);
		setenv("WAYLAND_DISPLAY", comp_wayland, 1);
		setenv("XDG_RUNTIME_DIR", cfg.run_dir, 1);
		setenv("LANG", lang, 1);
		setenv("POMELO_DM_FD", "3", 1);
		setenv("POMELO_DM_STATE", cfg.state_dir, 1);
		setenv("POMELO_DM_CONF", cfg.conf_path, 1);
		setenv("POMELO_DM_SESSIONS", cfg.sessions_dir, 1);
		setenv("POMELO_DM_DEFAULT_SESSION", cfg.default_session, 1);
		for (unsigned i = 0; i < LEN(keep); i++) if (keepv[i][0]) setenv(keep[i], keepv[i], 1);
		if (cfg.test) setenv("POMELO_DM_TEST", "1", 1);
		if (greeter_safe) setenv("POMELO_GREETER_SAFE", "1", 1);
		execlp(cfg.greeter, cfg.greeter, (char *)NULL);
		_exit(127);
	}
	close(sv[1]);
	gfd = sv[0];
	return 0;
}

/* 結束某個 uid 的所有行程（登入畫面的系統帳號：就算它被攻破、自己 fork 出去也清得掉） */
static void kill_uid(uid_t uid)
{
	for (int round = 0; round < 3; round++) {
		DIR *d = opendir("/proc");
		struct dirent *e;
		int found = 0;
		while (d && (e = readdir(d))) {
			if (!isdigit((unsigned char)e->d_name[0])) continue;
			char p[64], line[256];
			snprintf(p, sizeof p, "/proc/%s/status", e->d_name);
			FILE *f = fopen(p, "r");
			if (!f) continue;
			unsigned ru = 0, eu = 0, su = 0, fu = 0;
			int got = 0;
			while (fgets(line, sizeof line, f))
				if (sscanf(line, "Uid: %u %u %u %u", &ru, &eu, &su, &fu) == 4) { got = 1; break; }
			fclose(f);
			if (got && (ru == uid || eu == uid || su == uid)) { kill((pid_t)atoi(e->d_name), SIGKILL); found = 1; }
		}
		if (d) closedir(d);
		if (!found) break;
		usleep(50000);
	}
}

/* 關掉 socket，給登入畫面 ms 毫秒自己結束（例如播完淡出），之後才強制結束；
 * 它的行程群組（setsid）與系統帳號的其他行程都一併結束，不會有東西殘留到使用者的工作階段 */
static void stop_greeter(int ms)
{
	if (gfd >= 0) { close(gfd); gfd = -1; }
	pid_t pg = greeter_pid;
	for (int i = 0; i < ms / 50 && greeter_pid > 0; i++) {
		if (waitpid(greeter_pid, NULL, WNOHANG) == greeter_pid) { greeter_pid = -1; break; }
		usleep(50000);
	}
	kill_wait(&greeter_pid, 1000);
	if (pg > 0) kill(-pg, SIGKILL);
	if (!cfg.test) {
		struct passwd *gp = getpwnam(cfg.greeter_user);
		if (gp && gp->pw_uid != 0) kill_uid(gp->pw_uid);
	}
}

/* ------------------------------------------------------------------ */
/* 工作階段子行程：PAM 驗證 → logind 登記 → 以使用者身分執行工作階段       */
/* ------------------------------------------------------------------ */

typedef struct { char *password; int answered; char msg[256]; } ConvData;

static int conv(int n, const struct pam_message **msg, struct pam_response **resp, void *data)
{
	ConvData *cd = data;
	*resp = calloc((size_t)n, sizeof **resp);
	if (!*resp) return PAM_BUF_ERR;
	for (int i = 0; i < n; i++) {
		int st = msg[i]->msg_style;
		if (st == PAM_PROMPT_ECHO_OFF || st == PAM_PROMPT_ECHO_ON) {
			/* 只用密碼回答第一個「不顯示輸入」的問題；會顯示輸入的問題（例如問帳號）或第二個問題
			 * （例如一次性密碼）一律拒絕，不要拿密碼亂答 */
			if (st == PAM_PROMPT_ECHO_ON || cd->answered || !cd->password) {
				for (int j = 0; j < i; j++) free((*resp)[j].resp);
				free(*resp);
				*resp = NULL;
				return PAM_CONV_ERR;
			}
			(*resp)[i].resp = strdup(cd->password);
			cd->answered = 1;
		} else if ((st == PAM_ERROR_MSG || st == PAM_TEXT_INFO) && msg[i]->msg) {
			snprintf(cd->msg, sizeof cd->msg, "%s", msg[i]->msg);
		}
	}
	return PAM_SUCCESS;
}

/* 可以從登入畫面登入的帳號：不是 root、登入 shell 是 /etc/shells 裡的正常 shell（不是 nologin、false）。
 * 登入畫面也會過濾，但那邊沒有權限、不能信任，這裡一定要再檢查一次 */
static int allowed_user(const struct passwd *pw)
{
	if (!pw) return 0;
	if (cfg.test) return pw->pw_uid == getuid();
	if (pw->pw_uid == 0) return 0;
	const char *sh = pw->pw_shell && *pw->pw_shell ? pw->pw_shell : "/bin/sh";
	if (strstr(sh, "nologin") || strstr(sh, "/false")) return 0;
	int ok = 0;
	setusershell();
	for (char *x; (x = getusershell()); ) if (!strcmp(x, sh)) { ok = 1; break; }
	endusershell();
	return ok;
}

static int pam_go(pam_handle_t **ph, const char *service, const char *user, struct pam_conv *c)
{
	if (cfg.pam_dir[0]) return pam_start_confdir(service, user, c, cfg.pam_dir, ph);
	return pam_start(service, user, c, ph);
}

/* 從已經切換成使用者身分的子行程讀資料，最多 max 位元組、最久 ms 毫秒；
 * 逾時（例如檔案其實是 FIFO、FUSE 卡住、使用者對子行程送 SIGSTOP）就強制結束它。
 * sink 為 NULL 時存進 buf，否則每讀到一段就交給 sink（回傳 0 代表不要了）。回傳讀到的總量，失敗 -1 */
typedef int (*Sink)(void *ctx, const char *data, size_t n);
static long read_child(pid_t c, int fd, char *buf, size_t max, Sink sink, void *ctx, int ms)
{
	long end = now_ms() + ms;
	size_t total = 0;
	int ok = 1;
	char tmp[65536];
	for (;;) {
		long left = end - now_ms();
		if (left <= 0) { ok = 0; break; }
		struct pollfd p = { fd, POLLIN, 0 };
		int r = poll(&p, 1, (int)left);
		if (r < 0 && errno == EINTR) continue;
		if (r <= 0) { ok = 0; break; }
		char *dst = sink ? tmp : buf + total;
		size_t room = sink ? sizeof tmp : (max > total + 1 ? max - total - 1 : 0);
		if (!room) { ok = 0; break; }
		ssize_t k = read(fd, dst, room);
		if (k < 0 && errno == EINTR) continue;
		if (k <= 0) break;   /* 讀完了 */
		total += (size_t)k;
		if (total > max) { ok = 0; break; }
		if (sink && !sink(ctx, tmp, (size_t)k)) { ok = 0; break; }
	}
	if (!sink && buf && max) buf[MIN(total, max - 1)] = 0;
	close(fd);
	int st = 0;
	if (!ok) kill(c, SIGKILL);
	/* 不要無限等（行程卡在核心裡時 SIGKILL 也可能慢）：最多再等 2 秒 */
	pid_t w = 0;
	for (int i = 0; i < 40 && (w = waitpid(c, &st, WNOHANG)) == 0; i++) usleep(50000);
	if (w != c || !WIFEXITED(st) || WEXITSTATUS(st)) ok = 0;
	return ok ? (long)total : -1;
}

/* 在子行程裡切換成使用者（讀使用者的檔案前一定要先做） */
static int become_user(const struct passwd *pw)
{
	if (cfg.test) return 0;
	if (initgroups(pw->pw_name, pw->pw_gid) || setgid(pw->pw_gid) || setuid(pw->pw_uid)) return -1;
	return setuid(0) == 0 ? -1 : 0;
}

/* 圖片的寬高（PNG 看 IHDR、JPEG 找 SOF 標記）；看不出來回傳 0 */
static int image_size(const unsigned char *b, size_t n, unsigned *w, unsigned *h)
{
	if (n >= 24 && !memcmp(b, "\x89PNG\r\n\x1a\n", 8) && !memcmp(b + 12, "IHDR", 4)) {
		*w = (unsigned)b[16] << 24 | (unsigned)b[17] << 16 | (unsigned)b[18] << 8 | b[19];
		*h = (unsigned)b[20] << 24 | (unsigned)b[21] << 16 | (unsigned)b[22] << 8 | b[23];
		return 1;
	}
	if (n < 4 || b[0] != 0xff || b[1] != 0xd8) return 0;
	size_t i = 2;
	while (i + 9 < n) {
		if (b[i] != 0xff) return 0;
		unsigned char m = b[i + 1];
		if (m == 0xff) { i++; continue; }
		if (m == 0xd8 || m == 0x01 || (m >= 0xd0 && m <= 0xd7)) { i += 2; continue; }
		size_t len = (size_t)b[i + 2] << 8 | b[i + 3];
		if (len < 2) return 0;
		if (m >= 0xc0 && m <= 0xcf && m != 0xc4 && m != 0xc8 && m != 0xcc) {
			*h = (unsigned)b[i + 5] << 8 | b[i + 6];
			*w = (unsigned)b[i + 7] << 8 | b[i + 8];
			return 1;
		}
		i += 2 + len;
	}
	return 0;
}

typedef struct { int out; unsigned char head[262144]; size_t hk; } CopyCtx;
static int copy_sink(void *v, const char *data, size_t n)
{
	CopyCtx *c = v;
	size_t k = MIN(n, sizeof c->head - c->hk);
	memcpy(c->head + c->hk, data, k);
	c->hk += k;
	return write(c->out, data, n) == (ssize_t)n;
}

static gid_t greeter_gid(void)
{
	struct passwd *gp = cfg.test ? NULL : getpwnam(cfg.greeter_user);
	return gp ? gp->pw_gid : (gid_t)-1;
}

/* 以使用者身分讀一個圖片檔（桌布、頭像），複製到 state/users/<名字>.<ext>，讓下次的登入畫面用。
 * 一定要先切換成使用者再讀：使用者可以把桌布路徑設成任何檔案（例如 /etc/shadow）。
 * 只收 PNG／JPEG，檔案大小與圖片尺寸都有上限（尺寸異常的圖可能讓登入畫面當掉，所有人都登不進來）。
 * 存起來的檔案只有 root 與登入畫面讀得到（使用者的家目錄可能是別人不能讀的）。 */
static void copy_user_image(const struct passwd *pw, const char *src, const char *ext, size_t max_bytes, unsigned max_dim)
{
	if (!src || !*src || strlen(src) > 900) return;
	int p[2];
	if (pipe(p)) return;
	pid_t c = fork();
	if (c < 0) { close(p[0]); close(p[1]); return; }
	if (c == 0) {
		close(p[0]);
		if (become_user(pw)) _exit(1);
		int fd = open(src, O_RDONLY | O_NOCTTY | O_NONBLOCK);
		struct stat st;
		if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) || (size_t)st.st_size > max_bytes) _exit(1);
		char buf[65536];
		ssize_t r;
		while ((r = read(fd, buf, sizeof buf)) > 0) if (write(p[1], buf, (size_t)r) != r) _exit(1);
		_exit(r < 0);
	}
	close(p[1]);
	char dir[320], tmp[400], dst[400];
	snprintf(dir, sizeof dir, "%s/users", cfg.state_dir);
	snprintf(dst, sizeof dst, "%s/%s.%s", dir, pw->pw_name, ext);
	snprintf(tmp, sizeof tmp, "%s.new", dst);
	unlink(tmp);
	static CopyCtx cc;
	cc.hk = 0;
	cc.out = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
	if (cc.out < 0) { close(p[0]); kill(c, SIGKILL); waitpid(c, NULL, 0); return; }
	long got = read_child(c, p[0], NULL, max_bytes, copy_sink, &cc, 5000);
	unsigned w = 0, h = 0;
	int ok = got > 0 && image_size(cc.head, cc.hk, &w, &h) && w && h && w <= max_dim && h <= max_dim &&
	         (unsigned long long)w * h <= 40000000ULL;
	gid_t gg = greeter_gid();
	if (ok && gg != (gid_t)-1 && (fchown(cc.out, 0, gg) || fchmod(cc.out, 0640))) ok = 0;
	close(cc.out);
	if (ok) rename(tmp, dst);
	else { unlink(tmp); if (got > 0) logmsg("%s 的%s不是可用的圖片（%ux%u），略過", pw->pw_name, ext, w, h); }
}

/* 以使用者身分讀 ~/.config/pomelo/settings.conf 的桌布設定 */
static void remember_appearance(const struct passwd *pw)
{
	int p[2];
	if (pipe(p)) return;
	pid_t c = fork();
	if (c < 0) { close(p[0]); close(p[1]); return; }
	if (c == 0) {
		close(p[0]);
		if (become_user(pw)) _exit(1);
		char path[600], line[1100];
		snprintf(path, sizeof path, "%s/.config/pomelo/settings.conf", pw->pw_dir);
		int fd = open(path, O_RDONLY | O_NOCTTY | O_NONBLOCK);
		struct stat st;
		if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode)) _exit(0);
		FILE *f = fdopen(fd, "r");
		while (f && fgets(line, sizeof line, f))
			if (!strncmp(line, "desktop.wallpaper=", 18) || !strncmp(line, "appearance.dark=", 16))
				if (write(p[1], line, strlen(line)) < 0) break;
		_exit(0);
	}
	close(p[1]);
	char buf[4096];
	if (read_child(c, p[0], buf, sizeof buf, NULL, NULL, 3000) < 0) return;
	char wall[1024] = "", key[100];
	for (char *l = strtok(buf, "\n"); l; l = strtok(NULL, "\n")) {
		if (!strncmp(l, "desktop.wallpaper=", 18)) snprintf(wall, sizeof wall, "%s", l + 18);
		if (!strncmp(l, "appearance.dark=", 16)) {
			snprintf(key, sizeof key, "dark.%s", pw->pw_name);
			state_set(key, atoi(l + 16) ? "1" : "0");
		}
	}
	char wpath[400];
	snprintf(wpath, sizeof wpath, "%s/users/%s.wallpaper", cfg.state_dir, pw->pw_name);
	if (wall[0] == '/') copy_user_image(pw, wall, "wallpaper", 25 * 1024 * 1024, 16384);
	else unlink(wpath);   /* 內建漸層或沒有設定 */
	char face[600];
	snprintf(face, sizeof face, "%s/.face", pw->pw_dir);
	copy_user_image(pw, face, "face", 2 * 1024 * 1024, 4096);
}

static void ws(int fd, char c, const char *msg)
{
	char buf[300];
	buf[0] = c;
	snprintf(buf + 1, sizeof buf - 1, "%s", msg ? msg : "");
	if (write(fd, buf, strlen(buf)) < 0) _exit(1);
}

/* 子行程本體。res：回報驗證結果（'O' 或 'F'+訊息）；go：讀到一個位元組就開始工作階段 */
static void worker_main(char *user, char *password, const char *session, int autologin, int res, int go)
{
	char exec[1024], names[256];
	if (!session_info(session, exec, sizeof exec, names, sizeof names)) {
		ws(res, 'F', N_("找不到選擇的工作階段"));
		_exit(1);
	}
	ConvData cd = { password, 0, "" };
	struct pam_conv c = { conv, &cd };
	pam_handle_t *ph = NULL;
	int r = pam_go(&ph, autologin ? "pomelo-dm-autologin" : "pomelo-dm", user, &c);
	if (r != PAM_SUCCESS) { ws(res, 'F', N_("PAM 無法啟動")); _exit(1); }
	pam_set_item(ph, PAM_TTY, cfg.display);
	pam_set_item(ph, PAM_XDISPLAY, cfg.display);
	pam_fail_delay(ph, 1500000);   /* 密碼錯誤時至少等 1.5 秒，防止猜密碼 */
	r = pam_authenticate(ph, 0);
	if (r == PAM_SUCCESS) {
		r = pam_acct_mgmt(ph, 0);
		if (r == PAM_NEW_AUTHTOK_REQD) {
			if (password) explicit_bzero(password, strlen(password));
			pam_end(ph, r);
			ws(res, 'F', N_("密碼已過期：請按 Ctrl+Alt+F3 到文字登入畫面，用 passwd 指令更改"));
			_exit(1);
		}
	}
	if (password) explicit_bzero(password, strlen(password));
	if (r != PAM_SUCCESS) {
		const char *m = r == PAM_AUTH_ERR || r == PAM_USER_UNKNOWN || r == PAM_CONV_ERR ? "" :
		                r == PAM_ACCT_EXPIRED ? N_("這個帳號已經過期") : r == PAM_PERM_DENIED ? N_("這個帳號不允許登入") :
		                r == PAM_MAXTRIES ? N_("嘗試太多次了，請稍後再試") : cd.msg[0] ? cd.msg : "";
		pam_end(ph, r);
		ws(res, 'F', m);
		_exit(1);
	}
	/* PAM 模組可能改了使用者名稱（例如大小寫） */
	const void *item = NULL;
	if (pam_get_item(ph, PAM_USER, &item) == PAM_SUCCESS && item) snprintf(user, 64, "%s", (const char *)item);
	struct passwd *pw = getpwnam(user);
	/* root 與停用的帳號（nologin）不能從登入畫面登入（跟 GDM、macOS 一樣） */
	if (!allowed_user(pw)) { pam_end(ph, PAM_PERM_DENIED); ws(res, 'F', N_("這個帳號不允許從登入畫面登入")); _exit(1); }
	struct passwd pwc = *pw;
	char pname[64], pdir[512], pshell[256];
	snprintf(pname, sizeof pname, "%s", pw->pw_name);
	snprintf(pdir, sizeof pdir, "%s", pw->pw_dir);
	snprintf(pshell, sizeof pshell, "%s", pw->pw_shell && *pw->pw_shell ? pw->pw_shell : "/bin/sh");
	pwc.pw_name = pname; pwc.pw_dir = pdir; pwc.pw_shell = pshell;
	ws(res, 'O', pwc.pw_name);   /* 回報 PAM 最後確定的帳號名稱 */

	/* 等主行程把 compositor 的顯示切給這個使用者（SETUID）之後送出的 'g' */
	unsigned char g = 0;
	ssize_t gk;
	do { gk = read(go, &g, 1); } while (gk < 0 && errno == EINTR);
	if (gk != 1 || g != 'g') { pam_end(ph, PAM_SUCCESS); _exit(1); }
	long t_go = now_ms();

	char vtnr[16];
	snprintf(vtnr, sizeof vtnr, "XDG_VTNR=%s", cfg.vt);
	char desk[96];
	snprintf(desk, sizeof desk, "XDG_SESSION_DESKTOP=%s", session);
	const char *penv[] = { "XDG_SESSION_TYPE=wayland", "XDG_SESSION_CLASS=user", "XDG_SEAT=seat0", vtnr, desk };
	for (unsigned i = 0; i < LEN(penv); i++) pam_putenv(ph, penv[i]);
	if (!cfg.test && initgroups(pwc.pw_name, pwc.pw_gid)) { logmsg("initgroups 失敗"); pam_end(ph, PAM_SYSTEM_ERR); _exit(1); }
	r = pam_setcred(ph, PAM_ESTABLISH_CRED);
	if (r != PAM_SUCCESS) { logmsg("pam_setcred：%s", pam_strerror(ph, r)); pam_end(ph, r); _exit(1); }
	r = pam_open_session(ph, 0);
	if (r != PAM_SUCCESS) {
		logmsg("pam_open_session：%s", pam_strerror(ph, r));
		pam_setcred(ph, PAM_DELETE_CRED);
		pam_end(ph, r);
		_exit(1);
	}
	sigset_t term;
	sigemptyset(&term);
	sigaddset(&term, SIGTERM);
	sigaddset(&term, SIGCHLD);
	sigprocmask(SIG_BLOCK, &term, NULL);

	char **pamenv = pam_getenvlist(ph);
	/* 0.40 起程式直接用原生 Wayland：請 compositor 在使用者的 /run/user/UID 開一個 socket
	 * （logind 剛在 pam_open_session 建好這個目錄；為什麼不用 /run/pomelo-dm 那個，見 pomelo-compositor/ipc.c）。
	 * 舊版 compositor 不認得這個指令就照舊當 X11 工作階段 */
	char wl_name[32] = "";
	if (!cfg.test) {
		char req[64], rep[128] = "";
		snprintf(req, sizeof req, "USERSOCK %ld", (long)pwc.pw_uid);
		if (ipc_call(req, rep, sizeof rep) == 0) snprintf(wl_name, sizeof wl_name, "%s", rep + 3);
		else logmsg("compositor 沒有開使用者的 Wayland socket：%s", rep[0] ? rep + 4 : "沒有回應");
	} else if (comp_wayland[0]) {
		snprintf(wl_name, sizeof wl_name, "%s/%s", cfg.run_dir, comp_wayland);   /* 巢狀測試：直接用絕對路徑 */
	}
	pid_t child = fork();
	if (child == 0) {
		setsid();
		if (!cfg.test) {
			if (setgid(pwc.pw_gid) || setuid(pwc.pw_uid)) _exit(126);
			if (setuid(0) == 0) _exit(126);
		}
		const char *keep_path = getenv("PATH");
		char path_env[1024], keepv[3][512];
		const char *keep[] = { "POMELO_I18N_DIR", "LD_LIBRARY_PATH", "NO_AT_BRIDGE" };
		snprintf(path_env, sizeof path_env, "%s", cfg.test && keep_path ? keep_path :
		         "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:/usr/games:/usr/local/games:/snap/bin");
		for (unsigned i = 0; i < LEN(keep); i++) snprintf(keepv[i], sizeof keepv[i], "%s", cfg.test && getenv(keep[i]) ? getenv(keep[i]) : "");
		char lang[64];
		system_lang(lang, sizeof lang);
		clearenv();
		for (char **e = pamenv; e && *e; e++) putenv(*e);
		for (unsigned i = 0; i < LEN(keep); i++) if (keepv[i][0]) setenv(keep[i], keepv[i], 1);
		setenv("PATH", path_env, 1);
		setenv("HOME", pwc.pw_dir, 1);
		setenv("USER", pwc.pw_name, 1);
		setenv("LOGNAME", pwc.pw_name, 1);
		setenv("SHELL", pwc.pw_shell, 1);
		setenv("LANG", lang, 0);
		setenv("DISPLAY", cfg.display, 1);
		/* 0.40 起 compositor 有 xdg-shell，GTK、Firefox 這些程式直接用原生 Wayland（0.36 以前只有 XWayland，
		 * 那時給了 WAYLAND_DISPLAY，Qt 6、Chrome 會選 Wayland 結果開不出視窗，所以當 X11 工作階段）。
		 * Pomelo 自己的程式照舊走 DISPLAY（XWayland）。使用者可以在設定裡關掉原生 Wayland，見 pomelo-session */
		if (wl_name[0]) {
			setenv("WAYLAND_DISPLAY", wl_name, 1);
			setenv("XDG_SESSION_TYPE", "wayland", 1);
		} else {
			unsetenv("WAYLAND_DISPLAY");
			setenv("XDG_SESSION_TYPE", "x11", 1);
		}
		setenv("XDG_SESSION_CLASS", "user", 1);
		setenv("XDG_SESSION_DESKTOP", session, 1);
		setenv("DESKTOP_SESSION", session, 1);
		setenv("XDG_CURRENT_DESKTOP", names[0] ? names : session, 1);
		setenv("XDG_SEAT", "seat0", 1);
		setenv("XDG_VTNR", cfg.vt, 1);
		if (chdir(pwc.pw_dir)) { if (chdir("/")) _exit(1); }
		/* 輸出寫到 ~/.xsession-errors（太大就重新開始） */
		char errs[600];
		snprintf(errs, sizeof errs, "%s/.xsession-errors", pwc.pw_dir);
		struct stat st;
		int trunc = stat(errs, &st) == 0 && st.st_size > 1024 * 1024;
		int ef = open(errs, O_WRONLY | O_CREAT | O_NOFOLLOW | O_NOCTTY | (trunc ? O_TRUNC : O_APPEND), 0600);
		int nul = open("/dev/null", O_RDWR);
		if (nul >= 0) dup2(nul, 0);
		/* 開不了（例如被換成捷徑）就丟掉輸出，不要寫進 pomelo-dm 的系統日誌 */
		if (ef < 0) ef = nul;
		if (ef >= 0) { dup2(ef, 1); dup2(ef, 2); }
		if (ef > 2) close(ef);
		if (nul > 2 && nul != ef) close(nul);
		sigset_t all;
		sigemptyset(&all);
		sigprocmask(SIG_SETMASK, &all, NULL);
		/* 跟其他登入管理員一樣先讀 /etc/profile 與 ~/.profile（PATH、環境變數） */
		char script[1400];
		snprintf(script, sizeof script,
		         "for f in /etc/profile \"$HOME/.profile\"; do [ -r \"$f\" ] && . \"$f\"; done >/dev/null 2>&1; exec %s", exec);
		execl("/bin/sh", "sh", "-c", script, (char *)NULL);
		_exit(127);
	}
	int st = 0;
	if (child > 0 && debug) logmsg("PAM 開啟工作階段：%ld 毫秒", now_ms() - t_go);
	if (child > 0) remember_appearance(&pwc);   /* 工作階段已經在啟動了，這裡慢慢做 */
	if (child > 0) {
		/* 結束訊號（例如關機）傳給工作階段；6 秒還不結束就強制結束，一定會走到 pam_close_session */
		long term_at = 0;
		for (;;) {
			struct timespec ts = { 1, 0 };
			int sig = sigtimedwait(&term, NULL, &ts);
			if (sig == SIGTERM && !term_at) { kill(-child, SIGTERM); kill(child, SIGTERM); term_at = now_ms(); }
			if (waitpid(child, &st, WNOHANG) == child) break;
			if (term_at && now_ms() - term_at > 6000) { kill(-child, SIGKILL); kill(child, SIGKILL); }
		}
	}
	r = pam_close_session(ph, 0);
	pam_setcred(ph, PAM_DELETE_CRED);
	pam_end(ph, r);
	_exit(0);
}

/* 工作階段子行程的狀態 */
static int w_res = -1, w_go = -1;
static enum { IDLE, AUTH, SESSION } phase = IDLE;
static char w_user[64], w_session[64];

/* 開機時 systemd 會先放一個 /run/nologin（「系統還在開機」），等 systemd-user-sessions 啟動才拿掉，
 * 這之前 PAM（pam_nologin）不讓一般帳號登入。pomelo-dm 為了讓登入畫面早點出來，不再排在
 * systemd-user-sessions（它又排在 network.target 後面，要等 NetworkManager 起來）之後，所以改成真正要登入時
 * 才在這裡等它拿掉。最多等 30 秒；還在的話照樣登入，讓 PAM 回報原因（例如管理員排了關機）。 */
static void wait_nologin(void)
{
	if (cfg.test) return;
	for (int i = 0; i < 600 && !quitting && access("/run/nologin", F_OK) == 0; i++) idle_ms(50);
}

static int start_worker(const char *user, char *password, const char *session, int autologin)
{
	int rp[2], gp[2];
	wait_nologin();
	if (pipe2(rp, O_CLOEXEC)) return -1;
	if (pipe2(gp, O_CLOEXEC)) { close(rp[0]); close(rp[1]); return -1; }
	snprintf(w_user, sizeof w_user, "%s", user);
	snprintf(w_session, sizeof w_session, "%s", session);
	worker_pid = fork();
	if (worker_pid < 0) return -1;
	if (worker_pid == 0) {
		close(rp[0]); close(gp[1]);
		if (gfd >= 0) close(gfd);
		close(sigfd);
		sigset_t m;
		sigemptyset(&m);
		sigaddset(&m, SIGTERM);   /* SIGTERM 擋住：開了工作階段後在迴圈裡處理；開之前 pomelo-dm 會用 SIGKILL */
		sigprocmask(SIG_SETMASK, &m, NULL);
		signal(SIGTERM, SIG_DFL);
		char u[64];
		snprintf(u, sizeof u, "%s", user);
		worker_main(u, password, session, autologin, rp[1], gp[0]);
		_exit(0);
	}
	close(rp[1]); close(gp[0]);
	w_res = rp[0];
	w_go = gp[1];
	phase = AUTH;
	return 0;
}

static void worker_reset(void)
{
	if (w_res >= 0) close(w_res);
	if (w_go >= 0) close(w_go);
	w_res = w_go = -1;
	phase = IDLE;
}

/* ------------------------------------------------------------------ */
/* 主迴圈                                                              */
/* ------------------------------------------------------------------ */

static int greeter_crashes;
static long greeter_started;

static int handle_signal(const struct signalfd_siginfo *si)
{
	if (si->ssi_signo == SIGTERM || si->ssi_signo == SIGINT) { quitting = 1; return 1; }
	if (si->ssi_signo == SIGCHLD) {
		int st;
		pid_t p;
		while ((p = waitpid(-1, &st, WNOHANG)) > 0) {
			if (p == x_pid) { x_pid = -1; logmsg("pomelo-compositor 結束了"); }
			else if (p == greeter_pid) greeter_pid = -1;
			else if (p == worker_pid) worker_pid = -1;
		}
	}
	return 0;
}

/* 執行一個指令（不經過 shell），最多等 ms 毫秒；out 可以是 NULL。回傳結束碼，失敗或逾時 -1 */
static int run_out(char *const argv[], char *out, size_t n, int ms)
{
	int p[2];
	if (pipe2(p, O_CLOEXEC)) return -1;
	pid_t c = fork();
	if (c < 0) { close(p[0]); close(p[1]); return -1; }
	if (c == 0) {
		sigset_t all;
		sigemptyset(&all);
		sigprocmask(SIG_SETMASK, &all, NULL);
		dup2(p[1], 1);
		int nul = open("/dev/null", O_RDWR);
		if (nul >= 0) { dup2(nul, 0); dup2(nul, 2); }
		execvp(argv[0], argv);
		_exit(127);
	}
	close(p[1]);
	char tmp[256];
	long got = read_child(c, p[0], out ? out : tmp, out ? n : sizeof tmp, NULL, NULL, ms);
	return got < 0 ? -1 : 0;
}

/* 還有沒有其他人登入中（例如 ssh、別的 VT）；已經登出、只是還在收尾（closing）的不算 */
static int others_logged_in(void)
{
	char list[4096] = "";
	char *a[] = { "loginctl", "list-sessions", "--no-legend", NULL };
	if (run_out(a, list, sizeof list, 5000) < 0) return 0;
	for (char *l = strtok(list, "\n"); l; l = strtok(NULL, "\n")) {
		char id[64] = "";
		if (sscanf(l, " %63s", id) != 1 || !valid_id(id)) continue;
		char state[64] = "";
		char *b[] = { "loginctl", "show-session", id, "-p", "State", "--value", NULL };
		if (run_out(b, state, sizeof state, 3000) < 0) continue;
		if (!strncmp(state, "active", 6) || !strncmp(state, "online", 6)) return 1;
	}
	return 0;
}

/* 登入畫面的睡眠、重新開機、關機。跟 GDM 一樣：還有其他人登入時不准（不然沒登入的人就能打斷別人的 ssh），
 * 也尊重程式的「暫時不要關機」要求（例如系統更新中） */
static void power(const char *what)
{
	const char *verb = !strcmp(what, "poweroff") ? "poweroff" : !strcmp(what, "reboot") ? "reboot" : !strcmp(what, "suspend") ? "suspend" : NULL;
	if (!verb) return;
	logmsg("電源：%s", verb);
	if (cfg.test) return;
	if (others_logged_in()) {
		send_greeter("INFO", N_("還有其他人登入這台電腦，不能從登入畫面關機、重新開機或睡眠"));
		return;
	}
	char *a[] = { "systemctl", "--check-inhibitors=yes", (char *)verb, NULL };
	if (run_out(a, NULL, 0, 15000) != 0)
		send_greeter("INFO", N_("有程式暫時不讓電腦關機或睡眠（例如正在更新），請稍後再試"));
}

/* 處理一則登入畫面的訊息 */
static void greeter_msg(void)
{
	char buf[1024];
	ssize_t n = recv(gfd, buf, sizeof buf - 1, 0);
	if (n <= 0) { close(gfd); gfd = -1; return; }
	buf[n] = 0;
	/* 欄位：buf、buf+strlen+1… */
	char *f[4] = { 0 };
	int nf = 0;
	for (char *p = buf; p < buf + n && nf < 4; p += strlen(p) + 1) f[nf++] = p;
	if (debug) logmsg("登入畫面：%s（%d 個欄位）", f[0] ? f[0] : "?", nf);
	if (nf >= 1 && !strcmp(f[0], "LOGIN") && nf == 4) {
		if (phase != IDLE) { send_greeter("FAIL", N_("正在登入中")); explicit_bzero(buf, sizeof buf); return; }
		const char *session = valid_id(f[3]) ? f[3] : cfg.default_session;
		char exec[1024], names[256];
		if (!session_info(session, exec, sizeof exec, names, sizeof names)) session = cfg.default_session;
		struct passwd *pw = strlen(f[1]) <= 60 ? getpwnam(f[1]) : NULL;
		if (!allowed_user(pw)) {
			/* 不存在或不能登入的帳號（root、nologin）：密碼不用驗證，但照樣等 2 秒、回一樣的錯誤，免得被拿來猜帳號 */
			explicit_bzero(buf, sizeof buf);
			idle_ms(2000);
			send_greeter("FAIL", "");
			return;
		}
		if (start_worker(f[1], f[2], session, 0)) send_greeter("FAIL", N_("無法開始登入"));
		explicit_bzero(buf, sizeof buf);
	} else if (nf == 2 && !strcmp(f[0], "POWER")) {
		power(f[1]);
	}
	explicit_bzero(buf, sizeof buf);
}

/* 讓工作階段子行程開始：把 compositor 的顯示切給剛登入的使用者（登入畫面／自動登入帳號
 * 都要走這裡），再放行 worker 去 exec 工作階段。Xorg 時代這裡要先強制 X 重設、生新 cookie，
 * 現在只是一次 chown，見 ipc_setuid()。 */
static int go_session(void)
{
	struct passwd *pw = getpwnam(w_user);
	if (!pw) { logmsg("找不到使用者 %s", w_user); return -1; }
	if (ipc_setuid(pw->pw_uid)) return -1;
	unsigned char g = 'g';
	return write(w_go, &g, 1) == 1 ? 0 : -1;
}

/* 跑一輪：啟動 compositor → 登入畫面 → 工作階段 → 登出後關掉 compositor */
/* 回傳 0＝合成器沒起來（這一輪什麼都沒做，下一輪還可以再試自動登入）、1＝有走到登入階段 */
static int run_cycle(int try_autologin)
{
	if (start_compositor()) { stop_compositor(); idle_ms(2000); return 0; }
	int session_started = 0;
	if (try_autologin && cfg.autologin_user[0]) {
		const char *s = cfg.autologin_session[0] ? cfg.autologin_session : cfg.default_session;
		struct passwd *ap = getpwnam(cfg.autologin_user);
		if (!allowed_user(ap)) logmsg("自動登入的帳號 %s 不存在或不能登入，改顯示登入畫面", cfg.autologin_user);
		else if (start_worker(cfg.autologin_user, NULL, s, 1) == 0) {
			char r[300] = "";
			struct pollfd pf = { w_res, POLLIN, 0 };
			ssize_t k = poll(&pf, 1, 30000) > 0 ? read(w_res, r, sizeof r - 1) : 0;
			if (k > 1 && r[0] == 'O' && strlen(r + 1) < sizeof w_user && getpwnam(r + 1))
				snprintf(w_user, sizeof w_user, "%s", r + 1);
			if (k > 0 && r[0] == 'O' && go_session() == 0) {
				phase = SESSION;
				session_started = 1;
				logmsg("%s 自動登入（%s）", w_user, w_session);
			} else {
				logmsg("自動登入失敗，改顯示登入畫面");
				kill_now(&worker_pid);
				worker_reset();
			}
		}
	}
	if (!session_started && start_greeter()) { logmsg("無法啟動登入畫面"); stop_compositor(); idle_ms(3000); return 1; }
	greeter_started = (long)time(NULL);

	while (!quitting) {
		struct pollfd p[3];
		int np = 0;
		p[np++] = (struct pollfd){ sigfd, POLLIN, 0 };
		int ig = -1, iw = -1;
		if (gfd >= 0) { ig = np; p[np++] = (struct pollfd){ gfd, POLLIN, 0 }; }
		if (phase == AUTH && w_res >= 0) { iw = np; p[np++] = (struct pollfd){ w_res, POLLIN, 0 }; }
		if (poll(p, (nfds_t)np, 1000) < 0 && errno != EINTR) break;
		if (p[0].revents & POLLIN) {
			struct signalfd_siginfo si;
			if (read(sigfd, &si, sizeof si) == sizeof si) handle_signal(&si);
		}
		if (ig >= 0 && p[ig].revents) greeter_msg();
		if (iw >= 0 && p[iw].revents) {
			char r[300] = "";
			ssize_t k = read(w_res, r, sizeof r - 1);
			if (debug) logmsg("驗證結果：%c", k > 0 ? r[0] : '-');
			if (k > 0 && r[0] == 'O') {
				if (k > 1 && strlen(r + 1) < sizeof w_user && getpwnam(r + 1)) snprintf(w_user, sizeof w_user, "%s", r + 1);
				long t_ok = now_ms();
				send_greeter("OK", NULL);
				state_set("last-user", w_user);
				char key[100];
				snprintf(key, sizeof key, "session.%s", w_user);
				state_set(key, w_session);
				stop_greeter(1000);   /* 等登入畫面淡出、結束（連同它的所有行程） */
				if (debug) logmsg("登入畫面結束：%ld 毫秒", now_ms() - t_ok);
				if (go_session() == 0) { phase = SESSION; session_started = 1; logmsg("%s 登入（%s）", w_user, w_session); }
				else { kill_now(&worker_pid); worker_reset(); break; }
			} else {
				send_greeter("FAIL", k > 1 ? r + 1 : "");
				kill_now(&worker_pid);   /* 還沒開工作階段，直接結束 */
				worker_reset();
			}
		}
		if (x_pid < 0) break;                                   /* X 結束了：重來一輪 */
		if (phase == SESSION && worker_pid < 0) { logmsg("%s 登出", w_user); break; }
		if (phase == AUTH && worker_pid < 0 && w_res < 0) worker_reset();
		if (!session_started && greeter_pid < 0 && phase == IDLE) {
			/* 登入畫面當掉了：重開。連續當掉就改成不載入使用者圖片的模式（圖片可能有問題）；
			 * 太多次就等一下再試 */
			if ((long)time(NULL) - greeter_started < 60) greeter_crashes++;
			else greeter_crashes = 0;
			if (greeter_crashes >= 2 && !greeter_safe) { greeter_safe = 1; logmsg("登入畫面一直結束，改成不顯示使用者圖片"); }
			stop_greeter(0);
			if (greeter_crashes > 5) { logmsg("登入畫面一直結束，30 秒後重試（可以按 Ctrl+Alt+F3 用文字登入）"); idle_ms(30000); greeter_crashes = 0; }
			if (quitting || start_greeter()) break;
			greeter_started = (long)time(NULL);
		}
	}
	stop_greeter(0);
	if (worker_pid > 0) {
		if (phase == SESSION) kill_wait(&worker_pid, 12000);   /* 子行程會結束工作階段並關閉 PAM session */
		else kill_now(&worker_pid);
	}
	worker_reset();
	stop_compositor();
	return 1;
}


/* ------------------------------------------------------------------ */
/* 系統設定「登入畫面」用：寫入 /etc/pomelo/dm.conf（經由 pkexec，以 root 執行）  */
/* ------------------------------------------------------------------ */

static int is_bool(const char *v) { return !strcmp(v, "true") || !strcmp(v, "false"); }

/* 檢查一個設定值；不合法回傳錯誤訊息 */
static const char *check_setting(const char *k, const char *v)
{
	size_t l = strlen(v);
	for (const unsigned char *c = (const unsigned char *)v; *c; c++) if (*c < 32 || *c == 127) return "不能有控制字元或換行";
	if (!strcmp(k, "autologin-user")) {
		if (!*v) return NULL;
		struct passwd *pw = getpwnam(v);
		return pw && allowed_user(pw) ? NULL : "這個帳號不存在或不能登入";
	}
	if (!strcmp(k, "autologin-session") || !strcmp(k, "default-session")) {
		char e[1024], n[256];
		if (!*v && !strcmp(k, "autologin-session")) return NULL;
		return session_info(v, e, sizeof e, n, sizeof n) ? NULL : "找不到這個工作階段";
	}
	if (!strcmp(k, "hide-users")) {
		if (l > 400) return "太長了";
		for (const char *c = v; *c; c++) if (!isalnum((unsigned char)*c) && !strchr("._-,", *c)) return "帳號名稱只能有英文、數字與 . _ -";
		return NULL;
	}
	if (!strcmp(k, "show-other-user") || !strcmp(k, "user-list") || !strcmp(k, "power-buttons")) return is_bool(v) ? NULL : "要是 true 或 false";
	if (!strcmp(k, "clock")) return !strcmp(v, "24h") || !strcmp(v, "12h") || !strcmp(v, "off") ? NULL : "要是 24h、12h 或 off";
	if (!strcmp(k, "text-size")) return !strcmp(v, "normal") || !strcmp(v, "large") ? NULL : "要是 normal 或 large";
	if (!strcmp(k, "background")) return !strcmp(v, "user") || !strcmp(v, "image") || !strcmp(v, "gradient") ? NULL : "要是 user、image 或 gradient";
	if (!strcmp(k, "message")) return l <= 300 ? NULL : "訊息太長了（最多約 100 個中文字）";
	return "不認得這個設定";
}

/* 把 key=value 寫進設定檔：已有的行就換掉，沒有就加在最後；註解與其他行保持原樣 */
static int write_settings(char **kv, int n)
{
	char buf[16384] = "";
	size_t k = 0;
	FILE *f = fopen(cfg.conf_path, "r");
	char line[1024];
	int done[64] = { 0 };
	while (f && fgets(line, sizeof line, f)) {
		const char *rep = NULL;
		for (int i = 0; i < n && i < 64; i++) {
			size_t kl = strcspn(kv[i], "=");
			if (!strncmp(line, kv[i], kl) && line[kl] == '=') { rep = kv[i]; done[i] = 1; }
		}
		int w = rep ? snprintf(buf + k, sizeof buf - k, "%s\n", rep) : snprintf(buf + k, sizeof buf - k, "%s", line);
		if (w < 0 || (size_t)w >= sizeof buf - k) { if (f) fclose(f); return -1; }
		k += (size_t)w;
	}
	if (f) fclose(f);
	for (int i = 0; i < n && i < 64; i++) {
		if (done[i]) continue;
		int w = snprintf(buf + k, sizeof buf - k, "%s\n", kv[i]);
		if (w < 0 || (size_t)w >= sizeof buf - k) return -1;
		k += (size_t)w;
	}
	char tmp[300];
	snprintf(tmp, sizeof tmp, "%s.new", cfg.conf_path);
	unlink(tmp);
	int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644);
	if (fd < 0) return -1;
	int ok = write(fd, buf, k) == (ssize_t)k && fsync(fd) == 0;
	close(fd);
	if (!ok || rename(tmp, cfg.conf_path)) { unlink(tmp); return -1; }
	return 0;
}

/* 呼叫的人（pkexec 會設 PKEXEC_UID）；測試模式就是自己 */
static struct passwd *caller(void)
{
	const char *u = getenv("PKEXEC_UID");
	if (!u) u = getenv("SUDO_UID");
	return u ? getpwuid((uid_t)atoi(u)) : getpwuid(getuid());
}

/* 登入畫面的背景圖：用「呼叫的人」的身分讀（不能拿 root 權限讀別人的檔案），
 * 只收 PNG／JPEG、16384 像素以內、25 MB 以內，存成 state/background */
static int set_background(const char *src)
{
	struct passwd *pw = caller();
	if (!pw) { fputs("pomelo-dm: 找不到呼叫的使用者\n", stderr); return 1; }
	struct passwd pwc = *pw;
	char name[64], dir[512];
	snprintf(name, sizeof name, "%s", pw->pw_name);
	snprintf(dir, sizeof dir, "%s", pw->pw_dir);
	pwc.pw_name = name; pwc.pw_dir = dir;
	int p[2];
	if (pipe(p)) return 1;
	pid_t c = fork();
	if (c == 0) {
		close(p[0]);
		if (getuid() == 0 && (initgroups(pwc.pw_name, pwc.pw_gid) || setgid(pwc.pw_gid) || setuid(pwc.pw_uid) || setuid(0) == 0)) _exit(1);
		int fd = open(src, O_RDONLY | O_NOCTTY | O_NONBLOCK);
		struct stat st;
		if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size > 25 * 1024 * 1024) _exit(1);
		char buf[65536];
		ssize_t r;
		while ((r = read(fd, buf, sizeof buf)) > 0) if (write(p[1], buf, (size_t)r) != r) _exit(1);
		_exit(r < 0);
	}
	close(p[1]);
	char dst[400], tmp[420];
	snprintf(dst, sizeof dst, "%s/background", cfg.state_dir);
	snprintf(tmp, sizeof tmp, "%s.new", dst);
	unlink(tmp);
	static CopyCtx cc;
	cc.hk = 0;
	cc.out = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644);
	if (cc.out < 0) { close(p[0]); kill(c, SIGKILL); waitpid(c, NULL, 0); fputs("pomelo-dm: 無法寫入背景圖\n", stderr); return 1; }
	long got = read_child(c, p[0], NULL, 25 * 1024 * 1024, copy_sink, &cc, 10000);
	unsigned w = 0, h = 0;
	int ok = got > 0 && image_size(cc.head, cc.hk, &w, &h) && w && h && w <= 16384 && h <= 16384 && (unsigned long long)w * h <= 40000000ULL;
	close(cc.out);
	if (!ok || rename(tmp, dst)) { unlink(tmp); fputs("pomelo-dm: 不是可用的圖片（只收 PNG 或 JPEG，而且你要能讀這個檔案）\n", stderr); return 1; }
	return 0;
}

/* pomelo-dm --set key=value … [--set-background 圖片] */
static int set_config(int argc, char *argv[])
{
	if (!cfg.test && getuid() != 0) { fputs("pomelo-dm: 要用管理員身分執行（pkexec）\n", stderr); return 1; }
	char *kv[64];
	int n = 0;
	const char *bg = NULL;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--set")) continue;
		if (!strcmp(argv[i], "--set-background") && i + 1 < argc) { bg = argv[++i]; continue; }
		if (!strcmp(argv[i], "--test")) continue;
		if ((!strcmp(argv[i], "--config") || !strcmp(argv[i], "--state")) && i + 1 < argc) { i++; continue; }
		char *eq = strchr(argv[i], '=');
		if (!eq || n >= 64) { fprintf(stderr, "pomelo-dm: 看不懂「%s」（要寫成 key=value）\n", argv[i]); return 2; }
		*eq = 0;
		const char *err = check_setting(argv[i], eq + 1);
		if (err) { fprintf(stderr, "pomelo-dm: %s：%s\n", argv[i], err); return 2; }
		*eq = '=';
		kv[n++] = argv[i];
	}
	if (bg && set_background(bg)) return 1;
	if (n && write_settings(kv, n)) { fprintf(stderr, "pomelo-dm: 無法寫入 %s\n", cfg.conf_path); return 1; }
	return 0;
}

static void usage(void)
{
	puts("用法：pomelo-dm                         由 systemd 啟動的登入管理員（需要 root）\n"
	     "      pomelo-dm --version\n"
	     "      pomelo-dm --test [--display :9] [--compositor \"pomelo-compositor --nested\"]\n"
	     "                [--pam-dir 目錄] [--state 目錄] [--sessions 目錄] [--greeter 程式]\n"
	     "                以目前的使用者測試整個流程，不切換身分、不登記 logind、不真的關機\n"
	     "      pomelo-dm --set key=value … [--set-background 圖片]\n"
	     "                修改 /etc/pomelo/dm.conf（系統設定的「登入畫面」用，要 pkexec）");
}

int main(int argc, char *argv[])
{
	/* --set：只寫設定檔，不啟動登入管理員 */
	for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--set") || !strcmp(argv[i], "--set-background")) {
		for (int j = 1; j + 1 < argc; j++) {
			if (!strcmp(argv[j], "--config")) snprintf(cfg.conf_path, sizeof cfg.conf_path, "%s", argv[j + 1]);
			if (!strcmp(argv[j], "--state")) snprintf(cfg.state_dir, sizeof cfg.state_dir, "%s", argv[j + 1]);
			if (!strcmp(argv[j], "--test")) cfg.test = 1;
		}
		if (!strcmp(argv[argc - 1], "--test")) cfg.test = 1;
		/* 只有測試模式（一般使用者）才能改路徑；以 root 執行一律寫 /etc/pomelo/dm.conf 與 /var/lib/pomelo-dm */
		if (!cfg.test || getuid() == 0 || geteuid() == 0) {
			if (cfg.test) { fputs("pomelo-dm: --test 不能以 root 執行\n", stderr); return 1; }
			snprintf(cfg.conf_path, sizeof cfg.conf_path, "%s", "/etc/pomelo/dm.conf");
			snprintf(cfg.state_dir, sizeof cfg.state_dir, "%s", "/var/lib/pomelo-dm");
		}
		return set_config(argc, argv);
	}
	for (int i = 1; i < argc; i++) {
		const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
		if (!strcmp(a, "--version")) { puts("pomelo-dm " VERSION); return 0; }
		else if (!strcmp(a, "--help") || !strcmp(a, "-h")) { usage(); return 0; }
		else if (!strcmp(a, "--test")) cfg.test = 1;
		else if (v && !strcmp(a, "--display")) { snprintf(cfg.display, sizeof cfg.display, "%s", v); i++; }
		else if (v && !strcmp(a, "--compositor")) { snprintf(cfg.compositor, sizeof cfg.compositor, "%s", v); i++; }
		else if (v && !strcmp(a, "--pam-dir")) { snprintf(cfg.pam_dir, sizeof cfg.pam_dir, "%s", v); i++; }
		else if (v && !strcmp(a, "--state")) { snprintf(cfg.state_dir, sizeof cfg.state_dir, "%s", v); i++; }
		else if (v && !strcmp(a, "--sessions")) { snprintf(cfg.sessions_dir, sizeof cfg.sessions_dir, "%s", v); i++; }
		else if (v && !strcmp(a, "--greeter")) { snprintf(cfg.greeter, sizeof cfg.greeter, "%s", v); i++; }
		else if (v && !strcmp(a, "--config")) { snprintf(cfg.conf_path, sizeof cfg.conf_path, "%s", v); i++; }
		else { usage(); return 2; }
	}
	char xs[sizeof cfg.compositor];
	snprintf(xs, sizeof xs, "%s", cfg.compositor);   /* 命令列的 --compositor 優先於設定檔 */
	load_conf();
	if (xs[0]) snprintf(cfg.compositor, sizeof cfg.compositor, "%s", xs);
	if (cfg.display[0] != ':' || !isdigit((unsigned char)cfg.display[1])) { logmsg("display 必須像 :0"); return 2; }
	if (!cfg.test && getuid() != 0) { logmsg("需要以 root 執行（測試請加 --test）"); return 1; }
	if (cfg.test) {
		snprintf(cfg.run_dir, sizeof cfg.run_dir, "%s/run", cfg.state_dir);
		mkdir(cfg.state_dir, 0700);
	} else {
		mkdir(cfg.state_dir, 0755);
		chmod(cfg.state_dir, 0755);
	}
	/* 使用者的桌布、頭像：只有 root 與登入畫面讀得到 */
	char udir[320];
	snprintf(udir, sizeof udir, "%s/users", cfg.state_dir);
	mkdir(udir, 0750);
	if (!cfg.test) {
		gid_t gg = greeter_gid();
		if (gg != (gid_t)-1 && chown(udir, 0, gg)) logmsg("無法設定 %s 的權限", udir);
		chmod(udir, 0750);
	}
	mkdir(cfg.run_dir, 0711);
	umask(022);

	sigset_t mask;
	sigemptyset(&mask);
	sigaddset(&mask, SIGCHLD);
	sigaddset(&mask, SIGUSR1);
	sigaddset(&mask, SIGTERM);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGHUP);
	sigprocmask(SIG_BLOCK, &mask, NULL);
	sigfd = signalfd(-1, &mask, SFD_CLOEXEC);
	if (sigfd < 0) { logmsg("signalfd 失敗"); return 1; }
	signal(SIGPIPE, SIG_IGN);

	debug = getenv("POMELO_DM_DEBUG") != NULL;
	logmsg("啟動（%s）", VERSION);
	int first = 1, quick = 0;
	while (!quitting) {
		long t0 = (long)time(NULL);
		/* 自動登入只在第一次真的走到登入階段時試一次（登出後要回到登入畫面）；
		 * 合成器第一次沒在時限內起來（慢的機器、沒有硬體 3D 的虛擬機）那一輪不算，下一輪還是會自動登入 */
		if (run_cycle(first)) first = 0;
		/* 一直馬上結束（例如 X 起不來）：放慢速度，不要佔滿 CPU */
		if ((long)time(NULL) - t0 < 5) { if (++quick > 3) idle_ms(5000); }
		else quick = 0;
	}
	logmsg("結束");
	return 0;
}
