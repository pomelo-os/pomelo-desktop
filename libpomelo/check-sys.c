/* SPDX-License-Identifier: GPL-3.0-or-later */
/* check-sys.c — 開機與系統穩定：啟動失敗的服務、一直重新啟動的服務、最近當掉的程式、磁碟與檔案系統錯誤、
 * 開機花多久與哪個步驟最慢、日誌是不是大到拖慢開機、磁碟快滿了、時間有沒有自動校正、多久沒重新開機。
 * 讀日誌（journalctl）需要使用者在 adm 或 systemd-journal 群組；讀不到的項目會說明。
 * 另外放兩個各類共用的小工具：chk_kernel_log、chk_listening。 */
#define _GNU_SOURCE
#include "internal.h"
#include "check-int.h"
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>

/* 最近幾天的 kernel 訊息（0＝本次開機），只要警告以上；讀不到回傳 NULL */
char *chk_kernel_log(int days)
{
	char cmd[160];
	if (days > 0) snprintf(cmd, sizeof cmd, "journalctl -k -q --no-pager -o cat -p warning --since -%dd", days);
	else snprintf(cmd, sizeof cmd, "journalctl -k -b -q --no-pager -o cat -p warning");
	char *o = chk_run(cmd);
	if (o && !*o) {   /* 沒權限時 journalctl 什麼都不印：分不出來，再試 dmesg（可能也被限制） */
		free(o);
		o = days > 0 ? NULL : chk_run("dmesg --level=emerg,alert,crit,err,warn");
	}
	return o;
}

static int listen_in(const char *path, int port)
{
	FILE *f = fopen(chk_path(path), "r");
	if (!f) return 0;
	char line[512], local[64], st[8];
	int found = 0;
	while (!found && fgets(line, sizeof line, f)) {
		if (sscanf(line, "%*s %63s %*s %7s", local, st) != 2 || strcmp(st, "0A")) continue;
		char *c = strrchr(local, ':');
		if (c && (int)strtol(c + 1, NULL, 16) == port) found = 1;
	}
	fclose(f);
	return found;
}

int chk_listening(int port) { return listen_in("/proc/net/tcp", port) || listen_in("/proc/net/tcp6", port); }

static int count_lines(const char *o, const char *pat)
{
	int n = 0;
	for (const char *p = o; p && (p = strstr(p, pat)); p += strlen(pat)) n++;
	return n;
}

static void fmt_size(char *out, size_t n, double b) { pm_human_size(out, n, b); }

/* 資料夾大小（看得到多少算多少） */
static double dir_size(const char *path, int depth)
{
	double total = 0;
	DIR *d = opendir(chk_path(path));
	struct dirent *e;
	while (d && (e = readdir(d))) {
		if (e->d_name[0] == '.' && (!e->d_name[1] || (e->d_name[1] == '.' && !e->d_name[2]))) continue;
		char p[1024];
		snprintf(p, sizeof p, "%s/%s", path, e->d_name);
		struct stat st;
		if (lstat(chk_path(p), &st)) continue;
		if (S_ISDIR(st.st_mode) && depth < 4) total += dir_size(p, depth + 1);
		else if (S_ISREG(st.st_mode)) total += (double)st.st_blocks * 512;
	}
	if (d) closedir(d);
	return total;
}

