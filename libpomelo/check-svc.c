/* SPDX-License-Identifier: GPL-3.0-or-later */
/* check-svc.c — 系統服務（伺服器）：常見的服務（SSH、Apache、Nginx、MySQL／MariaDB、PostgreSQL、Docker、Redis、MongoDB、
 * Samba）有裝的話：有沒有在執行、開機會不會自動啟動、有沒有一直重新啟動、該聽的埠有沒有在聽；網頁伺服器另外看
 * 本機 443 埠的憑證什麼時候過期；Docker 看有沒有容器一直重新啟動或不健康（docker ps，只讀）。
 * 回答「為什麼我的網站打不開」：服務沒跑、埠沒在聽、憑證過期、最後的錯誤訊息。 */
#define _GNU_SOURCE
#include "internal.h"
#include "check-int.h"
#include <time.h>

typedef struct { const char *name; const char *units[4]; int ports[3]; } Svc;

static const Svc svcs[] = {
	{ "SSH", { "ssh.service", "sshd.service" }, { 22 } },
	{ "Apache", { "apache2.service", "httpd.service" }, { 80, 443 } },
	{ "Nginx", { "nginx.service" }, { 80, 443 } },
	{ "MySQL／MariaDB", { "mysql.service", "mariadb.service", "mysqld.service" }, { 3306 } },
	{ "PostgreSQL", { "postgresql.service" }, { 5432 } },
	{ "Docker", { "docker.service" }, { 0 } },
	{ "Redis", { "redis-server.service", "redis.service" }, { 6379 } },
	{ "MongoDB", { "mongod.service" }, { 27017 } },
	{ "Samba", { "smbd.service", "smb.service" }, { 445 } },
};

/* systemctl show 一個服務的幾個屬性 */
static void unit_props(const char *unit, char *load, char *active, char *enabled, int *restarts)
{
	char cmd[300];
	snprintf(cmd, sizeof cmd, "systemctl show -p LoadState -p ActiveState -p UnitFileState -p NRestarts %s", unit);
	char *o = chk_run(cmd);
	load[0] = active[0] = enabled[0] = 0;
	*restarts = 0;
	for (char *l = o, *nx; l && *l; l = nx) {
		nx = chk_line(l);
		if (!strncmp(l, "LoadState=", 10)) snprintf(load, 32, "%s", l + 10);
		else if (!strncmp(l, "ActiveState=", 12)) snprintf(active, 32, "%s", l + 12);
		else if (!strncmp(l, "UnitFileState=", 14)) snprintf(enabled, 32, "%s", l + 14);
		else if (!strncmp(l, "NRestarts=", 10)) *restarts = atoi(l + 10);
	}
	free(o);
}

