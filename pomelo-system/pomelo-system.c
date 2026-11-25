/* SPDX-License-Identifier: GPL-3.0-or-later */
/* pomelo-system — Pomelo 系統設定
 *
 * 首頁：一般、外觀、背景圖片、桌面與 Dock、選單列、顯示器、Spotlight、通知、聲音、鍵盤、
 *       滑鼠與觸控板、螢幕使用時間、使用者與群組、印表機與掃描器、網路、藍牙、電源與鎖定
 *       （外觀等交給其他設定程式，沒安裝的會標示出來）
 * 一般：關於、軟體更新、儲存空間、日期與時間、語言與地區、裝置管理
 *
 * 需要管理員權限的動作用 pkexec 或 timedatectl／hostnamectl（polkit 會跳出密碼視窗）。
 *
 * 用法：pomelo-system [--page 頁面名稱]   例如 --page general、--page users
 *       pomelo-system --apply             套用鍵盤與滑鼠設定（登入時由 pomelo-session 呼叫）
 */
#define _GNU_SOURCE
#include <pomelo.h>
#include <pomelo-wl.h>
#include <pomelo-check.h>
#include <dirent.h>
#include <malloc.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <pthread.h>
#include <ftw.h>
#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include "saver-idle.h"   /* 螢幕保護程式的閒置時間表（純計算，與 pomelo-power、pomelo-screensaver 共用） */

enum { P_MAIN, P_GENERAL, P_ABOUT, P_UPDATE, P_STORAGE, P_DATETIME, P_DEVICES, P_POWER,
       P_SPOTLIGHT, P_NOTIFY, P_KEYBOARD, P_MOUSE, P_SCREENTIME, P_USERS, P_PRINTERS, P_CAMERA,
       P_LOGIN, P_APPS, P_APP_EDIT, P_HEALTH, P_VIRUS, P_SBOM };
static int page = P_MAIN;
static const char *page_names[] = { "main", "general", "about", "update", "storage", "datetime", "devices", "power",
                                    "spotlight", "notifications", "keyboard", "mouse", "screentime", "users", "printers", "camera",
                                    "login", "apps", "app", "health", "virus", "sbom" };
static char status[512];

static void go(PmUI *u, int p);

/* ------------------------------------------------------------------ */
/* 共用：管理員權限、背景執行緒                                         */
/* ------------------------------------------------------------------ */

static pid_t job;
static char job_title[256];
static void (*job_done)(int ok);

static int has_polkit_agent(void)
{
	static const char *agents[] = { "pomelo-polkit", "polkit-gnome-au", "lxpolkit", "lxqt-policykit-", "polkit-kde-auth",
	                                "polkit-mate-aut", "gnome-shell", "xfce-polkit" };
	DIR *d = opendir("/proc");
	struct dirent *e;
	int found = 0;
	while (d && (e = readdir(d)) && !found) {
		if (!isdigit((unsigned char)e->d_name[0])) continue;
		char p[64], c[32] = "";
		snprintf(p, sizeof p, "/proc/%s/comm", e->d_name);
		FILE *f = fopen(p, "r");
		if (!f) continue;
		if (fgets(c, sizeof c, f)) c[strcspn(c, "\n")] = 0;
		fclose(f);
		for (size_t i = 0; i < LEN(agents); i++) if (!strncmp(c, agents[i], strlen(agents[i]))) found = 1;
	}
	if (d) closedir(d);
	return found;
}

/* pkexec 執行（argv 不經過 shell）；沒有密碼視窗時改開終端機跑 sudo */
static void run_admin(char *const argv[], const char *title, void (*done)(int ok))
{
	if (job > 0) return;
	/* 介面元件預設把 SIGCHLD 設成忽略（子行程自動回收），那樣 waitpid 永遠拿不到結果、工作永遠不算完成 */
	signal(SIGCHLD, SIG_DFL);
	snprintf(job_title, sizeof job_title, "%s", title);
	status[0] = 0;
	if (!has_polkit_agent()) {
		char cmd[2048] = "sudo";
		for (int i = 0; argv[i]; i++) { strcat(cmd, " '"); strcat(cmd, argv[i]); strcat(cmd, "'"); }
		strcat(cmd, "; echo; echo '");
		strcat(cmd, _("完成，按 Enter 關閉這個視窗"));
		strcat(cmd, "'; read x");
		if (fork() == 0) {
			if (dpy) close(ConnectionNumber(dpy));   /* 原生 Wayland（pm_ui_native）時沒有 X 連線 */
			setsid();
			execlp("pomelo-terminal", "pomelo-terminal", "-t", title, "-e", "sh", "-c", cmd, (char *)NULL);
			_exit(127);
		}
		snprintf(status, sizeof status, "%s", _("已在終端機中執行，請在那裡輸入密碼。"));
		return;
	}
	job_done = done;
	job = fork();
	if (job == 0) {
		int nul = open("/dev/null", O_RDWR);
		if (nul >= 0) { dup2(nul, 0); dup2(nul, 1); dup2(nul, 2); }
		if (dpy) close(ConnectionNumber(dpy));   /* 原生 Wayland（pm_ui_native）時沒有 X 連線 */
		char *full[32] = { "pkexec", "env", "DEBIAN_FRONTEND=noninteractive", "LC_ALL=C" };
		int k = 4;
		for (int i = 0; argv[i] && k < 31; i++) full[k++] = argv[i];
		full[k] = NULL;
		execvp("pkexec", full);
		_exit(127);
	}
}

/* 不需要 pkexec 的指令（timedatectl、hostnamectl 自己會透過 polkit 問密碼） */
static void run_cmd(char *const argv[], const char *title, void (*done)(int ok))
{
	if (job > 0) return;
	/* 介面元件預設把 SIGCHLD 設成忽略（子行程自動回收），那樣 waitpid 永遠拿不到結果、工作永遠不算完成 */
	signal(SIGCHLD, SIG_DFL);
	snprintf(job_title, sizeof job_title, "%s", title);
	status[0] = 0;
	job_done = done;
	job = fork();
	if (job == 0) {
		int nul = open("/dev/null", O_RDWR);
		if (nul >= 0) { dup2(nul, 0); dup2(nul, 1); dup2(nul, 2); }
		if (dpy) close(ConnectionNumber(dpy));   /* 原生 Wayland（pm_ui_native）時沒有 X 連線 */
		execvp(argv[0], argv);
		_exit(127);
	}
}

static void poll_job(void)
{
	if (job <= 0) return;
	int st;
	if (waitpid(job, &st, WNOHANG) != job) return;
	job = 0;
	int ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
	if (!ok && WIFEXITED(st) && (WEXITSTATUS(st) == 126 || WEXITSTATUS(st) == 127))
		snprintf(status, sizeof status, "%s", _("已取消（沒有取得管理員權限）"));
	else if (!ok)
		snprintf(status, sizeof status, _("「%s」沒有成功完成"), job_title);
	else
		snprintf(status, sizeof status, _("「%s」已完成"), job_title);
	if (job_done) job_done(ok);
}

/* 背景執行緒：讀取比較慢的資訊（apt、ubuntu-drivers） */
static pthread_t worker;
static volatile int worker_busy;
static int worker_started;
static void (*worker_finish)(void);

static void start_worker(void *(*fn)(void *), void (*finish)(void))
{
	if (worker_started) return;   /* 一次一個；這些工作都不長 */
	worker_busy = 1;
	worker_started = 1;
	worker_finish = finish;
	pthread_create(&worker, NULL, fn, NULL);
}

static void poll_worker(void)
{
	if (!worker_started || worker_busy) return;
	pthread_join(worker, NULL);
	worker_started = 0;
	if (worker_finish) worker_finish();
}

static void spawn_argv(char *const argv[])
{
	if (fork() == 0) {
		if (dpy) close(ConnectionNumber(dpy));   /* 原生 Wayland（pm_ui_native）時沒有 X 連線 */
		setsid();
		execvp(argv[0], argv);
		_exit(127);
	}
}

static void human(char *out, size_t n, double b) { pm_human_size(out, n, b); }

static void file_time(const char *path, char *out, size_t n)
{
	struct stat st;
	if (stat(path, &st)) { snprintf(out, n, "%s", _("從未")); return; }
	strftime(out, n, _("%-m月%-d日 %H:%M"), localtime(&st.st_mtime));
}

/* ------------------------------------------------------------------ */
/* 首頁                                                                */
/* ------------------------------------------------------------------ */

/* 首頁的項目：bin 有安裝才能開（cmd 是實際執行的指令）；page 是本程式裡的頁面 */
typedef struct { const char *icon, *title, *bin, *cmd; int page; } Pane;
static const Pane panes[][6] = {
	{ { "@general", N_("一般"), NULL, NULL, P_GENERAL },
	  { "@health", N_("主機醫生（健康檢查）"), "pomelo-doctor", "pomelo-doctor", 0 } },
	{ { "@appearance", N_("外觀"), "pomelo-theme", "pomelo-theme --page appearance", 0 },
	  { "@wallpaper", N_("背景圖片"), "pomelo-theme", "pomelo-theme --page wallpaper", 0 },
	  { "@dock", N_("桌面與 Dock"), "pomelo-theme", "pomelo-theme --page dock", 0 },
	  { "@menubar", N_("選單列"), "pomelo-theme", "pomelo-theme --page menubar", 0 },
	  { "@workspace", N_("多桌面"), "pomelo-theme", "pomelo-theme --page workspace", 0 },
	  { "@display", N_("顯示器"), "pomelo-display", "pomelo-display", 0 } },
	{ { "@spotlight", N_("搜尋"), NULL, NULL, P_SPOTLIGHT },
	  { "@apps", N_("應用程式選單"), NULL, NULL, P_APPS },
	  { "@assistant", N_("小幫手"), "pomelo-assistant", "pomelo-assistant --settings", 0 },
	  { "@notify", N_("通知"), NULL, NULL, P_NOTIFY },
	  { "@sound", N_("聲音"), "pomelo-sound", "pomelo-sound", 0 },
	  { "@camera", N_("視訊"), NULL, NULL, P_CAMERA } },
	{ { "@keyboard", N_("鍵盤"), NULL, NULL, P_KEYBOARD },
	  { "@mouse", N_("滑鼠與觸控板"), NULL, NULL, P_MOUSE } },
	{ { "@screentime", N_("螢幕使用時間"), NULL, NULL, P_SCREENTIME },
	  { "@users", N_("使用者與群組"), NULL, NULL, P_USERS },
	  { "@login", N_("登入畫面"), "pomelo-dm", NULL, P_LOGIN },
	  { "@keyring", N_("密碼與鑰匙圈"), "pomelo-keyring", "pomelo-keyring --settings", 0 },
	  { "@printer", N_("印表機與掃描器"), NULL, NULL, P_PRINTERS } },
	{ { "@network", N_("網路"), "pomelo-network", "pomelo-network", 0 },
	  { "@firewall", N_("防火牆"), "pomelo-firewall", "pomelo-firewall", 0 },
	  { "@bluetooth", N_("藍牙"), "pomelo-bluetooth", "pomelo-bluetooth", 0 },
	  { "@power", N_("電源與鎖定"), NULL, NULL, P_POWER },
	  { "@saver", N_("螢幕保護程式"), "pomelo-screensaver", "pomelo-screensaver --settings", 0 },
	  { "@control", N_("控制中心"), "pomelo-control-center", "pomelo-control-center", 0 } },
};

static void build_main(PmUI *u)
{
	pm_ui_title(u, _("系統設定"), NULL);
	for (unsigned g = 0; g < LEN(panes); g++) {
		pm_ui_group(u, NULL);
		for (int i = 0; i < 6 && panes[g][i].title; i++) {
			const Pane *p = &panes[g][i];
			int have = !p->bin || pm_have(p->bin);
			if (pm_ui_item(u, p->icon, _(p->title), NULL, have ? "›" : _("未安裝"), 0) && have) {
				if (p->cmd) pm_spawn(p->cmd);
				else { go(u, p->page); return; }
			}
		}
		pm_ui_end(u);
	}
}

/* ------------------------------------------------------------------ */
/* 一般                                                                */
/* ------------------------------------------------------------------ */

static int upd_count = -1, snap_upd = -1;   /* -1 = 還不知道 */
static char upd_list[40][160];
static int nupd_list;
static int upd_checked;

static void *updates_thread(void *arg)
{
	(void)arg;
	int n = 0, k = 0;
	char *t = pm_pkg_upgradable();   /* apt 或 dnf；每行「名稱<Tab>新版本」 */
	char *save = NULL;
	for (char *l = t ? strtok_r(t, "\n", &save) : NULL; l; l = strtok_r(NULL, "\n", &save)) {
		if (!strchr(l, '\t')) continue;
		if (k < (int)LEN(upd_list)) snprintf(upd_list[k++], sizeof upd_list[0], "%s", l);
		n++;
	}
	free(t);
	nupd_list = k;
	upd_count = n;
	upd_checked = 1;   /* 套件數先顯示，Snap 檢查比較慢 */
	if (pm_have("snap")) {
		t = pm_run("snap refresh --list");
		int s = 0;
		save = NULL;
		for (char *l = t ? strtok_r(t, "\n", &save) : NULL; l; l = strtok_r(NULL, "\n", &save))
			if (strncmp(l, "Name", 4) && strncmp(l, "All snaps", 9) && *l) s++;
		free(t);
		snap_upd = s;
	} else snap_upd = 0;
	upd_checked = 1;
	worker_busy = 0;
	return NULL;
}

static void refresh_updates(void) { start_worker(updates_thread, NULL); }

static unsigned long long root_size, root_avail;

static void read_root(void)
{
	struct statvfs vs;
	if (!statvfs("/", &vs)) {
		root_avail = (unsigned long long)vs.f_bavail * vs.f_frsize;
		root_size = (unsigned long long)(vs.f_blocks - vs.f_bfree) * vs.f_frsize + root_avail;
	}
}

static char tz[128];
static int ntp = -1;

static void read_time(void)
{
	char link[256];
	ssize_t k = readlink("/etc/localtime", link, sizeof link - 1);
	if (k > 0) {
		link[k] = 0;
		char *z = strstr(link, "zoneinfo/");
		snprintf(tz, sizeof tz, "%s", z ? z + 9 : link);
	}
	char *t = pm_run("timedatectl show -p NTP --value");
	ntp = t && *t ? !strncmp(t, "yes", 3) : -1;
	free(t);
}

static void build_general(PmUI *u)
{
	if (pm_ui_back(u, _("系統設定"))) { go(u, P_MAIN); return; }
	pm_ui_title(u, _("一般"), NULL);
	char right[96], a[32];

	pm_ui_group(u, NULL);
	if (pm_ui_item(u, "@about", _("關於"), _("這台電腦的名稱、作業系統與硬體"), "›", 0)) { go(u, P_ABOUT); return; }
	if (upd_count < 0) snprintf(right, sizeof right, "%s", "›");
	else if (upd_count + MAX(snap_upd, 0) > 0) snprintf(right, sizeof right, _("%d 個更新 ›"), upd_count + MAX(snap_upd, 0));
	else snprintf(right, sizeof right, "%s", _("已是最新 ›"));
	if (pm_ui_item(u, "@update", _("軟體更新"), _("系統與應用程式的更新"), right, 0)) { go(u, P_UPDATE); return; }
	human(a, sizeof a, root_avail);
	snprintf(right, sizeof right, _("可用 %s ›"), a);
	if (pm_ui_item(u, "@storage", _("儲存空間"), _("磁碟用量、外接裝置、清理"), right, 0)) { go(u, P_STORAGE); return; }
	pm_ui_end(u);

	pm_ui_group(u, NULL);
	if (pm_ui_item(u, "@datetime", _("日期與時間"), _("時區、自動校時、時鐘格式"), "›", 0)) { go(u, P_DATETIME); return; }
	int have_lang = pm_have("pomelo-language");
	if (pm_ui_item(u, "@language", _("語言與地區"), _("介面語言、日期格式、鍵盤與輸入法"),
	               have_lang ? "›" : _("未安裝"), 0) && have_lang)
		pm_spawn("pomelo-language");
	pm_ui_end(u);

	pm_ui_group(u, NULL);
	if (pm_ui_item(u, "@devices", _("裝置管理"), _("硬體、使用中的驅動程式、額外驅動程式"), "›", 0)) { go(u, P_DEVICES); return; }
	pm_ui_end(u);
}

/* ------------------------------------------------------------------ */
/* 關於                                                                */
/* ------------------------------------------------------------------ */

static char host[256], new_host[64], os[256], kernel[128], cpu[256], mem[64], model[192], gpu[256];

static void read_sysfs(const char *name, char *out, size_t n)
{
	char p[128];
	snprintf(p, sizeof p, "/sys/class/dmi/id/%s", name);
	FILE *f = fopen(p, "r");
	out[0] = 0;
	if (f) { if (fgets(out, n, f)) out[strcspn(out, "\n")] = 0; fclose(f); }
	/* 主機板廠商常留的佔位字串 */
	if (!strcmp(out, "To be filled by O.E.M.") || !strcmp(out, "System Product Name") || !strcmp(out, "Default string"))
		out[0] = 0;
}

static void read_info(void)
{
	gethostname(host, sizeof host);
	snprintf(new_host, sizeof new_host, "%s", host);
	FILE *f = fopen("/etc/os-release", "r");
	char line[512];
	while (f && fgets(line, sizeof line, f))
		if (!strncmp(line, "PRETTY_NAME=", 12)) {
			char *v = line + 12;
			v[strcspn(v, "\n")] = 0;
			if (*v == '"') { v++; v[strcspn(v, "\"")] = 0; }
			snprintf(os, sizeof os, "%s", v);
		}
	if (f) fclose(f);
	struct utsname un;
	if (!uname(&un)) snprintf(kernel, sizeof kernel, _("Linux %s（%s）"), un.release, un.machine);
	f = fopen("/proc/cpuinfo", "r");
	int cores = 0;
	while (f && fgets(line, sizeof line, f)) {
		if (!strncmp(line, "processor", 9)) cores++;
		if (!cpu[0] && !strncmp(line, "model name", 10)) {
			char *v = strchr(line, ':');
			if (v) { v += 2; v[strcspn(v, "\n")] = 0; snprintf(cpu, sizeof cpu, "%s", v); }
		}
	}
	if (f) fclose(f);
	if (cores) { size_t l = strlen(cpu); snprintf(cpu + l, sizeof cpu - l, _("（%d 核心）"), cores); }
	f = fopen("/proc/meminfo", "r");
	long total = 0;
	while (f && fgets(line, sizeof line, f)) sscanf(line, "MemTotal: %ld kB", &total);
	if (f) fclose(f);
	snprintf(mem, sizeof mem, "%.0f GB", total / 1048576.0 + 0.4);
	char vendor[96], product[96], board[96];
	read_sysfs("sys_vendor", vendor, sizeof vendor);
	read_sysfs("product_name", product, sizeof product);
	read_sysfs("board_name", board, sizeof board);
	if (product[0]) snprintf(model, sizeof model, "%s %s", vendor, product);
	else if (board[0]) snprintf(model, sizeof model, "%s %s", vendor, board);
	/* 顯示卡：lspci 的 VGA／3D 裝置 */
	char *t = pm_have("lspci") ? pm_run("lspci") : NULL;
	char *save = NULL;
	gpu[0] = 0;
	for (char *l = t ? strtok_r(t, "\n", &save) : NULL; l; l = strtok_r(NULL, "\n", &save)) {
		if (!strstr(l, "VGA compatible controller: ") && !strstr(l, "3D controller: ") && !strstr(l, "Display controller: ")) continue;
		char *v = strstr(l, ": ") + 2;
		size_t n = strlen(gpu);
		snprintf(gpu + n, sizeof gpu - n, "%s%s", n ? "\n" : "", v);
	}
	free(t);
	gpu[strcspn(gpu, "\n")] = 0;   /* 只顯示第一張 */
	read_root();
}

static int valid_hostname(const char *s)
{
	if (!*s || strlen(s) > 63 || *s == '-') return 0;
	for (const char *p = s; *p; p++) if (!isalnum((unsigned char)*p) && *p != '-') return 0;
	return 1;
}

static void hostname_done(int ok) { if (ok) gethostname(host, sizeof host); }

static void build_about(PmUI *u)
{
	if (pm_ui_back(u, _("一般"))) { go(u, P_GENERAL); return; }
	pm_ui_title(u, _("關於"), NULL);
	char a[64], b[96];
	pm_ui_group(u, NULL);
	if (pm_ui_entry(u, _("電腦名稱"), new_host, sizeof new_host, 0) && strcmp(new_host, host)) {
		if (!valid_hostname(new_host))
			snprintf(status, sizeof status, "%s", _("名稱只能用英文字母、數字和減號"));
		else {
			char *argv[] = { "hostnamectl", "set-hostname", new_host, NULL };
			run_cmd(argv, _("更改電腦名稱"), hostname_done);
		}
	}
	pm_ui_end(u);
	pm_ui_note(u, _("輸入新名稱後按 Enter；區域網路上的其他電腦會看到這個名稱。"));

	pm_ui_group(u, NULL);
	if (model[0]) pm_ui_info(u, _("型號"), model);
	pm_ui_info(u, _("處理器"), cpu);
	if (gpu[0]) pm_ui_info(u, _("顯示卡"), gpu);
	pm_ui_info(u, _("記憶體"), mem);
	human(a, sizeof a, root_size);
	human(b, sizeof b, root_avail);
	char d[160];
	snprintf(d, sizeof d, _("%s（可用 %s）"), a, b);
	pm_ui_info(u, _("磁碟"), d);
	pm_ui_end(u);

	pm_ui_group(u, NULL);
	pm_ui_info(u, _("作業系統"), os);
	pm_ui_info(u, _("核心"), kernel);
	pm_ui_info(u, _("桌面"), "Pomelo " POMELO_VERSION "（" POMELO_DATE "）");
	pm_ui_info(u, _("開發者"), "pake@fedora.tw");
	pm_ui_info(u, _("組織"), "https://fedora.tw");
	pm_ui_end(u);
	pm_ui_note(u, _("Pomelo 是自由軟體，以 GNU 通用公眾授權條款第 3 版（或更新的版本）授權：可以自由使用、修改、散布，不提供任何擔保。"));
}


/* ------------------------------------------------------------------ */
/* 關於這台電腦（選單列左上角 → 關於 Pomelo）：像 macOS 的小視窗，放在螢幕正中間  */
/* ------------------------------------------------------------------ */

/* 「Intel(R) Core(TM) i5-11400 CPU @ 2.60GHz（12 核心）」→「Intel Core i5-11400（12 核心）」 */
static void tidy_cpu(const char *in, char *out, size_t n)
{
	char t[256];
	snprintf(t, sizeof t, "%s", in);
	const char *junk[] = { "(R)", "(TM)", "(tm)", " CPU", " Processor", " with Radeon Graphics" };
	/* 「11th Gen Intel…」「12th Gen Intel…」：世代從型號就看得出來 */
	const char *gen = strstr(t, " Gen ");
	if (gen && gen - t <= 5 && isdigit((unsigned char)t[0])) memmove(t, gen + 5, strlen(gen + 5) + 1);
	for (unsigned i = 0; i < LEN(junk); i++)
		for (char *p; (p = strstr(t, junk[i])); ) memmove(p, p + strlen(junk[i]), strlen(p + strlen(junk[i])) + 1);
	char *at = strstr(t, " @ ");
	char *paren = strstr(t, "（");
	if (at) { char tail[96] = ""; if (paren) snprintf(tail, sizeof tail, "%s", paren); snprintf(at, sizeof t - (size_t)(at - t), "%s", tail); }
	/* 多個空白合成一個 */
	char *w = t;
	for (char *r = t; *r; r++) if (!(*r == ' ' && (w == t || w[-1] == ' '))) *w++ = *r;
	*w = 0;
	snprintf(out, n, "%s", t);
}

/* 「Intel Corporation RocketLake-S GT1 [UHD Graphics 730] (rev 04)」→「Intel UHD Graphics 730」 */
static void tidy_gpu(const char *in, char *out, size_t n)
{
	char vendor[64] = "";
	sscanf(in, "%63s", vendor);
	if (!strcmp(vendor, "Advanced")) snprintf(vendor, sizeof vendor, "AMD");
	const char *lb = strrchr(in, '['), *rb = lb ? strchr(lb, ']') : NULL;
	if (lb && rb && rb > lb + 1) snprintf(out, n, "%s %.*s", vendor, (int)(rb - lb - 1), lb + 1);
	else {
		snprintf(out, n, "%s", in);
		char *rev = strstr(out, " (rev");
		if (rev) *rev = 0;
	}
}

/* 「ASUSTeK COMPUTER INC. PRIME B560M-A」→「ASUS PRIME B560M-A」：廠商用常見的簡稱 */
static void tidy_model(const char *in, char *out, size_t n)
{
	static const struct { const char *from, *to; } names[] = {
		{ "ASUSTeK COMPUTER INC.", "ASUS" }, { "ASUSTeK Computer Inc.", "ASUS" }, { "LENOVO", "Lenovo" },
		{ "Dell Inc.", "Dell" }, { "Hewlett-Packard", "HP" }, { "HP Inc.", "HP" },
		{ "Micro-Star International Co., Ltd.", "MSI" }, { "Gigabyte Technology Co., Ltd.", "Gigabyte" },
		{ "Acer Inc.", "Acer" }, { "Apple Inc.", "Apple" }, { "Microsoft Corporation", "Microsoft" },
		{ "Samsung Electronics Co., Ltd.", "Samsung" }, { "TOSHIBA", "Toshiba" }, { "FUJITSU", "Fujitsu" },
		{ "Intel Corporation", "Intel" }, { "ASRock", "ASRock" }, { "innotek GmbH", "VirtualBox" }, { "QEMU", "QEMU" },
	};
	for (unsigned i = 0; i < LEN(names); i++) {
		size_t l = strlen(names[i].from);
		if (!strncmp(in, names[i].from, l)) { snprintf(out, n, "%s%s", names[i].to, in + l); goto dedup; }
	}
	snprintf(out, n, "%s", in);
	/* 其他廠商：去掉公司型態 */
	const char *suffix[] = { " Co., Ltd.", " Corporation", " Inc.", " INC.", " Ltd.", " GmbH" };
	for (unsigned i = 0; i < LEN(suffix); i++) {
		char *p = strstr(out, suffix[i]);
		if (p) memmove(p, p + strlen(suffix[i]), strlen(p + strlen(suffix[i])) + 1);
	}
dedup:
	/* 產品名稱本身就有廠商名（「ASUS ASUS D500TC」）：只留一次 */
	{
		size_t l1 = strcspn(out, " ");
		if (out[l1] == ' ' && !strncasecmp(out, out + l1 + 1, l1) && (out[2 * l1 + 1] == ' ' || out[2 * l1 + 1] == '_' || !out[2 * l1 + 1]))
			memmove(out, out + l1 + 1, strlen(out + l1 + 1) + 1);
	}
}

/* 一台螢幕，畫面是 Pomelo 的漸層桌布，中間一顆柚子 */
static void draw_computer(cairo_t *cr, double cx, double top)
{
	double w = 196, h = 124, x = cx - w / 2;
	/* 底座 */
	cairo_move_to(cr, cx - 22, top + h); cairo_line_to(cr, cx + 22, top + h);
	cairo_line_to(cr, cx + 30, top + h + 22); cairo_line_to(cr, cx - 30, top + h + 22);
	cairo_close_path(cr);
	pm_rgb(cr, pm_dark ? 0x8a8a8f : 0xc7c7cc);
	cairo_fill(cr);
	pm_rrect(cr, cx - 44, top + h + 20, 88, 6, 3, 3);
	pm_rgb(cr, pm_dark ? 0x6e6e73 : 0xaeaeb2);
	cairo_fill(cr);
	/* 外框與畫面 */
	pm_rrect(cr, x, top, w, h, 9, 9);
	pm_rgb(cr, 0x1d1d1f);
	cairo_fill(cr);
	double sx = x + 6, sy = top + 6, sw_ = w - 12, sh_ = h - 12;
	cairo_save(cr);
	pm_rrect(cr, sx, sy, sw_, sh_, 4, 4);
	cairo_clip(cr);
	cairo_pattern_t *g = cairo_pattern_create_linear(sx, sy, sx + sw_, sy + sh_);
	cairo_pattern_add_color_stop_rgb(g, 0.0, 0.20, 0.36, 0.66);
	cairo_pattern_add_color_stop_rgb(g, 0.5, 0.52, 0.42, 0.74);
	cairo_pattern_add_color_stop_rgb(g, 1.0, 0.98, 0.62, 0.48);
	cairo_set_source(cr, g);
	cairo_paint(cr);
	cairo_pattern_destroy(g);
	/* 小小的選單列與 Dock */
	cairo_rectangle(cr, sx, sy, sw_, 6);
	pm_rgba(cr, 0xffffff, 0.55);
	cairo_fill(cr);
	pm_rrect(cr, cx - 30, sy + sh_ - 12, 60, 8, 3, 3);
	pm_rgba(cr, 0xffffff, 0.45);
	cairo_fill(cr);
	/* 柚子 */
	double py = sy + sh_ / 2 - 2, r = 22;
	cairo_pattern_t *f = cairo_pattern_create_radial(cx - 7, py - 8, 3, cx, py, r);
	cairo_pattern_add_color_stop_rgb(f, 0, 1.0, 0.93, 0.55);
	cairo_pattern_add_color_stop_rgb(f, 1, 0.93, 0.70, 0.12);
	cairo_arc(cr, cx, py, r, 0, 2 * M_PI);
	cairo_set_source(cr, f);
	cairo_fill(cr);
	cairo_pattern_destroy(f);
	cairo_save(cr);
	cairo_translate(cr, cx + 6, py - r - 2);
	cairo_rotate(cr, -0.5);
	cairo_scale(cr, 1, 0.5);
	cairo_arc(cr, 0, 0, 9, 0, 2 * M_PI);
	cairo_restore(cr);
	cairo_set_source_rgb(cr, 0.30, 0.66, 0.30);
	cairo_fill(cr);
	cairo_restore(cr);
}

static int about_xerror(Display *d, XErrorEvent *e) { (void)d; (void)e; return 0; }

