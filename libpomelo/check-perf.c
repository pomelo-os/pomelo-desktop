/* SPDX-License-Identifier: GPL-3.0-or-later */
/* check-perf.c — 電腦為什麼變慢：量 1.5 秒，看 CPU、記憶體、硬碟哪個是瓶頸、誰造成的；CPU 有沒有過熱降速、
 * 是不是省電模式、背景是不是正在更新或建索引、最近有沒有因為記憶體不夠而被強制結束的程式、殭屍程序、
 * 系統是不是裝在傳統硬碟。 */
#define _GNU_SOURCE
#include "internal.h"
#include "check-int.h"
#include <ctype.h>
#include <dirent.h>
#include <pwd.h>
#include <time.h>

typedef struct { int pid; unsigned long long t; char comm[64]; uid_t uid; long rss_kb; } Proc;

static int read_procs(Proc *p, int max)
{
	int n = 0;
	DIR *d = opendir(chk_path("/proc"));
	struct dirent *e;
	while (d && (e = readdir(d)) && n < max) {
		if (!isdigit((unsigned char)e->d_name[0])) continue;
		char path[300], buf[1024];
		snprintf(path, sizeof path, "/proc/%s/stat", e->d_name);
		if (!chk_read(path, buf, sizeof buf)) continue;
		/* pid (comm) state ppid … utime(14) stime(15) … rss(24) */
		char *l = strchr(buf, '('), *rp = strrchr(buf, ')');
		if (!l || !rp) continue;
		Proc *q = &p[n];
		q->pid = atoi(e->d_name);
		snprintf(q->comm, sizeof q->comm, "%.*s", (int)(rp - l - 1), l + 1);
		unsigned long long ut = 0, st = 0;
		long rss = 0;
		/* 從 ) 之後第 1 欄是 state：utime 是第 12 欄、stime 第 13、rss 第 22 */
		char *f = rp + 2;
		int col = 0;
		for (char *tok = strtok(f, " "); tok; tok = strtok(NULL, " "), col++) {
			if (col == 11) ut = strtoull(tok, NULL, 10);
			else if (col == 12) st = strtoull(tok, NULL, 10);
			else if (col == 21) { rss = atol(tok); break; }
		}
		q->t = ut + st;
		q->rss_kb = rss * (sysconf(_SC_PAGESIZE) / 1024);
		snprintf(path, sizeof path, "/proc/%s/status", e->d_name);
		q->uid = (uid_t)-1;
		FILE *sf = fopen(chk_path(path), "r");
		char line[256];
		while (sf && fgets(line, sizeof line, sf)) if (!strncmp(line, "Uid:", 4)) { q->uid = (uid_t)atol(line + 4); break; }
		if (sf) fclose(sf);
		n++;
	}
	if (d) closedir(d);
	return n;
}