static time_t parse_notafter(const char *s)
{
	/* notAfter=Mar  4 12:00:00 2027 GMT */
	static const char *mon[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
	char m[8];
	int d, H, M, S, y;
	const char *p = strchr(s, '=');
	if (!p || sscanf(p + 1, "%7s %d %d:%d:%d %d", m, &d, &H, &M, &S, &y) != 6) return 0;
	struct tm t = { .tm_mday = d, .tm_hour = H, .tm_min = M, .tm_sec = S, .tm_year = y - 1900 };
	for (int i = 0; i < 12; i++) if (!strcmp(m, mon[i])) t.tm_mon = i;
	return timegm(&t);
}

void chk_services(PmReport *r, int flags)
{
	(void)flags;
	PmCheck *c;
	int any = 0;
	for (size_t i = 0; i < LEN(svcs); i++) {
		const Svc *s = &svcs[i];
		char load[32], active[32], enabled[32], unit[64] = "";
		int restarts = 0;
		for (int k = 0; k < 4 && s->units[k]; k++) {
			unit_props(s->units[k], load, active, enabled, &restarts);
			if (!strcmp(load, "loaded")) { snprintf(unit, sizeof unit, "%s", s->units[k]); break; }
		}
		if (!unit[0]) continue;   /* 沒裝 */
		any = 1;
		char id[32];
		snprintf(id, sizeof id, "svc.%.24s", unit);
		int running = !strcmp(active, "active") || !strcmp(active, "reloading");
		int on_boot = !strcmp(enabled, "enabled") || !strcmp(enabled, "static") || !strcmp(enabled, "alias");
		/* 用 socket 啟動的（例如 Ubuntu 24.04 的 ssh.socket）：有連線才啟動，也算開機會啟動 */
		if (!on_boot) {
			char sock[64], l2[32], a2[32], e2[32];
			int r2;
			snprintf(sock, sizeof sock, "%.*s.socket", (int)(strstr(unit, ".service") ? strstr(unit, ".service") - unit : (long)strlen(unit)), unit);
			unit_props(sock, l2, a2, e2, &r2);
			if (!strcmp(l2, "loaded") && (!strcmp(e2, "enabled") || !strcmp(a2, "active"))) on_boot = 1;
		}
		if (!running) {
			if (!on_boot && strcmp(active, "failed")) {
				c = chk_add(r, id, PM_CHK_INFO, _("%s 有安裝，但沒有啟用"), s->name);
				chk_detail(c, _("服務：%s。不需要的話可以不用管。"), unit);
				continue;
			}
			int failed = !strcmp(active, "failed");
			c = chk_add(r, id, failed ? PM_CHK_BAD : PM_CHK_WARN, failed ? _("%s 啟動失敗") : _("%s 設定成開機啟動，但現在沒有在執行"), s->name);
			chk_detail(c, _("服務：%s（狀態：%s）"), unit, active);
			/* 埠被別的程式用走了（例如 Apache 跟 Nginx 都要 80、443） */
			char used[64] = "";
			for (int k = 0; k < 3 && s->ports[k]; k++)
				if (chk_listening(s->ports[k])) snprintf(used + strlen(used), sizeof used - strlen(used), "%s%d", used[0] ? "、" : "", s->ports[k]);
			if (used[0]) {
				chk_detail(c, _("它要用的 %s 埠現在被別的程式佔用（可能是另一個網頁伺服器），所以啟動不了或被停掉了。"), used);
				chk_advice(c, _("同一台電腦裝了兩個用同樣埠的伺服器；不需要 %s 的話，可以關掉它的開機啟動。"), s->name);
				chk_fix(c, "svc.disable", unit, _("關掉開機啟動"));
				continue;
			}
			char cmd[300];
			snprintf(cmd, sizeof cmd, "journalctl -u %s -n 3 -q --no-pager -o cat", unit);
			char *lg = chk_run(cmd);
			if (lg && *lg) chk_detail(c, _("最後的記錄：\n%.400s"), lg);
			free(lg);
			chk_advice(c, "%s", _("先試著重新啟動；馬上又停掉的話，看上面的記錄或設定檔有沒有寫錯。"));
			chk_fix(c, "svc.restart", unit, _("重新啟動"));
			continue;
		}
		/* 在執行：該聽的埠 */
		char ports[64] = "", missing[64] = "";
		for (int k = 0; k < 3 && s->ports[k]; k++) {
			int on = chk_listening(s->ports[k]);
			snprintf(on ? ports + strlen(ports) : missing + strlen(missing), 32, "%s%d", (on ? ports : missing)[0] ? "、" : "", s->ports[k]);
		}
		if (restarts >= 3) {
			c = chk_add(r, id, PM_CHK_WARN, _("%s 一直當掉又被重新啟動（%d 次）"), s->name, restarts);
			chk_advice(c, _("用 journalctl -u %s 看它為什麼一直結束。"), unit);
		} else if (s->ports[0] && !ports[0]) {
			c = chk_add(r, id, PM_CHK_WARN, _("%s 在執行，但沒有在聽 %s 埠"), s->name, missing);
			chk_detail(c, "%s", _("別的電腦（或瀏覽器）會連不上；可能是設定改了埠號，或還在啟動中。"));
		} else {
			c = chk_add(r, id, PM_CHK_OK, ports[0] ? _("%s 正常執行（%s 埠）") : _("%s 正常執行"), s->name, ports);
			if (!on_boot) chk_detail(c, "%s", _("開機不會自動啟動。"));
		}
	}

	/* 網頁伺服器的憑證（本機 443 埠） */
	if (chk_listening(443) && pm_have("openssl")) {
		char *o = chk_run("timeout 6 openssl s_client -connect 127.0.0.1:443 </dev/null 2>/dev/null | openssl x509 -noout -enddate -subject 2>/dev/null");
		char *na = o ? strstr(o, "notAfter=") : NULL;
		if (na) {
			time_t end = parse_notafter(na);
			char *sub = strstr(o, "subject=");
			char subj[200] = "";
			if (sub) { sub += 8; snprintf(subj, sizeof subj, "%.*s", (int)strcspn(sub, "\n"), sub); }
			long days = end ? (long)((end - time(NULL)) / 86400) : 9999;
			char date[32] = "";
			struct tm tm;
			if (end) { gmtime_r(&end, &tm); strftime(date, sizeof date, "%Y-%m-%d", &tm); }
			if (days < 0) {
				c = chk_add(r, "svc.cert", PM_CHK_BAD, _("網站的憑證已經過期（%s）"), date);
				chk_detail(c, "%s", subj);
				chk_advice(c, "%s", _("瀏覽器會擋住這個網站；請更新憑證（Let's Encrypt：sudo certbot renew）。"));
			} else if (days < 14) {
				c = chk_add(r, "svc.cert", PM_CHK_WARN, _("網站的憑證再 %ld 天就過期（%s）"), days, date);
				chk_detail(c, "%s", subj);
				chk_advice(c, "%s", _("請盡快更新憑證；用 Let's Encrypt 的話檢查自動更新（certbot）有沒有在運作。"));
			} else {
				c = chk_add(r, "svc.cert", PM_CHK_OK, _("網站的憑證到 %s"), date);
				chk_detail(c, "%s", subj);
			}
			any = 1;
		}
		free(o);
	}

	/* Docker 容器：一直重新啟動、不健康 */
	if (pm_have("docker")) {
		char *o = chk_run("timeout 10 docker ps -a --format '{{.Names}}\t{{.Status}}'");
		char bad[1024] = "";
		int nb = 0, nrun = 0;
		for (char *l = o, *nx; l && *l; l = nx) {
			nx = chk_line(l);
			char *t = strchr(l, '\t');
			if (!t) continue;
			*t++ = 0;
			if (!strncmp(t, "Up", 2) && !strstr(t, "unhealthy")) { nrun++; continue; }
			if (strncmp(t, "Restarting", 10) && !strstr(t, "unhealthy")) continue;
			nb++;
			if (strlen(bad) < sizeof bad - 200) snprintf(bad + strlen(bad), sizeof bad - strlen(bad), "%s%s：%s", bad[0] ? "\n" : "", l, t);
		}
		free(o);
		if (nb) {
			c = chk_add(r, "svc.docker", PM_CHK_WARN, _("有 %d 個容器一直重新啟動或不健康"), nb);
			chk_detail(c, "%s", bad);
			chk_advice(c, "%s", _("在「容器」程式看它的記錄（日誌）找原因。"));
			chk_fix(c, "svc.docker", NULL, _("開啟容器"));
		} else if (nrun) chk_add(r, "svc.docker", PM_CHK_OK, _("%d 個容器正常執行中"), nrun);
	}

	if (!any) chk_add(r, "svc.none", PM_CHK_SKIP, "%s", _("這台電腦沒有安裝常見的伺服器服務"));
}

int chk_svc_fix(const PmCheck *c, char **argv, int max, int *admin, char *msg, size_t n)
{
	(void)max;
	int k = 0;
	if (!strcmp(c->fix, "svc.restart")) {
		*admin = 1;
		snprintf(msg, n, _("重新啟動 %s。"), c->fix_arg);
		argv[k++] = "systemctl"; argv[k++] = "restart"; argv[k++] = (char *)c->fix_arg;
	} else if (!strcmp(c->fix, "svc.disable")) {
		*admin = 1;
		snprintf(msg, n, _("讓 %s 以後開機不要自動啟動（不會移除，之後隨時可以再開）。"), c->fix_arg);
		argv[k++] = "systemctl"; argv[k++] = "disable"; argv[k++] = (char *)c->fix_arg;
	} else if (!strcmp(c->fix, "svc.docker")) { argv[k++] = "@open"; argv[k++] = "pomelo-docker"; }
	argv[k] = NULL;
	return k;
}