static int about_window(void)
{
	/* Wayland 工作階段（POMELO_BACKEND=x11 可強制走 X）：原生小視窗，不開 X 連線 */
	PmWl *wl = NULL;
	int native = pmwl_connect();
	if (native) pm_ui_native = 1;
	else if (!(dpy = XOpenDisplay(NULL))) return 1;
	if (!native) XSetErrorHandler(about_xerror);
	pm_init(pm_conf_get("appearance.font", "Noto Sans CJK TC"), 13, pm_conf_int("appearance.dark", 0),
	        pm_conf_get("appearance.icon_theme", "Yaru"));
	pm_set_theme(pm_dark, pm_conf_color("appearance.accent", 0));
	if (native) {
		/* 已經開著就叫到前面（視窗清單：foreign-toplevel） */
		if (pmwl_toplevels_start()) {
			PmwEvent e0;
			pmwl_next(&e0, 150);
			PmwToplevel t[64];
			int n = pmwl_toplevels(t, 64);
			for (int i = 0; i < n; i++)
				if (!strcmp(t[i].app_id, "pomelo-about")) { pmwl_toplevel_do(t[i].id, PMWL_TL_ACTIVATE); return 0; }
		}
	} else pm_update_monitors();
	/* 已經開著就叫到前面 */
	Atom a_list = None, a_act = None;
	Atom type; int fmt; unsigned long nw = 0, after;
	unsigned char *pl = NULL;
	if (!native) { a_list = XInternAtom(dpy, "_NET_CLIENT_LIST", False); a_act = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False); }
	if (!native && XGetWindowProperty(dpy, pm_root, a_list, 0, 1024, False, XA_WINDOW, &type, &fmt, &nw, &after, &pl) == Success && pl) {
		Window *ws = (Window *)pl;
		for (unsigned long i = 0; i < nw; i++) {
			XClassHint ch = { 0 };
			if (XGetClassHint(dpy, ws[i], &ch)) {
				int me = ch.res_name && !strcmp(ch.res_name, "pomelo-about");
				if (ch.res_name) XFree(ch.res_name);
				if (ch.res_class) XFree(ch.res_class);
				if (me) { pm_send(pm_root, ws[i], a_act, 2, CurrentTime, 0); XFlush(dpy); XFree(pl); return 0; }
			}
		}
		XFree(pl);
	}
	read_info();
	char cpu2[256], gpu2[256], disk[160], a[64], b[64], ver[64], model2[192];
	tidy_model(model, model2, sizeof model2);
	tidy_cpu(cpu, cpu2, sizeof cpu2);
	tidy_gpu(gpu, gpu2, sizeof gpu2);
	human(a, sizeof a, root_size);
	human(b, sizeof b, root_avail);
	snprintf(disk, sizeof disk, _("%s（可用 %s）"), a, b);
	snprintf(ver, sizeof ver, "Pomelo %s（%s）", POMELO_VERSION, POMELO_DATE);
	const char *title = model2[0] ? model2 : host;
	const char *sub = model[0] ? host : os;
	struct { const char *k, *v; } rows[] = {
		{ N_("處理器"), cpu2 }, { N_("顯示卡"), gpu2 }, { N_("記憶體"), mem }, { N_("磁碟"), disk },
		{ N_("作業系統"), os }, { N_("核心"), kernel }, { N_("桌面"), ver },
	};

	int W = 420, H = 0, row_h = 26, nrows = 0;
	for (unsigned i = 0; i < LEN(rows); i++) if (rows[i].v[0]) nrows++;
	H = 262 + nrows * row_h + 96;
	Rect m = native ? (Rect){ 0, 0, 1280, 720 } : mons[pmon];
	int x = m.x + (m.w - W) / 2, y = m.y + (m.h - H) / 2 + 14;
	Window w = 0;
	Atom del = 0;
	if (native) {
		wl = pmwl_window(_("關於這台電腦"), "pomelo-about", W, H, W, H, NULL);
		if (!wl) return 1;
		pmwl_set_max_size(wl, W, H);   /* 固定大小 */
	} else {
	XSetWindowAttributes wa = { .background_pixel = pm_dark ? 0x1e1e1e : 0xf5f5f7,
	                            .event_mask = ExposureMask | KeyPressMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | LeaveWindowMask };
	w = XCreateWindow(dpy, pm_root, x, y, (unsigned)W, (unsigned)H, 0, CopyFromParent, InputOutput, CopyFromParent,
	                         CWBackPixel | CWEventMask, &wa);
	XSizeHints *sz = XAllocSizeHints();
	sz->flags = USPosition | PMinSize | PMaxSize;
	sz->x = x; sz->y = y;
	sz->min_width = sz->max_width = W;
	sz->min_height = sz->max_height = H;
	XSetWMNormalHints(dpy, w, sz);
	XFree(sz);
	XClassHint ch = { "pomelo-about", "pomelo-system" };
	XSetClassHint(dpy, w, &ch);
	const char *wt = _("關於這台電腦");
	XStoreName(dpy, w, wt);
	XChangeProperty(dpy, w, XInternAtom(dpy, "_NET_WM_NAME", False), XInternAtom(dpy, "UTF8_STRING", False), 8,
	                PropModeReplace, (const unsigned char *)wt, (int)strlen(wt));
	Atom wtype = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False), wdlg = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DIALOG", False);
	XChangeProperty(dpy, w, wtype, XA_ATOM, 32, PropModeReplace, (unsigned char *)&wdlg, 1);
	del = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
	XSetWMProtocols(dpy, w, &del, 1);
	XMapRaised(dpy, w);
	}

	int hover = 0, press = 0;
	double bx = W / 2.0 - 75, by = H - 78, bw_ = 150, bh = 30;
	for (;;) {
		Paint p;
		cairo_t *cr = wl ? pmwl_begin(wl, &W, &H) : pm_paint_begin(&p, w, W, H);
		pm_rgb(cr, pm_dark ? 0x1e1e1e : 0xf5f5f7);
		cairo_paint(cr);
		unsigned fg = pm_dark ? 0xf5f5f7 : 0x1d1d1f, dim = pm_dark ? 0x98989d : 0x6e6e73;
		draw_computer(cr, W / 2.0, 34);
		char el[256];
		/* 名稱太長就把字縮小一點，真的放不下才截斷 */
		double ts = 22;
		pm_font(cr, ts, 1);
		while (ts > 15 && pm_textw(cr, title) > W - 40) pm_font(cr, --ts, 1);
		pm_rgb(cr, fg);
		pm_ellipsize(cr, title, W - 40, el, sizeof el);
		pm_text(cr, W / 2.0 - pm_textw(cr, el) / 2, 212, el);
		pm_font(cr, 13, 0);
		pm_rgb(cr, dim);
		pm_ellipsize(cr, sub, W - 40, el, sizeof el);
		pm_text(cr, W / 2.0 - pm_textw(cr, el) / 2, 238, el);
		double ry = 276, mid = W * 0.33;
		for (unsigned i = 0; i < LEN(rows); i++) {
			if (!rows[i].v[0]) continue;
			pm_font(cr, 12.5, 0);
			pm_rgb(cr, dim);
			const char *k = _(rows[i].k);
			pm_text(cr, mid - pm_textw(cr, k), ry, k);
			pm_font(cr, 12.5, 0);
			pm_rgb(cr, fg);
			pm_ellipsize(cr, rows[i].v, W - mid - 32, el, sizeof el);
			pm_text(cr, mid + 10, ry, el);
			ry += row_h;
		}
		pm_rrect(cr, bx, by, bw_, bh, 7, 7);
		pm_rgb(cr, press ? (pm_dark ? 0x5a5a5e : 0xd1d1d6) : hover ? (pm_dark ? 0x4a4a4e : 0xe5e5ea) : (pm_dark ? 0x3a3a3c : 0xffffff));
		cairo_fill_preserve(cr);
		pm_rgba(cr, 0x000000, pm_dark ? 0.5 : 0.15);
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
		const char *more = _("更多資訊…");
		pm_font(cr, 13, 0);
		pm_rgb(cr, fg);
		pm_text(cr, bx + bw_ / 2 - pm_textw(cr, more) / 2, by + bh / 2, more);
		pm_font(cr, 11, 0);
		pm_rgb(cr, dim);
		const char *foot = _("Pomelo：輕量、省資源的桌面");
		pm_text(cr, W / 2.0 - pm_textw(cr, foot) / 2, H - 24, foot);
		XEvent ev;
		if (wl) {
			pmwl_end(wl, cr);
			PmwEvent pe;
			do {
				if (!pmwl_next(&pe, -1)) continue;
			} while (!pmwl_to_xevent(&pe, &ev));
		} else {
		pm_paint_end(&p, cr, 0, 0);
		XFlush(dpy);

		XNextEvent(dpy, &ev);
		while (ev.type == MotionNotify && XCheckTypedWindowEvent(dpy, w, MotionNotify, &ev)) ;
		}
		int in_btn = 0;
		if (ev.type == MotionNotify || ev.type == ButtonPress || ev.type == ButtonRelease) {
			int ex = ev.type == MotionNotify ? ev.xmotion.x : ev.xbutton.x, ey = ev.type == MotionNotify ? ev.xmotion.y : ev.xbutton.y;
			in_btn = ex >= bx && ex < bx + bw_ && ey >= by && ey < by + bh;
		}
		if (ev.type == MotionNotify) hover = in_btn;
		else if (ev.type == LeaveNotify) hover = press = 0;
		else if (ev.type == ButtonPress && ev.xbutton.button == Button1) press = in_btn;
		else if (ev.type == ButtonRelease && ev.xbutton.button == Button1) {
			if (press && in_btn) { pm_spawn("pomelo-system --page about"); break; }
			press = 0;
		} else if (ev.type == KeyPress) {
			KeySym ks;
			if (pmwl_xkey(&ev.xkey, NULL, 0, NULL, &ks) < 0) ks = XLookupKeysym(&ev.xkey, 0);
			if (ks == XK_Escape || ((ev.xkey.state & (Mod4Mask | ControlMask)) && (ks == XK_w || ks == XK_q))) break;
		} else if (ev.type == ClientMessage && (wl ? ev.xclient.data.l[0] == PMWL_XCLOSE : (Atom)ev.xclient.data.l[0] == del)) break;
	}
	if (wl) { pmwl_destroy(wl); return 0; }
	XDestroyWindow(dpy, w);
	XCloseDisplay(dpy);
	return 0;
}

/* ------------------------------------------------------------------ */
/* 軟體更新                                                            */
/* ------------------------------------------------------------------ */

static int auto_check = -1, auto_install = -1, have_unattended;

static void read_auto(void)
{
	auto_check = auto_install = 0;
	FILE *f = fopen("/etc/apt/apt.conf.d/20auto-upgrades", "r");
	char line[256];
	while (f && fgets(line, sizeof line, f)) {
		if (strstr(line, "Update-Package-Lists") && strstr(line, "\"1\"")) auto_check = 1;
		if (strstr(line, "Unattended-Upgrade") && strstr(line, "\"1\"")) auto_install = 1;
	}
	if (f) fclose(f);
	have_unattended = !access("/usr/bin/unattended-upgrade", X_OK);
}

static void after_update(int ok) { (void)ok; read_auto(); upd_count = -1; refresh_updates(); }

static void write_auto(int check, int install)
{
	static char script[256];
	snprintf(script, sizeof script,
	         "printf 'APT::Periodic::Update-Package-Lists \"%d\";\\nAPT::Periodic::Unattended-Upgrade \"%d\";\\n' "
	         "> /etc/apt/apt.conf.d/20auto-upgrades", check, install);
	char *argv[] = { "sh", "-c", script, NULL };
	run_admin(argv, _("更改自動更新設定"), after_update);
}

static void build_update(PmUI *u)
{
	if (pm_ui_back(u, _("一般"))) { go(u, P_GENERAL); return; }
	pm_ui_title(u, _("軟體更新"), NULL);
	char t[256], last[64];
	int apt = pm_pkg_kind() == PM_PKG_APT;
	file_time(apt ? "/var/lib/apt/periodic/update-success-stamp" : "/var/cache/dnf/last_makecache", last, sizeof last);

	pm_ui_group(u, NULL);
	if (!upd_checked) pm_ui_info(u, _("正在檢查…"), NULL);
	else {
		int total = upd_count + MAX(snap_upd, 0);
		if (total == 0) snprintf(t, sizeof t, "%s", _("你的系統已是最新版本"));
		else if (snap_upd > 0) snprintf(t, sizeof t, _("有 %d 個套件、%d 個 Snap 可以更新"), upd_count, snap_upd);
		else snprintf(t, sizeof t, _("有 %d 個套件可以更新"), upd_count);
		pm_ui_info(u, t, NULL);
	}
	pm_ui_info(u, _("上次檢查"), last);
	if (!job) {
		if (pm_ui_button(u, _("從伺服器取得最新的套件清單"), _("檢查更新"))) {
			static char *argv[8];
			if (pm_pkg_argv("refresh", NULL, 0, argv, 8)) run_admin(argv, _("檢查更新"), after_update);
		}
		if (upd_checked && upd_count + MAX(snap_upd, 0) > 0 && pm_ui_button(u, _("安裝所有更新"), _("立即更新"))) {
			if (snap_upd > 0) {
				static char script[64];
				snprintf(script, sizeof script, "%s -y upgrade; snap refresh", apt ? "apt-get" : "dnf");
				char *argv[] = { "sh", "-c", script, NULL };
				run_admin(argv, _("安裝更新"), after_update);
			} else {
				static char *argv[8];
				if (pm_pkg_argv("upgrade", NULL, 0, argv, 8)) run_admin(argv, _("安裝更新"), after_update);
			}
		}
	}
	pm_ui_end(u);
	if (job) {
		snprintf(t, sizeof t, _("正在%s，請稍候…"), job_title);
		pm_ui_note(u, t);
	}

	if (nupd_list) {
		snprintf(t, sizeof t, _("可以更新的套件（%d）"), upd_count);
		pm_ui_group(u, t);
		for (int i = 0; i < nupd_list && i < 12; i++) {
			char name[160], *tab = strchr(upd_list[i], '\t');
			snprintf(name, sizeof name, "%.*s", tab ? (int)(tab - upd_list[i]) : (int)strlen(upd_list[i]), upd_list[i]);
			pm_ui_info(u, name, tab ? tab + 1 : NULL);
		}
		if (upd_count > 12) {
			snprintf(t, sizeof t, _("還有 %d 個…"), upd_count - 12);
			pm_ui_info(u, t, NULL);
		}
		pm_ui_end(u);
	}

	if (apt) {   /* 自動更新的設定是 apt 的（APT::Periodic）；dnf 的系統用 dnf-automatic */
		pm_ui_group(u, _("自動更新"));
		int c = auto_check, in = auto_install;
		if (pm_ui_switch(u, _("每天自動檢查更新"), &c) && !job) write_auto(c, c ? MAX(in, 0) : 0);
		if (have_unattended && pm_ui_switch(u, _("自動安裝安全性更新"), &in) && !job) write_auto(MAX(c, in), in);
		pm_ui_end(u);
	} else pm_ui_note(u, _("想要自動更新：安裝 dnf-automatic，再啟用 dnf-automatic.timer（或 dnf5-automatic.timer）。"));
	if (pm_have("pomelo-software")) {
		pm_ui_group(u, NULL);
		if (pm_ui_item(u, "pomelo-software", _("在軟體中心查看更新…"), NULL, "›", 0)) pm_spawn("pomelo-software --updates");
		pm_ui_end(u);
	}
}

/* ------------------------------------------------------------------ */
/* 儲存空間（摘要，完整功能在 pomelo-storage）                          */
/* ------------------------------------------------------------------ */

static void build_storage(PmUI *u)
{
	if (pm_ui_back(u, _("一般"))) { go(u, P_GENERAL); return; }
	pm_ui_title(u, _("儲存空間"), NULL);
	char a[32], b[32], c[96];
	pm_ui_group(u, NULL);
	human(a, sizeof a, root_size - root_avail);
	human(b, sizeof b, root_avail);
	snprintf(c, sizeof c, _("已用 %s，剩 %s"), a, b);
	double fr = root_size ? (double)(root_size - root_avail) / root_size : 0;
	pm_ui_meter(u, _("系統磁碟"), c, fr, fr > 0.9 ? 0xff453a : fr > 0.75 ? 0xff9f0a : 0x0a84ff);
	struct statvfs vs;
	struct stat sr, sh;
	const char *home = getenv("HOME");
	/* /home 在另一顆磁碟上就也顯示 */
	if (home && !stat("/", &sr) && !stat(home, &sh) && sr.st_dev != sh.st_dev && !statvfs(home, &vs)) {
		unsigned long long av = (unsigned long long)vs.f_bavail * vs.f_frsize;
		unsigned long long used = (unsigned long long)(vs.f_blocks - vs.f_bfree) * vs.f_frsize;
		human(a, sizeof a, used);
		human(b, sizeof b, av);
		snprintf(c, sizeof c, _("已用 %s，剩 %s"), a, b);
		double f2 = used + av ? (double)used / (used + av) : 0;
		pm_ui_meter(u, _("家目錄"), c, f2, f2 > 0.9 ? 0xff453a : f2 > 0.75 ? 0xff9f0a : 0x0a84ff);
	}
	pm_ui_end(u);
	int have = pm_have("pomelo-storage");
	if (!have) {
		pm_ui_note(u, _("安裝 pomelo-storage 可以管理外接裝置、清理與分析磁碟空間。"));
		return;
	}
	pm_ui_group(u, NULL);
	if (pm_ui_item(u, "user-trash", _("清理"), _("垃圾桶、快取、舊套件、系統日誌"), "›", 0)) pm_spawn("pomelo-storage --cleanup");
	if (pm_ui_item(u, "folder", _("分析"), _("找出哪些資料夾佔空間"), "›", 0)) pm_spawn("pomelo-storage --analyze");
	if (pm_ui_item(u, "drive-removable-media", _("外接裝置"), _("掛載、退出、格式化"), "›", 0)) pm_spawn("pomelo-storage --devices");
	if (pm_ui_item(u, "drive-harddisk", _("硬碟健康狀態"), _("各磁碟用量與 SMART 狀態"), "›", 0)) pm_spawn("pomelo-storage");
	pm_ui_end(u);
}

/* ------------------------------------------------------------------ */
/* 日期與時間                                                          */
/* ------------------------------------------------------------------ */

static const char *clock_fmts[] = {
	N_("%-m月%-d日 %A  %H:%M"), N_("%-m月%-d日 %A  %p %-I:%M"), N_("%-m月%-d日 %H:%M"), "%A %H:%M", "%H:%M", "%Y/%m/%d %H:%M",
};
static char tz_query[64], set_date[16], set_clock[8];
static char *zones;          /* timedatectl list-timezones 的結果（換行分隔） */

static void time_done(int ok) { (void)ok; read_time(); }

static void build_datetime(PmUI *u)
{
	if (pm_ui_back(u, _("一般"))) { go(u, P_GENERAL); return; }
	pm_ui_title(u, _("日期與時間"), NULL);
	char now[96];
	time_t t = time(NULL);
	strftime(now, sizeof now, _("%Y年%-m月%-d日 %A %H:%M"), localtime(&t));

	pm_ui_group(u, NULL);
	pm_ui_info(u, _("目前時間"), now);
	int n = ntp;
	if (ntp >= 0 && pm_ui_switch(u, _("自動設定日期與時間"), &n) && !job) {
		char *argv[] = { "timedatectl", "set-ntp", n ? "true" : "false", NULL };
		run_cmd(argv, _("更改自動校時"), time_done);
	}
	pm_ui_end(u);

	if (ntp == 0) {
		/* 手動設定 */
		if (!set_date[0]) strftime(set_date, sizeof set_date, "%Y-%m-%d", localtime(&t));
		if (!set_clock[0]) strftime(set_clock, sizeof set_clock, "%H:%M", localtime(&t));
		pm_ui_group(u, _("手動設定"));
		pm_ui_entry(u, _("日期（年-月-日）"), set_date, sizeof set_date, 0);
		pm_ui_entry(u, _("時間（時:分）"), set_clock, sizeof set_clock, 0);
		if (pm_ui_button(u, NULL, _("設定")) && !job) {
			int y, mo, d, h, mi;
			if (sscanf(set_date, "%d-%d-%d", &y, &mo, &d) == 3 && sscanf(set_clock, "%d:%d", &h, &mi) == 2 &&
			    y >= 2000 && y < 2100 && mo >= 1 && mo <= 12 && d >= 1 && d <= 31 && h >= 0 && h < 24 && mi >= 0 && mi < 60) {
				static char ts[32];
				snprintf(ts, sizeof ts, "%04d-%02d-%02d %02d:%02d:00", y, mo, d, h, mi);
				char *argv[] = { "timedatectl", "set-time", ts, NULL };
				run_cmd(argv, _("設定時間"), time_done);
			} else snprintf(status, sizeof status, "%s", _("日期或時間格式不正確"));
		}
		pm_ui_end(u);
	}

	pm_ui_group(u, _("時區"));
	pm_ui_info(u, _("目前時區"), tz);
	pm_ui_entry(u, _("搜尋城市"), tz_query, sizeof tz_query, 0);
	if (strlen(tz_query) >= 2) {
		if (!zones) zones = pm_run("timedatectl list-timezones");
		int shown = 0;
		for (char *l = zones; l && *l && shown < 8;) {
			char *e = strchr(l, '\n');
			int len = e ? (int)(e - l) : (int)strlen(l);
			char z[96];
			snprintf(z, sizeof z, "%.*s", len, l);
			/* 比對城市名稱（底線當空白），不分大小寫 */
			char zz[96];
			snprintf(zz, sizeof zz, "%s", z);
			for (char *c = zz; *c; c++) if (*c == '_') *c = ' ';
			if (strcasestr(zz, tz_query)) {
				shown++;
				if (pm_ui_item(u, NULL, zz, NULL, !strcmp(z, tz) ? NULL : _("選擇"), !strcmp(z, tz)) && strcmp(z, tz) && !job) {
					static char zone[96];
					snprintf(zone, sizeof zone, "%s", z);
					char *argv[] = { "timedatectl", "set-timezone", zone, NULL };
					run_cmd(argv, _("更改時區"), time_done);
					tz_query[0] = 0;
				}
			}
			l = e ? e + 1 : NULL;
		}
		if (!shown) pm_ui_info(u, _("找不到符合的時區"), NULL);
	}
	pm_ui_end(u);
	pm_ui_note(u, _("用英文輸入城市名稱，例如 Taipei、Tokyo、New York。"));

	/* 選單列時鐘格式（跟外觀設定裡的是同一個設定） */
	static char examples[LEN(clock_fmts)][64];
	static const char *ex_ptr[LEN(clock_fmts)];
	for (unsigned i = 0; i < LEN(clock_fmts); i++) {
		strftime(examples[i], sizeof examples[i], _(clock_fmts[i]), localtime(&t));
		ex_ptr[i] = examples[i];
	}
	const char *cf = pm_conf_get("bar.clock_format", _(clock_fmts[0]));
	int sel = 0;
	for (unsigned i = 0; i < LEN(clock_fmts); i++) if (!strcmp(_(clock_fmts[i]), cf)) sel = (int)i;
	pm_ui_group(u, _("選單列"));
	if (pm_ui_choice(u, _("時鐘格式"), ex_ptr, LEN(clock_fmts), &sel)) pm_conf_set("bar.clock_format", _(clock_fmts[sel]));
	pm_ui_end(u);
}

/* ------------------------------------------------------------------ */
/* 裝置管理                                                            */
/* ------------------------------------------------------------------ */

enum { K_GPU, K_AUDIO, K_NET, K_STORAGE, K_USBCTL, K_INPUT, K_CAMERA, K_BT, K_USB, K_OTHER, K_N };
static const char *kind_names[K_N] = {
	N_("顯示卡"), N_("音效"), N_("網路"), N_("儲存控制器"), N_("USB 控制器"), N_("鍵盤與滑鼠"), N_("相機"), N_("藍牙"),
	N_("USB 裝置"), N_("其他系統裝置"),
};
static const char *kind_icons[K_N] = {
	"video-display", "audio-card", "network-wired", "drive-harddisk", "drive-removable-media", "input-keyboard",
	"camera-web", "bluetooth", "drive-removable-media", "computer",
};
typedef struct { int kind; char name[160], vendor[96], driver[64]; int need_driver; } Dev;
static Dev devs[256];
static int ndevs;
static int show_other;

static void add_dev(int kind, const char *name, const char *vendor, const char *driver, int need)
{
	if (ndevs >= (int)LEN(devs)) return;
	Dev *d = &devs[ndevs++];
	d->kind = kind;
	snprintf(d->name, sizeof d->name, "%s", name);
	snprintf(d->vendor, sizeof d->vendor, "%s", vendor ? vendor : "");
	snprintf(d->driver, sizeof d->driver, "%s", driver ? driver : "");
	d->need_driver = need;
}

static int pci_kind(const char *cls)
{
	if (strstr(cls, "VGA") || strstr(cls, "3D controller") || strstr(cls, "Display controller")) return K_GPU;
	if (strstr(cls, "Audio") || strstr(cls, "audio")) return K_AUDIO;
	if (strstr(cls, "Network") || strstr(cls, "Ethernet")) return K_NET;
	if (strstr(cls, "SATA") || strstr(cls, "Non-Volatile") || strstr(cls, "RAID") || strstr(cls, "IDE") || strstr(cls, "SCSI"))
		return K_STORAGE;
	if (strstr(cls, "USB controller")) return K_USBCTL;
	return K_OTHER;
}

static void read_devices(void)
{
	ndevs = 0;
	/* PCI：lspci -vmmk 每個裝置一段，空行分隔 */
	char *t = pm_have("lspci") ? pm_run("lspci -vmmk") : NULL;
	char cls[128] = "", vendor[96] = "", device[160] = "", driver[64] = "";
	int have = 0;
	for (char *l = t; l;) {
		char *e = strchr(l, '\n');
		if (e) *e = 0;
		if (!*l) {
			if (have) {
				int k = pci_kind(cls);
				add_dev(k, device, vendor, driver, k == K_GPU || k == K_NET || k == K_AUDIO);
			}
			have = 0;
			cls[0] = vendor[0] = device[0] = driver[0] = 0;
		} else if (!strncmp(l, "Class:\t", 7)) { snprintf(cls, sizeof cls, "%s", l + 7); have = 1; }
		else if (!strncmp(l, "Vendor:\t", 8)) snprintf(vendor, sizeof vendor, "%s", l + 8);
		else if (!strncmp(l, "Device:\t", 8)) snprintf(device, sizeof device, "%s", l + 8);
		else if (!strncmp(l, "Driver:\t", 8)) snprintf(driver, sizeof driver, "%s", l + 8);
		l = e ? e + 1 : NULL;
	}
	if (have) add_dev(pci_kind(cls), device, vendor, driver, 0);
	free(t);

	/* 輸入裝置：/proc/bus/input/devices，有 kbd 或 mouse 處理器的 */
	FILE *f = fopen("/proc/bus/input/devices", "r");
	char line[512], name[160] = "";
	while (f && fgets(line, sizeof line, f)) {
		if (!strncmp(line, "N: Name=", 8)) {
			char *v = line + 8;
			v[strcspn(v, "\n")] = 0;
			if (*v == '"') { v++; v[strcspn(v, "\"")] = 0; }
			snprintf(name, sizeof name, "%s", v);
		} else if (!strncmp(line, "H: Handlers=", 12) && name[0]) {
			int kbd = strstr(line, "kbd") != NULL, mouse = strstr(line, "mouse") != NULL;
			/* 電源鍵、視訊鍵等虛擬鍵盤不列 */
			int virt = strstr(name, "Power Button") || strstr(name, "Video Bus") || strstr(name, "Sleep Button") ||
			           strstr(name, "PC Speaker") || strstr(name, "HDA ") || strstr(name, "WMI") || strstr(name, "Lid Switch");
			if ((kbd || mouse) && !virt && strstr(line, "event")) add_dev(K_INPUT, name, NULL, NULL, 0);
			name[0] = 0;
		}
	}
	if (f) fclose(f);

	/* 相機 */
	DIR *d = opendir("/sys/class/video4linux");
	struct dirent *de;
	while (d && (de = readdir(d))) {
		if (de->d_name[0] == '.') continue;
		char p[PATH_MAX], n[160] = "";
		snprintf(p, sizeof p, "/sys/class/video4linux/%s/name", de->d_name);
		FILE *g = fopen(p, "r");
		if (g) { if (fgets(n, sizeof n, g)) n[strcspn(n, "\n")] = 0; fclose(g); }
		/* 一台相機常有好幾個節點（影像、中繼資料），名稱一樣的只列一次 */
		int dup = 0;
		for (int i = 0; i < ndevs; i++) if (devs[i].kind == K_CAMERA && !strcmp(devs[i].name, n)) dup = 1;
		if (n[0] && !dup) add_dev(K_CAMERA, n, NULL, "uvcvideo", 0);
	}
	if (d) closedir(d);

	/* 藍牙控制器 */
	d = opendir("/sys/class/bluetooth");
	while (d && (de = readdir(d)))
		if (!strncmp(de->d_name, "hci", 3) && !strchr(de->d_name, ':')) add_dev(K_BT, de->d_name, NULL, "btusb", 0);
	if (d) closedir(d);

	/* USB 裝置（不含根集線器）：lsusb 的「ID 1234:5678 廠商 產品」 */
	t = pm_have("lsusb") ? pm_run("lsusb") : NULL;
	char *save = NULL;
	for (char *l = t ? strtok_r(t, "\n", &save) : NULL; l; l = strtok_r(NULL, "\n", &save)) {
		char *id = strstr(l, "ID ");
		if (!id || strstr(l, "root hub")) continue;
		char *desc = strchr(id + 3, ' ');
		if (!desc || !desc[1]) continue;
		add_dev(K_USB, desc + 1, NULL, NULL, 0);
	}
	free(t);
}

/* 額外驅動程式（ubuntu-drivers） */
static char drivers[16][96];
static int ndrivers = -1, drivers_rec = -1;
static void *drivers_thread(void *arg)
{
	(void)arg;
	int n = 0;
	char *t = pm_run("ubuntu-drivers list");
	char *save = NULL;
	for (char *l = t ? strtok_r(t, "\n", &save) : NULL; l && n < (int)LEN(drivers); l = strtok_r(NULL, "\n", &save)) {
		/* 「nvidia-driver-535, (kernel modules provided by …)」→ 套件名稱 */
		char pkg[96];
		if (sscanf(l, "%95[a-zA-Z0-9.+-]", pkg) != 1 || strlen(pkg) < 3) continue;
		snprintf(drivers[n++], sizeof drivers[0], "%s", pkg);
	}
	free(t);
	t = pm_run("ubuntu-drivers devices");
	drivers_rec = -1;
	save = NULL;
	for (char *l = t ? strtok_r(t, "\n", &save) : NULL; l; l = strtok_r(NULL, "\n", &save))
		if (strstr(l, "recommended"))
			for (int i = 0; i < n; i++) if (strstr(l, drivers[i])) drivers_rec = i;
	free(t);
	ndrivers = n;
	worker_busy = 0;
	return NULL;
}

static void drivers_done(int ok)
{
	if (ok) snprintf(status, sizeof status, "%s", _("驅動程式已安裝，重新開機後生效"));
	read_devices();
}

