/* SPDX-License-Identifier: GPL-3.0-or-later */
/* check-upd.c — 系統與軟體更新：系統版本還有沒有支援、有幾個更新（安全性更新）、套件清單多久沒更新、
 * 自動安全性更新、裝到一半的套件、要不要重新開機才能完成更新、韌體更新、空間夠不夠裝更新。
 * 不連網：只看本機已經下載的套件清單與韌體資料（fwupd 的快取）。 */
#define _GNU_SOURCE
#include "internal.h"
#include "check-int.h"
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <time.h>

static time_t ymd(const char *s)
{
	struct tm t = { 0 };
	int y, m, d = 1;
	if (sscanf(s, "%d-%d-%d", &y, &m, &d) < 2) return 0;
	t.tm_year = y - 1900; t.tm_mon = m - 1; t.tm_mday = d; t.tm_hour = 12;
	return mktime(&t);
}

/* 系統版本的支援期限：Debian、Ubuntu 讀 distro-info 的資料；Fedora、Rocky 用下面的表（Fedora 是大約的日期） */
static int os_eol(const char *id, const char *ver, char *date, size_t n)
{
	date[0] = 0;
	if (!strcmp(id, "ubuntu") || !strcmp(id, "debian")) {
		char p[128];
		snprintf(p, sizeof p, "/usr/share/distro-info/%s.csv", id);
		FILE *f = fopen(chk_path(p), "r");
		char line[256];
		while (f && fgets(line, sizeof line, f)) {
			char *fld[9] = { 0 };
			int k = 0;
			for (char *s = line, *t; k < 9 && (t = strsep(&s, ",\n")); ) fld[k++] = t;
			if (k < 6 || !fld[0]) continue;
			/* Ubuntu 的版本寫「24.04 LTS」，Debian 寫「13」 */
			if (strncmp(fld[0], ver, strlen(ver)) || (fld[0][strlen(ver)] && fld[0][strlen(ver)] != ' ')) continue;
			snprintf(date, n, "%s", fld[5]);
		}
		if (f) fclose(f);
		if (date[0]) return 1;
		/* 沒裝 distro-info-data（例如精簡安裝）：用下面的表 */
	}
	static const struct { const char *id, *ver, *eol; } t[] = {
		{ "ubuntu", "22.04", "2027-06-01" }, { "ubuntu", "24.04", "2029-05-31" }, { "ubuntu", "26.04", "2031-05-29" },
		{ "debian", "12", "2026-07-11" }, { "debian", "13", "2028-08-09" },
		{ "fedora", "42", "2026-05" }, { "fedora", "43", "2026-12" }, { "fedora", "44", "2027-05" }, { "fedora", "45", "2027-12" },
		{ "rocky", "8", "2029-05-31" }, { "rocky", "9", "2032-05-31" }, { "rocky", "10", "2035-05-31" },
		{ "rhel", "9", "2032-05-31" }, { "almalinux", "9", "2032-05-31" },
	};
	for (size_t i = 0; i < LEN(t); i++)
		if (!strcmp(id, t[i].id) && (!strcmp(ver, t[i].ver) || (!strncmp(ver, t[i].ver, strlen(t[i].ver)) && ver[strlen(t[i].ver)] == '.'))) {
			snprintf(date, n, "%s", t[i].eol);
			return 1;
		}
	return 0;
}

static void os_field(const char *buf, const char *key, char *out, size_t n)
{
	out[0] = 0;
	char k[64];
	snprintf(k, sizeof k, "\n%s=", key);
	const char *p = strstr(buf, k);
	if (!p && !strncmp(buf, k + 1, strlen(k + 1))) p = buf - 1;
	if (!p) return;
	p += strlen(k);
	if (*p == '"') p++;
	size_t l = strcspn(p, "\"\n");
	snprintf(out, n, "%.*s", (int)l, p);
}

