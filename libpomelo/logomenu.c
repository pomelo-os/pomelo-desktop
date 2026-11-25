/* SPDX-License-Identifier: GPL-3.0-or-later */
/* logomenu.c — 選單列左上角的 Pomelo 選單（使用者可以編輯）
 *
 * 存在 ~/.config/pomelo/menu.conf，一行一項：「種類<Tab>內容<Tab>名稱」
 *   item  內建項目的代號（about、settings…）
 *   app   .desktop 的代號（firefox、pomelo-terminal…）
 *   cmd   指令（用 sh -c 執行）
 *   sep   分隔線
 * 名稱留空就用預設的名稱（內建項目的翻譯、程式自己的名稱）。沒有這個檔案就用內建的預設選單。
 */
#include "pomelo.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

const PmMenuBuiltin pm_menu_builtins[] = {
	{ "about",          N_("關於 Pomelo") },
	{ "settings",       N_("系統設定…") },
	{ "software",       N_("軟體中心…") },
	{ "monitor",        N_("活動監視器…") },
	{ "storage",        N_("儲存空間…") },
	{ "launcher",       N_("啟動器") },
	{ "search",         N_("搜尋") },
	{ "overview",       N_("所有視窗") },
	{ "lock",           N_("鎖定螢幕") },
	{ "sleep",          N_("睡眠") },
	{ "restart",        N_("重新開機…") },
	{ "shutdown",       N_("關機…") },
	{ "restart-pomelo", N_("重新啟動 Pomelo") },
	{ "logout",         N_("登出…") },
};
const int pm_menu_nbuiltins = (int)(sizeof pm_menu_builtins / sizeof pm_menu_builtins[0]);

/* 預設選單（"-" 是分隔線） */
static const char *const defaults[] = {
	"about", "-", "settings", "software", "monitor", "storage", "-", "launcher", "search", "overview", "-",
	"lock", "sleep", "restart", "shutdown", "-", "restart-pomelo", "logout",
};

static void menu_path(char *out, size_t n)
{
	const char *cfg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
	if (cfg && *cfg) snprintf(out, n, "%s/pomelo/menu.conf", cfg);
	else snprintf(out, n, "%s/.config/pomelo/menu.conf", home ? home : "/tmp");
}

const char *pm_menu_builtin_label(const char *id)
{
	for (int i = 0; i < pm_menu_nbuiltins; i++)
		if (!strcmp(pm_menu_builtins[i].id, id)) return pm_menu_builtins[i].label;
	return NULL;
}

int pm_menu_customized(void)
{
	char p[4096];
	menu_path(p, sizeof p);
	return access(p, F_OK) == 0;
}

int pm_menu_load(PmMenuEntry *e, int max)
{
	char p[4096], line[1024];
	int n = 0;
	menu_path(p, sizeof p);
	FILE *f = fopen(p, "r");
	if (!f) {
		for (unsigned i = 0; i < sizeof defaults / sizeof defaults[0] && n < max; i++, n++) {
			memset(&e[n], 0, sizeof e[n]);
			e[n].kind = strcmp(defaults[i], "-") ? PM_MENU_ITEM : PM_MENU_SEP;
			if (e[n].kind == PM_MENU_ITEM) snprintf(e[n].arg, sizeof e[n].arg, "%s", defaults[i]);
		}
		return n;
	}
	while (n < max && fgets(line, sizeof line, f)) {
		line[strcspn(line, "\r\n")] = 0;
		if (!line[0] || line[0] == '#') continue;
		char *arg = strchr(line, '\t'), *label = NULL;
		if (arg) { *arg++ = 0; if ((label = strchr(arg, '\t'))) *label++ = 0; }
		PmMenuEntry *x = &e[n];
		memset(x, 0, sizeof *x);
		if (!strcmp(line, "sep")) x->kind = PM_MENU_SEP;
		else if (!strcmp(line, "item") && arg && pm_menu_builtin_label(arg)) x->kind = PM_MENU_ITEM;
		else if (!strcmp(line, "app") && arg && *arg) x->kind = PM_MENU_APP;
		else if (!strcmp(line, "cmd") && arg) x->kind = PM_MENU_CMD;
		else continue;   /* 看不懂的行（例如新版才有的內建項目）略過 */
		if (arg) snprintf(x->arg, sizeof x->arg, "%s", arg);
		if (label) snprintf(x->label, sizeof x->label, "%s", label);
		n++;
	}
	fclose(f);
	return n;
}