static void build_devices(PmUI *u)
{
	if (pm_ui_back(u, _("一般"))) { go(u, P_GENERAL); return; }
	pm_ui_title(u, _("裝置管理"), NULL);
	int missing = 0;
	for (int k = 0; k < K_N; k++) {
		int cnt = 0;
		for (int i = 0; i < ndevs; i++) if (devs[i].kind == k) cnt++;
		if (!cnt) continue;
		char cap[96];
		snprintf(cap, sizeof cap, "%s（%d）", _(kind_names[k]), cnt);
		if (k == K_OTHER) {
			pm_ui_group(u, NULL);
			if (pm_ui_item(u, kind_icons[k], cap, _("晶片組、橋接器、感應器等"), show_other ? "⌄" : "›", 0)) {
				show_other = !show_other;
				u->again = 1;
			}
			pm_ui_end(u);
			if (!show_other) continue;
		}
		pm_ui_group(u, k == K_OTHER ? NULL : cap);
		for (int i = 0; i < ndevs; i++) {
			Dev *d = &devs[i];
			if (d->kind != k) continue;
			char sub[256], right[96];
			snprintf(sub, sizeof sub, "%s", d->vendor);
			if (d->driver[0]) snprintf(right, sizeof right, _("驅動：%s"), d->driver);
			else if (d->need_driver) { snprintf(right, sizeof right, "⚠ %s", _("沒有驅動程式")); missing++; }
			else right[0] = 0;
			pm_ui_item(u, kind_icons[k], d->name, sub[0] ? sub : NULL, right[0] ? right : NULL, 0);
		}
		pm_ui_end(u);
	}
	if (!ndevs) pm_ui_note(u, _("讀不到硬體資訊（需要 pciutils 與 usbutils）。"));
	if (missing) pm_ui_note(u, _("有裝置沒有驅動程式，可以在下方檢查是否有可安裝的驅動程式。"));

	/* 額外驅動程式：NVIDIA、Broadcom Wi-Fi 等（ubuntu-drivers，只有 Ubuntu 有） */
	if (pm_pkg_kind() != PM_PKG_APT) goto no_drivers;
	pm_ui_group(u, _("額外驅動程式"));
	if (!pm_have("ubuntu-drivers")) {
		if (!job && pm_ui_button(u, _("需要 ubuntu-drivers-common 才能檢查"), _("安裝"))) {
			char *argv[] = { "apt-get", "install", "-y", "--no-install-recommends", "ubuntu-drivers-common", NULL };
			run_admin(argv, _("安裝驅動程式工具"), NULL);
		}
	} else if (ndrivers < 0) {
		if (worker_started) pm_ui_info(u, _("正在檢查…"), NULL);
		else if (pm_ui_button(u, _("檢查這台電腦可以安裝的驅動程式"), _("檢查"))) start_worker(drivers_thread, NULL);
	} else if (ndrivers == 0) {
		pm_ui_info(u, _("不需要額外的驅動程式"), NULL);
	} else {
		for (int i = 0; i < ndrivers; i++) {
			char lab[160];
			snprintf(lab, sizeof lab, "%s%s", drivers[i], i == drivers_rec ? _("（建議）") : "");
			if (pm_ui_button(u, lab, _("安裝")) && !job) {
				static char pkg[96];
				snprintf(pkg, sizeof pkg, "%s", drivers[i]);
				char *argv[] = { "apt-get", "install", "-y", pkg, NULL };
				run_admin(argv, _("安裝驅動程式"), drivers_done);
			}
		}
	}
	pm_ui_end(u);
no_drivers:
	if (job) {
		char t[300];
		snprintf(t, sizeof t, _("正在%s，請稍候…"), job_title);
		pm_ui_note(u, t);
	}
}

/* ------------------------------------------------------------------ */
/* 電源與鎖定                                                          */
/* ------------------------------------------------------------------ */

static void build_power(PmUI *u)
{
	if (pm_ui_back(u, _("系統設定"))) { go(u, P_MAIN); return; }
	pm_ui_title(u, _("電源與鎖定"), NULL);
	if (!access("/sys/class/power_supply/BAT0", F_OK) || !access("/sys/class/power_supply/BAT1", F_OK)) {
		pm_ui_group(u, NULL);
		if (pm_have("pomelo-doctor") && pm_ui_button(u, _("電池健康度、充放電次數"), _("檢查電池"))) {
			pm_spawn("pomelo-doctor --page check --category battery");   /* 健康檢查已併入主機醫生 */
		}
		pm_ui_end(u);
	}
	static const char *mins[] = { N_("從不"), N_("1 分鐘"), N_("5 分鐘"), N_("10 分鐘"), N_("15 分鐘"), N_("30 分鐘"), N_("1 小時") };
	static const int minv[] = { 0, 1, 5, 10, 15, 30, 60 };
	static const struct { const char *label, *key; int def; } pw[] = {
		{ N_("閒置後鎖定螢幕"), "power.lock_after", 10 },
		{ N_("閒置後關閉螢幕"), "power.screen_off_after", 15 },
		{ N_("閒置後休眠（接電源）"), "power.suspend_after", 0 },
		{ N_("閒置後休眠（用電池）"), "power.suspend_after_battery", 30 },
	};
	pm_ui_group(u, NULL);
	for (unsigned i = 0; i < LEN(pw); i++) {
		int v = pm_conf_int(pw[i].key, pw[i].def), sel = 0;
		for (unsigned k = 0; k < LEN(minv); k++) if (minv[k] == v) sel = (int)k;
		if (pm_ui_choice(u, pw[i].label, mins, LEN(mins), &sel)) pm_conf_set_int(pw[i].key, minv[sel]);
	}
	if (pm_ui_button(u, _("立即鎖定螢幕"), _("鎖定"))) pm_spawn("pomelo-lock");
	pm_ui_end(u);
	{   /* 閒置後的順序：螢幕保護程式 → 鎖定 → 關螢幕 → 休眠；互相矛盾時提示（邏輯與螢幕保護程式的設定頁共用 saver-idle.h） */
		SvIdle t = { pm_conf_int("saver.enable", 1), pm_conf_int("saver.after", 5), pm_conf_int("power.lock_after", 10),
		             pm_conf_int("power.screen_off_after", 15), pm_conf_int("power.suspend_after", 0) };
		char seq[400], line[520];
		sv_idle_sentence(&t, seq, sizeof seq, _("螢幕保護程式（%d 分鐘）"), _("鎖定螢幕（%d 分鐘）"), _("關閉螢幕（%d 分鐘）"),
		                 _("休眠（%d 分鐘）"), _("目前沒有任何自動動作。"), _(" → "), "");
		snprintf(line, sizeof line, _("閒置後依序：%s"), seq);
		pm_ui_note(u, line);
		int w = sv_idle_warn(&t);
		if (w & SVW_OFF_FIRST) { snprintf(line, sizeof line, _("注意：關閉螢幕設在 %d 分鐘，比螢幕保護程式早（或同時），保護程式不會出現。"), t.off_min); pm_ui_note(u, line); }
		if (w & SVW_LOCK_FIRST) { snprintf(line, sizeof line, _("注意：鎖定螢幕設在 %d 分鐘，比螢幕保護程式早（或同時），畫面會先鎖起來，看不到保護程式。"), t.lock_min); pm_ui_note(u, line); }
		if (w & SVW_SUSP_FIRST) { snprintf(line, sizeof line, _("注意：休眠設在 %d 分鐘，比螢幕保護程式早（或同時），保護程式不會出現。"), t.susp_min); pm_ui_note(u, line); }
		if (pm_have("pomelo-screensaver")) {
			pm_ui_group(u, NULL);
			if (pm_ui_button(u, _("選擇螢幕保護程式、預覽與設定啟動時間"), _("螢幕保護程式…"))) pm_spawn("pomelo-screensaver --settings");
			pm_ui_end(u);
		}
	}
}

/* ------------------------------------------------------------------ */
/* Spotlight                                                           */
/* ------------------------------------------------------------------ */

static void build_spotlight(PmUI *u)
{
	if (pm_ui_back(u, _("系統設定"))) { go(u, P_MAIN); return; }
	pm_ui_title(u, _("搜尋"), NULL);
	int have_locate = pm_have("plocate") || pm_have("locate");
	int files = pm_conf_int("spotlight.files", 1), cmds = pm_conf_int("spotlight.commands", 1);
	int calc = pm_conf_int("spotlight.calculator", 1);
	pm_ui_group(u, _("搜尋結果"));
	pm_ui_info(u, _("應用程式"), _("一律顯示"));
	if (pm_ui_switch(u, _("檔案與資料夾"), &files)) pm_conf_set_int("spotlight.files", files);
	if (pm_ui_switch(u, _("指令"), &cmds)) pm_conf_set_int("spotlight.commands", cmds);
	if (pm_ui_switch(u, _("計算機"), &calc)) pm_conf_set_int("spotlight.calculator", calc);
	pm_ui_end(u);
	if (files && !have_locate) {
		char *h = pm_pkg_hint("plocate"), note[400];
		snprintf(note, sizeof note, _("搜尋檔案需要 plocate（%s），檔案索引每天自動更新。"), h ? h : "plocate");
		free(h);
		pm_ui_note(u, note);
	} else
		pm_ui_note(u, _("檔案只搜尋家目錄裡的檔名，不含隱藏資料夾；新檔案要等每天的索引更新後才找得到。"));
	pm_ui_group(u, _("快捷鍵"));
	pm_ui_info(u, _("開啟搜尋"), "Super + Space");
	pm_ui_info(u, _("也可以用"), "Super + D");
	pm_ui_end(u);
}

/* ------------------------------------------------------------------ */
/* 通知                                                                */
/* ------------------------------------------------------------------ */

static char notify_apps[48][96];
static int nnotify_apps;

static void read_notify_apps(void)
{
	nnotify_apps = 0;
	const char *c = getenv("XDG_CONFIG_HOME"), *h = getenv("HOME");
	char p[PATH_MAX], line[128];
	if (c && *c) snprintf(p, sizeof p, "%s/pomelo/notify-apps", c);
	else snprintf(p, sizeof p, "%s/.config/pomelo/notify-apps", h ? h : "/");
	FILE *f = fopen(p, "r");
	while (f && nnotify_apps < (int)LEN(notify_apps) && fgets(line, sizeof line, f)) {
		line[strcspn(line, "\n")] = 0;
		if (*line) snprintf(notify_apps[nnotify_apps++], sizeof notify_apps[0], "%s", line);
	}
	if (f) fclose(f);
}

/* notify.muted 是逗號分隔的清單 */
static int is_muted(const char *app)
{
	const char *m = pm_conf_get("notify.muted", "");
	size_t n = strlen(app);
	for (const char *p = m; (p = strstr(p, app)); p += n)
		if ((p == m || p[-1] == ',') && (p[n] == ',' || !p[n])) return 1;
	return 0;
}

static void set_muted(const char *app, int mute)
{
	char out[2048] = "";
	const char *m = pm_conf_get("notify.muted", "");
	char copy[2048], *save = NULL;
	snprintf(copy, sizeof copy, "%s", m);
	for (char *t = strtok_r(copy, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
		if (!strcmp(t, app)) continue;
		size_t n = strlen(out);
		snprintf(out + n, sizeof out - n, "%s%s", n ? "," : "", t);
	}
	if (mute && !strchr(app, ',')) {
		size_t n = strlen(out);
		snprintf(out + n, sizeof out - n, "%s%s", n ? "," : "", app);
	}
	pm_conf_set("notify.muted", out);
}

static void build_notify(PmUI *u)
{
	if (pm_ui_back(u, _("系統設定"))) { go(u, P_MAIN); return; }
	pm_ui_title(u, _("通知"), NULL);
	static const char *secs[] = { N_("3 秒"), N_("6 秒"), N_("10 秒"), N_("20 秒") };
	static const int secv[] = { 3, 6, 10, 20 };
	int dnd = pm_conf_int("notify.dnd", 0), cur = pm_conf_int("notify.seconds", 6), sel = 1;
	for (unsigned i = 0; i < LEN(secv); i++) if (secv[i] == cur) sel = (int)i;
	pm_ui_group(u, NULL);
	if (pm_ui_switch(u, _("勿擾模式"), &dnd)) pm_conf_set_int("notify.dnd", dnd);
	if (pm_ui_choice(u, _("通知顯示時間"), secs, LEN(secs), &sel)) pm_conf_set_int("notify.seconds", secv[sel]);
	if (pm_ui_button(u, _("看看通知長什麼樣子"), _("傳送測試通知"))) {
		if (pm_have("notify-send")) {
			char *argv[] = { "notify-send", "-a", "Pomelo", (char *)_("測試通知"), (char *)_("通知會顯示在螢幕右上角。"), NULL };
			spawn_argv(argv);
		} else {
			char *argv[] = { "gdbus", "call", "--session", "--dest", "org.freedesktop.Notifications",
			                 "--object-path", "/org/freedesktop/Notifications", "--method",
			                 "org.freedesktop.Notifications.Notify", "Pomelo", "0", "", (char *)_("測試通知"),
			                 (char *)_("通知會顯示在螢幕右上角。"), "[]", "{}", "-1", NULL };
			spawn_argv(argv);
		}
	}
	pm_ui_end(u);
	pm_ui_note(u, _("勿擾模式開啟時只會顯示緊急通知；螢幕鎖定時不顯示通知。"));

	pm_ui_group(u, _("應用程式通知"));
	if (!nnotify_apps) pm_ui_info(u, _("程式發出通知後會出現在這裡"), NULL);
	for (int i = 0; i < nnotify_apps; i++) {
		int on = !is_muted(notify_apps[i]);
		if (pm_ui_switch(u, notify_apps[i], &on)) set_muted(notify_apps[i], !on);
	}
	pm_ui_end(u);
}

/* ------------------------------------------------------------------ */
/* 鍵盤、滑鼠與觸控板（登入時由 pomelo-session 呼叫 --apply 套用）       */
/* ------------------------------------------------------------------ */

static void apply_keyboard(void)
{
	char cmd[96];
	snprintf(cmd, sizeof cmd, "xset r rate %d %d", pm_conf_int("keyboard.repeat_delay", 500),
	         pm_conf_int("keyboard.repeat_rate", 30));
	if (pm_have("xset")) { int r = system(cmd); (void)r; }
}

static int have_touchpad = -1, have_mouse;

/* 對每個指標裝置套用 libinput 設定；觸控板（有 Tapping 屬性）用觸控板的設定 */
static void apply_pointer(void)
{
	if (!pm_have("xinput")) { have_touchpad = 0; return; }
	char *t = pm_run("xinput list");
	char *save = NULL;
	int tp = 0, ms = 0;
	for (char *l = t ? strtok_r(t, "\n", &save) : NULL; l; l = strtok_r(NULL, "\n", &save)) {
		if (!strstr(l, "slave  pointer") || strstr(l, "XTEST")) continue;
		char *idp = strstr(l, "id=");
		if (!idp) continue;
		int id = atoi(idp + 3);
		char cmd[128];
		snprintf(cmd, sizeof cmd, "xinput list-props %d", id);
		char *props = pm_run(cmd);
		if (!props || !strstr(props, "libinput Accel Speed")) { free(props); continue; }
		int touch = strstr(props, "libinput Tapping Enabled") != NULL;
		const char *pre = touch ? "touchpad" : "mouse";
		char key[64];
		snprintf(key, sizeof key, "%s.speed", pre);
		double speed = atof(pm_conf_get(key, "0"));
		snprintf(key, sizeof key, "%s.natural", pre);
		int natural = pm_conf_int(key, touch ? 1 : 0);
		char full[512];
		snprintf(full, sizeof full, "xinput set-prop %d 'libinput Accel Speed' %.2f 2>/dev/null", id, speed);
		int r = system(full);
		if (strstr(props, "libinput Natural Scrolling Enabled")) {
			snprintf(full, sizeof full, "xinput set-prop %d 'libinput Natural Scrolling Enabled' %d 2>/dev/null", id, natural);
			r = system(full);
		}
		if (!touch && strstr(props, "libinput Left Handed Enabled")) {
			snprintf(full, sizeof full, "xinput set-prop %d 'libinput Left Handed Enabled' %d 2>/dev/null", id,
			         pm_conf_int("mouse.left_handed", 0));
			r = system(full);
		}
		if (touch) {
			snprintf(full, sizeof full, "xinput set-prop %d 'libinput Tapping Enabled' %d 2>/dev/null", id,
			         pm_conf_int("touchpad.tap", 1));
			r = system(full);
			if (strstr(props, "libinput Disable While Typing Enabled")) {
				snprintf(full, sizeof full, "xinput set-prop %d 'libinput Disable While Typing Enabled' %d 2>/dev/null", id,
				         pm_conf_int("touchpad.dwt", 1));
				r = system(full);
			}
		}
		(void)r;
		free(props);
		if (touch) tp = 1; else ms = 1;
	}
	free(t);
	have_touchpad = tp;
	have_mouse = ms;
}

static double rep_delay, rep_rate;

static void build_keyboard(PmUI *u)
{
	if (pm_ui_back(u, _("系統設定"))) { go(u, P_MAIN); return; }
	pm_ui_title(u, _("鍵盤"), NULL);
	pm_ui_group(u, _("按住按鍵時重複輸入"));
	int changed = pm_ui_slider(u, _("開始重複前的延遲"), &rep_delay, 150, 1000);
	changed |= pm_ui_slider(u, _("重複速度"), &rep_rate, 10, 60);
	pm_ui_end(u);
	char t[96];
	snprintf(t, sizeof t, _("延遲 %d 毫秒，每秒 %d 次。"), (int)rep_delay, (int)rep_rate);
	pm_ui_note(u, t);
	/* 拖曳時只更新數字，放開才套用 */
	static int dirty;
	if (changed) dirty = 1;
	if (dirty && !u->down) {
		pm_conf_set_int("keyboard.repeat_delay", (int)rep_delay);
		pm_conf_set_int("keyboard.repeat_rate", (int)rep_rate);
		apply_keyboard();
		dirty = 0;
	}
	int have_lang = pm_have("pomelo-language");
	pm_ui_group(u, NULL);
	if (pm_ui_item(u, "@language", _("鍵盤配置與輸入法"), NULL, have_lang ? "›" : _("未安裝"), 0) && have_lang)
		pm_spawn("pomelo-language");
	pm_ui_end(u);

	static const struct { const char *keys, *what; } sc[] = {
		{ "Super + Space", N_("搜尋") }, { "Super + A", N_("啟動器") },
		{ "Super + Tab", N_("所有視窗") }, { "Alt + Tab", N_("切換視窗") },
		{ "Super + Q", N_("關閉視窗") }, { "Super + M", N_("最小化") },
		{ "Super + ↑", N_("最大化") }, { "Super + ← / →", N_("貼齊螢幕左半／右半") },
		{ "Super + Ctrl + F", N_("全螢幕") }, { "Super + L", N_("鎖定螢幕") },
		{ "Super + Return", N_("開啟終端機") }, { "Super + Shift + R", N_("重新啟動 Pomelo") },
		{ "Super + Shift + 3", N_("截取整個螢幕") }, { "Super + Shift + 4", N_("截取選取範圍（空白鍵：視窗）") },
		{ "Super + Shift + 5", N_("截圖與錄影工具列") }, { "Super + Ctrl + Esc", N_("停止錄影") },
	};
	pm_ui_group(u, _("快捷鍵"));
	for (unsigned i = 0; i < LEN(sc); i++) pm_ui_info(u, _(sc[i].what), sc[i].keys);
	pm_ui_end(u);
	pm_ui_note(u, _("Super 是 Windows 鍵（Mac 鍵盤上的 ⌘）。"));
}

static double mouse_speed, touch_speed;

static void build_mouse(PmUI *u)
{
	if (pm_ui_back(u, _("系統設定"))) { go(u, P_MAIN); return; }
	pm_ui_title(u, _("滑鼠與觸控板"), NULL);
	if (!pm_have("xinput")) {
		char *h = pm_pkg_hint("xinput"), note[400];
		snprintf(note, sizeof note, _("需要 xinput 才能調整滑鼠與觸控板（%s）。"), h ? h : "xinput");
		free(h);
		pm_ui_note(u, note);
		return;
	}
	static int dirty;
	char v[32];
	pm_ui_group(u, _("滑鼠"));
	if (pm_ui_slider(u, _("游標速度"), &mouse_speed, -1, 1)) dirty = 1;
	int nat = pm_conf_int("mouse.natural", 0), left = pm_conf_int("mouse.left_handed", 0);
	if (pm_ui_switch(u, _("自然捲動"), &nat)) { pm_conf_set_int("mouse.natural", nat); apply_pointer(); }
	if (pm_ui_switch(u, _("左手模式（左右鍵對調）"), &left)) { pm_conf_set_int("mouse.left_handed", left); apply_pointer(); }
	pm_ui_end(u);
	if (have_touchpad > 0) {
		pm_ui_group(u, _("觸控板"));
		if (pm_ui_slider(u, _("游標速度"), &touch_speed, -1, 1)) dirty = 1;
		int tap = pm_conf_int("touchpad.tap", 1), tnat = pm_conf_int("touchpad.natural", 1);
		int dwt = pm_conf_int("touchpad.dwt", 1);
		if (pm_ui_switch(u, _("輕觸來點按"), &tap)) { pm_conf_set_int("touchpad.tap", tap); apply_pointer(); }
		if (pm_ui_switch(u, _("自然捲動"), &tnat)) { pm_conf_set_int("touchpad.natural", tnat); apply_pointer(); }
		if (pm_ui_switch(u, _("打字時停用觸控板"), &dwt)) { pm_conf_set_int("touchpad.dwt", dwt); apply_pointer(); }
		pm_ui_end(u);
	}
	if (dirty && !u->down) {
		snprintf(v, sizeof v, "%.2f", mouse_speed);
		pm_conf_set("mouse.speed", v);
		snprintf(v, sizeof v, "%.2f", touch_speed);
		pm_conf_set("touchpad.speed", v);
		apply_pointer();
		dirty = 0;
	}
	pm_ui_note(u, _("自然捲動：內容跟著手指方向移動（跟手機一樣）。"));
}

/* ------------------------------------------------------------------ */
/* 螢幕使用時間（資料由 pomelo-panel 記錄）                              */
/* ------------------------------------------------------------------ */

static struct { char name[96]; long sec; } st_today[128];
static int nst_today;
static long st_days[7];          /* 最近 7 天每天的總秒數，[6] 是今天 */
static char st_daynames[7][16], st_wday[7][16];
static int st_clear_confirm;

static void st_dir(char *out, size_t n)
{
	const char *d = getenv("XDG_DATA_HOME"), *h = getenv("HOME");
	if (d && *d) snprintf(out, n, "%s/pomelo/screentime", d);
	else snprintf(out, n, "%s/.local/share/pomelo/screentime", h ? h : "/");
}

static int cmp_st(const void *a, const void *b)
{
	long x = ((const typeof(st_today[0]) *)a)->sec, y = ((const typeof(st_today[0]) *)b)->sec;
	return x < y ? 1 : x > y ? -1 : 0;
}

static void read_screentime(void)
{
	char dir[PATH_MAX], p[PATH_MAX + 32], line[256];
	st_dir(dir, sizeof dir);
	time_t now = time(NULL);
	nst_today = 0;
	for (int d = 0; d < 7; d++) {
		time_t t = now - (6 - d) * 86400L;
		char day[16];
		strftime(day, sizeof day, "%Y-%m-%d", localtime(&t));
		strftime(st_daynames[d], sizeof st_daynames[0], _("%-m/%-d %a"), localtime(&t));
		strftime(st_wday[d], sizeof st_wday[0], "%a", localtime(&t));
		snprintf(p, sizeof p, "%s/%s", dir, day);
		st_days[d] = 0;
		FILE *f = fopen(p, "r");
		while (f && fgets(line, sizeof line, f)) {
			char *tab = strchr(line, '\t');
			if (!tab) continue;
			*tab = 0;
			long sec = atol(tab + 1);
			st_days[d] += sec;
			if (d == 6 && nst_today < (int)LEN(st_today)) {
				snprintf(st_today[nst_today].name, sizeof st_today[0].name, "%s", line);
				st_today[nst_today++].sec = sec;
			}
		}
		if (f) fclose(f);
	}
	qsort(st_today, nst_today, sizeof st_today[0], cmp_st);
}

static void fmt_dur(char *out, size_t n, long sec)
{
	if (sec >= 3600) snprintf(out, n, _("%ld 小時 %ld 分"), sec / 3600, sec % 3600 / 60);
	else if (sec >= 60) snprintf(out, n, _("%ld 分鐘"), sec / 60);
	else snprintf(out, n, "%s", _("不到 1 分鐘"));
}

static void clear_screentime(void)
{
	char dir[PATH_MAX], p[PATH_MAX + 300];
	st_dir(dir, sizeof dir);
	DIR *d = opendir(dir);
	struct dirent *e;
	while (d && (e = readdir(d))) {
		if (e->d_name[0] == '.') continue;
		snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
		unlink(p);
	}
	if (d) closedir(d);
}

static void build_screentime(PmUI *u)
{
	if (pm_ui_back(u, _("系統設定"))) { go(u, P_MAIN); return; }
	pm_ui_title(u, _("螢幕使用時間"), NULL);
	char a[64], b[128];
	int on = pm_conf_int("screentime.enabled", 1);
	pm_ui_group(u, NULL);
	if (pm_ui_switch(u, _("記錄螢幕使用時間"), &on)) pm_conf_set_int("screentime.enabled", on);
	pm_ui_end(u);
	if (!on) {
		pm_ui_note(u, _("開啟後會記錄每個程式在前景的時間；資料只存在這台電腦。"));
		return;
	}
	fmt_dur(a, sizeof a, st_days[6]);
	long week = 0;
	int used_days = 0;
	for (int d = 0; d < 7; d++) { week += st_days[d]; if (st_days[d]) used_days++; }
	pm_ui_group(u, NULL);
	pm_ui_info(u, _("今天"), a);
	fmt_dur(a, sizeof a, used_days ? week / used_days : 0);
	pm_ui_info(u, _("最近 7 天平均"), a);
	pm_ui_end(u);

	double v[7], mx = 1;
	for (int d = 0; d < 7; d++) mx = MAX(mx, (double)st_days[d]);
	for (int d = 0; d < 7; d++) v[d] = st_days[d] / mx;
	pm_ui_group(u, NULL);
	const char *wl[7];
	for (int d = 0; d < 7; d++) wl[d] = d == 6 ? _("今天") : st_wday[d];
	pm_ui_bars(u, _("最近 7 天"), v, wl, 7, 0x0a84ff, 0x0a84ff);
	for (int d = 6; d >= 0; d--) {
		if (!st_days[d]) continue;
		fmt_dur(a, sizeof a, st_days[d]);
		pm_ui_info(u, d == 6 ? _("今天") : st_daynames[d], a);
	}
	pm_ui_end(u);

	if (nst_today) {
		pm_ui_group(u, _("今天最常用"));
		for (int i = 0; i < nst_today && i < 12; i++) {
			fmt_dur(b, sizeof b, st_today[i].sec);
			pm_ui_meter(u, st_today[i].name, b, (double)st_today[i].sec / MAX(1, st_today[0].sec), 0x0a84ff);
		}
		pm_ui_end(u);
	} else pm_ui_note(u, _("今天還沒有記錄（每分鐘更新一次）。"));

	pm_ui_group(u, NULL);
	if (!st_clear_confirm) {
		if (pm_ui_button(u, _("刪除所有螢幕使用時間記錄"), _("清除記錄…"))) { st_clear_confirm = 1; u->again = 1; }
	} else {
		if (pm_ui_button(u, _("確定要刪除所有記錄嗎？"), _("刪除"))) {
			clear_screentime();
			read_screentime();
			st_clear_confirm = 0;
			snprintf(status, sizeof status, "%s", _("已清除螢幕使用時間記錄"));
		}
		if (pm_ui_button(u, NULL, _("取消"))) st_clear_confirm = 0;
	}
	pm_ui_end(u);
	pm_ui_note(u, _("閒置超過 5 分鐘的時間不會計入；資料只存在 ~/.local/share/pomelo/screentime。"));
}

/* ------------------------------------------------------------------ */
/* 使用者與群組                                                        */
/* ------------------------------------------------------------------ */

typedef struct { char name[64], full[128]; uid_t uid; int admin; } User;
static User users[64];
static int nusers;
static char sel_user[64];
static char nu_name[33], nu_full[64], nu_pass[64], nu_pass2[64], chg_pass[64];
static int nu_admin, del_confirm, autologin = -1;
static char groups_txt[24][256];
static int ngroups_txt;

static int in_group(const char *user, const char *group)
{
	struct group *g = getgrnam(group);
	if (!g) return 0;
	for (char **m = g->gr_mem; *m; m++) if (!strcmp(*m, user)) return 1;
	struct passwd *pw = getpwnam(user);
	return pw && pw->pw_gid == g->gr_gid;
}

static void read_autologin(void)
{
	autologin = -1;
	if (access("/usr/sbin/lightdm", X_OK)) return;   /* 只支援 LightDM */
	autologin = 0;
	FILE *f = fopen("/etc/lightdm/lightdm.conf.d/70-pomelo-autologin.conf", "r");
	char line[256];
	struct passwd *me = getpwuid(getuid());
	while (f && fgets(line, sizeof line, f)) {
		line[strcspn(line, "\n")] = 0;
		if (!strncmp(line, "autologin-user=", 15) && me && !strcmp(line + 15, me->pw_name)) autologin = 1;
	}
	if (f) fclose(f);
}

static void read_users(void)
{
	nusers = 0;
	setpwent();
	struct passwd *pw;
	while ((pw = getpwent()) && nusers < (int)LEN(users)) {
		if (pw->pw_uid < 1000 || pw->pw_uid >= 60000) continue;
		if (strstr(pw->pw_shell, "nologin") || strstr(pw->pw_shell, "false")) continue;
		User *x = &users[nusers++];
		snprintf(x->name, sizeof x->name, "%s", pw->pw_name);
		snprintf(x->full, sizeof x->full, "%s", pw->pw_gecos ? pw->pw_gecos : "");
		x->full[strcspn(x->full, ",")] = 0;
		x->uid = pw->pw_uid;
		x->admin = in_group(pw->pw_name, "sudo") || in_group(pw->pw_name, "admin") || in_group(pw->pw_name, "wheel");
	}
	endpwent();
	/* 群組：有成員的一般群組，以及常用的系統群組 */
	ngroups_txt = 0;
	static const char *common[] = { "sudo", "adm", "lpadmin", "docker", "plugdev", "dialout", "audio", "video", "netdev" };
	setgrent();
	struct group *g;
	while ((g = getgrent()) && ngroups_txt < (int)LEN(groups_txt)) {
		int show = g->gr_gid >= 1000 && g->gr_gid < 60000;
		for (unsigned i = 0; i < LEN(common); i++) if (!strcmp(g->gr_name, common[i])) show = 1;
		if (!show || !g->gr_mem[0]) continue;
		char mem[200] = "";
		for (char **m = g->gr_mem; *m; m++) {
			size_t n = strlen(mem);
			snprintf(mem + n, sizeof mem - n, "%s%s", n ? ", " : "", *m);
		}
		snprintf(groups_txt[ngroups_txt++], sizeof groups_txt[0], "%s\t%s", g->gr_name, mem);
	}
	endgrent();
	read_autologin();
}

static void users_done(int ok)
{
	(void)ok;
	read_users();
	nu_name[0] = nu_full[0] = nu_pass[0] = nu_pass2[0] = chg_pass[0] = 0;
	nu_admin = 0;
	del_confirm = 0;
}

/* 以管理員身分執行，並把 input 送到標準輸入（chpasswd 用，密碼不會出現在指令列） */
static void run_admin_stdin(char *const argv[], const char *input, const char *title, void (*done)(int ok))
{
	if (job > 0) return;
	/* 介面元件預設把 SIGCHLD 設成忽略（子行程自動回收），那樣 waitpid 永遠拿不到結果、工作永遠不算完成 */
	signal(SIGCHLD, SIG_DFL);
	if (!has_polkit_agent()) {
		snprintf(status, sizeof status, "%s", _("需要密碼認證視窗（policykit-1-gnome）才能管理使用者。"));
		return;
	}
	snprintf(job_title, sizeof job_title, "%s", title);
	status[0] = 0;
	int fds[2];
	if (pipe(fds)) return;
	job_done = done;
	job = fork();
	if (job == 0) {
		dup2(fds[0], 0);
		close(fds[1]);
		int nul = open("/dev/null", O_RDWR);
		if (nul >= 0) { dup2(nul, 1); dup2(nul, 2); }
		if (dpy) close(ConnectionNumber(dpy));   /* 原生 Wayland（pm_ui_native）時沒有 X 連線 */
		char *full[32] = { "pkexec" };
		int k = 1;
		for (int i = 0; argv[i] && k < 31; i++) full[k++] = argv[i];
		full[k] = NULL;
		execvp("pkexec", full);
		_exit(127);
	}
	close(fds[0]);
	ssize_t w = write(fds[1], input, strlen(input));
	(void)w;
	close(fds[1]);
}

static int valid_user(const char *s)
{
	if (!*s || strlen(s) > 32 || !(islower((unsigned char)*s) || *s == '_')) return 0;
	for (const char *p = s; *p; p++) if (!islower((unsigned char)*p) && !isdigit((unsigned char)*p) && *p != '_' && *p != '-') return 0;
	return getpwnam(s) == NULL;
}

static void build_users(PmUI *u)
{
	if (pm_ui_back(u, _("系統設定"))) { go(u, P_MAIN); return; }
	pm_ui_title(u, _("使用者與群組"), NULL);
	struct passwd *me = getpwuid(getuid());
	pm_ui_group(u, _("使用者"));
	for (int i = 0; i < nusers; i++) {
		User *x = &users[i];
		char sub[160];
		snprintf(sub, sizeof sub, "%s · %s", x->name, x->admin ? _("管理員") : _("標準使用者"));
		int self = me && x->uid == me->pw_uid;
		if (pm_ui_item(u, "avatar-default", x->full[0] ? x->full : x->name, sub, self ? _("目前使用者") : NULL,
		               !strcmp(sel_user, x->name))) {
			snprintf(sel_user, sizeof sel_user, "%s", strcmp(sel_user, x->name) ? x->name : "");
			chg_pass[0] = 0;
			del_confirm = 0;
			u->again = 1;
		}
	}
	pm_ui_end(u);

	User *s = NULL;
	for (int i = 0; i < nusers; i++) if (!strcmp(users[i].name, sel_user)) s = &users[i];
	if (s && !job) {
		int self = me && s->uid == me->pw_uid;
		char cap[160];
		snprintf(cap, sizeof cap, "%s", s->full[0] ? s->full : s->name);
		pm_ui_group(u, cap);
		if (self) {
			if (pm_ui_button(u, _("更改自己的密碼（在終端機裡輸入）"), _("更改密碼…"))) {
				char *argv[] = { "pomelo-terminal", "-t", (char *)_("更改密碼"), "-e", "sh", "-c", "passwd; echo; read -p '按 Enter 關閉' x", NULL };
				spawn_argv(argv);
			}
		} else {
			pm_ui_entry(u, _("新密碼"), chg_pass, sizeof chg_pass, 1);
			if (pm_ui_button(u, NULL, _("設定密碼")) && chg_pass[0]) {
				char in[160];
				snprintf(in, sizeof in, "%s:%s\n", s->name, chg_pass);
				char *argv[] = { "chpasswd", NULL };
				run_admin_stdin(argv, in, _("設定密碼"), users_done);
			}
		}
		int adm = s->admin;
		if (!self && pm_ui_switch(u, _("允許管理這台電腦（管理員）"), &adm)) {
			static char who[64];
			snprintf(who, sizeof who, "%s", s->name);
			char *argv1[] = { "usermod", "-aG", "sudo", who, NULL };
			char *argv2[] = { "gpasswd", "-d", who, "sudo", NULL };
			run_admin(adm ? argv1 : argv2, _("更改管理員權限"), users_done);
		}
		if (!self) {
			if (!del_confirm) {
				if (pm_ui_button(u, _("刪除這個使用者（家目錄會保留）"), _("刪除使用者…"))) { del_confirm = 1; u->again = 1; }
			} else {
				char w[200];
				snprintf(w, sizeof w, _("確定要刪除「%s」嗎？"), s->name);
				if (pm_ui_button(u, w, _("刪除"))) {
					static char who[64];
					snprintf(who, sizeof who, "%s", s->name);
					char *argv[] = { "userdel", who, NULL };
					run_admin(argv, _("刪除使用者"), users_done);
					sel_user[0] = 0;
				}
				if (pm_ui_button(u, NULL, _("取消"))) del_confirm = 0;
			}
		}
		pm_ui_end(u);
	}

	if (autologin >= 0 && me) {
		int al = autologin;
		pm_ui_group(u, NULL);
		if (pm_ui_switch(u, _("開機後自動登入（不用輸入密碼）"), &al) && !job) {
			static char script[256];
			if (al)
				snprintf(script, sizeof script, "mkdir -p /etc/lightdm/lightdm.conf.d && printf '[Seat:*]\\nautologin-user=%s\\n' "
				         "> /etc/lightdm/lightdm.conf.d/70-pomelo-autologin.conf", me->pw_name);
			else
				snprintf(script, sizeof script, "rm -f /etc/lightdm/lightdm.conf.d/70-pomelo-autologin.conf");
			char *argv[] = { "sh", "-c", script, NULL };
			run_admin(argv, _("更改自動登入"), users_done);
		}
		pm_ui_end(u);
	}

	pm_ui_group(u, _("新增使用者"));
	pm_ui_entry(u, _("帳號名稱"), nu_name, sizeof nu_name, 0);
	pm_ui_entry(u, _("全名"), nu_full, sizeof nu_full, 0);
	pm_ui_entry(u, _("密碼"), nu_pass, sizeof nu_pass, 1);
	pm_ui_entry(u, _("確認密碼"), nu_pass2, sizeof nu_pass2, 1);
	pm_ui_switch(u, _("管理員"), &nu_admin);
	if (pm_ui_button(u, NULL, _("新增使用者")) && !job) {
		if (!valid_user(nu_name))
			snprintf(status, sizeof status, "%s", _("帳號名稱要用小寫英文字母開頭，只能有小寫字母、數字、_ 和 -，而且不能跟現有的重複"));
		else if (!nu_pass[0] || strcmp(nu_pass, nu_pass2))
			snprintf(status, sizeof status, "%s", _("兩次輸入的密碼不一樣"));
		else {
			/* 全名去掉會破壞 /etc/passwd 格式的字元 */
			char full[64];
			size_t k = 0;
			for (const char *p = nu_full; *p && k < sizeof full - 1; p++) if (!strchr(":,\"'\\\n", *p)) full[k++] = *p;
			full[k] = 0;
			static char script[512];
			snprintf(script, sizeof script, "useradd -m -s /bin/bash -c '%s' %s && chpasswd%s", full, nu_name,
			         nu_admin ? " && usermod -aG sudo " : "");
			if (nu_admin) strncat(script, nu_name, sizeof script - strlen(script) - 1);
			char in[160];
			snprintf(in, sizeof in, "%s:%s\n", nu_name, nu_pass);
			char *argv[] = { "sh", "-c", script, NULL };
			run_admin_stdin(argv, in, _("新增使用者"), users_done);
		}
	}
	pm_ui_end(u);

	if (ngroups_txt) {
		pm_ui_group(u, _("群組"));
		for (int i = 0; i < ngroups_txt; i++) {
			char name[64], *tab = strchr(groups_txt[i], '\t');
			snprintf(name, sizeof name, "%.*s", tab ? (int)(tab - groups_txt[i]) : 0, groups_txt[i]);
			pm_ui_info(u, name, tab ? tab + 1 : "");
		}
		pm_ui_end(u);
		pm_ui_note(u, _("sudo：管理員；lpadmin：管理印表機；docker：使用 Docker。"));
	}
	if (job) {
		char t[300];
		snprintf(t, sizeof t, _("正在%s，請稍候…"), job_title);
		pm_ui_note(u, t);
	}
}

/* ------------------------------------------------------------------ */
/* 印表機與掃描器（CUPS、SANE）                                         */
/* ------------------------------------------------------------------ */

typedef struct { char name[96], state[32]; int def, jobs, enabled; } Printer;
static Printer printers[32];
static int nprinters;
static char sel_printer[96];
static int rm_confirm;
static char scanners[8][192];
static volatile int nscanners = -1;

static void read_printers(void)
{
	nprinters = 0;
	if (!pm_have("lpstat")) return;
	char *t = pm_run("lpstat -p -d");
	char *save = NULL, def[96] = "";
	for (char *l = t ? strtok_r(t, "\n", &save) : NULL; l; l = strtok_r(NULL, "\n", &save)) {
		char name[96];
		if (sscanf(l, "printer %95s", name) == 1 && nprinters < (int)LEN(printers)) {
			Printer *p = &printers[nprinters++];
			memset(p, 0, sizeof *p);
			snprintf(p->name, sizeof p->name, "%s", name);
			p->enabled = !strstr(l, "disabled");
			snprintf(p->state, sizeof p->state, "%s", strstr(l, "printing") ? "printing" : p->enabled ? "idle" : "disabled");
		}
	}
	free(t);
	/* lpstat -d 會反映使用者用 lpoptions -d 設的預設 */
	t = pm_run("lpstat -d");
	if (t) sscanf(t, "system default destination: %95s", def);
	free(t);
	t = pm_run("lpstat -o");
	save = NULL;
	for (char *l = t ? strtok_r(t, "\n", &save) : NULL; l; l = strtok_r(NULL, "\n", &save))
		for (int i = 0; i < nprinters; i++) {
			size_t n = strlen(printers[i].name);
			if (!strncmp(l, printers[i].name, n) && l[n] == '-') printers[i].jobs++;
		}
	free(t);
	for (int i = 0; i < nprinters; i++) printers[i].def = !strcmp(printers[i].name, def);
}

static void *scan_thread(void *arg)
{
	(void)arg;
	int n = 0;
	char *t = pm_run("scanimage -L");
	char *save = NULL;
	for (char *l = t ? strtok_r(t, "\n", &save) : NULL; l && n < (int)LEN(scanners); l = strtok_r(NULL, "\n", &save)) {
		/* device `escl:https://…' is a HP LaserJet … */
		char *is = strstr(l, "' is a ");
		if (!strncmp(l, "device ", 7) && is) snprintf(scanners[n++], sizeof scanners[0], "%s", is + 7);
	}
	free(t);
	nscanners = n;
	worker_busy = 0;
	return NULL;
}

static void printers_done(int ok) { (void)ok; read_printers(); rm_confirm = 0; }

static void build_printers(PmUI *u)
{
	if (pm_ui_back(u, _("系統設定"))) { go(u, P_MAIN); return; }
	pm_ui_title(u, _("印表機與掃描器"), NULL);
	if (!pm_have("lpstat")) {
		pm_ui_group(u, NULL);
		pm_ui_info(u, _("沒有安裝列印服務（CUPS）"), NULL);
		if (!job && pm_ui_button(u, _("安裝後就能使用印表機"), _("安裝 CUPS"))) {
			static char *argv[12];
			const char *p = "cups";
			if (pm_pkg_argv("install-min", &p, 1, argv, 12)) run_admin(argv, _("安裝 CUPS"), printers_done);
		}
		pm_ui_end(u);
	} else {
		pm_ui_group(u, _("印表機"));
		if (!nprinters) pm_ui_info(u, _("還沒有印表機"), NULL);
		for (int i = 0; i < nprinters; i++) {
			Printer *p = &printers[i];
			char sub[160];
			const char *st = !strcmp(p->state, "printing") ? _("列印中") : p->enabled ? _("待機") : _("已停用");
			if (p->jobs) snprintf(sub, sizeof sub, _("%s · %d 個列印工作"), st, p->jobs);
			else snprintf(sub, sizeof sub, "%s", st);
			if (pm_ui_item(u, "printer", p->name, sub, p->def ? _("預設") : NULL, !strcmp(sel_printer, p->name))) {
				snprintf(sel_printer, sizeof sel_printer, "%s", strcmp(sel_printer, p->name) ? p->name : "");
				rm_confirm = 0;
				u->again = 1;
			}
		}
		pm_ui_end(u);
		Printer *sp = NULL;
		for (int i = 0; i < nprinters; i++) if (!strcmp(printers[i].name, sel_printer)) sp = &printers[i];
		if (sp && !job) {
			static char pname[96];
			snprintf(pname, sizeof pname, "%s", sp->name);
			pm_ui_group(u, sp->name);
			if (!sp->def && pm_ui_button(u, _("列印時預設使用這台"), _("設為預設"))) {
				char *argv[] = { "lpoptions", "-d", pname, NULL };
				run_cmd(argv, _("設為預設印表機"), printers_done);
			}
			if (!access("/usr/share/cups/data/testprint", R_OK) && pm_ui_button(u, _("印一張測試頁"), _("列印測試頁"))) {
				char *argv[] = { "lp", "-d", pname, "/usr/share/cups/data/testprint", NULL };
				run_cmd(argv, _("列印測試頁"), printers_done);
			}
			if (sp->jobs && pm_ui_button(u, _("取消這台印表機的所有列印工作"), _("全部取消"))) {
				char *argv[] = { "cancel", "-a", pname, NULL };
				run_cmd(argv, _("取消列印工作"), printers_done);
			}
			if (!rm_confirm) {
				if (pm_ui_button(u, _("從這台電腦移除這台印表機"), _("移除…"))) { rm_confirm = 1; u->again = 1; }
			} else if (pm_ui_button(u, _("確定要移除嗎？"), _("移除"))) {
				char *argv[] = { "lpadmin", "-x", pname, NULL };
				run_admin(argv, _("移除印表機"), printers_done);
				sel_printer[0] = 0;
			}
			pm_ui_end(u);
		}
		pm_ui_group(u, NULL);
		if (pm_ui_button(u, _("新增 USB 或網路印表機"), _("新增印表機…"))) {
			if (pm_have("system-config-printer")) pm_spawn("system-config-printer");
			else {
				char *argv[] = { "pomelo-open", "http://localhost:631/admin", NULL };
				spawn_argv(argv);
			}
		}
		if (pm_ui_button(u, NULL, _("重新整理"))) read_printers();
		pm_ui_end(u);
		pm_ui_note(u, _("同一個網路上支援 AirPrint／IPP 的印表機通常會自動出現。沒有 system-config-printer 時，會用瀏覽器開啟 CUPS 管理頁面。"));
	}

	pm_ui_group(u, _("掃描器"));
	if (!pm_have("scanimage")) {
		if (!job && pm_ui_button(u, _("需要 sane-utils 才能尋找掃描器"), _("安裝"))) {
			static char *argv[12];
			const char *p = "sane-utils";
			if (pm_pkg_argv("install-min", &p, 1, argv, 12)) run_admin(argv, _("安裝掃描器工具"), printers_done);
		}
	}
	else if (worker_started) pm_ui_info(u, _("正在尋找…"), NULL);
	else if (nscanners < 0) { if (pm_ui_button(u, _("尋找連接的掃描器"), _("尋找"))) start_worker(scan_thread, NULL); }
	else if (!nscanners) pm_ui_info(u, _("找不到掃描器"), NULL);
	else for (int i = 0; i < nscanners; i++) pm_ui_item(u, "scanner", scanners[i], NULL, NULL, 0);
	if (pm_have("simple-scan") && pm_ui_item(u, "org.gnome.SimpleScan", _("開啟「文件掃描器」"), NULL, "›", 0))
		pm_spawn("simple-scan");
	pm_ui_end(u);
	if (job) {
		char t[300];
		snprintf(t, sizeof t, _("正在%s，請稍候…"), job_title);
		pm_ui_note(u, t);
	}
}

/* ------------------------------------------------------------------ */
/* 視訊（攝影機）：V4L2                                                */
/* ------------------------------------------------------------------ */

typedef struct { char dev[32], name[128], driver[32], bus[64]; } Camera;
static Camera cams[8];
static int ncams, cam_sel;
static char cam_users[512];          /* 正在用攝影機的程式 */
static int cam_blocked;              /* 已用 modprobe 黑名單停用 */

#define CAM_OFF_FILE "/etc/modprobe.d/pomelo-camera-off.conf"

static void read_cameras(void)
{
	ncams = 0;
	for (int i = 0; i < 64 && ncams < (int)LEN(cams); i++) {
		char dev[32];
		snprintf(dev, sizeof dev, "/dev/video%d", i);
		int fd = open(dev, O_RDWR | O_NONBLOCK);
		if (fd < 0) continue;
		struct v4l2_capability cap;
		memset(&cap, 0, sizeof cap);
		if (!ioctl(fd, VIDIOC_QUERYCAP, &cap)) {
			unsigned caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
			/* 一台攝影機常有好幾個節點，只列能擷取影像的 */
			if ((caps & V4L2_CAP_VIDEO_CAPTURE) && (caps & V4L2_CAP_STREAMING)) {
				Camera *c = &cams[ncams++];
				snprintf(c->dev, sizeof c->dev, "%s", dev);
				snprintf(c->name, sizeof c->name, "%s", (char *)cap.card);
				snprintf(c->driver, sizeof c->driver, "%s", (char *)cap.driver);
				snprintf(c->bus, sizeof c->bus, "%s", (char *)cap.bus_info);
			}
		}
		close(fd);
	}
	if (cam_sel >= ncams) cam_sel = 0;
	cam_blocked = !access(CAM_OFF_FILE, F_OK);
	/* 誰開著 /dev/video*（只看得到自己的程式） */
	cam_users[0] = 0;
	DIR *d = opendir("/proc");
	struct dirent *e;
	while (d && (e = readdir(d))) {
		if (!isdigit((unsigned char)e->d_name[0]) || atoi(e->d_name) == getpid()) continue;
		char p[64];
		snprintf(p, sizeof p, "/proc/%s/fd", e->d_name);
		DIR *fdd = opendir(p);
		struct dirent *f;
		int uses = 0;
		while (fdd && (f = readdir(fdd)) && !uses) {
			char link[300], target[64] = "";
			snprintf(link, sizeof link, "%s/%s", p, f->d_name);
			ssize_t k = readlink(link, target, sizeof target - 1);
			if (k > 0) { target[k] = 0; uses = !strncmp(target, "/dev/video", 10); }
		}
		if (fdd) closedir(fdd);
		if (!uses) continue;
		char comm[64] = "", cp[64];
		snprintf(cp, sizeof cp, "/proc/%s/comm", e->d_name);
		FILE *cf = fopen(cp, "r");
		if (cf) { if (fgets(comm, sizeof comm, cf)) comm[strcspn(comm, "\n")] = 0; fclose(cf); }
		if (comm[0] && !strstr(cam_users, comm) && strlen(cam_users) + strlen(comm) + 3 < sizeof cam_users) {
			if (cam_users[0]) strcat(cam_users, "、");
			strcat(cam_users, comm);
		}
	}
	if (d) closedir(d);
}

/* ---------------- 預覽 ---------------- */
static int pv_fd = -1, pv_w, pv_h;
static unsigned pv_fmt;
static struct { void *p; size_t len; } pv_buf[4];
static int pv_nbuf;
static cairo_surface_t *pv_img;
static char pv_err[160];

static void preview_stop(void)
{
	if (pv_fd < 0) return;
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	ioctl(pv_fd, VIDIOC_STREAMOFF, &type);
	for (int i = 0; i < pv_nbuf; i++) munmap(pv_buf[i].p, pv_buf[i].len);
	pv_nbuf = 0;
	close(pv_fd);
	pv_fd = -1;
	if (pv_img) { cairo_surface_destroy(pv_img); pv_img = NULL; }
}

static void preview_start(const char *dev)
{
	preview_stop();
	pv_err[0] = 0;
	pv_fd = open(dev, O_RDWR | O_NONBLOCK);
	if (pv_fd < 0) {
		snprintf(pv_err, sizeof pv_err, "%s", errno == EBUSY ? _("攝影機正在被其他程式使用") : _("無法開啟攝影機"));
		return;
	}
	/* 先試 YUYV（不用解碼），不行再用 MJPEG */
	static const unsigned fmts[] = { V4L2_PIX_FMT_YUYV, V4L2_PIX_FMT_MJPEG };
	struct v4l2_format f;
	int ok = 0;
	for (unsigned i = 0; i < LEN(fmts) && !ok; i++) {
		memset(&f, 0, sizeof f);
		f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		f.fmt.pix.width = 640;
		f.fmt.pix.height = 480;
		f.fmt.pix.pixelformat = fmts[i];
		f.fmt.pix.field = V4L2_FIELD_ANY;
		ok = !ioctl(pv_fd, VIDIOC_S_FMT, &f) && f.fmt.pix.pixelformat == fmts[i];
	}
	if (!ok) { snprintf(pv_err, sizeof pv_err, "%s", _("不支援這台攝影機的影像格式")); preview_stop(); return; }
	pv_w = f.fmt.pix.width;
	pv_h = f.fmt.pix.height;
	pv_fmt = f.fmt.pix.pixelformat;
	struct v4l2_requestbuffers rb = { .count = 4, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP };
	if (ioctl(pv_fd, VIDIOC_REQBUFS, &rb) || rb.count < 2) { snprintf(pv_err, sizeof pv_err, "%s", _("無法開啟攝影機")); preview_stop(); return; }
	for (unsigned i = 0; i < rb.count && i < LEN(pv_buf); i++) {
		struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP, .index = i };
		if (ioctl(pv_fd, VIDIOC_QUERYBUF, &b)) break;
		pv_buf[i].len = b.length;
		pv_buf[i].p = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, pv_fd, b.m.offset);
		if (pv_buf[i].p == MAP_FAILED) break;
		pv_nbuf++;
		ioctl(pv_fd, VIDIOC_QBUF, &b);
	}
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (pv_nbuf < 2 || ioctl(pv_fd, VIDIOC_STREAMON, &type)) {
		snprintf(pv_err, sizeof pv_err, "%s", _("無法開啟攝影機"));
		preview_stop();
	}
}