void chk_updates(PmReport *r, int flags)
{
	(void)flags;
	PmCheck *c;
	int apt = pm_pkg_kind() == PM_PKG_APT, dnf = pm_pkg_kind() == PM_PKG_DNF;

	/* 1. 系統版本還有沒有支援 */
	{
		char os[4096], id[64], ver[64], name[160];
		if (!chk_read("/etc/os-release", os, sizeof os)) chk_read("/usr/lib/os-release", os, sizeof os);
		os_field(os, "ID", id, sizeof id);
		os_field(os, "VERSION_ID", ver, sizeof ver);
		os_field(os, "PRETTY_NAME", name, sizeof name);
		char eol[32];
		if (os_eol(id, ver, eol, sizeof eol)) {
			time_t end = ymd(eol), now = time(NULL);
			long days = (long)((end - now) / 86400);
			/* 只有年月的是大約的日期（Fedora 依下下一版發行的時間而定） */
			char shown[48];
			snprintf(shown, sizeof shown, strlen(eol) <= 7 ? _("約 %s") : "%s", eol);
			snprintf(eol, sizeof eol, "%s", shown);
			if (days < 0) {
				c = chk_add(r, "upd.os", PM_CHK_BAD, _("%s 已經停止支援了（%s）"), name, eol);
				chk_detail(c, "%s", _("停止支援之後就沒有安全性更新，新發現的漏洞都不會修補。"));
				chk_advice(c, "%s", _("請升級到新版本的系統，或重新安裝新版的 Pomelo 安裝光碟。"));
			} else if (days < 90) {
				c = chk_add(r, "upd.os", PM_CHK_WARN, _("%s 再 %ld 天就停止支援（%s）"), name, days, eol);
				chk_advice(c, "%s", _("請開始規劃升級到新版本的系統。"));
			} else chk_add(r, "upd.os", PM_CHK_OK, _("%s 的支援到 %s"), name, eol);
		} else chk_add(r, "upd.os", PM_CHK_SKIP, _("不知道 %s 的支援期限"), name[0] ? name : id);
	}

	/* 2. 可以安裝的更新 */
	{
		int total = 0, sec = 0;
		char names[512] = "";
		if (apt) {
			char *o = chk_run("apt list --upgradable");
			for (char *l = o, *nx; l && *l; l = nx) {
				nx = chk_line(l);
				if (!strstr(l, "upgradable")) continue;
				total++;
				char *sl = strchr(l, '/'), *sp = strchr(l, ' ');
				if (sp) *sp = 0;
				if (strstr(l, "-security")) sec++;
				if (sl && strlen(names) < sizeof names - 64) snprintf(names + strlen(names), sizeof names - strlen(names), "%s%.*s", names[0] ? "、" : "", (int)(sl - l), l);
			}
			free(o);
		} else if (dnf) {
			char *o = chk_run("timeout 60 dnf -q --cacheonly updateinfo list --security");
			for (char *l = o, *nx; l && *l; l = nx) { nx = chk_line(l); if (strstr(l, "/Sec.") || strstr(l, "Important") || strstr(l, "Critical") || strstr(l, "Moderate")) sec++; }
			free(o);
			o = pm_pkg_upgradable();
			for (char *l = o, *nx; l && *l; l = nx) {
				nx = chk_line(l);
				if (!*l) continue;
				total++;
				char *tab = strchr(l, '\t');
				if (tab && strlen(names) < sizeof names - 64) snprintf(names + strlen(names), sizeof names - strlen(names), "%s%.*s", names[0] ? "、" : "", (int)(tab - l), l);
			}
			free(o);
		}
		if (strlen(names) >= sizeof names - 64) strcat(names, "…");
		if (sec) {
			c = chk_add(r, "upd.pending", PM_CHK_WARN, _("有 %d 個更新，其中 %d 個是安全性更新"), total, sec);
			chk_detail(c, "%s", names);
			chk_advice(c, "%s", _("安全性更新修補的是已經公開的漏洞，請盡快安裝。"));
			chk_fix(c, "upd.open", NULL, _("開啟軟體更新"));
		} else if (total) {
			c = chk_add(r, "upd.pending", PM_CHK_INFO, _("有 %d 個更新可以安裝"), total);
			chk_detail(c, "%s", names);
			chk_fix(c, "upd.open", NULL, _("開啟軟體更新"));
		} else chk_add(r, "upd.pending", PM_CHK_OK, "%s", _("軟體都是最新的"));
	}

	/* 3. 套件清單多久沒更新（太久的話上面的數字不準） */
	{
		struct stat st;
		const char *p = apt ? "/var/lib/apt/lists" : "/var/cache/dnf";
		if ((apt || dnf) && !stat(chk_path(p), &st)) {
			long days = (time(NULL) - st.st_mtime) / 86400;
			if (days > 14) {
				c = chk_add(r, "upd.lists", PM_CHK_WARN, _("套件清單 %ld 天沒有更新"), days);
				chk_detail(c, "%s", _("電腦不知道有沒有新的更新，上面的數字可能不準。"));
				chk_fix(c, "upd.open", NULL, _("開啟軟體更新"));
			} else chk_add(r, "upd.lists", PM_CHK_OK, _("套件清單 %ld 天前更新過"), days);
		}
	}

	/* 4. 自動安全性更新 */
	if (apt) {
		char conf[4096] = "", all[8192] = "";
		DIR *d = opendir(chk_path("/etc/apt/apt.conf.d"));
		struct dirent *e;
		while (d && (e = readdir(d))) {
			if (e->d_name[0] == '.') continue;
			char p[300];
			snprintf(p, sizeof p, "/etc/apt/apt.conf.d/%s", e->d_name);
			if (chk_read(p, conf, sizeof conf) && strlen(all) + strlen(conf) < sizeof all - 2) { strcat(all, conf); strcat(all, "\n"); }
		}
		if (d) closedir(d);
		int installed = !access(chk_path("/usr/bin/unattended-upgrade"), X_OK);
		int on = installed && strstr(all, "Unattended-Upgrade \"1\"");
		if (on) chk_add(r, "upd.auto", PM_CHK_OK, "%s", _("安全性更新會自動安裝"));
		else {
			c = chk_add(r, "upd.auto", PM_CHK_INFO, "%s", _("安全性更新不會自動安裝"));
			chk_detail(c, "%s", _("開啟後，電腦會在背景自動安裝安全性更新（一般更新還是由你決定）。"));
			chk_fix(c, "upd.auto", NULL, _("開啟自動安全性更新"));
		}
	} else if (dnf) {
		char *o = chk_run("systemctl is-enabled dnf-automatic.timer dnf5-automatic.timer dnf-automatic-install.timer");
		int on = o && strstr(o, "enabled") && !strstr(o, "not-found\nnot-found\nnot-found");
		free(o);
		if (on) chk_add(r, "upd.auto", PM_CHK_OK, "%s", _("有設定自動更新（dnf-automatic）"));
		else {
			c = chk_add(r, "upd.auto", PM_CHK_INFO, "%s", _("安全性更新不會自動安裝"));
			chk_detail(c, "%s", _("開啟後，電腦會在背景自動安裝安全性更新（一般更新還是由你決定）。"));
			chk_fix(c, "upd.auto", NULL, _("開啟自動安全性更新"));
		}
	}

	/* 5. 裝到一半的套件 */
	if (apt) {
		char *o = chk_run("dpkg --audit");
		int broken = o && *o;
		if (broken) {
			c = chk_add(r, "upd.broken", PM_CHK_BAD, "%s", _("有套件沒有安裝完成"));
			chk_detail(c, "%s", _("上次安裝或更新可能被中斷了（例如關機、斷電）；之後的更新也會一直失敗。"));
			chk_fix(c, "upd.dpkg", NULL, _("繼續完成安裝"));
		} else chk_add(r, "upd.broken", PM_CHK_OK, "%s", _("套件都安裝完整"));
		free(o);
		o = chk_run("apt-mark showhold");
		if (o && *o) {
			int nh = 0;
			char list[512] = "";
			for (char *l = o, *nx; *l; l = nx) {
				nx = chk_line(l);
				if (!*l) continue;
				nh++;
				if (strlen(list) < sizeof list - 64) snprintf(list + strlen(list), sizeof list - strlen(list), "%s%s", list[0] ? "、" : "", l);
			}
			c = chk_add(r, "upd.hold", PM_CHK_INFO, _("有 %d 個套件被設定成不更新"), nh);
			chk_detail(c, "%s", list);
			chk_advice(c, "%s", _("通常是刻意設定的；不記得的話可以用 sudo apt-mark unhold 套件名稱 取消。"));
		}
		free(o);
	}

	/* 6. 要重新開機才能完成更新 */
	{
		char pk[1024] = "";
		int need = !access(chk_path("/run/reboot-required"), F_OK) || !access(chk_path("/var/run/reboot-required"), F_OK);
		if (need) chk_read("/run/reboot-required.pkgs", pk, sizeof pk);
		/* 裝了比正在執行的更新的核心 */
		struct utsname u;
		char newest[128] = "";
		if (!uname(&u)) {
			DIR *d = opendir(chk_path("/lib/modules"));
			struct dirent *e;
			while (d && (e = readdir(d))) {
				if (e->d_name[0] == '.') continue;
				char p[400];
				snprintf(p, sizeof p, "/lib/modules/%s/modules.dep", e->d_name);   /* 只算完整裝好的 */
				if (access(chk_path(p), F_OK)) continue;
				if (!newest[0] || strverscmp(e->d_name, newest) > 0) snprintf(newest, sizeof newest, "%s", e->d_name);
			}
			if (d) closedir(d);
			if (newest[0] && strverscmp(newest, u.release) > 0) {
				need = 1;
				snprintf(pk + strlen(pk), sizeof pk - strlen(pk), "%s%s %s → %s", pk[0] ? "\n" : "", _("核心"), u.release, newest);
			}
		}
		if (need) {
			c = chk_add(r, "upd.reboot", PM_CHK_WARN, "%s", _("要重新開機才能完成更新"));
			if (pk[0]) { for (char *p = pk; *p; p++) if (*p == '\n' && p[1]) *p = '\n'; chk_detail(c, "%s", pk); }
			chk_detail(c, "%s", _("在重新開機之前，有些更新（例如核心的安全性修補）還沒有生效。"));
			chk_fix(c, "upd.reboot", NULL, _("重新開機"));
		} else chk_add(r, "upd.reboot", PM_CHK_OK, "%s", _("不需要重新開機"));
	}

	/* 7. 韌體更新（fwupd：只看已經下載的資料，不連網） */
	if (pm_have("fwupdmgr")) {
		char *o = chk_run("timeout 20 fwupdmgr get-updates --no-unreported-check --no-metadata-check --no-reboot-check");
		int n = 0;
		char list[512] = "";
		for (char *l = o, *nx; l && *l; l = nx) {
			nx = chk_line(l);
			/* 「├─裝置名稱:」這種樹狀行的下一層有「New version」 */
			char *nv = strstr(l, "New version:");
			if (nv) n++;
		}
		free(o);
		if (n) {
			c = chk_add(r, "upd.firmware", PM_CHK_INFO, _("有 %d 個韌體更新"), n);
			chk_advice(c, "%s", _("韌體更新可以修正硬體的問題與漏洞；在終端機執行 fwupdmgr update 安裝（要插著電源）。"));
			(void)list;
		} else chk_add(r, "upd.firmware", PM_CHK_OK, "%s", _("沒有找到韌體更新"));
	}

	/* 8. 空間夠不夠裝更新 */
	{
		struct statvfs v;
		if (!statvfs(chk_path("/var"), &v)) {
			double free_gb = (double)v.f_bavail * v.f_frsize / 1e9;
			if (free_gb < 2) {
				c = chk_add(r, "upd.space", free_gb < 0.5 ? PM_CHK_BAD : PM_CHK_WARN, _("系統磁碟只剩 %.1f GB，更新可能會失敗"), free_gb);
				chk_fix(c, "upd.storage", NULL, _("開啟儲存空間"));
			}
		}
	}

	/* 9. Pomelo 的版本 */
	chk_add(r, "upd.pomelo", PM_CHK_OK, _("Pomelo %s"), POMELO_VERSION);
}

