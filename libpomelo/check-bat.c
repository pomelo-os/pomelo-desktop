/* SPDX-License-Identifier: GPL-3.0-or-later */
/* check-bat.c — 電池：還剩原本容量的幾成（健康度）、充放電次數、充電上限、插著電卻沒在充電。 */
#define _GNU_SOURCE
#include "internal.h"
#include "check-int.h"
#include <dirent.h>

void chk_battery(PmReport *r, int flags)
{
	(void)flags;
	PmCheck *c;
	int nb = 0;
	DIR *d = opendir(chk_path("/sys/class/power_supply"));
	struct dirent *e;
	while (d && (e = readdir(d))) {
		if (strncmp(e->d_name, "BAT", 3) && strncmp(e->d_name, "CMB", 3)) continue;
		char p[300], b[64];
		snprintf(p, sizeof p, "/sys/class/power_supply/%s/type", e->d_name);
		if (chk_read(p, b, sizeof b) && strcmp(b, "Battery")) continue;
		nb++;
		/* 有的電池給 energy（µWh），有的給 charge（µAh） */
		long full = -1, design = -1;
		const char *kinds[] = { "energy", "charge" };
		for (int k = 0; k < 2 && (full <= 0 || design <= 0); k++) {
			snprintf(p, sizeof p, "/sys/class/power_supply/%s/%s_full", e->d_name, kinds[k]);
			full = chk_read_long(p, -1);
			snprintf(p, sizeof p, "/sys/class/power_supply/%s/%s_full_design", e->d_name, kinds[k]);
			design = chk_read_long(p, -1);
		}
		snprintf(p, sizeof p, "/sys/class/power_supply/%s/cycle_count", e->d_name);
		long cycles = chk_read_long(p, -1);
		snprintf(p, sizeof p, "/sys/class/power_supply/%s/capacity", e->d_name);
		long cap = chk_read_long(p, -1);
		char status[32] = "";
		snprintf(p, sizeof p, "/sys/class/power_supply/%s/status", e->d_name);
		chk_read(p, status, sizeof status);
		char id[32];
		snprintf(id, sizeof id, "bat.health%d", nb);
		if (full > 0 && design > 0) {
			double h = 100.0 * full / design;
			if (h > 100) h = 100;
			int lv = h < 60 ? PM_CHK_BAD : h < 80 ? PM_CHK_WARN : PM_CHK_OK;
			c = chk_add(r, id, lv, _("電池健康度 %.0f%%"), h);
			chk_detail(c, _("充飽時只能存原本容量的 %.0f%%%s。"), h, lv == PM_CHK_OK ? "" : _("，用電時間會比新的時候短"));
			if (cycles > 0) chk_detail(c, _("充放電 %ld 次。"), cycles);
			if (cap >= 0) chk_detail(c, _("目前電量 %ld%%（%s）。"), cap, !strcmp(status, "Charging") ? _("充電中") : !strcmp(status, "Discharging") ? _("用電池") :
			                                                                 !strcmp(status, "Full") ? _("已充飽") :
			                                                                 !strcmp(status, "Not charging") ? _("沒在充電") : status);
			if (lv == PM_CHK_BAD) chk_advice(c, "%s", _("電池老化了；用電時間太短的話可以考慮換電池。"));
		} else {
			c = chk_add(r, id, PM_CHK_INFO, "%s", _("讀不到電池的設計容量，無法判斷健康度"));
			if (cap >= 0) chk_detail(c, _("目前電量 %ld%%。"), cap);
		}
		/* 插著電，但沒在充電（不是充飽） */
		if (!strcmp(status, "Not charging") && cap >= 0 && cap < 95) {
			snprintf(p, sizeof p, "/sys/class/power_supply/%s/charge_control_end_threshold", e->d_name);
			long th = chk_read_long(p, -1);
			c = chk_add(r, "bat.notcharging", th > 0 && cap >= th - 2 ? PM_CHK_OK : PM_CHK_WARN, "%s",
			            th > 0 && cap >= th - 2 ? _("已充到設定的上限，暫停充電（保護電池）") : _("插著電，但電池沒有在充電"));
			if (!(th > 0 && cap >= th - 2)) chk_advice(c, "%s", _("檢查變壓器與插頭；變壓器功率不夠時也會這樣。"));
		}
		/* 充電上限：常插著電的筆電設 80% 可以延長電池壽命 */
		snprintf(p, sizeof p, "/sys/class/power_supply/%s/charge_control_end_threshold", e->d_name);
		long th = chk_read_long(p, -1);
		if (th >= 95) {
			c = chk_add(r, "bat.threshold", PM_CHK_INFO, "%s", _("電池會一直充到 100%"));
			chk_detail(c, "%s", _("這台筆電支援設定充電上限；常常插著電使用的話，設成 80% 可以讓電池老化得慢一點。"));
		} else if (th > 0) chk_add(r, "bat.threshold", PM_CHK_OK, _("充電上限設在 %ld%%"), th);
	}
	if (d) closedir(d);
	if (!nb) chk_add(r, "bat.none", PM_CHK_SKIP, "%s", _("這台電腦沒有電池"));
}

int chk_bat_fix(const PmCheck *c, char **argv, int max, int *admin, char *msg, size_t n)
{
	(void)c; (void)max; (void)admin; (void)msg; (void)n;
	argv[0] = NULL;
	return 0;
}