static inline uint8_t clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v; }

/* 取出一張畫面（沒有新畫面就保留上一張） */
static void preview_poll(void)
{
	if (pv_fd < 0) return;
	struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP };
	if (ioctl(pv_fd, VIDIOC_DQBUF, &b)) return;   /* EAGAIN：還沒有 */
	const uint8_t *src = pv_buf[b.index].p;
	if (pv_fmt == V4L2_PIX_FMT_MJPEG) {
		cairo_surface_t *s = pm_image_jpeg_mem(src, b.bytesused, 0);
		if (s) { if (pv_img) cairo_surface_destroy(pv_img); pv_img = s; }
	} else if (b.bytesused >= (unsigned)(pv_w * pv_h * 2)) {
		/* YUYV → RGB（BT.601），左右鏡像，跟照鏡子一樣 */
		if (!pv_img) pv_img = cairo_image_surface_create(CAIRO_FORMAT_RGB24, pv_w, pv_h);
		cairo_surface_flush(pv_img);
		uint8_t *dst = cairo_image_surface_get_data(pv_img);
		int stride = cairo_image_surface_get_stride(pv_img);
		for (int y = 0; y < pv_h; y++) {
			const uint8_t *s = src + (size_t)y * pv_w * 2;
			uint32_t *d = (uint32_t *)(dst + (size_t)y * stride);
			for (int x = 0; x + 1 < pv_w; x += 2, s += 4) {
				int u = s[1] - 128, v = s[3] - 128;
				for (int k = 0; k < 2; k++) {
					int yy = (s[k * 2] - 16) * 298;
					int r = (yy + 409 * v + 128) >> 8, g = (yy - 100 * u - 208 * v + 128) >> 8, bl = (yy + 516 * u + 128) >> 8;
					d[pv_w - 1 - (x + k)] = (clamp8(r) << 16) | (clamp8(g) << 8) | clamp8(bl);
				}
			}
		}
		cairo_surface_mark_dirty(pv_img);
	}
	ioctl(pv_fd, VIDIOC_QBUF, &b);
}

/* ---------------- 影像調整（V4L2 控制項） ---------------- */
static const struct { unsigned id; const char *name, *key; } cam_ctrls[] = {
	{ V4L2_CID_BRIGHTNESS, N_("亮度"), "brightness" },
	{ V4L2_CID_CONTRAST, N_("對比"), "contrast" },
	{ V4L2_CID_SATURATION, N_("飽和度"), "saturation" },
	{ V4L2_CID_SHARPNESS, N_("銳利度"), "sharpness" },
	{ V4L2_CID_AUTO_WHITE_BALANCE, N_("自動白平衡"), "auto_wb" },
};

/* 設定檔裡的鍵：camera.<裝置名稱>.<控制項>，名稱只留英數字 */
static void cam_key(const Camera *c, const char *ctrl, char *out, size_t n)
{
	char name[64];
	size_t k = 0;
	for (const char *p = c->name; *p && k < sizeof name - 1; p++)
		name[k++] = isalnum((unsigned char)*p) ? (char)tolower((unsigned char)*p) : '_';
	name[k] = 0;
	snprintf(out, n, "camera.%s.%s", name, ctrl);
}

/* 登入時套用保存的設定（pomelo-system --apply） */
static void apply_cameras(void)
{
	read_cameras();
	for (int i = 0; i < ncams; i++) {
		int fd = open(cams[i].dev, O_RDWR | O_NONBLOCK);
		if (fd < 0) continue;
		for (unsigned k = 0; k < LEN(cam_ctrls); k++) {
			char key[160];
			cam_key(&cams[i], cam_ctrls[k].key, key, sizeof key);
			const char *v = pm_conf_get(key, NULL);
			if (!v || !*v) continue;   /* 空的 = 用攝影機預設值 */
			struct v4l2_control c = { .id = cam_ctrls[k].id, .value = atoi(v) };
			ioctl(fd, VIDIOC_S_CTRL, &c);
		}
		close(fd);
	}
}

static void camera_done(int ok) { (void)ok; read_cameras(); }

static void build_camera(PmUI *u)
{
	if (pm_ui_back(u, _("系統設定"))) { preview_stop(); go(u, P_MAIN); return; }
	pm_ui_title(u, _("視訊"), NULL);

	if (cam_blocked) {
		pm_ui_group(u, NULL);
		int on = 0;
		if (pm_ui_switch(u, _("允許使用攝影機"), &on) && on && !job) {
			char *argv[] = { "sh", "-c", "rm -f " CAM_OFF_FILE "; modprobe uvcvideo", NULL };
			run_admin(argv, _("啟用攝影機"), camera_done);
		}
		pm_ui_end(u);
		pm_ui_note(u, _("攝影機已經停用，所有程式都無法使用。"));
		return;
	}
	if (!ncams) {
		pm_ui_group(u, NULL);
		pm_ui_info(u, _("沒有偵測到攝影機"), NULL);
		pm_ui_end(u);
		pm_ui_note(u, _("插上 USB 攝影機後，回到這一頁就會出現。"));
		return;
	}

	/* 選哪一台 */
	static const char *names[LEN(cams)];
	for (int i = 0; i < ncams; i++) names[i] = cams[i].name;
	Camera *c = &cams[cam_sel];
	pm_ui_group(u, NULL);
	int old = cam_sel;
	if (ncams > 1 && pm_ui_choice(u, _("攝影機"), names, ncams, &cam_sel) && old != cam_sel) {
		preview_stop();
		c = &cams[cam_sel];
	}
	if (ncams == 1) pm_ui_info(u, _("攝影機"), c->name);
	char sub[160];
	snprintf(sub, sizeof sub, "%s · %s", c->dev, c->driver);
	pm_ui_info(u, _("裝置"), sub);
	pm_ui_end(u);

	/* 預覽 */
	pm_ui_group(u, _("預覽"));
	if (pv_fd >= 0) {
		pm_ui_image(u, pv_img, 480, 360);
		if (pm_ui_button(u, NULL, _("停止預覽"))) preview_stop();
	} else {
		if (pv_err[0]) pm_ui_info(u, pv_err, NULL);
		if (pm_ui_button(u, NULL, _("開始預覽"))) preview_start(c->dev);
	}
	pm_ui_end(u);

	/* 影像調整：只顯示這台攝影機有支援的 */
	int fd = pv_fd >= 0 ? pv_fd : open(c->dev, O_RDWR | O_NONBLOCK);
	if (fd >= 0) {
		int shown = 0;
		for (unsigned k = 0; k < LEN(cam_ctrls); k++) {
			struct v4l2_queryctrl q = { .id = cam_ctrls[k].id };
			if (ioctl(fd, VIDIOC_QUERYCTRL, &q) || (q.flags & V4L2_CTRL_FLAG_DISABLED)) continue;
			struct v4l2_control cur = { .id = q.id };
			if (ioctl(fd, VIDIOC_G_CTRL, &cur)) continue;
			if (!shown++) pm_ui_group(u, _("影像調整"));
			char key[160], val[32];
			cam_key(c, cam_ctrls[k].key, key, sizeof key);
			int changed = 0, nv = cur.value;
			if (q.type == V4L2_CTRL_TYPE_BOOLEAN) {
				int on = cur.value != 0;
				if (pm_ui_switch(u, _(cam_ctrls[k].name), &on)) { nv = on; changed = 1; }
			} else if (q.maximum > q.minimum) {
				double v = cur.value;
				if (pm_ui_slider(u, _(cam_ctrls[k].name), &v, q.minimum, q.maximum)) { nv = (int)(v + 0.5); changed = 1; }
			}
			if (changed) {
				struct v4l2_control sc = { .id = q.id, .value = nv };
				ioctl(fd, VIDIOC_S_CTRL, &sc);
				snprintf(val, sizeof val, "%d", nv);
				pm_conf_set(key, val);
			}
		}
		if (shown) {
			if (pm_ui_button(u, _("恢復這台攝影機的預設值"), _("重設"))) {
				for (unsigned k = 0; k < LEN(cam_ctrls); k++) {
					struct v4l2_queryctrl q = { .id = cam_ctrls[k].id };
					if (ioctl(fd, VIDIOC_QUERYCTRL, &q)) continue;
					struct v4l2_control sc = { .id = q.id, .value = q.default_value };
					ioctl(fd, VIDIOC_S_CTRL, &sc);
					char key[160];
					cam_key(c, cam_ctrls[k].key, key, sizeof key);
					pm_conf_set(key, "");
				}
			}
			pm_ui_end(u);
		}
		if (fd != pv_fd) close(fd);
	}

	/* 隱私 */
	pm_ui_group(u, _("隱私"));
	pm_ui_info(u, _("正在使用攝影機"), cam_users[0] ? cam_users : _("沒有"));
	int on = 1;
	if (pm_ui_switch(u, _("允許使用攝影機"), &on) && !on && !job) {
		preview_stop();
		char *argv[] = { "sh", "-c", "echo 'blacklist uvcvideo' > " CAM_OFF_FILE "; modprobe -r uvcvideo", NULL };
		run_admin(argv, _("停用攝影機"), camera_done);
	}
	pm_ui_end(u);
	pm_ui_note(u, _("關閉後所有程式都無法使用攝影機（需要管理員密碼）；正在使用的程式要先關掉才能停用。"));
}

/* ------------------------------------------------------------------ */
/* 首頁圖示：macOS 風格的彩色圓角方塊 + 白色圖形                        */
/* ------------------------------------------------------------------ */

static cairo_t *tile(cairo_surface_t **out, unsigned top, unsigned bottom)
{
	*out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 56, 56);
	cairo_t *cr = cairo_create(*out);
	pm_rrect(cr, 2, 2, 52, 52, 12, 12);
	cairo_pattern_t *g = cairo_pattern_create_linear(0, 2, 0, 54);
	cairo_pattern_add_color_stop_rgb(g, 0, ((top >> 16) & 255) / 255.0, ((top >> 8) & 255) / 255.0, (top & 255) / 255.0);
	cairo_pattern_add_color_stop_rgb(g, 1, ((bottom >> 16) & 255) / 255.0, ((bottom >> 8) & 255) / 255.0, (bottom & 255) / 255.0);
	cairo_set_source(cr, g);
	cairo_fill(cr);
	cairo_pattern_destroy(g);
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_set_line_width(cr, 3.2);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
	return cr;
}