void chk_stability(PmReport *r, int flags)
{
	(void)flags;
	PmCheck *c;

	/* 1. 啟動失敗的服務（系統的、自己的） */
	{
		char list[1024] = "", logs[1024] = "";
		int n = 0;
		for (int user = 0; user < 2; user++) {
			char *o = chk_run(user ? "systemctl --user --failed --no-legend --plain" : "systemctl --failed --no-legend --plain");
			for (char *l = o, *nx; l && *l; l = nx) {
				nx = chk_line(l);
				char unit[256];
				if (sscanf(l, " %255s", unit) != 1 || unit[0] == '0') continue;
				n++;
				if (strlen(list) < sizeof list - 300) snprintf(list + strlen(list), sizeof list - strlen(list), "%s%s", list[0] ? "、" : "", unit);
				/* 最後一行記錄，常常就說明了原因 */
				if (n <= 3) {
					char cmd[400];
					snprintf(cmd, sizeof cmd, "journalctl %s-u %s -n 1 -q --no-pager -o cat", user ? "--user " : "", unit);
					char *lg = chk_run(cmd);
					if (lg && *lg && strlen(logs) < sizeof logs - 300) {
						lg[strcspn(lg, "\n")] = 0;
						/* 記錄常常自己就以「服務名稱: 」開頭，不要重複 */
						const char *m = lg;
						if (!strncmp(m, unit, strlen(unit)) && m[strlen(unit)] == ':') m += strlen(unit) + 1;
						while (*m == ' ') m++;
						snprintf(logs + strlen(logs), sizeof logs - strlen(logs), "%s%s：%.200s", logs[0] ? "\n" : "", unit, m);
					}
					free(lg);
				}
			}
			free(o);
		}
		if (n) {
			c = chk_add(r, "sys.failed", PM_CHK_WARN, _("有 %d 個服務啟動失敗"), n);
			chk_detail(c, "%s", list);
			if (logs[0]) chk_detail(c, "%s", logs);
			chk_advice(c, "%s", _("不認得的服務可以先不用管；跟你正在用的功能有關的，用 systemctl status 服務名稱 看詳細原因。"));
		} else chk_add(r, "sys.failed", PM_CHK_OK, "%s", _("所有服務都正常啟動"));
	}

	/* 2. 一直重新啟動的服務（NRestarts） */
	{
		char *o = chk_run("systemctl list-units --type=service --state=running --no-legend --plain");
		char units[8192] = "";
		for (char *l = o, *nx; l && *l; l = nx) {
			nx = chk_line(l);
			char u[256];
			if (sscanf(l, " %255s", u) == 1 && strlen(units) + strlen(u) + 2 < sizeof units) { strcat(units, " "); strcat(units, u); }
		}
		free(o);
		char list[512] = "";
		int n = 0;
		if (units[0]) {
			char cmd[8400];
			snprintf(cmd, sizeof cmd, "systemctl show -p Id -p NRestarts%s", units);
			o = chk_run(cmd);
			char id[256] = "";
			for (char *l = o, *nx; l && *l; l = nx) {
				nx = chk_line(l);
				if (!strncmp(l, "Id=", 3)) snprintf(id, sizeof id, "%s", l + 3);
				else if (!strncmp(l, "NRestarts=", 10) && atoi(l + 10) >= 3) {
					n++;
					if (strlen(list) < sizeof list - 300) snprintf(list + strlen(list), sizeof list - strlen(list), "%s%s（%d 次）", list[0] ? "、" : "", id, atoi(l + 10));
				}
			}
			free(o);
		}
		if (n) {
			c = chk_add(r, "sys.restarts", PM_CHK_WARN, _("有 %d 個服務一直當掉又被重新啟動"), n);
			chk_detail(c, "%s", list);
			chk_advice(c, "%s", _("用 journalctl -u 服務名稱 看它為什麼一直結束。"));
		}
	}

	/* 3. 最近 30 天當掉的程式（Ubuntu 的 /var/crash、systemd-coredump） */
	{
		char list[1024] = "";
		int n = 0;
		DIR *d = opendir(chk_path("/var/crash"));
		struct dirent *e;
		time_t now = time(NULL);
		while (d && (e = readdir(d))) {
			char *dot = strstr(e->d_name, ".crash");
			if (!dot) continue;
			char p[512];
			snprintf(p, sizeof p, "/var/crash/%s", e->d_name);
			struct stat st;
			if (stat(chk_path(p), &st) || now - st.st_mtime > 30 * 86400) continue;
			/* _usr_bin_pomelo-panel.1000.crash → pomelo-panel */
			char name[256];
			snprintf(name, sizeof name, "%.*s", (int)(dot - e->d_name), e->d_name);
			char *u = strrchr(name, '_'), *pt = strchr(u ? u : name, '.');
			if (pt) *pt = 0;
			n++;
			if (strlen(list) < sizeof list - 120) snprintf(list + strlen(list), sizeof list - strlen(list), "%s%s", list[0] ? "、" : "", u ? u + 1 : name);
		}
		if (d) closedir(d);
		if (pm_have("coredumpctl")) {
			char *o = chk_run("coredumpctl list --since=-30d --no-legend -q --no-pager");
			for (char *l = o, *nx; l && *l; l = nx) {
				nx = chk_line(l);
				char *exe = strrchr(l, '/');   /* 最後一欄是程式路徑 */
				if (!exe) continue;
				exe++;
				exe[strcspn(exe, " \t")] = 0;
				n++;
				if (!strstr(list, exe) && strlen(list) < sizeof list - 120) snprintf(list + strlen(list), sizeof list - strlen(list), "%s%s", list[0] ? "、" : "", exe);
			}
			free(o);
		}
		if (n) {
			c = chk_add(r, "sys.crash", n >= 5 ? PM_CHK_WARN : PM_CHK_INFO, _("最近 30 天有 %d 次程式當掉"), n);
			chk_detail(c, "%s", list);
			chk_advice(c, "%s", _("同一個程式一直當掉的話，請回報給我們（附上是哪個程式、做了什麼）。"));
		} else chk_add(r, "sys.crash", PM_CHK_OK, "%s", _("最近 30 天沒有程式當掉"));
	}

	/* 4. 磁碟與檔案系統錯誤（本次開機的 kernel 訊息） */
	{
		char *o = chk_kernel_log(0);
		if (!o) chk_add(r, "sys.kernel", PM_CHK_SKIP, "%s", _("讀不到系統訊息，略過磁碟錯誤的檢查（帳號要在 adm 群組）"));
		else {
			int io = count_lines(o, "I/O error") + count_lines(o, "Buffer I/O") + count_lines(o, "critical medium error") +
			         count_lines(o, "UNC") + count_lines(o, "failed command: READ") + count_lines(o, "failed command: WRITE");
			int fs = count_lines(o, "EXT4-fs error") + count_lines(o, "BTRFS error") + count_lines(o, "Corruption of in-memory data") +
			         count_lines(o, "Remounting filesystem read-only");
			int mce = count_lines(o, "Hardware Error") + count_lines(o, "Machine check");
			if (io || fs) {
				c = chk_add(r, "sys.disk_err", PM_CHK_BAD, "%s", io ? _("硬碟有讀寫錯誤") : _("檔案系統有錯誤"));
				chk_detail(c, _("本次開機以來出現 %d 次。"), io + fs);
				chk_advice(c, "%s", _("請盡快備份重要的檔案，並到「儲存空間」看硬碟的健康狀態；硬碟可能快壞了。"));
				chk_fix(c, "sys.storage", NULL, _("開啟儲存空間"));
			} else chk_add(r, "sys.disk_err", PM_CHK_OK, "%s", _("沒有硬碟讀寫或檔案系統錯誤"));
			if (mce) {
				c = chk_add(r, "sys.mce", PM_CHK_BAD, "%s", _("CPU 或記憶體回報了硬體錯誤"));
				chk_detail(c, _("本次開機以來出現 %d 次。可能是記憶體或 CPU 有問題、過熱、或電壓不穩。"), mce);
				chk_advice(c, "%s", _("如果常常當機或畫面花掉，請送修，或用 memtest 測試記憶體。"));
			}
			free(o);
		}
	}

	/* 5. 開機時間 */
	{
		char *o = chk_run("systemd-analyze");
		double total = -1;
		char *p = o ? strstr(o, "= ") : NULL;
		if (p) {
			p += 2;
			double m = 0, sec = 0;
			if (strstr(p, "min ")) { sscanf(p, "%lfmin %lfs", &m, &sec); total = m * 60 + sec; }
			else sscanf(p, "%lfs", &total);
		}
		free(o);
		if (total > 0) {
			char slow[512] = "";
			o = chk_run("systemd-analyze blame --no-pager");
			int k = 0;
			/* blame 也會列出開機之後才由計時器啟動的工作（apt-daily、updatedb…），它們不影響開機，不算 */
			static const char *const later[] = { "apt-daily", "plocate-updatedb", "mlocate", "updatedb", "man-db", "certbot", "logrotate", "fstrim",
				"e2scrub", "motd-news", "dpkg-db-backup", "sysstat", "fwupd-refresh", "snapd.refresh", "unattended-upgrades", "dnf-makecache",
				"packagekit", "systemd-tmpfiles-clean", "anacron", "raid-check", "mdcheck", "phpsessionclean", "ua-timer", "esm-cache" };
			for (char *l = o, *nx; l && *l && k < 3; l = nx) {
				nx = chk_line(l);
				while (*l == ' ') l++;
				if (!*l) continue;
				int skip = 0;
				for (size_t i = 0; i < LEN(later); i++) if (strstr(l, later[i])) skip = 1;
				if (skip) continue;
				snprintf(slow + strlen(slow), sizeof slow - strlen(slow), "%s%s", k ? "、" : "", l);
				k++;
			}
			free(o);
			if (total > 60) {
				c = chk_add(r, "sys.boot", PM_CHK_WARN, _("上次開機花了 %.0f 秒"), total);
				chk_detail(c, _("最慢的步驟：%s"), slow);
				if (strstr(slow, "journal-flush")) chk_advice(c, "%s", _("systemd-journal-flush 慢，是因為系統日誌太大（見日誌那一項）；清掉舊日誌開機就會快很多。"));
				else if (strstr(slow, "wait-online")) chk_advice(c, "%s", _("開機在等網路連上；桌上型電腦通常可以關掉 NetworkManager-wait-online。"));
				else chk_advice(c, "%s", _("最慢的那幾個通常就是原因；跟網路有關的（例如等網路連上）可以設定成不等。"));
			} else if (total > 30) {
				c = chk_add(r, "sys.boot", PM_CHK_INFO, _("上次開機花了 %.0f 秒"), total);
				chk_detail(c, _("最慢的步驟：%s"), slow);
			} else chk_add(r, "sys.boot", PM_CHK_OK, _("開機很快（%.0f 秒）"), total);
		}
	}

	/* 6. 日誌太大（傳統硬碟上會拖慢開機） */
	{
		double journal = dir_size("/var/log/journal", 0), all = dir_size("/var/log", 0);
		char a[32], b[32];
		fmt_size(a, sizeof a, journal);
		fmt_size(b, sizeof b, all);
		if (journal > 2e9) {
			c = chk_add(r, "sys.journal", PM_CHK_WARN, _("系統日誌佔了 %s"), a);
			chk_detail(c, "%s", _("日誌太大會讓開機變慢（特別是傳統硬碟），也佔空間。"));
			chk_fix(c, "sys.vacuum", NULL, _("只保留最近的 500 MB"));
		} else if (all > 5e9) {
			c = chk_add(r, "sys.logs", PM_CHK_WARN, _("/var/log 佔了 %s"), b);
			chk_advice(c, "%s", _("通常是某個程式一直寫錯誤訊息；在「儲存空間」的分析可以看是哪個檔案。"));
			chk_fix(c, "sys.analyze", NULL, _("開啟儲存空間分析"));
		} else chk_add(r, "sys.journal", PM_CHK_OK, _("系統日誌大小正常（%s）"), b);
	}

	/* 7. 磁碟快滿了（硬碟健康在「硬體與驅動」，清理在「儲存空間」） */
	{
		const char *mp[] = { "/", "/home" };
		for (size_t i = 0; i < LEN(mp); i++) {
			struct statvfs v;
			if (statvfs(chk_path(mp[i]), &v) || !v.f_blocks) continue;
			if (i) {   /* /home 跟 / 在同一個分割區就不用再說一次 */
				struct stat a, b;
				if (!stat(chk_path("/"), &a) && !stat(chk_path("/home"), &b) && a.st_dev == b.st_dev) continue;
			}
			double fr = (double)v.f_bavail * v.f_frsize, tot = (double)v.f_blocks * v.f_frsize;
			char a[32];
			fmt_size(a, sizeof a, fr);
			char id[32];
			snprintf(id, sizeof id, "sys.space%zu", i);
			if (fr < 1e9 || fr / tot < 0.03) {
				c = chk_add(r, id, PM_CHK_BAD, _("%s 只剩 %s"), mp[i], a);
				chk_detail(c, "%s", _("磁碟滿了會讓程式當掉、更新失敗、甚至無法登入。"));
				chk_fix(c, "sys.storage", NULL, _("開啟儲存空間清理"));
			} else if (fr < 5e9 || fr / tot < 0.10) {
				c = chk_add(r, id, PM_CHK_WARN, _("%s 只剩 %s"), mp[i], a);
				chk_fix(c, "sys.storage", NULL, _("開啟儲存空間清理"));
			} else chk_add(r, id, PM_CHK_OK, _("%s 還有 %s"), mp[i], a);
		}
	}

	/* 8. 時間自動校正 */
	{
		char *o = chk_run("timedatectl show -p NTPSynchronized -p NTP --value");
		if (o && *o) {
			char sync[8] = "", ntp[8] = "";
			sscanf(o, "%7s %7s", sync, ntp);
			if (!strcmp(sync, "yes")) chk_add(r, "sys.time", PM_CHK_OK, "%s", _("時間有自動校正"));
			else {
				c = chk_add(r, "sys.time", PM_CHK_WARN, "%s", !strcmp(ntp, "no") ? _("時間沒有設定成自動校正") : _("時間還沒有校正成功"));
				chk_detail(c, "%s", _("時間不準會讓網站的安全連線失敗（憑證錯誤）、檔案時間錯亂。"));
				if (!strcmp(ntp, "no")) chk_fix(c, "sys.ntp", NULL, _("開啟自動校正"));
			}
		}
		free(o);
	}

	/* 9. 多久沒重新開機 */
	{
		char up[64];
		if (chk_read("/proc/uptime", up, sizeof up)) {
			long days = (long)(atof(up) / 86400);
			if (days >= 30) {
				c = chk_add(r, "sys.uptime", PM_CHK_INFO, _("已經 %ld 天沒有重新開機"), days);
				chk_detail(c, "%s", _("有些更新要重新開機才會生效；長時間不關機也可能讓記憶體越來越少。"));
			} else chk_add(r, "sys.uptime", PM_CHK_OK, _("%ld 天前開機"), days);
		}
	}
}

int chk_sys_fix(const PmCheck *c, char **argv, int max, int *admin, char *msg, size_t n)
{
	(void)max;
	int k = 0;
	if (!strcmp(c->fix, "sys.storage")) { argv[k++] = "@open"; argv[k++] = "pomelo-storage --cleanup"; }
	else if (!strcmp(c->fix, "sys.analyze")) { argv[k++] = "@open"; argv[k++] = "pomelo-storage --analyze /var/log"; }
	else if (!strcmp(c->fix, "sys.vacuum")) {
		*admin = 1;
		snprintf(msg, n, "%s", _("刪除舊的系統日誌，只保留最近的 500 MB。"));
		argv[k++] = "journalctl"; argv[k++] = "--vacuum-size=500M";
	} else if (!strcmp(c->fix, "sys.ntp")) {
		snprintf(msg, n, "%s", _("讓電腦自動從網路校正時間。"));
		argv[k++] = "timedatectl"; argv[k++] = "set-ntp"; argv[k++] = "true";
	}
	argv[k] = NULL;
	return k;
}