int pm_menu_save(const PmMenuEntry *e, int n)
{
	static const char *kinds[] = { "item", "app", "cmd", "sep" };
	char p[4096], tmp[4200];
	menu_path(p, sizeof p);
	char dir[4096];
	snprintf(dir, sizeof dir, "%s", p);
	*strrchr(dir, '/') = 0;
	mkdir(dir, 0755);
	snprintf(tmp, sizeof tmp, "%s.new", p);
	FILE *f = fopen(tmp, "w");
	if (!f) return -1;
	fputs("# Pomelo 選單（選單列左上角）：種類<Tab>內容<Tab>名稱；種類是 item（內建）、app、cmd（sh 指令）、sep（分隔線）\n", f);
	for (int i = 0; i < n; i++) {
		if (e[i].kind < 0 || e[i].kind > PM_MENU_SEP) continue;
		/* Tab 和換行會破壞格式，換成空白 */
		char a[sizeof e[i].arg], l[sizeof e[i].label];
		snprintf(a, sizeof a, "%s", e[i].arg);
		snprintf(l, sizeof l, "%s", e[i].label);
		for (char *c = a; *c; c++) if (*c == '\t' || *c == '\n' || *c == '\r') *c = ' ';
		for (char *c = l; *c; c++) if (*c == '\t' || *c == '\n' || *c == '\r') *c = ' ';
		if (e[i].kind == PM_MENU_SEP) fputs("sep\n", f);
		else fprintf(f, "%s\t%s\t%s\n", kinds[e[i].kind], a, l);
	}
	int ok = fclose(f) == 0;
	if (!ok || rename(tmp, p)) { unlink(tmp); return -1; }
	return 0;
}

void pm_menu_reset(void)
{
	char p[4096];
	menu_path(p, sizeof p);
	unlink(p);
}

const char *pm_menu_label(const PmMenuEntry *e)
{
	if (e->label[0]) return e->label;
	if (e->kind == PM_MENU_ITEM) { const char *l = pm_menu_builtin_label(e->arg); return l ? pm_tr(l) : e->arg; }
	if (e->kind == PM_MENU_APP) { PmApp *a = pm_app_by_id(e->arg); return a && a->name ? a->name : e->arg; }
	return e->arg;
}

/* 桌面按右鍵：Wayland 工作階段由 pomelo-panel 的桌面視窗、X11 工作階段由 pomelo-wm（點在 root 上）叫用 */
static void desk_spawn(const Arg *a) { pm_spawn(a->s); }

static const MenuItem desk_items[] = {
	{ N_("更換背景圖片…"), desk_spawn, { .s = "pomelo-theme --page wallpaper" } },
	{ N_("外觀…"), desk_spawn, { .s = "pomelo-theme --page appearance" } },
	{ N_("桌面與 Dock…"), desk_spawn, { .s = "pomelo-theme --page dock" } },
	{ N_("顯示器設定…"), desk_spawn, { .s = "pomelo-display" } },
	{ NULL, NULL, { 0 } },
	{ N_("開啟終端機"), desk_spawn, { .s = "pomelo-terminal" } },
	{ N_("系統設定…"), desk_spawn, { .s = "pomelo-system" } },
};
/* 同一份選單給原生 Wayland 的面板自己用 pmwl_menu 開 */
const MenuItem *pm_desktop_menu_items(int *n) { *n = LEN(desk_items); return desk_items; }

void pm_desktop_menu(int x, int y)
{
	static const MenuItem m[] = {
		{ N_("更換背景圖片…"), desk_spawn, { .s = "pomelo-theme --page wallpaper" } },
		{ N_("外觀…"), desk_spawn, { .s = "pomelo-theme --page appearance" } },
		{ N_("桌面與 Dock…"), desk_spawn, { .s = "pomelo-theme --page dock" } },
		{ N_("顯示器設定…"), desk_spawn, { .s = "pomelo-display" } },
		{ NULL, NULL, { 0 } },
		{ N_("開啟終端機"), desk_spawn, { .s = "pomelo-terminal" } },
		{ N_("系統設定…"), desk_spawn, { .s = "pomelo-system" } },
	};
	pm_popup(m, LEN(m), x, y);
}