static void make_icons(void)
{
	cairo_surface_t *s;
	cairo_t *cr;
	/* 一般：齒輪 */
	cr = tile(&s, 0x9a9aa0, 0x6e6e74);
	for (int i = 0; i < 8; i++) {
		cairo_save(cr); cairo_translate(cr, 28, 28); cairo_rotate(cr, i * M_PI / 4);
		cairo_rectangle(cr, -3.5, -17, 7, 7); cairo_fill(cr); cairo_restore(cr);
	}
	cairo_arc(cr, 28, 28, 12, 0, 2 * M_PI); cairo_fill(cr);
	cairo_set_source_rgb(cr, 0.45, 0.45, 0.49); cairo_arc(cr, 28, 28, 5, 0, 2 * M_PI); cairo_fill(cr);
	cairo_destroy(cr); pm_ui_add_icon("@general", s);
	/* 外觀：半黑半白的圓 */
	cr = tile(&s, 0x3a3a3c, 0x1c1c1e);
	cairo_arc(cr, 28, 28, 14, 0, 2 * M_PI); cairo_stroke(cr);
	cairo_arc(cr, 28, 28, 14, -M_PI / 2, M_PI / 2); cairo_fill(cr);
	cairo_destroy(cr); pm_ui_add_icon("@appearance", s);
	/* 背景圖片：山與太陽 */
	cr = tile(&s, 0x5ac8fa, 0x0a84ff);
	cairo_move_to(cr, 10, 42); cairo_line_to(cr, 22, 24); cairo_line_to(cr, 30, 34); cairo_line_to(cr, 36, 27);
	cairo_line_to(cr, 46, 42); cairo_close_path(cr); cairo_fill(cr);
	cairo_arc(cr, 39, 16, 4.5, 0, 2 * M_PI); cairo_fill(cr);
	cairo_destroy(cr); pm_ui_add_icon("@wallpaper", s);
	/* 桌面與 Dock：螢幕下方一條 Dock */
	cr = tile(&s, 0x3a3a3c, 0x1c1c1e);
	pm_rrect(cr, 10, 13, 36, 26, 3, 3); cairo_stroke(cr);
	pm_rrect(cr, 17, 33, 22, 4, 2, 2); cairo_fill(cr);
	cairo_destroy(cr); pm_ui_add_icon("@dock", s);
	/* 選單列：螢幕上方一條 */
	cr = tile(&s, 0x3a3a3c, 0x1c1c1e);
	pm_rrect(cr, 10, 13, 36, 30, 3, 3); cairo_stroke(cr);
	cairo_rectangle(cr, 10, 13, 36, 6); cairo_fill(cr);
	cairo_destroy(cr); pm_ui_add_icon("@menubar", s);
	/* 多桌面：四個小桌面，其中一個是目前的 */
	cr = tile(&s, 0x5e5ce6, 0x3634a3);
	pm_rrect(cr, 10, 12, 16, 13, 2.5, 2.5); cairo_fill(cr);
	pm_rrect(cr, 30, 12, 16, 13, 2.5, 2.5); cairo_stroke(cr);
	pm_rrect(cr, 10, 31, 16, 13, 2.5, 2.5); cairo_stroke(cr);
	pm_rrect(cr, 30, 31, 16, 13, 2.5, 2.5); cairo_stroke(cr);
	cairo_destroy(cr); pm_ui_add_icon("@workspace", s);
	/* 顯示器 */
	cr = tile(&s, 0x0a84ff, 0x0060df);
	pm_rrect(cr, 10, 13, 36, 24, 3, 3); cairo_stroke(cr);
	cairo_move_to(cr, 22, 44); cairo_line_to(cr, 34, 44); cairo_stroke(cr);
	cairo_move_to(cr, 28, 37); cairo_line_to(cr, 28, 44); cairo_stroke(cr);
	cairo_destroy(cr); pm_ui_add_icon("@display", s);
	/* Spotlight：放大鏡 */
	cr = tile(&s, 0x8e8e93, 0x636366);
	cairo_arc(cr, 25, 25, 9, 0, 2 * M_PI); cairo_stroke(cr);
	cairo_set_line_width(cr, 4.5); cairo_move_to(cr, 32, 32); cairo_line_to(cr, 41, 41); cairo_stroke(cr);
	cairo_destroy(cr); pm_ui_add_icon("@spotlight", s);
	/* 應用程式選單：九宮格 */
	cr = tile(&s, 0xff9f0a, 0xf27d00);
	for (int y = 0; y < 3; y++) for (int x = 0; x < 3; x++) { pm_rrect(cr, 14 + x * 10, 14 + y * 10, 7, 7, 2, 2); cairo_fill(cr); }
	cairo_destroy(cr); pm_ui_add_icon("@apps", s);
	/* 登入畫面：大頭像加一條密碼框 */
	cr = tile(&s, 0x5e5ce6, 0x3634a3);
	cairo_arc(cr, 28, 21, 8, 0, 2 * M_PI); cairo_fill(cr);
	pm_rrect(cr, 13, 34, 30, 8, 4, 4); cairo_stroke(cr);
	for (int i = 0; i < 3; i++) { cairo_arc(cr, 21 + i * 7, 38, 1.6, 0, 2 * M_PI); cairo_fill(cr); }
	cairo_destroy(cr); pm_ui_add_icon("@login", s);
	/* 通知：鈴鐺 */
	cr = tile(&s, 0xff453a, 0xd70015);
	cairo_move_to(cr, 16, 37); cairo_curve_to(cr, 19, 33, 18, 28, 19, 24); cairo_curve_to(cr, 20, 14, 36, 14, 37, 24);
	cairo_curve_to(cr, 38, 28, 37, 33, 40, 37); cairo_close_path(cr); cairo_fill(cr);
	cairo_arc(cr, 28, 40, 3.5, 0, 2 * M_PI); cairo_fill(cr);
	cairo_destroy(cr); pm_ui_add_icon("@notify", s);
	/* 聲音：喇叭 */
	cr = tile(&s, 0xff375f, 0xd30f45);
	cairo_move_to(cr, 12, 23); cairo_line_to(cr, 19, 23); cairo_line_to(cr, 27, 15); cairo_line_to(cr, 27, 41);
	cairo_line_to(cr, 19, 33); cairo_line_to(cr, 12, 33); cairo_close_path(cr); cairo_fill(cr);
	cairo_arc(cr, 28, 28, 8, -0.8, 0.8); cairo_stroke(cr);
	cairo_arc(cr, 28, 28, 14, -0.8, 0.8); cairo_stroke(cr);
	cairo_destroy(cr); pm_ui_add_icon("@sound", s);
	/* 鍵盤 */
	cr = tile(&s, 0x8e8e93, 0x636366);
	pm_rrect(cr, 8, 17, 40, 22, 4, 4); cairo_stroke(cr);
	for (int r = 0; r < 2; r++) for (int c = 0; c < 5; c++) { cairo_rectangle(cr, 13 + c * 6.5, 22 + r * 5.5, 3.5, 3); cairo_fill(cr); }
	cairo_rectangle(cr, 18, 33, 20, 2.5); cairo_fill(cr);
	cairo_destroy(cr); pm_ui_add_icon("@keyboard", s);
	/* 滑鼠 */
	cr = tile(&s, 0x8e8e93, 0x636366);
	pm_rrect(cr, 18, 11, 20, 34, 10, 10); cairo_stroke(cr);
	cairo_move_to(cr, 28, 13); cairo_line_to(cr, 28, 22); cairo_stroke(cr);
	cairo_destroy(cr); pm_ui_add_icon("@mouse", s);
	/* 螢幕使用時間：沙漏 */
	cr = tile(&s, 0xbf5af2, 0x8944ab);
	cairo_move_to(cr, 17, 12); cairo_line_to(cr, 39, 12); cairo_line_to(cr, 29.5, 28); cairo_line_to(cr, 39, 44);
	cairo_line_to(cr, 17, 44); cairo_line_to(cr, 26.5, 28); cairo_close_path(cr); cairo_fill(cr);
	cairo_destroy(cr); pm_ui_add_icon("@screentime", s);
	/* 使用者與群組：兩個人 */
	cr = tile(&s, 0x0a84ff, 0x0060df);
	cairo_arc(cr, 22, 22, 6, 0, 2 * M_PI); cairo_fill(cr);
	cairo_arc(cr, 22, 42, 11, M_PI, 2 * M_PI); cairo_fill(cr);
	cairo_set_source_rgba(cr, 1, 1, 1, 0.75);
	cairo_arc(cr, 36, 21, 5, 0, 2 * M_PI); cairo_fill(cr);
	cairo_arc(cr, 37, 40, 9, M_PI, 2 * M_PI); cairo_fill(cr);
	cairo_destroy(cr); pm_ui_add_icon("@users", s);
	/* 印表機 */
	cr = tile(&s, 0x8e8e93, 0x636366);
	cairo_rectangle(cr, 18, 11, 20, 9); cairo_stroke(cr);
	pm_rrect(cr, 10, 20, 36, 16, 3, 3); cairo_fill(cr);
	cairo_set_source_rgb(cr, 0.45, 0.45, 0.49); cairo_rectangle(cr, 18, 30, 20, 4); cairo_fill(cr);
	cairo_set_source_rgb(cr, 1, 1, 1); cairo_rectangle(cr, 18, 34, 20, 10); cairo_fill(cr);
	cairo_destroy(cr); pm_ui_add_icon("@printer", s);
	/* 網路：地球 */
	cr = tile(&s, 0x0a84ff, 0x0060df);
	cairo_set_line_width(cr, 2.6);
	cairo_arc(cr, 28, 28, 14, 0, 2 * M_PI); cairo_stroke(cr);
	cairo_save(cr); cairo_translate(cr, 28, 28); cairo_scale(cr, 0.45, 1); cairo_arc(cr, 0, 0, 14, 0, 2 * M_PI); cairo_restore(cr); cairo_stroke(cr);
	cairo_move_to(cr, 14, 28); cairo_line_to(cr, 42, 28); cairo_stroke(cr);
	cairo_destroy(cr); pm_ui_add_icon("@network", s);
	/* 藍牙 */
	cr = tile(&s, 0x0a84ff, 0x0060df);
	cairo_move_to(cr, 20, 21); cairo_line_to(cr, 35, 35); cairo_line_to(cr, 28, 42); cairo_line_to(cr, 28, 14);
	cairo_line_to(cr, 35, 21); cairo_line_to(cr, 20, 35); cairo_stroke(cr);
	cairo_destroy(cr); pm_ui_add_icon("@bluetooth", s);
	/* 電源與鎖定：鎖頭 */
	cr = tile(&s, 0x30d158, 0x248a3d);
	cairo_arc(cr, 28, 23, 7, M_PI, 2 * M_PI); cairo_line_to(cr, 35, 27); cairo_move_to(cr, 21, 27); cairo_line_to(cr, 21, 23); cairo_stroke(cr);
	pm_rrect(cr, 16, 27, 24, 17, 3, 3); cairo_fill(cr);
	cairo_destroy(cr); pm_ui_add_icon("@power", s);
	/* 螢幕保護程式：月亮與星星 */
	cr = tile(&s, 0x5e5ce6, 0x2d2b8f);
	cairo_set_source_rgb(cr, 1, 0.92, 0.6);
	cairo_arc(cr, 26, 29, 11, 0, 2 * M_PI); cairo_fill(cr);
	cairo_set_source_rgb(cr, 0.25, 0.23, 0.6);
	cairo_arc(cr, 31, 25, 10, 0, 2 * M_PI); cairo_fill(cr);
	cairo_set_source_rgb(cr, 1, 1, 1);
	for (int i = 0; i < 4; i++) { cairo_arc(cr, 36 + (i * 7) % 12, 13 + (i * 11) % 24, 1.3, 0, 7); cairo_fill(cr); }
	cairo_destroy(cr); pm_ui_add_icon("@saver", s);
	/* 健康檢查：心＋心電圖 */
	cr = tile(&s, 0xff5f6d, 0xd92d4a);
	cairo_move_to(cr, 28, 42);
	cairo_curve_to(cr, 8, 30, 12, 13, 28, 21);
	cairo_curve_to(cr, 44, 13, 48, 30, 28, 42);
	cairo_fill(cr);
	cairo_set_source_rgb(cr, 0.85, 0.18, 0.29);
	cairo_set_line_width(cr, 2.6);
	cairo_move_to(cr, 14, 29); cairo_line_to(cr, 22, 29); cairo_line_to(cr, 25, 22); cairo_line_to(cr, 30, 35); cairo_line_to(cr, 33, 29); cairo_line_to(cr, 42, 29);
	cairo_stroke(cr);
	cairo_destroy(cr); pm_ui_add_icon("@health", s);
	/* 防火牆：盾牌＋磚牆 */
	cr = tile(&s, 0xffb340, 0xe8730f);
	cairo_move_to(cr, 28, 11); cairo_curve_to(cr, 34, 15, 40, 16, 44, 16); cairo_line_to(cr, 44, 29); cairo_curve_to(cr, 44, 39, 37, 45, 28, 48); cairo_curve_to(cr, 19, 45, 12, 39, 12, 29); cairo_line_to(cr, 12, 16); cairo_curve_to(cr, 16, 16, 22, 15, 28, 11);
	cairo_fill(cr);
	cairo_set_source_rgb(cr, 0.91, 0.45, 0.06); cairo_set_line_width(cr, 2.4);
	for (int i = 0; i < 3; i++) { cairo_move_to(cr, 16, 23 + i * 7); cairo_line_to(cr, 40, 23 + i * 7); }
	cairo_move_to(cr, 25, 23); cairo_line_to(cr, 25, 30); cairo_move_to(cr, 33, 30); cairo_line_to(cr, 33, 37);
	cairo_stroke(cr);
	cairo_destroy(cr); pm_ui_add_icon("@firewall", s);
	/* 健康檢查各類的燈號（綠、藍、黃、紅、灰） */
	{
		static const unsigned col[] = { 0x34c759, 0x0a84ff, 0xff9f0a, 0xff3b30, 0xb0b0b5 };
		static const char *nm[] = { "@hok", "@hinfo", "@hwarn", "@hbad", "@hskip" };
		for (int i = 0; i < 5; i++) {
			s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 56, 56);
			cr = cairo_create(s);
			cairo_arc(cr, 28, 28, 13, 0, 2 * M_PI);
			cairo_set_source_rgb(cr, ((col[i] >> 16) & 255) / 255.0, ((col[i] >> 8) & 255) / 255.0, (col[i] & 255) / 255.0);
			cairo_fill(cr);
			cairo_destroy(cr);
			pm_ui_add_icon(nm[i], s);
		}
	}
	/* 控制中心：兩個開關 */
	cr = tile(&s, 0x8e8e93, 0x636366);
	pm_rrect(cr, 12, 15, 32, 11, 5.5, 5.5); cairo_stroke(cr);
	pm_rrect(cr, 12, 31, 32, 11, 5.5, 5.5); cairo_stroke(cr);
	cairo_arc(cr, 38, 20.5, 3.5, 0, 2 * M_PI); cairo_fill(cr);
	cairo_arc(cr, 18, 36.5, 3.5, 0, 2 * M_PI); cairo_fill(cr);
	cairo_destroy(cr); pm_ui_add_icon("@control", s);
	/* 視訊：攝影機 */
	cr = tile(&s, 0x30d158, 0x248a3d);
	pm_rrect(cr, 9, 18, 26, 20, 4, 4); cairo_fill(cr);
	cairo_move_to(cr, 36, 25); cairo_line_to(cr, 47, 19); cairo_line_to(cr, 47, 37); cairo_line_to(cr, 36, 31);
	cairo_close_path(cr); cairo_fill(cr);
	cairo_destroy(cr); pm_ui_add_icon("@camera", s);
	/* 小幫手：漸層底＋四角星 */
	s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 56, 56);
	cr = cairo_create(s);
	pm_rrect(cr, 2, 2, 52, 52, 12, 12);
	cairo_pattern_t *ag = cairo_pattern_create_linear(0, 2, 56, 54);
	cairo_pattern_add_color_stop_rgb(ag, 0, 1.0, 0.62, 0.20);
	cairo_pattern_add_color_stop_rgb(ag, 1, 0.75, 0.35, 0.95);
	cairo_set_source(cr, ag);
	cairo_fill(cr);
	cairo_pattern_destroy(ag);
	cairo_set_source_rgb(cr, 1, 1, 1);
	double sx = 27, sy = 29, sr = 15;
	cairo_move_to(cr, sx, sy - sr);
	cairo_curve_to(cr, sx + 2, sy - 3, sx + 3, sy - 2, sx + sr, sy);
	cairo_curve_to(cr, sx + 3, sy + 2, sx + 2, sy + 3, sx, sy + sr);
	cairo_curve_to(cr, sx - 2, sy + 3, sx - 3, sy + 2, sx - sr, sy);
	cairo_curve_to(cr, sx - 3, sy - 2, sx - 2, sy - 3, sx, sy - sr);
	cairo_fill(cr);
	cairo_destroy(cr); pm_ui_add_icon("@assistant", s);
	/* 密碼與鑰匙圈：鑰匙 */
	cr = tile(&s, 0xffb340, 0xe8780a);
	cairo_set_line_width(cr, 3.4);
	cairo_arc(cr, 21, 23, 7.5, 0, 2 * M_PI); cairo_stroke(cr);
	cairo_move_to(cr, 26.5, 28.5); cairo_line_to(cr, 42, 44); cairo_stroke(cr);
	cairo_move_to(cr, 38, 40); cairo_line_to(cr, 42, 36); cairo_stroke(cr);
	cairo_move_to(cr, 33, 35); cairo_line_to(cr, 37, 31); cairo_stroke(cr);
	cairo_destroy(cr); pm_ui_add_icon("@keyring", s);
	/* 關於：電腦 */
	cr = tile(&s, 0x8e8e93, 0x636366);
	pm_rrect(cr, 11, 13, 34, 23, 3, 3); cairo_stroke(cr);
	cairo_move_to(cr, 20, 43); cairo_line_to(cr, 36, 43); cairo_stroke(cr);
	cairo_destroy(cr); pm_ui_add_icon("@about", s);
	/* 軟體更新：循環箭頭 */
	cr = tile(&s, 0x8e8e93, 0x636366);
	cairo_arc(cr, 28, 28, 12, -M_PI * 0.15, M_PI * 1.55); cairo_stroke(cr);
	cairo_move_to(cr, 40, 18); cairo_line_to(cr, 39, 26); cairo_line_to(cr, 31, 24); cairo_stroke(cr);
	cairo_destroy(cr); pm_ui_add_icon("@update", s);
	/* 儲存空間：硬碟 */
	cr = tile(&s, 0x8e8e93, 0x636366);
	pm_rrect(cr, 10, 18, 36, 20, 4, 4); cairo_stroke(cr);
	cairo_arc(cr, 38, 28, 2.5, 0, 2 * M_PI); cairo_fill(cr);
	cairo_destroy(cr); pm_ui_add_icon("@storage", s);
	/* 日期與時間：時鐘 */
	cr = tile(&s, 0x8e8e93, 0x636366);
	cairo_arc(cr, 28, 28, 14, 0, 2 * M_PI); cairo_stroke(cr);
	cairo_move_to(cr, 28, 19); cairo_line_to(cr, 28, 28); cairo_line_to(cr, 35, 32); cairo_stroke(cr);
	cairo_destroy(cr); pm_ui_add_icon("@datetime", s);
	/* 語言與地區：地球 */
	cr = tile(&s, 0x8e8e93, 0x636366);
	cairo_set_line_width(cr, 2.6);
	cairo_arc(cr, 28, 28, 14, 0, 2 * M_PI); cairo_stroke(cr);
	cairo_save(cr); cairo_translate(cr, 28, 28); cairo_scale(cr, 0.45, 1); cairo_arc(cr, 0, 0, 14, 0, 2 * M_PI); cairo_restore(cr); cairo_stroke(cr);
	cairo_move_to(cr, 15, 23); cairo_line_to(cr, 41, 23); cairo_move_to(cr, 15, 33); cairo_line_to(cr, 41, 33); cairo_stroke(cr);
	cairo_destroy(cr); pm_ui_add_icon("@language", s);
	/* 裝置管理：晶片 */
	cr = tile(&s, 0x8e8e93, 0x636366);
	pm_rrect(cr, 17, 17, 22, 22, 3, 3); cairo_fill(cr);
	cairo_set_line_width(cr, 2.4);
	for (int i = 0; i < 3; i++) {
		double o = 21 + i * 7;
		cairo_move_to(cr, o, 11); cairo_line_to(cr, o, 17); cairo_move_to(cr, o, 39); cairo_line_to(cr, o, 45);
		cairo_move_to(cr, 11, o); cairo_line_to(cr, 17, o); cairo_move_to(cr, 39, o); cairo_line_to(cr, 45, o);
	}
	cairo_stroke(cr);
	cairo_destroy(cr); pm_ui_add_icon("@devices", s);
}

/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* 登入畫面（pomelo-dm 的外觀與文字；存在 /etc/pomelo/dm.conf，要管理員權限）     */
/* ------------------------------------------------------------------ */

typedef struct {
	int bg, clock, large, user_list, show_other, power;   /* bg：0 使用者的桌布 1 指定圖片 2 漸層；clock：0 24h 1 12h 2 不顯示 */
	char msg[320], hide[400], autologin[64];
} LoginConf;
static LoginConf lc, lc_orig;
static char lc_img[PATH_MAX];            /* 選了新的背景圖（還沒套用） */
static cairo_surface_t *lc_thumbs[24];
static char lc_paths[24][PATH_MAX];
static const char *lc_names[24];
static int lc_nthumbs, lc_pick = -1;
static char lc_custom[PATH_MAX];

static const char *dm_conf_path(void) { const char *p = getenv("POMELO_DM_CONF"); return p && *p ? p : "/etc/pomelo/dm.conf"; }
static const char *dm_state_dir(void) { const char *p = getenv("POMELO_DM_STATE"); return p && *p ? p : "/var/lib/pomelo-dm"; }

static void read_login(void)
{
	memset(&lc, 0, sizeof lc);
	lc.user_list = lc.power = 1;
	FILE *f = fopen(dm_conf_path(), "r");
	char line[640];
	while (f && fgets(line, sizeof line, f)) {
		line[strcspn(line, "\n")] = 0;
		char *eq = strchr(line, '=');
		if (line[0] == '#' || !eq) continue;
		*eq = 0;
		const char *k = line, *v = eq + 1;
		if (!strcmp(k, "background")) lc.bg = !strcmp(v, "image") ? 1 : !strcmp(v, "gradient") ? 2 : 0;
		else if (!strcmp(k, "clock")) lc.clock = !strcmp(v, "12h") ? 1 : !strcmp(v, "off") ? 2 : 0;
		else if (!strcmp(k, "text-size")) lc.large = !strcmp(v, "large");
		else if (!strcmp(k, "user-list")) lc.user_list = strcmp(v, "false") != 0;
		else if (!strcmp(k, "show-other-user")) lc.show_other = !strcmp(v, "true");
		else if (!strcmp(k, "power-buttons")) lc.power = strcmp(v, "false") != 0;
		else if (!strcmp(k, "message")) snprintf(lc.msg, sizeof lc.msg, "%s", v);
		else if (!strcmp(k, "hide-users")) snprintf(lc.hide, sizeof lc.hide, "%s", v);
		else if (!strcmp(k, "autologin-user")) snprintf(lc.autologin, sizeof lc.autologin, "%s", v);
	}
	if (f) fclose(f);
	lc_orig = lc;
	lc_img[0] = 0;
	lc_pick = -1;
	read_users();
}

/* 可以選的背景圖：目前的桌布、系統內建的桌布、「圖片」資料夾（縮圖第一次用到時才做） */
static void load_login_thumbs(void)
{
	if (lc_nthumbs) return;
	const char *home = getenv("HOME") ? getenv("HOME") : "/";
	char pics[PATH_MAX];
	snprintf(pics, sizeof pics, "%s/Pictures", home);
	const char *wp = pm_conf_get("desktop.wallpaper", "");
	if (wp[0] == '/' && access(wp, R_OK) == 0) snprintf(lc_paths[lc_nthumbs++], PATH_MAX, "%s", wp);
	const char *dirs[] = { "/usr/share/backgrounds", pics };
	for (unsigned d = 0; d < LEN(dirs); d++) {
		DIR *dp = opendir(dirs[d]);
		struct dirent *e;
		int took = 0;
		while (dp && (e = readdir(dp)) && lc_nthumbs < (int)LEN(lc_paths) && took < 12) {
			const char *x = strrchr(e->d_name, '.');
			if (!x || (strcasecmp(x, ".jpg") && strcasecmp(x, ".jpeg") && strcasecmp(x, ".png"))) continue;
			char p[PATH_MAX];
			snprintf(p, sizeof p, "%s/%s", dirs[d], e->d_name);
			if (!strcmp(p, wp)) continue;
			snprintf(lc_paths[lc_nthumbs++], PATH_MAX, "%s", p);
			took++;
		}
		if (dp) closedir(dp);
	}
	for (int i = 0; i < lc_nthumbs; i++) {
		lc_thumbs[i] = pm_thumb_cached(lc_paths[i], 128, 80, 0);   /* 沒有快取的由 tick() 一次解一張 */
		const char *b = strrchr(lc_paths[i], '/');
		lc_names[i] = b ? b + 1 : lc_paths[i];
	}
}

/* 解一張還沒有縮圖的背景圖（回傳 1＝做了一張） */
static int lc_thumb_fill_one(void)
{
	static char failed[24];
	for (int i = 0; i < lc_nthumbs; i++)
		if (!lc_thumbs[i] && !failed[i]) {
			lc_thumbs[i] = pm_thumb_load(lc_paths[i], 128, 80, 0);
			if (!lc_thumbs[i]) failed[i] = 1;
			malloc_trim(0);
			return 1;
		}
	return 0;
}

static int lc_changed(void) { return memcmp(&lc, &lc_orig, sizeof lc) != 0 || lc_img[0]; }

static void login_done(int ok)
{
	if (ok) { read_login(); snprintf(status, sizeof status, "%s", _("已套用；下次出現登入畫面時就會看到")); }
}

static const char *lc_bgs[] = { "user", "image", "gradient" }, *lc_clocks[] = { "24h", "12h", "off" };

/* 把畫面上的設定排成「pomelo-dm --set key=value …」 */
static void login_args(char *out[], char store[][420])
{
	int n = 0, k = 0;
	#define KV(fmt, ...) do { snprintf(store[k], 420, fmt, __VA_ARGS__); out[n++] = store[k++]; } while (0)
	out[n++] = "pomelo-dm";
	if (getenv("POMELO_DM_CONF")) {   /* 測試：不用 pkexec，直接改測試用的設定檔 */
		out[n++] = "--test"; out[n++] = "--config"; out[n++] = (char *)dm_conf_path();
		out[n++] = "--state"; out[n++] = (char *)dm_state_dir();
	}
	out[n++] = "--set";
	KV("background=%s", lc_bgs[lc.bg]);
	KV("clock=%s", lc_clocks[lc.clock]);
	KV("text-size=%s", lc.large ? "large" : "normal");
	KV("user-list=%s", lc.user_list ? "true" : "false");
	KV("show-other-user=%s", lc.show_other ? "true" : "false");
	KV("power-buttons=%s", lc.power ? "true" : "false");
	KV("message=%s", lc.msg);
	KV("hide-users=%s", lc.hide);
	KV("autologin-user=%s", lc.autologin);
	if (lc_img[0]) { out[n++] = "--set-background"; out[n++] = lc_img; }
	#undef KV
	out[n] = NULL;
}

static void login_apply(PmUI *u)
{
	static char *argv[40];
	static char store[16][420];
	login_args(argv, store);
	if (getenv("POMELO_DM_CONF")) run_cmd(argv, _("套用登入畫面設定"), login_done);
	else run_admin(argv, _("套用登入畫面設定"), login_done);
	u->scroll = 0;   /* 回到最上面看進度與結果 */
}

/* 預覽：用還沒套用的設定開一個登入畫面（按 Esc 離開） */
static void login_preview(void)
{
	const char *rt = getenv("XDG_RUNTIME_DIR");
	char tmp[PATH_MAX];
	snprintf(tmp, sizeof tmp, "%s/pomelo-login-preview.conf", rt && *rt ? rt : "/tmp");
	FILE *f = fopen(tmp, "w");
	if (!f) return;
	fprintf(f, "background=%s\nclock=%s\ntext-size=%s\nuser-list=%s\nshow-other-user=%s\npower-buttons=%s\nmessage=%s\nhide-users=%s\n",
	        lc_bgs[lc.bg], lc_clocks[lc.clock], lc.large ? "large" : "normal", lc.user_list ? "true" : "false",
	        lc.show_other ? "true" : "false", lc.power ? "true" : "false", lc.msg, lc.hide);
	fclose(f);
	if (fork() == 0) {
		if (dpy) close(ConnectionNumber(dpy));   /* 原生 Wayland（pm_ui_native）時沒有 X 連線 */
		setsid();
		setenv("POMELO_DM_CONF", tmp, 1);
		setenv("POMELO_DM_STATE", dm_state_dir(), 1);
		if (lc_img[0]) setenv("POMELO_DM_BACKGROUND", lc_img, 1);
		execlp("pomelo-greeter", "pomelo-greeter", "--preview", (char *)NULL);
		_exit(127);
	}
	snprintf(status, sizeof status, "%s", _("預覽中：按 Esc 離開"));
}