int chk_upd_fix(const PmCheck *c, char **argv, int max, int *admin, char *msg, size_t n)
{
	(void)max;
	static char script[512];
	int k = 0;
	if (!strcmp(c->fix, "upd.open")) { argv[k++] = "@open"; argv[k++] = "pomelo-software --updates"; }
	else if (!strcmp(c->fix, "upd.storage")) { argv[k++] = "@open"; argv[k++] = "pomelo-storage --cleanup"; }
	else if (!strcmp(c->fix, "upd.dpkg")) {
		*admin = 1;
		snprintf(msg, n, "%s", _("繼續完成上次沒裝完的套件（dpkg --configure -a），可能要幾分鐘。"));
		argv[k++] = "env"; argv[k++] = "DEBIAN_FRONTEND=noninteractive"; argv[k++] = "dpkg"; argv[k++] = "--configure"; argv[k++] = "-a";
	} else if (!strcmp(c->fix, "upd.auto")) {
		*admin = 1;
		if (pm_pkg_kind() == PM_PKG_DNF) {
			/* dnf-automatic：只裝安全性更新、裝完不重開；timer 每天跑。Fedora 41+ 是 dnf5 外掛，設定檔在 /etc/dnf/dnf5-plugins/ */
			char *install = pm_have("dnf-automatic") ? NULL : pm_pkg_cmdline("install", "dnf-automatic");
			snprintf(script, sizeof script,
			         "%s%sf=/etc/dnf/automatic.conf; [ -f $f ] || { f=/etc/dnf/dnf5-plugins/automatic.conf; [ -f $f ] || cp /usr/share/dnf5/dnf5-plugins/automatic.conf $f; }; "
			         "sed -i 's/^apply_updates *=.*/apply_updates = yes/; s/^upgrade_type *=.*/upgrade_type = security/' $f && systemctl enable --now dnf-automatic.timer",
			         install ? install : "", install ? " && " : "");
			free(install);
		} else {
			char *install = pm_have("unattended-upgrade") ? NULL : pm_pkg_cmdline("install", "unattended-upgrades");
			snprintf(script, sizeof script,
			         "%s%sprintf 'APT::Periodic::Update-Package-Lists \"1\";\\nAPT::Periodic::Unattended-Upgrade \"1\";\\n' > /etc/apt/apt.conf.d/20auto-upgrades",
			         install ? install : "", install ? " && " : "");
			free(install);
		}
		snprintf(msg, n, "%s", _("讓電腦每天自動下載套件清單、在背景安裝安全性更新（一般更新不會自動裝）。"));
		argv[k++] = "sh"; argv[k++] = "-c"; argv[k++] = script;
	} else if (!strcmp(c->fix, "upd.reboot")) {
		snprintf(msg, n, "%s", _("馬上重新開機。請先儲存所有開著的檔案。"));
		argv[k++] = "systemctl"; argv[k++] = "reboot";
	}
	argv[k] = NULL;
	return k;
}