static unsigned long long cpu_total(unsigned long long *idle)
{
	char buf[512];
	if (!chk_read("/proc/stat", buf, sizeof buf)) return 0;
	unsigned long long v[10] = { 0 };
	sscanf(buf, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
	*idle = v[3] + v[4];
	unsigned long long t = 0;
	for (int i = 0; i < 8; i++) t += v[i];
	return t;
}

static double psi(const char *what)
{
	char p[64], buf[256];
	snprintf(p, sizeof p, "/proc/pressure/%s", what);
	if (!chk_read(p, buf, sizeof buf)) return -1;
	char *a = strstr(buf, "avg10=");
	return a ? atof(a + 6) : -1;
}

static long meminfo(const char *buf, const char *key)
{
	const char *p = strstr(buf, key);
	return p ? atol(p + strlen(key)) : 0;
}

/* 背景常見的大工作：認得的話直接說「做完就好」 */
static const struct { const char *comm; const char *what; } background[] = {
	{ "apt", N_("正在安裝或更新軟體") }, { "apt-get", N_("正在安裝或更新軟體") }, { "dpkg", N_("正在安裝或更新軟體") },
	{ "unattended-upgr", N_("正在自動安裝安全性更新") }, { "packagekitd", N_("正在檢查或安裝更新") }, { "dnf", N_("正在安裝或更新軟體") },
	{ "dnf5", N_("正在安裝或更新軟體") }, { "dnf5daemon-serv", N_("正在檢查更新") }, { "snapd", N_("Snap 正在更新") },
	{ "updatedb", N_("正在建立檔案索引") }, { "updatedb.plocat", N_("正在建立檔案索引") }, { "tracker-miner-f", N_("正在建立檔案索引") },
	{ "baloo_file", N_("正在建立檔案索引") }, { "fwupd", N_("正在檢查韌體更新") }, { "llama-server", N_("小柚的 AI 模型正在思考") },
	{ "clamscan", N_("正在掃描病毒") }, { "ffmpeg", N_("正在轉檔或錄影") }, { "man-db", N_("正在更新說明文件索引") }, { "mandb", N_("正在更新說明文件索引") },
};

void chk_performance(PmReport *r, int flags)
{
	(void)flags;
	PmCheck *c;
	static Proc a[4096], b[4096];
	unsigned long long i0, i1, t0 = cpu_total(&i0);
	int na = read_procs(a, 4096);
	char thr0[64] = "";
	chk_read("/sys/devices/system/cpu/cpu0/thermal_throttle/core_throttle_count", thr0, sizeof thr0);
	struct timespec ts = { 1, 500000000 };
	nanosleep(&ts, NULL);
	unsigned long long t1 = cpu_total(&i1);
	int nb = read_procs(b, 4096);
	char thr1[64] = "";
	chk_read("/sys/devices/system/cpu/cpu0/thermal_throttle/core_throttle_count", thr1, sizeof thr1);
	long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
	if (ncpu < 1) ncpu = 1;
	double busy = t1 > t0 ? 100.0 * (double)((t1 - t0) - (i1 - i0)) / (double)(t1 - t0) : 0;
	double hz = (double)sysconf(_SC_CLK_TCK), secs = 1.5;

	/* 每個程式用了多少 CPU（一顆核心＝100%） */
	typedef struct { int idx; double pct; } Top;
	Top top[3] = { { -1, 0 }, { -1, 0 }, { -1, 0 } };
	char bg[512] = "";
	for (int j = 0; j < nb; j++) {
		unsigned long long prev = 0;
		int found = 0;
		for (int k = 0; k < na; k++) if (a[k].pid == b[j].pid) { prev = a[k].t; found = 1; break; }
		if (!found) continue;
		double pct = 100.0 * (double)(b[j].t - prev) / hz / secs;
		for (int k = 0; k < 3; k++)
			if (pct > top[k].pct) { memmove(&top[k + 1], &top[k], (size_t)(2 - k) * sizeof top[0]); top[k] = (Top){ j, pct }; break; }
		if (pct > 10)
			for (size_t k = 0; k < LEN(background); k++)
				if (!strcmp(b[j].comm, background[k].comm) && !strstr(bg, _(background[k].what)) && strlen(bg) < sizeof bg - 100)
					snprintf(bg + strlen(bg), sizeof bg - strlen(bg), "%s%s（%s）", bg[0] ? "、" : "", _(background[k].what), b[j].comm);
	}
	char topl[512] = "";
	for (int k = 0; k < 3; k++)
		if (top[k].idx >= 0 && top[k].pct >= 5)
			snprintf(topl + strlen(topl), sizeof topl - strlen(topl), "%s%s %.0f%%", topl[0] ? "、" : "", b[top[k].idx].comm, top[k].pct);

	/* 1. CPU */
	if (busy > 85 || (top[0].idx >= 0 && top[0].pct > 90)) {
		c = chk_add(r, "perf.cpu", PM_CHK_WARN, _("CPU 很忙（整體 %.0f%%）"), busy);
		if (topl[0]) chk_detail(c, _("用最多的程式：%s（一顆核心＝100%%，這台有 %ld 顆）"), topl, ncpu);
		if (bg[0]) chk_advice(c, _("背景%s，做完就會恢復。"), bg);
		else chk_advice(c, "%s", _("不認得或不需要的程式可以關掉；網頁瀏覽器開太多分頁也很吃 CPU。"));
		if (top[0].idx >= 0 && top[0].pct > 50 && b[top[0].idx].uid == getuid()) {
			const Proc *p = &b[top[0].idx];
			char arg[32];
			snprintf(arg, sizeof arg, "%d", p->pid);
			chk_fix(c, "perf.kill", arg, _("結束這個程式"));
			snprintf(c->fix_arg + strlen(c->fix_arg), sizeof c->fix_arg - strlen(c->fix_arg), " %s", p->comm);
		}
	} else {
		c = chk_add(r, "perf.cpu", PM_CHK_OK, _("CPU 不忙（%.0f%%）"), busy);
		if (topl[0]) chk_detail(c, _("用最多的程式：%s"), topl);
	}
	if (bg[0] && busy <= 85) {
		c = chk_add(r, "perf.background", PM_CHK_INFO, _("背景%s"), bg);
		chk_detail(c, "%s", _("這段時間電腦會比較慢，做完就會恢復。"));
	}

	/* 2. 記憶體 */
	{
		char mi[4096];
		if (chk_read("/proc/meminfo", mi, sizeof mi)) {
			long total = meminfo(mi, "MemTotal:"), avail = meminfo(mi, "MemAvailable:"), swt = meminfo(mi, "SwapTotal:"), swf = meminfo(mi, "SwapFree:");
			double pa = total ? 100.0 * avail / total : 100, swu = swt ? 100.0 * (swt - swf) / swt : 0;
			double pm = psi("memory");
			/* 用最多記憶體的三個程式 */
			char ml[512] = "";
			int used[3] = { -1, -1, -1 };
			for (int k = 0; k < 3; k++) {
				int best = -1;
				for (int j = 0; j < nb; j++) {
					if (j == used[0] || j == used[1]) continue;
					if (best < 0 || b[j].rss_kb > b[best].rss_kb) best = j;
				}
				if (best < 0) break;
				used[k] = best;
				char sz[32];
				pm_human_size(sz, sizeof sz, (double)b[best].rss_kb * 1024);
				snprintf(ml + strlen(ml), sizeof ml - strlen(ml), "%s%s %s", ml[0] ? "、" : "", b[best].comm, sz);
			}
			char tot[32], av[32];
			pm_human_size(tot, sizeof tot, (double)total * 1024);
			pm_human_size(av, sizeof av, (double)avail * 1024);
			if (pa < 10 || pm > 20) {
				c = chk_add(r, "perf.mem", PM_CHK_BAD, _("記憶體快用完了（剩 %s／%s）"), av, tot);
				chk_detail(c, _("用最多的程式：%s"), ml);
				chk_advice(c, "%s", _("關掉不用的程式與瀏覽器分頁；常常這樣的話，加記憶體會有很明顯的改善。"));
			} else if (pa < 20 || (swu > 50 && pm > 5)) {
				c = chk_add(r, "perf.mem", PM_CHK_WARN, _("記憶體有點緊（剩 %s／%s）"), av, tot);
				chk_detail(c, _("用最多的程式：%s"), ml);
				if (swu > 50) chk_detail(c, _("已經在用硬碟當記憶體（Swap 用了 %.0f%%），所以會變慢。"), swu);
				chk_advice(c, "%s", _("關掉不用的程式與瀏覽器分頁。"));
			} else {
				c = chk_add(r, "perf.mem", PM_CHK_OK, _("記憶體足夠（剩 %s／%s）"), av, tot);
				chk_detail(c, _("用最多的程式：%s"), ml);
			}
		}
	}

	/* 3. 硬碟 I/O */
	{
		double pio = psi("io");
		if (pio > 20) {
			c = chk_add(r, "perf.io", PM_CHK_WARN, "%s", _("硬碟忙不過來"));
			chk_detail(c, _("過去 10 秒有 %.0f%% 的時間，程式在等硬碟。"), pio);
			if (bg[0]) chk_advice(c, _("背景%s，做完就會恢復。"), bg);
			else chk_advice(c, "%s", _("可能是正在複製大檔案、更新，或記憶體不夠而一直用到 Swap。"));
		} else if (pio >= 0) chk_add(r, "perf.io", PM_CHK_OK, "%s", _("硬碟不忙"));
		/* 系統裝在傳統硬碟上 */
		char *o = chk_run("findmnt -n -o SOURCE /");
		if (o) {
			o[strcspn(o, "\n")] = 0;
			char *dev = strrchr(o, '/');
			char p[256], rot[8] = "";
			/* /dev/sda2 → sda；/dev/mapper/… 不好判斷，就不說 */
			if (dev && !strstr(o, "mapper")) {
				char base[64];
				snprintf(base, sizeof base, "%s", dev + 1);
				for (int i = (int)strlen(base) - 1; i > 0 && isdigit((unsigned char)base[i]); i--) base[i] = 0;
				if (!strncmp(base, "nvme", 4) && strchr(base, 'p')) *strrchr(base, 'p') = 0;
				snprintf(p, sizeof p, "/sys/block/%s/queue/rotational", base);
				if (chk_read(p, rot, sizeof rot) && rot[0] == '1') {
					c = chk_add(r, "perf.hdd", PM_CHK_INFO, "%s", _("系統裝在傳統硬碟上"));
					chk_detail(c, "%s", _("開機、開程式、更新都會比較慢；換成 SSD 是讓電腦變快最有效的方法。"));
				}
			}
			free(o);
		}
	}

	/* 4. 過熱、降速 */
	{
		double hot = -1;
		DIR *d = opendir(chk_path("/sys/class/hwmon"));
		struct dirent *e;
		while (d && (e = readdir(d))) {
			if (e->d_name[0] == '.') continue;
			char p[300], name[64];
			snprintf(p, sizeof p, "/sys/class/hwmon/%s/name", e->d_name);
			if (!chk_read(p, name, sizeof name)) continue;
			if (strcmp(name, "coretemp") && strcmp(name, "k10temp") && strcmp(name, "zenpower") && strcmp(name, "cpu_thermal")) continue;
			for (int i = 1; i < 64; i++) {
				snprintf(p, sizeof p, "/sys/class/hwmon/%s/temp%d_input", e->d_name, i);
				long t = chk_read_long(p, -1);
				if (t > 0 && t / 1000.0 > hot) hot = t / 1000.0;
			}
		}
		if (d) closedir(d);
		int throttled = thr0[0] && thr1[0] && atol(thr1) > atol(thr0);
		if (hot >= 90 || throttled) {
			c = chk_add(r, "perf.thermal", PM_CHK_WARN, "%s", throttled ? _("CPU 過熱，正在降速") : _("CPU 很燙"));
			if (hot > 0) chk_detail(c, _("目前 %.0f°C。"), hot);
			chk_advice(c, "%s", _("清一下風扇與通風口的灰塵；筆電放在硬的桌面上，不要放在棉被或腿上。"));
		} else if (hot > 0) chk_add(r, "perf.thermal", PM_CHK_OK, _("CPU 溫度正常（%.0f°C）"), hot);
	}

	/* 5. 省電模式 */
	if (pm_have("powerprofilesctl")) {
		char *o = chk_run("powerprofilesctl get");
		if (o && !strncmp(o, "power-saver", 11)) {
			c = chk_add(r, "perf.powersave", PM_CHK_INFO, "%s", _("目前是省電模式"));
			chk_detail(c, "%s", _("省電模式會讓 CPU 跑慢一點，換取比較長的電池時間。"));
			chk_fix(c, "perf.balanced", NULL, _("改成平衡模式"));
		}
		free(o);
	}

	/* 6. 最近 7 天記憶體不夠、被系統強制結束的程式 */
	{
		char *o = chk_kernel_log(7);
		if (o) {
			int n = 0;
			char list[512] = "";
			for (char *l = o, *nx; *l; l = nx) {
				nx = chk_line(l);
				char *k = strstr(l, "Killed process ");
				if (!k) continue;
				n++;
				char *lp = strchr(k, '('), *rp2 = lp ? strchr(lp, ')') : NULL;
				if (lp && rp2 && strlen(list) < sizeof list - 80) {
					char name[64];
					snprintf(name, sizeof name, "%.*s", (int)(rp2 - lp - 1), lp + 1);
					if (!strstr(list, name)) snprintf(list + strlen(list), sizeof list - strlen(list), "%s%s", list[0] ? "、" : "", name);
				}
			}
			free(o);
			if (n) {
				c = chk_add(r, "perf.oom", PM_CHK_WARN, _("最近 7 天有 %d 次記憶體不夠，系統強制結束了程式"), n);
				chk_detail(c, _("被結束的程式：%s"), list);
				chk_advice(c, "%s", _("同時開的程式太多、或某個程式用掉太多記憶體；可以加記憶體或開啟 zram。"));
			}
		}
	}

	/* 7. 殭屍程序 */
	{
		int nz = 0;
		char list[256] = "";
		DIR *d = opendir(chk_path("/proc"));
		struct dirent *e;
		while (d && (e = readdir(d))) {
			if (!isdigit((unsigned char)e->d_name[0])) continue;
			char p[300], buf[512];
			snprintf(p, sizeof p, "/proc/%s/stat", e->d_name);
			if (!chk_read(p, buf, sizeof buf)) continue;
			char *rp2 = strrchr(buf, ')');
			if (!rp2 || rp2[2] != 'Z') continue;
			nz++;
			char *lp = strchr(buf, '(');
			if (lp && strlen(list) < sizeof list - 40) snprintf(list + strlen(list), sizeof list - strlen(list), "%s%.*s", list[0] ? "、" : "", (int)(rp2 - lp - 1), lp + 1);
		}
		if (d) closedir(d);
		if (nz >= 5) {
			c = chk_add(r, "perf.zombie", PM_CHK_INFO, _("有 %d 個已經結束、但還沒被收走的程序（殭屍程序）"), nz);
			chk_detail(c, "%s", list);
			chk_detail(c, "%s", _("它們幾乎不佔資源；數量一直增加的話，是產生它們的程式有問題，重新啟動那個程式就會清掉。"));
		}
	}
}

int chk_perf_fix(const PmCheck *c, char **argv, int max, int *admin, char *msg, size_t n)
{
	(void)max; (void)admin;
	static char pid[32];
	int k = 0;
	if (!strcmp(c->fix, "perf.kill")) {
		const char *name = strchr(c->fix_arg, ' ');
		snprintf(pid, sizeof pid, "%.*s", name ? (int)(name - c->fix_arg) : (int)strlen(c->fix_arg), c->fix_arg);
		snprintf(msg, n, _("結束「%s」（PID %s）。還沒儲存的內容會不見。"), name ? name + 1 : "", pid);
		argv[k++] = "kill"; argv[k++] = "-TERM"; argv[k++] = pid;
	} else if (!strcmp(c->fix, "perf.balanced")) {
		snprintf(msg, n, "%s", _("改成平衡模式。"));
		argv[k++] = "powerprofilesctl"; argv[k++] = "set"; argv[k++] = "balanced";
	}
	argv[k] = NULL;
	return k;
}