static void build_login(PmUI *u)
{
	if (pm_ui_back(u, _("系統設定"))) { go(u, P_MAIN); return; }
	pm_ui_title(u, _("登入畫面"), _("開機或登出後看到的畫面"));
	if (!pm_have("pomelo-dm")) { pm_ui_note(u, _("需要安裝 pomelo-dm 與 pomelo-greeter")); return; }

	/* 改過還沒套用的話放在最上面：以前只有頁面最底下有「套用」，選好背景、預覽過，很容易以為已經存好了 */
	int banner = job || lc_changed();
	if (banner) {
		pm_ui_group(u, NULL);
		if (job) pm_ui_info(u, _("正在套用…"), _("請在跳出的視窗輸入管理員密碼"));
		else {
			if (pm_ui_button(u, _("設定還沒套用（需要管理員密碼）"), _("套用"))) login_apply(u);
			if (pm_ui_button(u, _("不要儲存這些變更"), _("復原"))) { lc = lc_orig; lc_img[0] = 0; lc_pick = -1; status[0] = 0; }
		}
		pm_ui_end(u);
		if (!job) pm_ui_note(u, _("改過的設定要按「套用」才會出現在登入畫面；「預覽」只是先看看。"));
	}
	if (status[0]) pm_ui_note(u, status);

	const char *bg_opts[] = { _("使用者自己的桌布"), _("指定的圖片"), _("漸層") };
	const char *clock_opts[] = { _("24 小時制"), _("12 小時制"), _("不顯示") };
	const char *size_opts[] = { _("標準"), _("大") };
	pm_ui_group(u, _("畫面"));
	pm_ui_choice(u, _("背景"), bg_opts, 3, &lc.bg);
	pm_ui_choice(u, _("時鐘"), clock_opts, 3, &lc.clock);
	pm_ui_choice(u, _("文字大小"), size_opts, 2, &lc.large);
	pm_ui_end(u);
	if (lc.bg == 1) {
		load_login_thumbs();
		if (pm_ui_images(u, lc_thumbs, lc_names, lc_nthumbs, &lc_pick, 128, 80) && lc_pick >= 0)
			snprintf(lc_img, sizeof lc_img, "%s", lc_paths[lc_pick]);
		pm_ui_group(u, NULL);
		if (pm_ui_entry(u, _("其他圖片的路徑"), lc_custom, sizeof lc_custom, 0) && lc_custom[0]) {
			if (access(lc_custom, R_OK) == 0) { snprintf(lc_img, sizeof lc_img, "%s", lc_custom); lc_pick = -1; }
			else snprintf(status, sizeof status, "%s", _("找不到這個檔案"));
		}
		pm_ui_end(u);
		char m[PATH_MAX + 64];
		if (lc_img[0]) snprintf(m, sizeof m, _("新的背景：%s（按「套用」後生效）"), lc_img);
		else snprintf(m, sizeof m, "%s", _("點一張圖，或輸入路徑後按 Enter（只能用 PNG 或 JPEG）"));
		pm_ui_note(u, m);
	}

	const char *list_opts[] = { _("使用者清單"), _("名稱與密碼") };
	int lv = lc.user_list ? 0 : 1;
	pm_ui_group(u, _("使用者"));
	if (pm_ui_choice(u, _("登入方式"), list_opts, 2, &lv)) lc.user_list = lv == 0;
	if (lc.user_list) pm_ui_switch(u, _("顯示「其他使用者」（手動輸入帳號）"), &lc.show_other);
	pm_ui_entry(u, _("不顯示的帳號"), lc.hide, sizeof lc.hide, 0);
	/* 自動登入：關閉或其中一個帳號 */
	const char *auto_opts[65];
	int na = 0, av = 0;
	auto_opts[na++] = _("關閉");
	for (int i = 0; i < nusers && na < 65; i++) {
		if (!strcmp(users[i].name, lc.autologin)) av = na;
		auto_opts[na++] = users[i].name;
	}
	if (pm_ui_choice(u, _("自動登入"), auto_opts, na, &av))
		snprintf(lc.autologin, sizeof lc.autologin, "%s", av ? auto_opts[av] : "");
	pm_ui_end(u);
	pm_ui_note(u, _("「不顯示的帳號」用逗號分開；自動登入時開機不必輸入密碼，鑰匙圈也不會自動解鎖。"));

	pm_ui_group(u, _("文字與按鈕"));
	pm_ui_entry(u, _("登入畫面訊息"), lc.msg, sizeof lc.msg, 0);
	pm_ui_switch(u, _("顯示睡眠、重新開機、關機按鈕"), &lc.power);
	pm_ui_end(u);
	pm_ui_note(u, _("訊息會顯示在登入畫面下方，例如「撿到這台電腦請聯絡 0912-345-678」；留空就不顯示。"));

	pm_ui_group(u, NULL);
	if (pm_ui_button(u, _("先看看登入畫面的樣子（按 Esc 離開）"), _("預覽"))) login_preview();
	if (lc_changed() && !job && pm_ui_button(u, _("儲存設定（需要管理員密碼）"), _("套用"))) login_apply(u);
	if (lc_changed() && !job && pm_ui_button(u, _("不要儲存這些變更"), _("復原"))) { lc = lc_orig; lc_img[0] = 0; lc_pick = -1; status[0] = 0; }
	pm_ui_end(u);
	if ((job || lc_changed()) != banner) u->again = 1;   /* 這一次才改的：再畫一次，上面的提示才會出現／消失 */
}

/* ------------------------------------------------------------------ */
/* 應用程式選單：啟動台、Spotlight、Dock 看到的程式（改的是 ~/.local/share/applications 的副本）  */
/* ------------------------------------------------------------------ */

static char app_q[64];
static int show_hidden_apps;

typedef struct {
	char id[256], src[PATH_MAX], user[PATH_MAX];
	char name[256], comment[512], exec[1024], icon[512];
	int cat, show, term, custom, overridden, isnew;
	int lcat;   /* 啟動器分類：0 = 自動，1～6 = 工具類～其他（設定 launcher.cat.<ID> 是 0～5） */
} AppEdit;
static AppEdit ae, ae_orig;

static const char *cat_keys[] = { "", "AudioVideo", "Graphics", "Network", "Office", "Development", "Game", "Education", "System", "Utility", "Settings" };
static const char *cat_names[] = { N_("其他"), N_("影音"), N_("圖形"), N_("網路"), N_("辦公"), N_("開發"), N_("遊戲"), N_("教育"), N_("系統"), N_("工具"), N_("設定") };

static void user_apps_dir(char *out, size_t n)
{
	const char *x = getenv("XDG_DATA_HOME"), *h = getenv("HOME");
	if (x && *x) snprintf(out, n, "%s/applications", x);
	else snprintf(out, n, "%s/.local/share/applications", h ? h : "/tmp");
}

/* 系統裡（不是使用者自己的）同名 .desktop 檔 */
static int system_desktop(const char *id, char *out, size_t n)
{
	const char *dirs[] = { "/var/lib/flatpak/exports/share/applications", "/var/lib/snapd/desktop/applications",
	                       "/usr/local/share/applications", "/usr/share/applications" };
	for (unsigned i = 0; i < LEN(dirs); i++) {
		snprintf(out, n, "%s/%s.desktop", dirs[i], id);
		if (access(out, R_OK) == 0) return 1;
	}
	out[0] = 0;
	return 0;
}

static void load_app_edit(const char *idp)
{
	char id[sizeof ae.id];
	snprintf(id, sizeof id, "%s", idp);   /* idp 可能就是 ae.id */
	memset(&ae, 0, sizeof ae);
	snprintf(ae.id, sizeof ae.id, "%s", id);
	char dir[PATH_MAX];
	user_apps_dir(dir, sizeof dir);
	snprintf(ae.user, sizeof ae.user, "%s/%s.desktop", dir, id);
	int has_sys = system_desktop(id, ae.src, sizeof ae.src);
	int has_user = access(ae.user, R_OK) == 0;
	ae.overridden = has_sys && has_user;
	ae.custom = !has_sys;
	const char *file = has_user ? ae.user : ae.src;
	/* 名稱、說明用目前語言顯示的（含 Ubuntu 的 .mo 翻譯） */
	PmApp *a = pm_app_by_id(id);
	if (a) {
		snprintf(ae.name, sizeof ae.name, "%s", a->name ? a->name : "");
		snprintf(ae.comment, sizeof ae.comment, "%s", a->comment ? a->comment : "");
	}
	ae.show = 1;
	{
		char ck[300];
		snprintf(ck, sizeof ck, "launcher.cat.%s", id);
		const char *cv = pm_conf_get(ck, "");
		if (cv[0] >= '0' && cv[0] < '0' + PM_CAT_N && !cv[1]) ae.lcat = cv[0] - '0' + 1;
	}
	FILE *f = file[0] ? fopen(file, "r") : NULL;
	char line[2048];
	int in = 0;
	while (f && fgets(line, sizeof line, f)) {
		line[strcspn(line, "\r\n")] = 0;
		if (line[0] == '[') { if (in) break; in = !strcmp(line, "[Desktop Entry]"); continue; }
		char *eq = strchr(line, '=');
		if (!in || !eq) continue;
		*eq = 0;
		const char *k = line, *v = eq + 1;
		if (!strcmp(k, "Exec")) snprintf(ae.exec, sizeof ae.exec, "%s", v);   /* 原樣保留 %U 等參數 */
		else if (!strcmp(k, "Icon")) snprintf(ae.icon, sizeof ae.icon, "%s", v);
		else if (!strcmp(k, "Terminal")) ae.term = !strcmp(v, "true");
		else if ((!strcmp(k, "NoDisplay") || !strcmp(k, "Hidden")) && !strcmp(v, "true")) ae.show = 0;
		else if (!strcmp(k, "Categories")) {
			for (unsigned i = 1; i < LEN(cat_keys); i++) if (strstr(v, cat_keys[i]) && !ae.cat) ae.cat = (int)i;
		} else if (!strcmp(k, "Name") && !ae.name[0]) snprintf(ae.name, sizeof ae.name, "%s", v);
	}
	if (f) fclose(f);
	ae_orig = ae;
}

static void new_app_edit(void)
{
	char id[64];
	for (int n = 0; ; n++) {   /* 同一秒新增兩個也不會撞名 */
		snprintf(id, sizeof id, n ? "pomelo-custom-%ld-%d" : "pomelo-custom-%ld", (long)time(NULL), n);
		char p[PATH_MAX], dir[PATH_MAX];
		user_apps_dir(dir, sizeof dir);
		snprintf(p, sizeof p, "%s/%s.desktop", dir, id);
		if (access(p, F_OK) != 0 || n > 99) break;
	}
	load_app_edit(id);
	ae.isnew = 1;
	ae.cat = 9;   /* 工具 */
	ae_orig = ae;
}

static int key_dropped(const char *k, int keep_name, int keep_comment, int keep_cat)
{
	static const char *drop[] = { "Name", "Comment", "Exec", "Icon", "Categories", "Terminal", "NoDisplay", "Hidden",
	                              "X-Ubuntu-Gettext-Domain", "X-GNOME-Gettext-Domain" };
	for (unsigned i = 0; i < LEN(drop); i++) {
		size_t l = strlen(drop[i]);
		if (!strncmp(k, drop[i], l) && (k[l] == '=' || k[l] == '[')) {
			/* 沒改到的欄位原樣保留（各語言的名稱、原本的多個分類不會被抹掉） */
			if (i == 0 && keep_name) return 0;
			if (i == 1 && keep_comment) return 0;
			if (i == 4 && keep_cat) return 0;
			if (i >= 8 && keep_name && keep_comment) return 0;
			return 1;
		}
	}
	return 0;
}

/* .desktop 的值不能有換行或其他控制字元（不然可以夾帶別的欄位），前後空白去掉 */
static int app_field_ok(char *s)
{
	size_t l = strlen(s);
	while (l && (s[l - 1] == ' ' || s[l - 1] == '\t')) s[--l] = 0;
	char *st = s;
	while (*st == ' ' || *st == '\t') st++;
	if (st != s) memmove(s, st, strlen(st) + 1);
	for (const unsigned char *p = (const unsigned char *)s; *p; p++)
		if (*p < 0x20 || *p == 0x7f) return 0;
	return 1;
}

static void set_launcher_cat(const char *id, int lcat)
{
	char ck[300], v[4] = "";
	snprintf(ck, sizeof ck, "launcher.cat.%s", id);
	if (lcat > 0) snprintf(v, sizeof v, "%d", lcat - 1);
	if (strcmp(pm_conf_get(ck, ""), v)) pm_conf_set(ck, v);
}

/* 寫使用者自己的 .desktop：原本的內容保留，只換掉這頁改過的欄位；先寫暫存檔再改名，不會留下寫一半的檔案 */
static int save_app_edit(void)
{
	if (!app_field_ok(ae.name) || !app_field_ok(ae.comment) || !app_field_ok(ae.exec) || !app_field_ok(ae.icon)) {
		snprintf(status, sizeof status, "%s", _("不能有換行或控制字元"));
		return 0;
	}
	if (!ae.name[0]) { snprintf(status, sizeof status, "%s", _("名稱不能空白")); return 0; }
	if (!ae.exec[0]) { snprintf(status, sizeof status, "%s", _("指令不能空白")); return 0; }
	char dir[PATH_MAX];
	user_apps_dir(dir, sizeof dir);
	char mk[PATH_MAX + 16];
	snprintf(mk, sizeof mk, "%s", dir);
	for (char *c = mk + 1; *c; c++) if (*c == '/') { *c = 0; mkdir(mk, 0755); *c = '/'; }
	mkdir(mk, 0755);
	int keep_name = !ae.isnew && !strcmp(ae.name, ae_orig.name);
	int keep_comment = !ae.isnew && !strcmp(ae.comment, ae_orig.comment);
	int keep_cat = !ae.isnew && ae.cat == ae_orig.cat;
	/* 新的 [Desktop Entry] 欄位 */
	char head[4096];
	size_t hl = 0;
	if (!keep_name) hl += snprintf(head + hl, sizeof head - hl, "Name=%s\n", ae.name);
	if (!keep_comment && ae.comment[0]) hl += snprintf(head + hl, sizeof head - hl, "Comment=%s\n", ae.comment);
	hl += snprintf(head + hl, sizeof head - hl, "Exec=%s\n", ae.exec);
	if (ae.icon[0]) hl += snprintf(head + hl, sizeof head - hl, "Icon=%s\n", ae.icon);
	if (!keep_cat) hl += snprintf(head + hl, sizeof head - hl, "Categories=%s%s\n", cat_keys[ae.cat], ae.cat ? ";" : "");
	hl += snprintf(head + hl, sizeof head - hl, "Terminal=%s\nNoDisplay=%s\n", ae.term ? "true" : "false", ae.show ? "false" : "true");
	if (ae.isnew) snprintf(head + hl, sizeof head - hl, "X-Pomelo-Custom=true\n");   /* 自己加的：這種才能在選單裡刪除 */
	char tmp[PATH_MAX + 8];
	snprintf(tmp, sizeof tmp, "%s.new", ae.user);
	FILE *out = fopen(tmp, "w");
	if (!out) { snprintf(status, sizeof status, "%s", _("無法存檔")); return 0; }
	const char *from = access(ae.user, R_OK) == 0 ? ae.user : ae.src;
	FILE *in = from[0] ? fopen(from, "r") : NULL;
	if (!in) fprintf(out, "[Desktop Entry]\nType=Application\n%s", head);
	char line[4096];
	int in_entry = 0, wrote = 0;
	while (in && fgets(line, sizeof line, in)) {
		if (line[0] == '[') {
			in_entry = !strncmp(line, "[Desktop Entry]", 15);
			fputs(line, out);
			if (in_entry && !wrote) { fputs(head, out); wrote = 1; }
			continue;
		}
		if (in_entry && key_dropped(line, keep_name, keep_comment, keep_cat)) continue;
		fputs(line, out);
	}
	if (in) fclose(in);
	int ok = fflush(out) == 0 && fclose(out) == 0;
	if (!ok || rename(tmp, ae.user)) { unlink(tmp); snprintf(status, sizeof status, "%s", _("無法存檔")); return 0; }
	set_launcher_cat(ae.id, ae.lcat);
	return 1;
}

/* 通知 Dock、啟動台重新讀應用程式清單 */
static void apps_changed(void)
{
	pm_apps_free();
	char t[32];
	snprintf(t, sizeof t, "%ld", (long)time(NULL));
	pm_conf_set("apps.changed", t);
}

static cairo_surface_t *icon(int size);

/* ---- 資料操作：Dock 固定的程式（dock.apps）、分類（launcher.cat.<ID>）、順序（launcher.order.<分類>） ---- */

#define AM_MAX 1024
#define AM_HID (-2)   /* 左邊「已隱藏」這一列 */
static const char *am_def_dock = "pomelo-launcher,pomelo-files,pomelo-terminal,pomelo-browser,firefox_firefox,firefox,"
                                 "pomelo-software,pomelo-system";

/* 讀 dock.apps（含沒安裝的，順序照設定）；回傳個數 */
static int dock_read(char ids[][256], int max)
{
	char list[8192];
	snprintf(list, sizeof list, "%s", pm_conf_get("dock.apps", am_def_dock));
	int n = 0;
	char *save = NULL;
	for (char *id = strtok_r(list, ",", &save); id && n < max; id = strtok_r(NULL, ",", &save)) {
		while (*id == ' ') id++;
		if (!*id || !strcmp(id, "-")) continue;
		int dup = 0;
		for (int i = 0; i < n; i++) if (!strcmp(ids[i], id)) dup = 1;
		if (!dup) snprintf(ids[n++], 256, "%s", id);
	}
	return n;
}

static void dock_write(char ids[][256], int n)
{
	char list[8192] = "";
	size_t l = 0;
	for (int i = 0; i < n; i++) l += snprintf(list + l, sizeof list - l, "%s%s", l ? "," : "", ids[i]);
	pm_conf_set("dock.apps", l ? list : "-");   /* 空字串會被當成「沒設定」而變回預設，所以全拿掉時寫 - */
}

static int dock_has(const char *id)
{
	static char ids[128][256];
	int n = dock_read(ids, 128);
	for (int i = 0; i < n; i++) if (!strcmp(ids[i], id)) return 1;
	return 0;
}

/* 把 id 放到第 at 個位置（原本就在 Dock 的就是移動；at < 0 或超過就放最後） */
static void dock_put(const char *id, int at)
{
	static char ids[129][256];
	int n = dock_read(ids, 128);
	for (int i = 0; i < n; i++)
		if (!strcmp(ids[i], id)) {
			for (int k = i; k + 1 < n; k++) memcpy(ids[k], ids[k + 1], 256);
			n--;
			if (at > i) at--;
			break;
		}
	if (at < 0 || at > n) at = n;
	if (n >= 128) return;
	for (int k = n; k > at; k--) memcpy(ids[k], ids[k - 1], 256);
	snprintf(ids[at], 256, "%s", id);
	dock_write(ids, n + 1);
}

static void dock_drop(const char *id)
{
	static char ids[128][256];
	int n = dock_read(ids, 128), k = 0;
	for (int i = 0; i < n; i++) if (strcmp(ids[i], id)) memcpy(ids[k++], ids[i], 256);
	dock_write(ids, k);
}

/* 這個程式 ID 從 6 個分類的自訂順序裡拿掉 */
static void order_forget(const char *id)
{
	for (int c = 0; c < PM_CAT_N; c++) {
		char key[48], out[8192] = "", list[8192];
		snprintf(key, sizeof key, "launcher.order.%d", c);
		snprintf(list, sizeof list, "%s", pm_conf_get(key, ""));
		if (!list[0]) continue;
		int found = 0;
		char *save = NULL;
		for (char *t = strtok_r(list, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
			if (!strcmp(t, id)) { found = 1; continue; }
			size_t l = strlen(out);
			snprintf(out + l, sizeof out - l, "%s%s", l ? "," : "", t);
		}
		if (found) pm_conf_set(key, out);
	}
}

/* 程式不在了（刪除）或回到預設：它的 Dock、分類、順序設定一起清掉 */
static void app_forget(const char *id, int keep_dock)
{
	char ck[300];
	snprintf(ck, sizeof ck, "launcher.cat.%s", id);
	if (pm_conf_get(ck, "")[0]) pm_conf_set(ck, "");
	order_forget(id);
	if (!keep_dock && dock_has(id)) dock_drop(id);
}

static int app_is_system(const char *id)
{
	char p[PATH_MAX];
	return system_desktop(id, p, sizeof p);
}

static int app_has_user_file(const char *id)
{
	char dir[PATH_MAX], p[PATH_MAX + 300];
	user_apps_dir(dir, sizeof dir);
	snprintf(p, sizeof p, "%s/%s.desktop", dir, id);
	return access(p, R_OK) == 0;
}

/* 自己加的（只有使用者資料夾裡有，系統裡沒有同名檔） */
static int app_is_custom(const char *id) { return app_has_user_file(id) && !app_is_system(id); }
static int app_is_overridden(const char *id) { return app_has_user_file(id) && app_is_system(id); }

static int app_set_show(const char *id, int show)
{
	load_app_edit(id);
	ae.show = show;
	int ok = save_app_edit();
	if (ok) apps_changed();
	return ok;
}

static int app_rename(const char *id, const char *name)
{
	load_app_edit(id);
	snprintf(ae.name, sizeof ae.name, "%s", name);
	int ok = save_app_edit();
	if (ok) apps_changed();
	return ok;
}

static void app_restore(const char *id)
{
	load_app_edit(id);
	if (ae.overridden) unlink(ae.user);
	app_forget(id, 1);
}

static void app_delete_custom(const char *id)
{
	load_app_edit(id);
	if (!ae.custom || !ae.user[0]) return;
	unlink(ae.user);
	app_forget(id, 0);
}

static int ask_yes(const char *title, const char *msg, const char *yes)
{
	const char *b[] = { _("取消"), yes };
	return pm_ask(title, "pomelo-system", icon, msg, b, 2, 0, 0) == 1;
}

static void am_say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void build_app_edit(PmUI *u)
{
	if (pm_ui_back(u, _("應用程式選單"))) { go(u, P_APPS); return; }
	pm_ui_title(u, ae.isnew ? _("新增項目") : ae.name, ae.id);
	pm_ui_group(u, NULL);
	pm_ui_entry(u, _("名稱"), ae.name, sizeof ae.name, 0);
	pm_ui_entry(u, _("說明"), ae.comment, sizeof ae.comment, 0);
	pm_ui_entry(u, _("指令"), ae.exec, sizeof ae.exec, 0);
	pm_ui_entry(u, _("圖示"), ae.icon, sizeof ae.icon, 0);
	const char *cats[LEN(cat_names)];
	for (unsigned i = 0; i < LEN(cat_names); i++) cats[i] = _(cat_names[i]);
	pm_ui_choice(u, _("分類"), cats, (int)LEN(cats), &ae.cat);
	static char lc_name[PM_CAT_N + 1][64];
	const char *lcats[PM_CAT_N + 1];
	snprintf(lc_name[0], sizeof lc_name[0], "%s", _("自動"));
	lcats[0] = lc_name[0];
	for (int i = 0; i < PM_CAT_N; i++) { snprintf(lc_name[i + 1], sizeof lc_name[i + 1], "%s", pm_category_name(i)); lcats[i + 1] = lc_name[i + 1]; }
	pm_ui_choice(u, _("啟動器的分頁"), lcats, PM_CAT_N + 1, &ae.lcat);
	pm_ui_end(u);
	pm_ui_note(u, _("圖示可以填圖示主題裡的名稱（例如 firefox）或圖片的完整路徑。「啟動器的分頁」選「自動」就依程式的類型分。"));
	pm_ui_group(u, NULL);
	pm_ui_switch(u, _("在啟動器與搜尋中顯示"), &ae.show);
	pm_ui_switch(u, _("在終端機裡執行"), &ae.term);
	pm_ui_end(u);

	int changed = memcmp(&ae, &ae_orig, sizeof ae) != 0 || ae.isnew;
	pm_ui_group(u, NULL);
	if (changed && pm_ui_button(u, _("儲存變更"), _("儲存")) && save_app_edit()) {
		apps_changed();
		int was_new = ae.isnew;
		load_app_edit(ae.id);
		snprintf(status, sizeof status, "%s", was_new ? _("已新增到啟動器") : _("已儲存"));
	}
	if (!ae.isnew && pm_ui_button(u, _("試著打開"), _("打開"))) {
		PmApp *a = pm_app_by_id(ae.id);
		if (a) pm_app_launch(a);
	}
	if (ae.overridden && pm_ui_button(u, _("還原成安裝時的樣子"), _("還原"))) {
		app_restore(ae.id);
		apps_changed();
		load_app_edit(ae.id);
		snprintf(status, sizeof status, "%s", _("已還原"));
	}
	if (ae.custom && !ae.isnew && pm_ui_button(u, _("刪除這個自己加的項目"), _("刪除"))) {
		char msg[600];
		snprintf(msg, sizeof msg, _("要刪除你自己加的「%s」嗎？\n它會從啟動器、搜尋與 Dock 一起消失；系統裡的其他程式不受影響。"), ae.name);
		if (ask_yes(_("刪除程式"), msg, _("刪除"))) {
			char id[sizeof ae.id];
			snprintf(id, sizeof id, "%s", ae.id);
			app_delete_custom(id);
			apps_changed();
			go(u, P_APPS);
			snprintf(status, sizeof status, "%s", _("已刪除"));
			am_say("%s", _("已刪除"));
			pm_ui_end(u);
			return;
		}
	}
	pm_ui_end(u);
}

/* ---- 畫面：左邊分類、右邊程式、底下 Dock 放置區；可以拖拉、按右鍵 ---- */

typedef struct { char name[200]; int size; cairo_surface_t *s; } AmIcon;
static AmIcon am_icons[400];
static int am_nicons;

static cairo_surface_t *am_icon(const char *name, int size)
{
	if (!name || !*name) name = "application-x-executable";
	for (int i = 0; i < am_nicons; i++)
		if (am_icons[i].size == size && !strcmp(am_icons[i].name, name)) return am_icons[i].s;
	if (am_nicons >= (int)LEN(am_icons)) {   /* 滿了就整個清掉重來 */
		for (int i = 0; i < am_nicons; i++) if (am_icons[i].s) cairo_surface_destroy(am_icons[i].s);
		am_nicons = 0;
	}
	AmIcon *e = &am_icons[am_nicons++];
	snprintf(e->name, sizeof e->name, "%s", name);
	e->size = size;
	e->s = pm_icon_load(name, size);
	if (!e->s) e->s = pm_icon_load("application-x-executable", size);
	return e->s;
}

static void am_draw_icon(cairo_t *cr, const char *name, double x, double y, int size, double alpha)
{
	cairo_surface_t *s = am_icon(name, size);
	if (s) {
		cairo_set_source_surface(cr, s, x, y);
		cairo_paint_with_alpha(cr, alpha);
	} else {
		pm_rrect(cr, x + 3, y + 3, size - 6, size - 6, size / 5.0, size / 5.0);
		pm_rgba(cr, 0x8e8e93, 0.5 * alpha);
		cairo_fill(cr);
	}
}

static int am_view = -1;        /* 左邊選的：-1 全部、0～5 分類、AM_HID 已隱藏 */
static int am_grid = -1;        /* 1 = 圖示格，0 = 清單（-1 = 還沒從設定讀） */
static char am_sel[256];        /* 選到的程式 ID */
static int am_scroll;
static PmApp *am_list[AM_MAX];
static int am_n;
static char am_msg[240];
static int am_renaming, am_rename_fresh;
static char am_rename_buf[256], am_rename_id[256];

enum { AMC_NONE, AMC_EDIT, AMC_RENAME, AMC_DUP, AMC_PIN, AMC_UNPIN, AMC_HIDE, AMC_UNHIDE, AMC_CAT, AMC_RESTORE, AMC_DELETE,
       AMC_NEW, AMC_SHOWHID, AMC_RESTORE_ALL, AMC_SORT, AMC_UNHIDE_ALL, AMC_DOCK_RESET, AMC_DOCK_LEFT, AMC_DOCK_RIGHT,
       AMC_DOCK_CLEAR, AMC_OPEN };
static int am_cmd, am_cmd_arg;
static char am_target[256];

static void am_say(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(am_msg, sizeof am_msg, fmt, ap);
	va_end(ap);
}

static void mi_am(const Arg *a) { am_cmd = a->i & 0xff; am_cmd_arg = a->i >> 8; }

/* 拖曳的狀態 */
enum { DG_NONE, DG_PRESS, DG_DRAG };
enum { DS_LIST, DS_DOCK };
static struct {
	int state, src;
	char id[256];
	int x0, y0, ox, oy, size;      /* 按下的位置、抓取點在圖示裡的位置、圖示大小 */
	/* 目前放開會怎樣（每個畫面重算） */
	int to_view;                   /* 放在左邊哪一列（-1 全部沒有意義，用 -3 表示沒有） */
	int to_dock;                   /* Dock 裡的位置（raw 索引；-1 = 不在 Dock 上） */
	int to_list;                   /* 清單裡的插入位置（-1 = 不在） */
	int to_out;                    /* 從 Dock 拖到外面 */
} dg;
#define DG_NOVIEW (-3)

typedef struct { int x, y, w, h; } AmR;
static struct {
	AmR side, search, main, dock, tool_new, tool_list, tool_grid, pill;
	AmR rows[PM_CAT_N + 2];        /* 0 = 全部、1～6 = 分類、7 = 已隱藏 */
	int rowview[PM_CAT_N + 2];
	int cols, cellw, cellh, content_h;
} L;
static int am_dock_vis[128];   /* 畫面上的 Dock 項目對到 dock.apps 的第幾個 */
static int am_dock_nvis, am_dock_nraw;
static char am_dock_ids[128][256];
static PmApp *am_dock_app[128];

static int in_r(const AmR *r, int x, int y) { return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h; }

static int am_hit(PmUI *u, const AmR *r)
{
	if (!u->clicked || u->claimed || !in_r(r, u->mx, u->my)) return 0;
	u->claimed = 1;
	return 1;
}

static int am_find(const char *id)
{
	for (int i = 0; i < am_n; i++) if (!strcmp(am_list[i]->id, id)) return i;
	return -1;
}

static void am_load_list(void)
{
	PmApp *tmp[AM_MAX];
	int n;
	if (app_q[0]) {   /* 搜尋：所有分類一起找 */
		n = pm_apps_in_category(-1, show_hidden_apps ? 1 : 0, tmp, AM_MAX);
		int k = 0;
		for (int i = 0; i < n; i++) if (pm_app_match(tmp[i], app_q)) am_list[k++] = tmp[i];
		am_n = k;
		return;
	}
	if (am_view == AM_HID) n = pm_apps_in_category(-1, 2, tmp, AM_MAX);
	else n = pm_apps_in_category(am_view, show_hidden_apps ? 1 : 0, tmp, AM_MAX);
	memcpy(am_list, tmp, n * sizeof *tmp);
	am_n = n;
}

static void am_load_dock(int skip_dragged)
{
	am_dock_nraw = dock_read(am_dock_ids, 128);
	am_dock_nvis = 0;
	for (int i = 0; i < am_dock_nraw; i++) {
		am_dock_app[i] = pm_app_by_id(am_dock_ids[i]);
		if (!am_dock_app[i]) continue;
		if (skip_dragged && !strcmp(am_dock_ids[i], dg.id)) continue;
		am_dock_vis[am_dock_nvis++] = i;
	}
}

/* ---- 版面 ---- */

#define AM_ROW 52
#define AM_SIDE_W 150
#define AM_DOCK_H 88
#define AM_PILL 44

static void am_layout(PmUI *u, int top)
{
	int W = u->w, H = u->h;
	L.side = (AmR){ 16, top, AM_SIDE_W, H - top - AM_DOCK_H - 44 };
	L.search = (AmR){ 16, top, AM_SIDE_W, 44 };
	int rowy = top + 52;
	for (int i = 0; i < PM_CAT_N + 2; i++) {
		int gap = i == PM_CAT_N + 1 ? 10 : 0;   /* 已隱藏前面空一點 */
		L.rows[i] = (AmR){ 16, rowy + gap, AM_SIDE_W, 32 };
		L.rowview[i] = i == 0 ? -1 : i == PM_CAT_N + 1 ? AM_HID : i - 1;
		rowy += 33 + gap;
	}
	int mx = 16 + AM_SIDE_W + 14;
	L.main = (AmR){ mx, top, W - 16 - mx, H - top - AM_DOCK_H - 44 };
	L.dock = (AmR){ 16, H - AM_DOCK_H - 32, W - 32, AM_DOCK_H };
	if (am_grid) {
		L.cols = MAX(1, L.main.w / 104);
		L.cellw = L.main.w / L.cols;
		L.cellh = 98;
		L.content_h = (am_n + L.cols - 1) / L.cols * L.cellh + 8;
	} else {
		L.cols = 1;
		L.cellw = L.main.w;
		L.cellh = AM_ROW;
		L.content_h = am_n * AM_ROW + 8;
	}
	int maxs = MAX(0, L.content_h - L.main.h);
	am_scroll = CLAMP(am_scroll, 0, maxs);
}

static AmR am_item_rect(int i)
{
	int r = i / L.cols, c = i % L.cols;
	return (AmR){ L.main.x + c * L.cellw, L.main.y + r * L.cellh - am_scroll, L.cellw, L.cellh };
}

static int am_item_at(int x, int y)
{
	if (!in_r(&L.main, x, y)) return -1;
	int r = (y - L.main.y + am_scroll) / L.cellh, c = (x - L.main.x) / L.cellw;
	if (c >= L.cols) return -1;
	int i = r * L.cols + c;
	return i >= 0 && i < am_n ? i : -1;
}

/* 在清單／格子裡，(x,y) 這個位置對應的插入位置（0～am_n：第 k 項的前面） */
static int am_insert_at(int x, int y)
{
	int r = (y - L.main.y + am_scroll) / L.cellh;
	if (y - L.main.y + am_scroll < 0) r = 0;
	if (am_grid) {
		int c = CLAMP((x - L.main.x) / L.cellw, 0, L.cols - 1);
		int rel = (x - L.main.x) - c * L.cellw;
		int idx = r * L.cols + c + (rel > L.cellw / 2 ? 1 : 0);
		return CLAMP(idx, 0, am_n);
	}
	int rel = (y - L.main.y + am_scroll) - r * L.cellh;
	return CLAMP(r + (rel > L.cellh / 2 ? 1 : 0), 0, am_n);
}

/* Dock 放置區裡圖示的位置 */
static int am_dock_x0(int nvis)
{
	int w = MAX(nvis, 1) * (AM_PILL + 10) - 10;
	return L.dock.x + (L.dock.w - w) / 2;
}

static int am_dock_insert_at(int x, int nvis)
{
	int x0 = am_dock_x0(nvis);
	int idx = (x - x0 + (AM_PILL + 10) / 2) / (AM_PILL + 10);
	return CLAMP(idx, 0, nvis);
}

static int am_dock_item_at(int x, int y)
{
	if (y < L.dock.y + 30 || y > L.dock.y + AM_DOCK_H - 6) return -1;
	int x0 = am_dock_x0(am_dock_nvis);
	for (int i = 0; i < am_dock_nvis; i++) {
		int ix = x0 + i * (AM_PILL + 10);
		if (x >= ix - 3 && x < ix + AM_PILL + 3) return i;
	}
	return -1;
}

/* ---- 繪圖用的小東西 ---- */

static unsigned am_col_card, am_col_line, am_col_text, am_col_dim, am_col_bg;
static const unsigned am_catcol[PM_CAT_N] = { 0x0a84ff, 0xff9f0a, 0x30b0c7, 0xff375f, 0x34c759, 0x8e8e93 };

static void am_text_at(cairo_t *cr, double x, double yc, double maxw, const char *s, double size, int bold, unsigned col, double alpha)
{
	char buf[300];
	pm_font(cr, size, bold);
	pm_ellipsize(cr, s, maxw, buf, sizeof buf);
	pm_rgba(cr, col, alpha);
	pm_text(cr, x, yc, buf);
}

static void am_pill(cairo_t *cr, double *x, double yc, const char *label, unsigned col)
{
	pm_font(cr, 11, 0);
	double w = pm_textw(cr, label) + 12;
	*x -= w;
	pm_rrect(cr, *x, yc - 9, w, 18, 9, 9);
	pm_rgba(cr, col, 0.16);
	cairo_fill(cr);
	pm_rgb(cr, col);
	pm_text(cr, *x + 6, yc, label);
	*x -= 6;
}

static void am_handle(cairo_t *cr, double x, double yc, double alpha)
{
	cairo_new_path(cr);
	pm_rgba(cr, am_col_dim, alpha);
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 2; c++) {
			cairo_arc(cr, x + c * 5, yc - 6 + r * 6, 1.5, 0, 2 * M_PI);
			cairo_fill(cr);
		}
}

/* 左邊每一列前面的小圖：全部＝四個方塊、分類＝色點、已隱藏＝劃掉的眼睛 */
static void am_row_glyph(cairo_t *cr, int view, double x, double yc, unsigned col)
{
	cairo_new_path(cr);   /* pm_text 之後目前點還在文字尾端，不清掉會多一條線 */
	if (view >= 0) {
		cairo_arc(cr, x + 6, yc, 5, 0, 2 * M_PI);
		pm_rgb(cr, col);
		cairo_fill(cr);
	} else if (view == -1) {
		for (int i = 0; i < 4; i++) {
			pm_rrect(cr, x + (i % 2) * 7, yc - 7 + (i / 2) * 7, 5.5, 5.5, 1.5, 1.5);
			pm_rgb(cr, col);
			cairo_fill(cr);
		}
	} else {
		cairo_save(cr);
		cairo_translate(cr, x + 6, yc);
		cairo_scale(cr, 1.0, 0.62);
		cairo_arc(cr, 0, 0, 7, 0, 2 * M_PI);
		cairo_restore(cr);
		pm_rgb(cr, col);
		cairo_set_line_width(cr, 1.5);
		cairo_stroke(cr);
		cairo_arc(cr, x + 6, yc, 2, 0, 2 * M_PI);
		cairo_fill(cr);
		cairo_move_to(cr, x - 1, yc + 6);
		cairo_line_to(cr, x + 13, yc - 6);
		cairo_stroke(cr);
	}
}

/* 目前放開會做什麼（顯示在跟著游標的小標籤上），沒有就回傳 NULL */
static const char *am_drop_text(char *buf, size_t n)
{
	if (dg.to_dock >= 0) return dg.src == DS_DOCK ? _("在 Dock 裡排序") : _("放進 Dock");
	if (dg.to_view == AM_HID) return _("從選單隱藏");
	if (dg.to_view >= 0) { snprintf(buf, n, _("移到%s"), pm_category_name(dg.to_view)); return buf; }
	if (dg.to_list >= 0) return _("排在這裡");
	if (dg.to_out) return _("移出 Dock");
	return NULL;
}

static void am_ghost(cairo_t *cr, PmUI *u)
{
	if (dg.state != DG_DRAG) return;
	PmApp *a = pm_app_by_id(dg.id);
	if (!a) return;
	double s = dg.size * 1.12, x = u->mx - dg.ox * 1.12, y = u->my - dg.oy * 1.12;
	/* 影子 */
	pm_rrect(cr, x + 2, y + 5, s, s, s / 5, s / 5);
	pm_rgba(cr, 0x000000, 0.20);
	cairo_fill(cr);
	cairo_save(cr);
	cairo_translate(cr, x, y);
	cairo_scale(cr, 1.12, 1.12);
	am_draw_icon(cr, a->icon, 0, 0, dg.size, 0.88);
	cairo_restore(cr);
	char tb[200];
	const char *t = am_drop_text(tb, sizeof tb);
	if (t) {
		pm_font(cr, 12, 1);
		double tw = pm_textw(cr, t) + 20, tx = x + s + 6, ty = y + s / 2 - 12;
		if (tx + tw > u->w - 6) tx = x - tw - 6;
		if (y + s > L.dock.y - 6 && y < L.dock.y + L.dock.h) ty = y - 30;   /* 在 Dock 上方時，標籤放在圖示上面，不蓋住 Dock 的圖示 */
		pm_rrect(cr, tx, ty, tw, 24, 12, 12);
		pm_rgba(cr, dg.to_out ? 0xff453a : T->sel, 0.95);
		cairo_fill(cr);
		pm_rgb(cr, 0xffffff);
		pm_text(cr, tx + 10, ty + 12, t);
	}
}

/* ---- 右鍵選單 ---- */

static MenuItem am_mi(const char *label, int cmd, int arg, int on)
{
	return (MenuItem){ label, on ? mi_am : pm_menu_off, { .i = cmd | (arg << 8) } };
}

/* 有快捷鍵的項目：「名稱<Tab>快捷鍵」，選單只翻譯名稱的部分 */
static MenuItem am_mik(const char *name, const char *key, int cmd, int arg, int on)
{
	static char ring[16][160];
	static int k;
	char *b = ring[k++ % 16];
	snprintf(b, 160, "%s\t%s", name, key);
	return am_mi(b, cmd, arg, on);
}
#define AM_SEP ((MenuItem){ NULL, NULL, { 0 } })

static void am_app_menu(PmUI *u, PmApp *a, int rx, int ry)
{
	snprintf(am_target, sizeof am_target, "%s", a->id);
	int pinned = dock_has(a->id), custom = app_is_custom(a->id), over = app_is_overridden(a->id), hid = a->hidden;
	int cur = pm_app_category(a);
	char ckey[300];
	snprintf(ckey, sizeof ckey, "launcher.cat.%s", a->id);
	static char lab[PM_CAT_N][96];
	MenuItem m[24];
	int n = 0;
	m[n++] = am_mi(N_("打開"), AMC_OPEN, 0, !hid);
	m[n++] = am_mik(N_("編輯…"), "Enter", AMC_EDIT, 0, 1);
	m[n++] = am_mik(N_("重新命名…"), "F2", AMC_RENAME, 0, 1);
	m[n++] = am_mi(N_("複製成自訂程式"), AMC_DUP, 0, 1);
	m[n++] = AM_SEP;
	m[n++] = pinned ? am_mi(N_("從 Dock 移除"), AMC_UNPIN, 0, 1) : am_mi(N_("保留在 Dock"), AMC_PIN, 0, 1);
	m[n++] = hid ? am_mi(N_("取消隱藏"), AMC_UNHIDE, 0, 1) : am_mi(N_("從選單隱藏"), AMC_HIDE, 0, 1);
	m[n++] = AM_SEP;
	for (int c = 0; c < PM_CAT_N; c++) {
		snprintf(lab[c], sizeof lab[c], "%c%s", c == cur ? '\1' : '\2', "");
		snprintf(lab[c] + 1, sizeof lab[c] - 1, _("移到%s"), pm_category_name(c));
		m[n++] = am_mi(lab[c], AMC_CAT, c, 1);
	}
	m[n++] = AM_SEP;
	if (over) m[n++] = am_mi(N_("還原成系統預設"), AMC_RESTORE, 0, 1);
	m[n++] = custom ? am_mik(N_("刪除…"), "Delete", AMC_DELETE, 0, 1)
	                : am_mi(N_("刪除（系統提供的程式不能刪，請用「從選單隱藏」）"), AMC_DELETE, 0, 0);
	u->mx = rx + 22; u->my = ry + 12;
	pm_ui_menu(u, m, n);
}

static void am_blank_menu(PmUI *u, int rx, int ry)
{
	am_target[0] = 0;
	MenuItem m[8];
	int n = 0;
	static char lab_h[96];
	snprintf(lab_h, sizeof lab_h, "%c%s", show_hidden_apps ? '\1' : '\2', _("顯示隱藏的程式"));
	m[n++] = am_mik(N_("新增程式或指令…"), "Ctrl+N", AMC_NEW, 0, 1);
	m[n++] = AM_SEP;
	m[n++] = am_mi(lab_h, AMC_SHOWHID, 0, 1);
	m[n++] = am_mi(N_("全部還原…"), AMC_RESTORE_ALL, 0, 1);
	u->mx = rx + 22; u->my = ry + 12;
	pm_ui_menu(u, m, n);
}

static void am_cat_menu(PmUI *u, int view, int rx, int ry)
{
	am_target[0] = 0;
	MenuItem m[6];
	int n = 0;
	char key[48];
	if (view >= 0) {
		snprintf(key, sizeof key, "launcher.order.%d", view);
		m[n++] = am_mi(N_("依名稱排序（清除自訂順序）"), AMC_SORT, view, pm_conf_get(key, "")[0] != 0);
		m[n++] = AM_SEP;
	}
	if (view == AM_HID) m[n++] = am_mi(N_("全部取消隱藏"), AMC_UNHIDE_ALL, 0, 1);
	m[n++] = am_mik(N_("新增程式或指令…"), "Ctrl+N", AMC_NEW, 0, 1);
	u->mx = rx + 22; u->my = ry + 12;
	pm_ui_menu(u, m, n);
}

static void am_dock_menu(PmUI *u, int dock_idx, int rx, int ry)
{
	MenuItem m[8];
	int n = 0;
	am_target[0] = 0;
	if (dock_idx >= 0) {
		snprintf(am_target, sizeof am_target, "%s", am_dock_ids[am_dock_vis[dock_idx]]);
		m[n++] = am_mik(N_("編輯…"), "Enter", AMC_EDIT, 0, 1);
		m[n++] = am_mi(N_("往左移"), AMC_DOCK_LEFT, 0, dock_idx > 0);
		m[n++] = am_mi(N_("往右移"), AMC_DOCK_RIGHT, 0, dock_idx < am_dock_nvis - 1);
		m[n++] = am_mi(N_("從 Dock 移除"), AMC_UNPIN, 0, 1);
		m[n++] = AM_SEP;
	}
	m[n++] = am_mi(N_("還原成預設的 Dock"), AMC_DOCK_RESET, 0, 1);
	m[n++] = am_mi(N_("清空 Dock"), AMC_DOCK_CLEAR, 0, am_dock_nvis > 0);
	u->mx = rx + 22; u->my = ry + 12;
	pm_ui_menu(u, m, n);
}

static void am_open_edit(PmUI *u, const char *id)
{
	load_app_edit(id);
	page = P_APP_EDIT;
	u->scroll = 0;
	u->again = 1;
	status[0] = 0;
}

static void am_new(PmUI *u)
{
	new_app_edit();
	if (am_view >= 0) { ae.lcat = am_view + 1; ae_orig = ae; }
	page = P_APP_EDIT;
	u->scroll = 0;
	u->again = 1;
	status[0] = 0;
}

static void am_duplicate(PmUI *u, const char *id)
{
	load_app_edit(id);
	AppEdit s = ae;
	new_app_edit();
	snprintf(ae.name, sizeof ae.name, _("%s（複本）"), s.name);
	snprintf(ae.comment, sizeof ae.comment, "%s", s.comment);
	snprintf(ae.exec, sizeof ae.exec, "%s", s.exec);
	snprintf(ae.icon, sizeof ae.icon, "%s", s.icon);
	ae.cat = s.cat; ae.term = s.term; ae.lcat = s.lcat;
	ae.src[0] = 0;
	ae_orig = ae;
	page = P_APP_EDIT;
	u->scroll = 0;
	u->again = 1;
	status[0] = 0;
}

/* 執行右鍵選單、鍵盤選的動作（選單關掉之後才做，不在選單的回呼裡做） */
static void am_exec(PmUI *u)
{
	int c = am_cmd, v = am_cmd_arg;
	am_cmd = 0;
	if (!c) return;
	char id[256];
	snprintf(id, sizeof id, "%s", am_target);
	PmApp *a = id[0] ? pm_app_by_id(id) : NULL;
	char name[256] = "";
	if (a) snprintf(name, sizeof name, "%s", a->name);
	switch (c) {
	case AMC_OPEN: if (a) pm_app_launch(a); break;
	case AMC_EDIT: if (a) am_open_edit(u, id); break;
	case AMC_RENAME:
		if (a) {
			am_renaming = 1; am_rename_fresh = 1;
			snprintf(am_rename_id, sizeof am_rename_id, "%s", id);
			snprintf(am_rename_buf, sizeof am_rename_buf, "%s", name);
		}
		break;
	case AMC_DUP: if (a) am_duplicate(u, id); break;
	case AMC_PIN: dock_put(id, -1); am_say(_("已把「%s」放進 Dock"), name); break;
	case AMC_UNPIN: dock_drop(id); am_say(_("已把「%s」從 Dock 移除"), name); break;
	case AMC_HIDE: if (a && app_set_show(id, 0)) am_say(_("已從選單隱藏「%s」（在左邊的「已隱藏」找得到）"), name); break;
	case AMC_UNHIDE: if (a && app_set_show(id, 1)) am_say(_("已取消隱藏「%s」"), name); break;
	case AMC_CAT:
		if (a) {
			set_launcher_cat(id, v + 1);
			if (a->hidden) app_set_show(id, 1);
			am_say(_("已把「%s」移到%s"), name, pm_category_name(v));
			apps_changed();
		}
		break;
	case AMC_RESTORE:
		if (a) { app_restore(id); apps_changed(); am_say(_("「%s」已還原成系統預設"), name); }
		break;
	case AMC_DELETE:
		if (a && app_is_custom(id)) {
			char msg[600];
			snprintf(msg, sizeof msg, _("要刪除你自己加的「%s」嗎？\n它會從啟動器、搜尋與 Dock 一起消失；系統裡的其他程式不受影響。"), name);
			if (ask_yes(_("刪除程式"), msg, _("刪除"))) {
				app_delete_custom(id);
				apps_changed();
				am_sel[0] = 0;
				am_say(_("已刪除「%s」"), name);
			}
		}
		break;
	case AMC_NEW: am_new(u); break;
	case AMC_SHOWHID: show_hidden_apps = !show_hidden_apps; break;
	case AMC_RESTORE_ALL:
		if (ask_yes(_("全部還原"), _("要把所有程式的名稱、圖示、隱藏、分類與排列順序都還原成系統預設嗎？\n你自己加的程式與 Dock 不受影響。"), _("全部還原"))) {
			int n;
			PmApp *vv = pm_apps(&n);
			char (*ids)[256] = malloc((size_t)(n ? n : 1) * 256);
			for (int i = 0; i < n; i++) snprintf(ids[i], 256, "%s", vv[i].id);
			for (int i = 0; i < n; i++) {
				char ck[300];
				snprintf(ck, sizeof ck, "launcher.cat.%s", ids[i]);
				if (pm_conf_get(ck, "")[0]) pm_conf_set(ck, "");
				if (app_is_overridden(ids[i])) {
					load_app_edit(ids[i]);
					unlink(ae.user);
				}
			}
			free(ids);
			for (int k = 0; k < PM_CAT_N; k++) {
				char key[48];
				snprintf(key, sizeof key, "launcher.order.%d", k);
				if (pm_conf_get(key, "")[0]) pm_conf_set(key, "");
			}
			apps_changed();
			am_say("%s", _("已全部還原"));
		}
		break;
	case AMC_SORT: pm_app_order_set(v, NULL, 0); am_say(_("「%s」已依名稱排序"), pm_category_name(v)); break;
	case AMC_UNHIDE_ALL: {
		int n;
		PmApp *vv = pm_apps(&n);
		char (*ids)[256] = malloc((size_t)(n ? n : 1) * 256);
		int k = 0;
		for (int i = 0; i < n; i++) if (vv[i].hidden) snprintf(ids[k++], 256, "%s", vv[i].id);
		for (int i = 0; i < k; i++) app_set_show(ids[i], 1);
		free(ids);
		am_say("%s", _("已全部取消隱藏"));
		break;
	}
	case AMC_DOCK_RESET: pm_conf_set("dock.apps", ""); am_say("%s", _("Dock 已還原成預設")); break;
	case AMC_DOCK_CLEAR: pm_conf_set("dock.apps", "-"); am_say("%s", _("已清空 Dock")); break;
	case AMC_DOCK_LEFT: case AMC_DOCK_RIGHT: {
		am_load_dock(0);
		for (int i = 0; i < am_dock_nvis; i++)
			if (!strcmp(am_dock_ids[am_dock_vis[i]], id)) {
				int to = i + (c == AMC_DOCK_LEFT ? -1 : 1);
				if (to >= 0 && to < am_dock_nvis) {
					int raw = am_dock_vis[to];
					dock_put(id, c == AMC_DOCK_LEFT ? raw : raw + 1);
				}
				break;
			}
		break;
	}
	}
}

/* 放開滑鼠：照 dg 裡算好的結果做 */
static void am_drop(PmUI *u)
{
	(void)u;
	PmApp *a = pm_app_by_id(dg.id);
	if (!a) return;
	char name[256], id[256];
	snprintf(name, sizeof name, "%s", a->name);
	snprintf(id, sizeof id, "%s", a->id);
	if (dg.to_dock >= 0) {
		int had = dock_has(id);
		dock_put(id, dg.to_dock);
		am_say(had ? _("已調整「%s」在 Dock 的位置") : _("已把「%s」放進 Dock"), name);
	} else if (dg.to_view == AM_HID) {
		if (app_set_show(id, 0)) am_say(_("已從選單隱藏「%s」（在左邊的「已隱藏」找得到）"), name);
	} else if (dg.to_view >= 0) {
		int hid = a->hidden;
		if (pm_app_category(a) != dg.to_view || hid) {
			set_launcher_cat(id, dg.to_view + 1);
			if (hid) app_set_show(id, 1);
			apps_changed();
			am_say(_("已把「%s」移到%s"), name, pm_category_name(dg.to_view));
		}
	} else if (dg.to_list >= 0 && dg.src == DS_LIST) {
		int s = am_find(id);
		if (s >= 0) {
			int to = dg.to_list > s ? dg.to_list - 1 : dg.to_list;
			if (to != s) {
				PmApp *tmp[AM_MAX];
				int k = 0;
				for (int i = 0; i < am_n; i++) if (i != s) tmp[k++] = am_list[i];
				for (int i = k; i > to; i--) tmp[i] = tmp[i - 1];
				tmp[to] = a;
				pm_app_order_set(am_view, tmp, am_n);
				am_say(_("已調整「%s」的順序"), name);
			}
		}
	} else if (dg.to_out && dg.src == DS_DOCK) {
		dock_drop(id);
		am_say(_("已把「%s」從 Dock 移除"), name);
	}
}

/* 拖曳中：算出現在放開會放到哪裡 */
static void am_drag_target(PmUI *u)
{
	dg.to_view = DG_NOVIEW; dg.to_dock = -1; dg.to_list = -1; dg.to_out = 0;
	int x = u->mx, y = u->my;
	PmApp *a = pm_app_by_id(dg.id);
	if (!a) return;
	if (in_r(&L.dock, x, y)) {
		am_load_dock(dg.src == DS_DOCK);
		int idx = am_dock_insert_at(x, am_dock_nvis);
		dg.to_dock = idx < am_dock_nvis ? am_dock_vis[idx] : am_dock_nraw;
		return;
	}
	if (dg.src == DS_DOCK) { dg.to_out = 1; return; }   /* 從 Dock 拖出來：放在別的地方就是移除 */
	for (int i = 0; i < PM_CAT_N + 2; i++)
		if (in_r(&L.rows[i], x, y) && L.rowview[i] != -1) {
			int v = L.rowview[i];
			if (v >= 0 && !a->hidden && pm_app_category(a) == v) return;   /* 本來就在這個分類 */
			if (v == AM_HID && a->hidden) return;
			dg.to_view = v;
			return;
		}
	if (in_r(&L.main, x, y) && am_view >= 0 && !app_q[0] && !a->hidden && pm_app_category(a) == am_view)
		dg.to_list = am_insert_at(x, y);
}

/* ---- 畫出左邊分類、程式清單／圖示格、Dock 放置區 ---- */

static void am_draw_side(PmUI *u, cairo_t *cr)
{
	int counts[PM_CAT_N] = { 0 }, total = 0, hidden = 0, n;
	PmApp *v = pm_apps(&n);
	for (int i = 0; i < n; i++) {
		if (v[i].hidden) { hidden++; continue; }
		counts[pm_app_category(&v[i])]++;
		total++;
	}
	for (int i = 0; i < PM_CAT_N + 2; i++) {
		AmR *r = &L.rows[i];
		int view = L.rowview[i];
		int on = !app_q[0] && am_view == view;
		int drop = dg.state == DG_DRAG && dg.to_view == view;
		int cnt = view == -1 ? total : view == AM_HID ? hidden : counts[view];
		unsigned col = view >= 0 ? am_catcol[view] : T->sel;
		if (on) {
			pm_rrect(cr, r->x, r->y, r->w, r->h, 9, 9);
			pm_rgb(cr, T->sel);
			cairo_fill(cr);
		} else if (drop) {
			pm_rrect(cr, r->x, r->y, r->w, r->h, 9, 9);
			pm_rgba(cr, T->sel, 0.20);
			cairo_fill_preserve(cr);
			pm_rgb(cr, T->sel);
			cairo_set_line_width(cr, 2);
			cairo_stroke(cr);
		}
		am_row_glyph(cr, view, r->x + 10, r->y + r->h / 2.0, on ? 0xffffff : col);
		char nm[120];
		snprintf(nm, sizeof nm, "%s", view == AM_HID ? _("已隱藏") : pm_category_name(view));
		am_text_at(cr, r->x + 32, r->y + r->h / 2.0, r->w - 32 - 38, nm, 13.5, on, on ? 0xffffff : am_col_text, 1);
		char cs[16];
		snprintf(cs, sizeof cs, "%d", cnt);
		pm_font(cr, 12, 0);
		pm_rgba(cr, on ? 0xffffff : am_col_dim, on ? 0.85 : 1);
		pm_text(cr, r->x + r->w - 10 - pm_textw(cr, cs), r->y + r->h / 2.0, cs);
	}
	(void)u;
}

static void am_draw_item(cairo_t *cr, int i, int reorder_ok)
{
	PmApp *a = am_list[i];
	AmR r = am_item_rect(i);
	int sel = !strcmp(a->id, am_sel);
	int lifted = dg.state == DG_DRAG && dg.src == DS_LIST && !strcmp(dg.id, a->id);
	double alpha = lifted ? 0.30 : (a->hidden ? 0.55 : 1.0);
	int pinned = dock_has(a->id);
	if (am_grid) {
		if (sel) {
			pm_rrect(cr, r.x + 4, r.y + 3, r.w - 8, r.h - 6, 12, 12);
			pm_rgba(cr, T->sel, 0.16);
			cairo_fill_preserve(cr);
			pm_rgb(cr, T->sel);
			cairo_set_line_width(cr, 1.5);
			cairo_stroke(cr);
		}
		am_draw_icon(cr, a->icon, r.x + (r.w - 52) / 2.0, r.y + 12, 52, alpha);
		pm_font(cr, 12.5, 0);
		char buf[200];
		pm_ellipsize(cr, a->name, r.w - 14, buf, sizeof buf);
		pm_rgba(cr, am_col_text, alpha);
		pm_text(cr, r.x + (r.w - pm_textw(cr, buf)) / 2, r.y + 78, buf);
		double bx = r.x + r.w - 18;
		cairo_new_path(cr);
		if (pinned) { cairo_arc(cr, bx, r.y + 14, 4.5, 0, 2 * M_PI); pm_rgb(cr, T->sel); cairo_fill(cr); bx -= 12; }
		if (a->hidden) { cairo_arc(cr, bx, r.y + 14, 4.5, 0, 2 * M_PI); pm_rgb(cr, am_col_dim); cairo_fill(cr); }
		return;
	}
	if (sel) {
		pm_rrect(cr, r.x + 5, r.y + 3, r.w - 10, r.h - 6, 10, 10);
		pm_rgba(cr, T->sel, 0.16);
		cairo_fill_preserve(cr);
		pm_rgb(cr, T->sel);
		cairo_set_line_width(cr, 1.5);
		cairo_stroke(cr);
	} else if (i) {   /* 兩列之間的細線 */
		pm_rgb(cr, am_col_line);
		cairo_rectangle(cr, r.x + 62, r.y, r.w - 74, 1);
		cairo_fill(cr);
	}
	double x = r.x + 14, yc = r.y + r.h / 2.0;
	if (reorder_ok) { am_handle(cr, x, yc, alpha * 0.8); x += 18; }
	am_draw_icon(cr, a->icon, x, yc - 18, 36, alpha);
	/* 右邊的標籤 */
	double rx = r.x + r.w - 16;
	if (app_is_custom(a->id)) am_pill(cr, &rx, yc, _("自訂"), 0xff9f0a);
	if (a->hidden) am_pill(cr, &rx, yc, _("已隱藏"), 0x8e8e93);
	if (pinned) am_pill(cr, &rx, yc, "Dock", 0x0a84ff);
	double tw = MAX(60.0, rx - (x + 36 + 12));
	am_text_at(cr, x + 36 + 12, yc - (a->comment ? 8 : 0), tw, a->name, 14, 0, am_col_text, alpha);
	if (a->comment) am_text_at(cr, x + 36 + 12, yc + 10, tw, a->comment, 11.5, 0, am_col_dim, alpha);
}

static void am_draw_main(PmUI *u, cairo_t *cr)
{
	AmR *m = &L.main;
	pm_rrect(cr, m->x, m->y, m->w, m->h, 12, 12);
	pm_rgb(cr, am_col_card);
	cairo_fill(cr);
	cairo_save(cr);
	pm_rrect(cr, m->x, m->y, m->w, m->h, 12, 12);
	cairo_clip(cr);
	int reorder_ok = am_view >= 0 && !app_q[0];
	if (!am_n) {
		const char *t = app_q[0] ? _("沒有符合的程式") : am_view == AM_HID ? _("沒有隱藏的程式") : _("這個分類還沒有程式");
		pm_font(cr, 14, 0);
		pm_rgb(cr, am_col_dim);
		pm_text(cr, m->x + (m->w - pm_textw(cr, t)) / 2, m->y + m->h / 2.0 - 10, t);
		if (!app_q[0] && am_view != AM_HID) {
			const char *h = _("把程式拖到左邊的分類，或按右鍵新增");
			pm_font(cr, 12, 0);
			pm_text(cr, m->x + (m->w - pm_textw(cr, h)) / 2, m->y + m->h / 2.0 + 14, h);
		}
	}
	int first = MAX(0, am_scroll / L.cellh) * L.cols, last = MIN(am_n, ((am_scroll + m->h) / L.cellh + 1) * L.cols);
	for (int i = first; i < last; i++) am_draw_item(cr, i, reorder_ok);
	/* 插入位置的指示線 */
	if (dg.state == DG_DRAG && dg.to_list >= 0) {
		int ins = dg.to_list;
		cairo_new_path(cr);
		pm_rgb(cr, T->sel);
		if (am_grid) {
			int r = ins / L.cols, c = ins % L.cols;
			double x = m->x + c * L.cellw, y = m->y + r * L.cellh - am_scroll;
			if (ins == am_n && c == 0 && ins > 0) { r = (ins - 1) / L.cols; c = L.cols; x = m->x + L.cellw * L.cols; y = m->y + r * L.cellh - am_scroll; }
			pm_rrect(cr, x - 1.5, y + 8, 3, L.cellh - 16, 1.5, 1.5);
			cairo_fill(cr);
		} else {
			double y = CLAMP((double)(m->y + ins * L.cellh - am_scroll), m->y + 3.0, m->y + m->h - 3.0);
			pm_rrect(cr, m->x + 10, y - 1.5, m->w - 20, 3, 1.5, 1.5);
			cairo_fill(cr);
			cairo_arc(cr, m->x + 12, y, 4.5, 0, 2 * M_PI);
			cairo_fill(cr);
		}
	}
	cairo_restore(cr);
	/* 捲軸 */
	if (L.content_h > m->h) {
		double bh = MAX(30.0, (double)m->h * m->h / L.content_h);
		double by = m->y + (m->h - bh) * am_scroll / (L.content_h - m->h);
		pm_rrect(cr, m->x + m->w - 8, by + 2, 5, bh - 4, 2.5, 2.5);
		pm_rgba(cr, pm_dark ? 0xffffff : 0x000000, 0.25);
		cairo_fill(cr);
	}
	/* 拖到清單邊緣時自動捲動 */
	if (dg.state == DG_DRAG && dg.src == DS_LIST && u->down && in_r(&L.main, u->mx, u->my)) {
		if (u->my < m->y + 28) am_scroll -= 14;
		else if (u->my > m->y + m->h - 28) am_scroll += 14;
		am_scroll = CLAMP(am_scroll, 0, MAX(0, L.content_h - m->h));
	}
}

static void am_draw_dock(PmUI *u, cairo_t *cr)
{
	AmR *d = &L.dock;
	int dragging = dg.state == DG_DRAG, out = dragging && dg.to_out, over = dragging && dg.to_dock >= 0;
	pm_rrect(cr, d->x, d->y, d->w, d->h, 14, 14);
	pm_rgb(cr, am_col_card);
	cairo_fill_preserve(cr);
	if (out) { pm_rgba(cr, 0xff453a, 0.14); cairo_fill_preserve(cr); pm_rgb(cr, 0xff453a); cairo_set_line_width(cr, 2); }
	else if (over || (dragging && dg.src == DS_LIST)) { pm_rgba(cr, T->sel, over ? 0.14 : 0.06); cairo_fill_preserve(cr); pm_rgb(cr, T->sel); cairo_set_line_width(cr, over ? 2 : 1); }
	else { pm_rgb(cr, am_col_line); cairo_set_line_width(cr, 1); }
	cairo_stroke(cr);
	pm_font(cr, 12.5, 1);
	pm_rgb(cr, am_col_text);
	pm_text(cr, d->x + 16, d->y + 16, "Dock");
	double hx = d->x + 16 + pm_textw(cr, "Dock") + 10;
	am_text_at(cr, hx, d->y + 16, d->w - (hx - d->x) - 14, out ? _("放開就從 Dock 移除") : _("把程式拖到這裡固定在 Dock；在這裡拖曳排順序，拖出去就移除"),
	           11.5, 0, out ? 0xff453a : am_col_dim, 1);
	int nv = am_dock_nvis, x0 = am_dock_x0(nv);
	double pw = MAX(nv, 1) * (AM_PILL + 10) + 10, px = x0 - 10, py = d->y + 28, ph = AM_PILL + 12;
	pm_rrect(cr, px, py, pw, ph, 14, 14);
	pm_rgba(cr, pm_dark ? 0xffffff : 0x000000, pm_dark ? 0.10 : 0.06);
	cairo_fill(cr);
	if (!nv) {
		pm_font(cr, 12.5, 0);
		pm_rgb(cr, am_col_dim);
		const char *t = _("Dock 是空的，把程式拖到這裡");
		pm_text(cr, d->x + (d->w - pm_textw(cr, t)) / 2, py + ph / 2, t);
	}
	for (int i = 0; i < nv; i++) {
		int raw = am_dock_vis[i];
		double ix = x0 + i * (AM_PILL + 10);
		if (!strcmp(am_dock_ids[raw], am_sel)) {
			pm_rrect(cr, ix - 4, py + 2, AM_PILL + 8, ph - 4, 11, 11);
			pm_rgba(cr, T->sel, 0.22);
			cairo_fill_preserve(cr);
			pm_rgb(cr, T->sel);
			cairo_set_line_width(cr, 1.5);
			cairo_stroke(cr);
		}
		am_draw_icon(cr, am_dock_app[raw]->icon, ix, py + 6, AM_PILL, 1);
	}
	/* 插入位置 */
	if (over) {
		int slot = nv;
		for (int i = 0; i < nv; i++) if (am_dock_vis[i] >= dg.to_dock) { slot = i; break; }
		double ix = x0 + slot * (AM_PILL + 10) - 5;
		pm_rrect(cr, ix - 1.5, py + 4, 3, ph - 8, 1.5, 1.5);
		pm_rgb(cr, T->sel);
		cairo_fill(cr);
	}
	(void)u;
}

/* ---- 整頁 ---- */

static void am_keys(PmUI *u)
{
	KeySym k = u->key;
	int ctrl = u->keystate & ControlMask;
	int cur = am_find(am_sel), done = 1;
	if (ctrl && (k == XK_n || k == XK_N)) { am_new(u); u->key = 0; return; }
	switch (k) {
	case XK_Escape:
		if (dg.state) dg.state = DG_NONE;
		else am_sel[0] = 0;
		break;
	case XK_Up: case XK_Down: case XK_Left: case XK_Right: case XK_Home: case XK_End: {
		if (!am_n) break;
		int to = cur < 0 ? 0 : cur;
		if (cur >= 0) {
			if (k == XK_Up) to = cur - L.cols;
			else if (k == XK_Down) to = cur + L.cols;
			else if (k == XK_Left && am_grid) to = cur - 1;
			else if (k == XK_Right && am_grid) to = cur + 1;
			else if (k == XK_Home) to = 0;
			else if (k == XK_End) to = am_n - 1;
		}
		to = CLAMP(to, 0, am_n - 1);
		snprintf(am_sel, sizeof am_sel, "%s", am_list[to]->id);
		AmR r = am_item_rect(to);   /* 捲到看得見 */
		if (r.y < L.main.y) am_scroll -= L.main.y - r.y;
		else if (r.y + r.h > L.main.y + L.main.h) am_scroll += r.y + r.h - L.main.y - L.main.h;
		break;
	}
	case XK_Return: case XK_KP_Enter:
		if (cur >= 0) { snprintf(am_target, sizeof am_target, "%s", am_sel); am_cmd = AMC_EDIT; }
		break;
	case XK_F2:
		if (cur >= 0) { snprintf(am_target, sizeof am_target, "%s", am_sel); am_cmd = AMC_RENAME; }
		break;
	case XK_Delete: case XK_KP_Delete:
		if (cur >= 0) {
			snprintf(am_target, sizeof am_target, "%s", am_sel);
			am_cmd = app_is_custom(am_sel) ? AMC_DELETE : am_list[cur]->hidden ? AMC_UNHIDE : AMC_HIDE;
		}
		break;
	default: done = 0;
	}
	if (done) u->key = 0;
}

static void build_apps(PmUI *u)
{
	u->y += u->scroll;   /* 這一頁自己管捲動（只有程式清單會捲），整個視窗不捲 */
	u->scroll = 0;
	cairo_t *cr = u->cr;
	if (pm_dark) { am_col_card = 0x2c2c2e; am_col_line = 0x3a3a3c; am_col_text = 0xf2f2f2; am_col_dim = 0x98989d; am_col_bg = 0x1c1c1e; }
	else { am_col_card = 0xffffff; am_col_line = 0xe5e5ea; am_col_text = 0x1d1d1f; am_col_dim = 0x8e8e93; am_col_bg = 0xf2f2f7; }
	if (am_grid < 0) am_grid = pm_conf_int("appmenu.grid", 0);
	int ox = u->x, ocw = u->cw;
	u->x = 16;
	u->cw = u->w - 32;
	int stash_click = 0;
	if (am_renaming) { stash_click = u->clicked; u->clicked = 0; }   /* 重新命名的小視窗開著時，後面的東西不要反應 */
	if (pm_ui_back(u, _("系統設定"))) { u->x = ox; u->cw = ocw; go(u, P_MAIN); return; }
	int ty = u->y;
	pm_ui_title(u, _("應用程式選單"), NULL);
	int top = u->y + 2;

	/* 右上角：清單／圖示格切換、新增 */
	L.tool_new = (AmR){ u->w - 16 - 92, ty + 4, 92, 30 };
	L.tool_grid = (AmR){ L.tool_new.x - 12 - 54, ty + 4, 54, 30 };
	L.tool_list = (AmR){ L.tool_grid.x - 54, ty + 4, 54, 30 };
	if (am_hit(u, &L.tool_new)) am_cmd = AMC_NEW;
	if (am_hit(u, &L.tool_list) && am_grid) { am_grid = 0; pm_conf_set_int("appmenu.grid", 0); am_scroll = 0; }
	if (am_hit(u, &L.tool_grid) && !am_grid) { am_grid = 1; pm_conf_set_int("appmenu.grid", 1); am_scroll = 0; }
	pm_rrect(cr, L.tool_list.x, L.tool_list.y, 108, 30, 8, 8);
	pm_rgb(cr, am_col_line);
	cairo_fill(cr);
	{
		const AmR *on = am_grid ? &L.tool_grid : &L.tool_list;
		pm_rrect(cr, on->x + 2, on->y + 2, on->w - 4, on->h - 4, 6, 6);
		pm_rgb(cr, am_col_card);
		cairo_fill(cr);
		am_text_at(cr, L.tool_list.x + 14, L.tool_list.y + 15, 40, _("清單"), 12.5, !am_grid, am_col_text, 1);
		am_text_at(cr, L.tool_grid.x + 14, L.tool_grid.y + 15, 40, _("圖示"), 12.5, am_grid, am_col_text, 1);
	}
	pm_rrect(cr, L.tool_new.x, L.tool_new.y, L.tool_new.w, L.tool_new.h, 8, 8);
	pm_rgb(cr, T->sel);
	cairo_fill(cr);
	pm_font(cr, 13, 1);
	pm_rgb(cr, 0xffffff);
	{
		const char *t = _("新增…");
		double tw = pm_textw(cr, t) + 18;
		double x = L.tool_new.x + (L.tool_new.w - tw) / 2;
		cairo_set_line_width(cr, 2);
		cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
		cairo_move_to(cr, x + 5, L.tool_new.y + 15);
		cairo_line_to(cr, x + 13, L.tool_new.y + 15);
		cairo_move_to(cr, x + 9, L.tool_new.y + 11);
		cairo_line_to(cr, x + 9, L.tool_new.y + 19);
		cairo_stroke(cr);
		pm_text(cr, x + 18, L.tool_new.y + 15, t);
	}

	am_load_list();
	am_load_dock(dg.state == DG_DRAG && dg.src == DS_DOCK);
	am_layout(u, top);

	/* 左上：搜尋 */
	{
		u->x = L.search.x;
		u->cw = L.search.w;
		u->y = L.search.y;
		pm_ui_group(u, NULL);
		int sid = u->id + 1;
		pm_ui_entry(u, NULL, app_q, sizeof app_q, 0);
		pm_ui_end(u);
		if (!app_q[0] && u->focus != sid) {
			cairo_new_path(cr);
			cairo_arc(cr, L.search.x + 28, L.search.y + 21, 4.5, 0, 2 * M_PI);
			pm_rgb(cr, am_col_dim);
			cairo_set_line_width(cr, 1.4);
			cairo_stroke(cr);
			cairo_move_to(cr, L.search.x + 31.5, L.search.y + 24.5);
			cairo_line_to(cr, L.search.x + 35, L.search.y + 28);
			cairo_stroke(cr);
			am_text_at(cr, L.search.x + 40, L.search.y + 22, 80, _("搜尋"), 13, 0, am_col_dim, 1);
		}
		u->x = 16;
		u->cw = u->w - 32;
	}

	/* ---- 輸入 ---- */
	int rx, ry, rc = pm_ui_rclick_at(u, &rx, &ry);
	if (pm_ui_wheel_steps) {
		if (in_r(&L.main, u->mx, u->my)) am_scroll += pm_ui_wheel_steps * 52;
		pm_ui_wheel_steps = 0;
	}
	if (am_renaming) { u->clicked = stash_click; rc = 0; }
	if (u->clicked && !u->claimed && !am_renaming) {
		int x = u->mx, y = u->my, hit_row = -1;
		for (int i = 0; i < PM_CAT_N + 2; i++) if (in_r(&L.rows[i], x, y)) hit_row = i;
		int di = am_dock_item_at(x, y), mi = am_item_at(x, y);
		if (hit_row >= 0 || di >= 0 || mi >= 0) u->focus = 0;   /* 點到別的東西：搜尋框不再拿著鍵盤，Delete、F2 這些快捷鍵才有用 */
		if (hit_row >= 0) {
			am_view = L.rowview[hit_row];
			app_q[0] = 0;
			am_scroll = 0;
			u->claimed = 1;
		} else if (di >= 0) {
			int raw = am_dock_vis[di];
			snprintf(am_sel, sizeof am_sel, "%s", am_dock_ids[raw]);
			dg = (typeof(dg)){ .state = DG_PRESS, .src = DS_DOCK, .x0 = x, .y0 = y, .size = AM_PILL, .to_view = DG_NOVIEW, .to_dock = -1, .to_list = -1 };
			snprintf(dg.id, sizeof dg.id, "%s", am_dock_ids[raw]);
			dg.ox = x - (am_dock_x0(am_dock_nvis) + di * (AM_PILL + 10));
			dg.oy = y - (L.dock.y + 34);
			if (u->dbl) { snprintf(am_target, sizeof am_target, "%s", am_sel); am_cmd = AMC_EDIT; dg.state = DG_NONE; }
			u->claimed = 1;
		} else if (mi >= 0) {
			PmApp *a = am_list[mi];
			snprintf(am_sel, sizeof am_sel, "%s", a->id);
			AmR r = am_item_rect(mi);
			int isz = am_grid ? 52 : 36;
			dg = (typeof(dg)){ .state = DG_PRESS, .src = DS_LIST, .x0 = x, .y0 = y, .size = isz, .to_view = DG_NOVIEW, .to_dock = -1, .to_list = -1 };
			snprintf(dg.id, sizeof dg.id, "%s", a->id);
			dg.ox = CLAMP(x - (am_grid ? r.x + (r.w - 52) / 2 : r.x + (am_view >= 0 && !app_q[0] ? 32 : 14)), 0, isz);
			dg.oy = CLAMP(y - (am_grid ? r.y + 12 : r.y + (r.h - 36) / 2), 0, isz);
			if (u->dbl) { snprintf(am_target, sizeof am_target, "%s", a->id); am_cmd = AMC_EDIT; dg.state = DG_NONE; }
			u->claimed = 1;
		} else if (in_r(&L.main, x, y)) {
			am_sel[0] = 0;
		}
	}
	if (dg.state == DG_PRESS) {
		u->active = 77;   /* 按住中：滑鼠一動就重畫，才量得出有沒有開始拖 */
		if (!u->down) dg.state = DG_NONE;
		else if (abs(u->mx - dg.x0) > 5 || abs(u->my - dg.y0) > 5) dg.state = DG_DRAG;
	}
	if (dg.state == DG_DRAG) {
		am_drag_target(u);
		u->active = 77;   /* 拖曳中：滑鼠一動就重畫 */
		if (u->released) { am_drop(u); dg.state = DG_NONE; u->again = 1; }
	} else if (u->released) dg.state = DG_NONE;
	if (rc && dg.state == DG_NONE) {
		int di = am_dock_item_at(rx, ry), mi = am_item_at(rx, ry), row = -1;
		for (int i = 0; i < PM_CAT_N + 2; i++) if (in_r(&L.rows[i], rx, ry)) row = i;
		if (di >= 0) { snprintf(am_sel, sizeof am_sel, "%s", am_dock_ids[am_dock_vis[di]]); am_dock_menu(u, di, rx, ry); }
		else if (in_r(&L.dock, rx, ry)) am_dock_menu(u, -1, rx, ry);
		else if (row >= 0) am_cat_menu(u, L.rowview[row], rx, ry);
		else if (mi >= 0) { snprintf(am_sel, sizeof am_sel, "%s", am_list[mi]->id); am_app_menu(u, am_list[mi], rx, ry); }
		else if (in_r(&L.main, rx, ry) || ry > L.main.y) am_blank_menu(u, rx, ry);
	}
	if (u->key && !u->focus && !am_renaming) am_keys(u);
	if (am_cmd) {
		am_exec(u);
		u->again = 1;
		if (page != P_APPS) { u->x = ox; u->cw = ocw; return; }   /* 跳去編輯頁了 */
	}

	/* 資料可能變了：重讀一次再畫 */
	am_load_list();
	am_load_dock(dg.state == DG_DRAG && dg.src == DS_DOCK);
	am_layout(u, top);

	am_draw_side(u, cr);
	am_draw_main(u, cr);
	am_draw_dock(u, cr);

	/* 底下一行：剛做的事，或操作提示 */
	{
		int y = u->h - 18;
		if (am_msg[0]) {
			cairo_new_path(cr);
			cairo_arc(cr, 22, y, 3.5, 0, 2 * M_PI);
			pm_rgb(cr, 0x34c759);
			cairo_fill(cr);
			am_text_at(cr, 32, y, u->w - 48, am_msg, 12.5, 0, am_col_text, 1);
		} else {
			am_text_at(cr, 16, y, u->w - 32,
			           _("拖到左邊的分類＝改分類；同分類內拖曳＝排順序；拖到 Dock＝固定。按右鍵看更多。"),
			           11.5, 0, am_col_dim, 1);
		}
	}

	/* 重新命名的小視窗 */
	if (am_renaming) {
		pm_rgba(cr, pm_dark ? 0x000000 : 0xffffff, 0.55);
		cairo_rectangle(cr, 0, top - 4, u->w, u->h - top + 4);
		cairo_fill(cr);
		double cw = MIN(380, u->w - 48), cx = (u->w - cw) / 2, cy = L.main.y + 40, ch = 112;
		pm_rrect(cr, cx + 1, cy + 4, cw, ch, 14, 14);
		pm_rgba(cr, 0x000000, 0.18);
		cairo_fill(cr);
		pm_rrect(cr, cx, cy, cw, ch, 14, 14);
		pm_rgb(cr, am_col_bg);
		cairo_fill_preserve(cr);
		pm_rgb(cr, am_col_line);
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
		am_text_at(cr, cx + 18, cy + 22, cw - 36, _("重新命名"), 15, 1, am_col_text, 1);
		AmR card = { (int)cx, (int)cy, (int)cw, (int)ch };
		if (stash_click && !in_r(&card, u->mx, u->my)) { am_renaming = 0; stash_click = 0; }
		else {
			u->x = (int)cx + 6;
			u->cw = (int)cw - 12;
			u->y = (int)cy + 38;
			u->claimed = 0;
			pm_ui_group(u, NULL);
			int myid = u->id + 1;
			if (am_rename_fresh) { u->focus = myid; am_rename_fresh = 0; }
			int enter = pm_ui_entry(u, NULL, am_rename_buf, sizeof am_rename_buf, 0);
			pm_ui_end(u);
			if (stash_click) u->claimed = 1;
			am_text_at(cr, cx + 18, cy + ch - 20, cw - 36, _("按 Enter 儲存，按 Esc 取消"), 12, 0, am_col_dim, 1);
			if (enter) {
				if (am_rename_buf[0] && app_rename(am_rename_id, am_rename_buf)) am_say(_("已重新命名為「%s」"), am_rename_buf);
				am_renaming = 0;
				u->again = 1;
			} else if (u->focus != myid) { am_renaming = 0; u->again = 1; }
		}
	}
	am_ghost(cr, u);
	u->x = ox;
	u->cw = ocw;
	u->y = 0;
}

static void go(PmUI *u, int p)
{
	/* 離開應用程式選單頁（80～300 個程式的清單、圖示）就放掉，不要常駐 */
	if (page == P_APPS && p != P_APPS && p != P_APP_EDIT) { pm_apps_free(); malloc_trim(0); }
	page = p;
	status[0] = 0;
	u->scroll = 0;
	u->again = 1;
	switch (p) {
	case P_GENERAL: read_root(); if (upd_count < 0) refresh_updates(); break;
	case P_ABOUT: read_info(); break;
	case P_UPDATE: read_auto(); if (!upd_checked) refresh_updates(); break;
	case P_STORAGE: read_root(); break;
	case P_DATETIME: read_time(); set_date[0] = set_clock[0] = 0; break;
	case P_DEVICES: read_devices(); break;
	case P_NOTIFY: read_notify_apps(); break;
	case P_KEYBOARD:
		rep_delay = pm_conf_int("keyboard.repeat_delay", 500);
		rep_rate = pm_conf_int("keyboard.repeat_rate", 30);
		break;
	case P_MOUSE:
		mouse_speed = atof(pm_conf_get("mouse.speed", "0"));
		touch_speed = atof(pm_conf_get("touchpad.speed", "0"));
		apply_pointer();   /* 順便偵測有沒有觸控板 */
		break;
	case P_SCREENTIME: read_screentime(); st_clear_confirm = 0; break;
	case P_USERS: read_users(); break;
	case P_PRINTERS: read_printers(); nscanners = -1; break;
	case P_CAMERA: read_cameras(); pv_err[0] = 0; break;
	case P_LOGIN: read_login(); break;
	case P_APPS: pm_apps_free(); am_msg[0] = 0; am_renaming = 0; dg.state = DG_NONE; break;
	}
}

/* ------------------------------------------------------------------ */
/* 健康檢查已併入主機醫生（pomelo-doctor）：這裡只剩一個轉介頁（舊的 --page health 與書籤會到這裡） */
/* ------------------------------------------------------------------ */

static void build_health(PmUI *u)
{
	if (pm_ui_back(u, _("系統設定"))) { go(u, P_MAIN); return; }
	pm_ui_title(u, _("健康檢查"), NULL);
	if (pm_have("pomelo-doctor")) {
		if (pm_ui_hero(u, PM_CHK_INFO, _("健康檢查搬到主機醫生了"), _("體檢、歷史與趨勢、為什麼變慢、安全總覽都在那裡"), _("開啟主機醫生"))) pm_spawn("pomelo-doctor --page check");
	} else pm_ui_hero(u, PM_CHK_SKIP, _("健康檢查搬到主機醫生了"), _("請先安裝主機醫生（pomelo-doctor）"), NULL);
}

/* ------------------------------------------------------------------ */
/* 病毒掃描、套件清單（SBOM／CVE）已經搬到主機醫生（pomelo-doctor）：這裡只剩轉介頁（舊的 --page virus／sbom 與書籤用） */
/* ------------------------------------------------------------------ */

static void build_moved(PmUI *u)
{
	if (pm_ui_back(u, _("系統設定"))) { go(u, P_MAIN); return; }
	const char *what = page == P_VIRUS ? _("病毒掃描") : _("套件清單（SBOM）");
	pm_ui_title(u, what, NULL);
	if (pm_have("pomelo-doctor")) {
		if (pm_ui_hero(u, PM_CHK_INFO, _("搬到主機醫生了"), _("病毒掃描、套件與漏洞、體檢、診斷、安全總覽都在那裡"), _("開啟主機醫生"))) pm_spawn(page == P_VIRUS ? "pomelo-doctor --page virus" : "pomelo-doctor --page sbom");
	} else pm_ui_hero(u, PM_CHK_SKIP, _("搬到主機醫生了"), _("請先安裝主機醫生（pomelo-doctor）"), NULL);
}

static void build(PmUI *u)
{
	switch (page) {
	case P_MAIN: build_main(u); break;
	case P_GENERAL: build_general(u); break;
	case P_ABOUT: build_about(u); break;
	case P_UPDATE: build_update(u); break;
	case P_STORAGE: build_storage(u); break;
	case P_DATETIME: build_datetime(u); break;
	case P_DEVICES: build_devices(u); break;
	case P_POWER: build_power(u); break;
	case P_SPOTLIGHT: build_spotlight(u); break;
	case P_NOTIFY: build_notify(u); break;
	case P_KEYBOARD: build_keyboard(u); break;
	case P_MOUSE: build_mouse(u); break;
	case P_SCREENTIME: build_screentime(u); break;
	case P_USERS: build_users(u); break;
	case P_PRINTERS: build_printers(u); break;
	case P_CAMERA: build_camera(u); break;
	case P_LOGIN: build_login(u); break;
	case P_APPS: build_apps(u); break;
	case P_APP_EDIT: build_app_edit(u); break;
	case P_HEALTH: build_health(u); break;
	case P_VIRUS: case P_SBOM: build_moved(u); break;
	}
	if (status[0] && page != P_LOGIN && page != P_APPS) pm_ui_note(u, status);
}

/* 有動畫、背景工作、拖曳自動捲動、檢查進度的時候 tick 要密（66 ms）；其餘閒置就拉長到 250 ms（閒置喚醒 15 → 4 次/秒） */
static int tick_busy(void)
{
	if (job != 0 || worker_started || dg.state != DG_NONE || pv_fd >= 0) return 1;
	return page == P_CAMERA;
}

static void tick_body(PmUI *u);
static void tick(PmUI *u)
{
	tick_body(u);
	pm_ui_tick_ms(tick_busy() ? 0 : 250);
}

static void tick_body(PmUI *u)
{
	if (page == P_LOGIN && lc.bg == 1 && lc_thumb_fill_one()) pm_ui_redraw(u);
	if (page == P_APPS && dg.state == DG_DRAG) pm_ui_redraw(u);   /* 拖到清單邊緣時持續自動捲動 */
	int had_job = job > 0;
	poll_job();
	if (had_job && job <= 0) pm_ui_redraw(u);   /* 做完了馬上更新畫面（結果訊息、按鈕），不要等下一次滑鼠事件 */
	if (job <= 0) while (waitpid(-1, NULL, WNOHANG) > 0) ;   /* 其他已結束的子行程（預覽、打開程式…） */
	poll_worker();
	static long last;
	if (page == P_CAMERA && pv_fd >= 0) { preview_poll(); pm_ui_redraw(u); return; }
	/* 日期與時間頁每秒更新；有背景工作時也要重畫（等結果） */
	long now = pm_now_ms();
	if (((page == P_DATETIME || job || worker_started) && now - last >= 1000) || now - last > 30000) {
		last = now;
		pm_ui_redraw(u);
	}
}

/* 程式圖示：齒輪 */
static cairo_surface_t *icon(int size)
{
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
	cairo_t *cr = cairo_create(s);
	cairo_scale(cr, size / 128.0, size / 128.0);
	pm_rrect(cr, 10, 10, 108, 108, 26, 26);
	cairo_pattern_t *g = cairo_pattern_create_linear(0, 10, 0, 118);
	cairo_pattern_add_color_stop_rgb(g, 0, 0.62, 0.64, 0.68);
	cairo_pattern_add_color_stop_rgb(g, 1, 0.40, 0.42, 0.46);
	cairo_set_source(cr, g);
	cairo_fill(cr);
	cairo_pattern_destroy(g);
	cairo_set_source_rgb(cr, 0.95, 0.95, 0.96);
	for (int i = 0; i < 10; i++) {
		cairo_save(cr);
		cairo_translate(cr, 64, 64);
		cairo_rotate(cr, i * M_PI / 5);
		cairo_rectangle(cr, -7, -40, 14, 14);
		cairo_fill(cr);
		cairo_restore(cr);
	}
	cairo_arc(cr, 64, 64, 30, 0, 2 * M_PI);
	cairo_fill(cr);
	cairo_set_source_rgb(cr, 0.48, 0.50, 0.54);
	cairo_arc(cr, 64, 64, 12, 0, 2 * M_PI);
	cairo_fill(cr);
	cairo_destroy(cr);
	return s;
}

/* 用 --page 開啟時，第一次畫面前先切到那一頁；--edit-app 程式代號：直接編輯那個程式 */
static int start_page = P_MAIN;
static char start_app[256];
static int start_new;
static void build_first(PmUI *u)
{
	static int done;
	if (!done) {
		done = 1;
		if (start_app[0]) { load_app_edit(start_app); page = P_APP_EDIT; }
		else if (start_new) { new_app_edit(); page = P_APP_EDIT; }
		else if (start_page != P_MAIN) go(u, start_page);
	}
	build(u);
}

/* --check [類別…｜all] [--json] [--admin]：舊的命令列，健康檢查已併入主機醫生，這裡原樣轉給 pomelo-doctor --check（參數相同） */
static int check_cli(int argc, char **argv)
{
	char *av[64];
	int k = 0;
	av[k++] = "pomelo-doctor";
	av[k++] = "--check";
	for (int i = 2; i < argc && k < 62; i++) av[k++] = argv[i];
	av[k] = NULL;
	execvp("pomelo-doctor", av);
	fprintf(stderr, "%s\n", _("健康檢查已併入主機醫生，請先安裝 pomelo-doctor"));
	return 127;
}

/* --sbom [--json|--csv|--text] [--update]：舊的命令列，套件清單與漏洞已搬到主機醫生，原樣轉給 pomelo-doctor --sbom */
static int sbom_cli(int argc, char **argv)
{
	char *av[16];
	int k = 0;
	av[k++] = "pomelo-doctor";
	av[k++] = "--sbom";
	for (int i = 2; i < argc && k < 14; i++) av[k++] = argv[i];
	av[k] = NULL;
	execvp("pomelo-doctor", av);
	fprintf(stderr, "%s\n", _("套件清單已搬到主機醫生，請先安裝 pomelo-doctor"));
	return 127;
}

int main(int argc, char *argv[])
{
	if (argc > 2 && !strcmp(argv[1], "--icon")) return pm_ui_write_icon(icon, argv[2]);
	pm_conf_load();
	if (argc > 1 && !strcmp(argv[1], "--check")) return check_cli(argc, argv);
	if (argc > 1 && !strcmp(argv[1], "--sbom")) return sbom_cli(argc, argv);
	/* --apply：登入時套用鍵盤與滑鼠設定（不開視窗） */
	if (argc > 1 && !strcmp(argv[1], "--apply")) {
		apply_keyboard();
		apply_pointer();
		apply_cameras();
		return 0;
	}
	/* --about：選單列「關於 Pomelo」的小視窗 */
	if (argc > 1 && !strcmp(argv[1], "--about")) { setlocale(LC_ALL, ""); read_root(); return about_window(); }
	/* 啟動器的右鍵選單用：--hide-app／--unhide-app 程式ID（不開視窗）、--delete-app 程式ID（先問過才刪自己加的） */
	if (argc > 2 && (!strcmp(argv[1], "--hide-app") || !strcmp(argv[1], "--unhide-app")))
		return app_set_show(argv[2], !strcmp(argv[1], "--unhide-app")) ? 0 : 1;
	if (argc > 2 && !strcmp(argv[1], "--delete-app")) {
		setlocale(LC_ALL, "");
		load_app_edit(argv[2]);
		if (!ae.custom || !app_has_user_file(argv[2])) return 1;
		char msg[600];
		snprintf(msg, sizeof msg, _("要刪除你自己加的「%s」嗎？\n它會從啟動器、搜尋與 Dock 一起消失；系統裡的其他程式不受影響。"), ae.name);
		if (!ask_yes(_("刪除程式"), msg, _("刪除"))) return 2;
		app_delete_custom(argv[2]);
		apps_changed();
		return 0;
	}
	make_icons();
	for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--new-app")) start_new = 1;
	for (int i = 1; i + 1 < argc; i++)
		if (!strcmp(argv[i], "--page"))
			for (unsigned k = 0; k < LEN(page_names); k++)
				if (!strcmp(argv[i + 1], page_names[k])) start_page = (int)k;
	for (int i = 1; i + 1 < argc; i++) if (!strcmp(argv[i], "--edit-app")) snprintf(start_app, sizeof start_app, "%s", argv[i + 1]);
	/* --page health [--category 類別]：健康檢查（活動監視器、電源等直接開到某一類） */
	if ((start_page == P_VIRUS || start_page == P_SBOM) && pm_have("pomelo-doctor")) {
		/* 舊的 --page virus [--path 資料夾]／--page sbom：轉到主機醫生（檔案管理員右鍵「掃描病毒…」也走這裡） */
		const char *path = NULL;
		for (int i = 1; i + 1 < argc; i++) if (!strcmp(argv[i], "--path") && argv[i + 1][0] == '/') path = argv[i + 1];
		char *av[8] = { "pomelo-doctor", "--page", start_page == P_VIRUS ? "virus" : "sbom", NULL, NULL, NULL };
		if (path && start_page == P_VIRUS) { av[3] = "--path"; av[4] = (char *)path; }
		execvp("pomelo-doctor", av);
	}
	if (start_page == P_HEALTH && pm_have("pomelo-doctor")) {
		/* 舊的 --page health [--category 類別]：轉到主機醫生的體檢頁 */
		const char *cat = NULL;
		for (int i = 1; i + 1 < argc; i++) if (!strcmp(argv[i], "--category")) cat = argv[i + 1];
		char *av[8] = { "pomelo-doctor", "--page", "check", NULL, NULL, NULL };
		if (cat && pm_check_cat_from_id(cat) >= 0) { av[3] = "--category"; av[4] = (char *)cat; }
		execvp("pomelo-doctor", av);
	}
	read_root();
	pm_ui_native = 1;   /* build() 裡沒有直接呼叫 X（--about 的小視窗另外走 X11）：Wayland 工作階段直接當原生程式 */
	return pm_ui_run(_("系統設定"), "pomelo-system", 680, 720, build_first, tick, 66, icon);
}
