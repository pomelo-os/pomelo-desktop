/* SPDX-License-Identifier: GPL-3.0-or-later */
/* pomelo-theme — Pomelo 外觀設定
 *
 * 淺色 / 深色、強調色、字型大小、圖示主題、桌布、Dock、選單列時鐘、終端機配色。
 * 設定寫到 ~/.config/pomelo/settings.conf，所有 Pomelo 程式會立即套用。
 */
#define _GNU_SOURCE
#include <pomelo.h>
#include <dirent.h>
#include <fcntl.h>
#include <malloc.h>
#include <sys/stat.h>
#include <sys/wait.h>

static const char *modes[] = { N_("淺色"), N_("深色") };
static const unsigned accents[] = { 0x007aff, 0xa550a7, 0xf74f9e, 0xff5257, 0xf7821b, 0xffc600, 0x62ba46, 0x8c8c8c };
static const char *sizes[] = { N_("小"), N_("標準"), N_("大") };
static const double size_val[] = { 12, 13, 14.5 };
static const char *fits[] = { N_("填滿"), N_("完整顯示"), N_("延展") };
static const char *clock_fmts[] = {
	N_("%-m月%-d日 %A  %H:%M"), N_("%-m月%-d日 %A  %p %-I:%M"), N_("%-m月%-d日 %H:%M"), "%A %H:%M", "%H:%M", "%Y/%m/%d %H:%M",
};
static const char *term_themes[] = { N_("跟隨系統"), N_("深色"), N_("淺色") };
static const char *term_keys[] = { "system", "dark", "light" };

static char *icon_themes[64];
static int nicon_themes;

#define MAXWP 40
static char *wp_path[MAXWP];
static const char *wp_name[MAXWP];
static cairo_surface_t *wp_thumb[MAXWP];
static int nwp;

static int dark, accent, fsize, icontheme, wp, wpmode, dock_show, dock_hide, clockfmt, termtheme, btn_side = 1;
static double dock_size;
static int dock_dirty;

static void scan_icon_themes(void)
{
	const char *dirs[] = { "/usr/share/icons", "/usr/local/share/icons" };
	for (unsigned d = 0; d < LEN(dirs); d++) {
		DIR *dp = opendir(dirs[d]);
		if (!dp) continue;
		struct dirent *e;
		while ((e = readdir(dp)) && nicon_themes < (int)LEN(icon_themes)) {
			if (e->d_name[0] == '.' || !strcmp(e->d_name, "default") || !strcmp(e->d_name, "hicolor")) continue;
			/* 有 index.theme 而且不是只有游標的主題 */
			char p[PATH_MAX];
			snprintf(p, sizeof p, "%s/%s/index.theme", dirs[d], e->d_name);
			FILE *f = fopen(p, "r");
			if (!f) continue;
			char line[512];
			int has_dirs = 0;
			while (fgets(line, sizeof line, f))
				if (!strncmp(line, "Directories=", 12) && strlen(line) > 14) has_dirs = 1;
			fclose(f);
			if (has_dirs) icon_themes[nicon_themes++] = strdup(e->d_name);
		}
		closedir(dp);
	}
	/* 排序 */
	for (int i = 1; i < nicon_themes; i++)
		for (int j = i; j > 0 && strcasecmp(icon_themes[j], icon_themes[j - 1]) < 0; j--) {
			char *t = icon_themes[j]; icon_themes[j] = icon_themes[j - 1]; icon_themes[j - 1] = t;
		}
}

/* 內建漸層的縮圖 */
static cairo_surface_t *gradient_thumb(int w, int h)
{
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
	cairo_t *cr = cairo_create(s);
	cairo_pattern_t *g = cairo_pattern_create_linear(0, 0, w, h);
	cairo_pattern_add_color_stop_rgb(g, 0.0, 0.20, 0.36, 0.66);
	cairo_pattern_add_color_stop_rgb(g, 0.5, 0.52, 0.42, 0.74);
	cairo_pattern_add_color_stop_rgb(g, 1.0, 0.98, 0.62, 0.48);
	cairo_set_source(cr, g);
	cairo_paint(cr);
	cairo_pattern_destroy(g);
	cairo_destroy(cr);
	return s;
}

static void add_wallpapers(const char *dir)
{
	DIR *dp = opendir(dir);
	if (!dp) return;
	struct dirent *e;
	while ((e = readdir(dp)) && nwp < MAXWP) {
		const char *dot = strrchr(e->d_name, '.');
		if (!dot || (strcasecmp(dot, ".jpg") && strcasecmp(dot, ".jpeg") && strcasecmp(dot, ".png"))) continue;
		char p[PATH_MAX];
		snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
		wp_path[nwp] = strdup(p);
		wp_name[nwp] = strrchr(wp_path[nwp], '/') + 1;
		nwp++;
	}
	closedir(dp);
}


/* 縮圖快取在 libpomelo（pm_thumb_cached／pm_thumb_load）：~/.cache/pomelo/wpthumb/ */
static char wp_failed[MAXWP];

static void scan_wallpapers(void)
{
	wp_path[0] = strdup("gradient");
	wp_name[0] = _("Pomelo 漸層");
	wp_thumb[0] = gradient_thumb(150, 94);
	nwp = 1;
	add_wallpapers("/usr/share/backgrounds");
	/* 使用者的「圖片」資料夾 */
	char pics[PATH_MAX] = "";
	const char *home = getenv("HOME");
	char cfg[PATH_MAX], line[PATH_MAX];
	snprintf(cfg, sizeof cfg, "%s/.config/user-dirs.dirs", home ? home : "");
	FILE *f = fopen(cfg, "r");
	while (f && fgets(line, sizeof line, f))
		if (!strncmp(line, "XDG_PICTURES_DIR=\"$HOME", 23)) {
			line[strcspn(line + 23, "\"") + 23] = 0;
			snprintf(pics, sizeof pics, "%s%s", home, line + 23);
		}
	if (f) fclose(f);
	if (pics[0]) add_wallpapers(pics);
	/* 目前的桌布不在清單中就加進來 */
	const char *cur = pm_conf_get("desktop.wallpaper", "gradient");
	int found = 0;
	for (int i = 0; i < nwp; i++) if (!strcmp(wp_path[i], cur)) found = 1;
	if (!found && nwp < MAXWP && strcmp(cur, "none")) {
		wp_path[nwp] = strdup(cur);
		wp_name[nwp] = strrchr(cur, '/') ? strrchr(wp_path[nwp], '/') + 1 : wp_path[nwp];
		nwp++;
	}
	/* 縮圖：有快取的現在就讀（很快）；沒有的留空白，由 tick() 一次解一張（原本每次開啟都要把 /usr/share/backgrounds
	 * 的大圖全部解碼，約 1.5 秒才出現視窗） */
	for (int i = 1; i < nwp; i++) wp_thumb[i] = pm_thumb_cached(wp_path[i], 150, 94, 0);
}

/* 解一張還沒有縮圖的桌布（回傳 1＝做了一張） */
static int thumb_fill_one(void)
{
	for (int i = 1; i < nwp; i++)
		if (!wp_thumb[i] && !wp_failed[i]) {
			wp_thumb[i] = pm_thumb_load(wp_path[i], 150, 94, 0);
			if (!wp_thumb[i]) wp_failed[i] = 1;
			malloc_trim(0);   /* 大張 PNG 解碼完就把記憶體還給系統 */
			return 1;
		}
	return 0;
}

/* 視窗陰影與動畫：picom（視窗管理器照 appearance.compositor 啟動或停止它） */
static int compositor, comp_have;
static pid_t comp_job;
static char comp_msg[200];

static void comp_install(void)
{
	const char *pkgs[] = { "picom" };
	char *argv[16];
	if (!pm_pkg_argv("install-min", pkgs, 1, argv, 16)) { snprintf(comp_msg, sizeof comp_msg, "%s", _("這個系統沒有 picom 套件")); return; }
	signal(SIGCHLD, SIG_DFL);   /* 介面元件把 SIGCHLD 設成忽略，那樣 waitpid 拿不到結果 */
	comp_msg[0] = 0;
	comp_job = fork();
	if (comp_job == 0) {
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
	if (comp_job < 0) comp_job = 0;
}

static void tick(PmUI *u)
{
	if (thumb_fill_one()) pm_ui_redraw(u);
	if (comp_job <= 0 || waitpid(comp_job, NULL, WNOHANG) != comp_job) return;
	comp_job = 0;
	comp_have = pm_have("picom");
	if (comp_have) { compositor = 1; pm_conf_set_int("appearance.compositor", 1); }
	else snprintf(comp_msg, sizeof comp_msg, "%s", _("沒有安裝成功（可能是取消了輸入密碼）"));
	pm_ui_redraw(u);
}

/* 多桌面（workspace.*，pomelo-compositor 與面板讀）：桌面數量（2～10）、切換動畫、Dock 與 Alt+Tab 的範圍、移動視窗是否跟過去、新視窗放哪、觸控板手勢 */
static int ws_count = 4, ws_anim = 1, ws_dock_all, ws_alt_all, ws_follow = 1, ws_newwin, ws_gesture = 1;

static void load_values(void)
{
	dark = pm_conf_int("appearance.dark", 0);
	compositor = pm_conf_int("appearance.compositor", 0);
	unsigned ac = pm_conf_color("appearance.accent", accents[0]);
	accent = 0;
	for (unsigned i = 0; i < LEN(accents); i++) if (accents[i] == ac) accent = (int)i;
	double fs = pm_conf_num("appearance.font_size", 13);
	fsize = fs < 12.5 ? 0 : fs < 14 ? 1 : 2;
	const char *it = pm_conf_get("appearance.icon_theme", "Yaru");
	icontheme = 0;
	for (int i = 0; i < nicon_themes; i++) if (!strcmp(icon_themes[i], it)) icontheme = i;
	const char *w = pm_conf_get("desktop.wallpaper", "gradient");
	wp = -1;
	for (int i = 0; i < nwp; i++) if (!strcmp(wp_path[i], w)) wp = i;
	wpmode = CLAMP(pm_conf_int("desktop.wallpaper_mode", 0), 0, 2);
	dock_show = pm_conf_int("dock.show", 1);
	dock_hide = pm_conf_int("dock.autohide", 0);
	dock_size = pm_conf_int("dock.size", 48);
	btn_side = pm_conf_int("appearance.buttons_right", 1);
	const char *cf = pm_conf_get("bar.clock_format", _(clock_fmts[0]));
	clockfmt = 0;
	for (unsigned i = 0; i < LEN(clock_fmts); i++) if (!strcmp(_(clock_fmts[i]), cf)) clockfmt = (int)i;
	ws_count = CLAMP(pm_conf_int("workspace.count", 4), 2, 10);
	ws_anim = pm_conf_int("workspace.animation", 1);
	ws_dock_all = pm_conf_int("workspace.dock_all", 0);
	ws_alt_all = pm_conf_int("workspace.alt_tab_all", 0);
	ws_follow = pm_conf_int("workspace.move_follow", 1);
	ws_newwin = CLAMP(pm_conf_int("workspace.new_window", 0), 0, 1);
	ws_gesture = pm_conf_int("workspace.gesture", 1);
	const char *tt = pm_conf_get("terminal.theme", "system");
	termtheme = 0;
	for (unsigned i = 0; i < LEN(term_keys); i++) if (!strcmp(term_keys[i], tt)) termtheme = (int)i;
}

/* --page：只顯示其中一段（系統設定的「外觀」「背景圖片」「桌面與 Dock」「選單列」各開一段） */
enum { S_ALL, S_APPEARANCE, S_WALLPAPER, S_DOCK, S_MENUBAR, S_WORKSPACE };
static int section = S_ALL;
static int bar_batt, bar_media;

/* ------------------------------------------------------------------ */
/* 左上角的 Pomelo 選單：清單 → 點一項進去改名稱、移動、移除；「加入…」選程式、指令、內建項目或分隔線 */
/* ------------------------------------------------------------------ */

static PmMenuEntry me[PM_MENU_MAX];
static int nme = -1, me_sel = -1, me_adding, add_kind;
static char add_q[64], add_name[128], add_cmd[512], me_status[160];

static void me_save(void)
{
	snprintf(me_status, sizeof me_status, "%s", pm_menu_save(me, nme) ? _("無法存檔") : "");
}

static void me_open(PmUI *u, int sel, int adding)
{
	me_sel = sel;
	me_adding = adding;
	u->scroll = 0;
	u->again = 1;
}

static void me_append(PmUI *u, int kind, const char *arg, const char *label)
{
	if (nme >= PM_MENU_MAX) { snprintf(me_status, sizeof me_status, "%s", _("選單已經滿了")); return; }
	PmMenuEntry *e = &me[nme++];
	memset(e, 0, sizeof *e);
	e->kind = kind;
	snprintf(e->arg, sizeof e->arg, "%s", arg ? arg : "");
	snprintf(e->label, sizeof e->label, "%s", label ? label : "");
	me_save();
	me_open(u, nme - 1, 0);   /* 直接進到新項目，可以馬上調整位置 */
	snprintf(me_status, sizeof me_status, "%s", _("已加入選單最後面；可以用「上移」調整位置"));
}

static const char *me_kind_name(const PmMenuEntry *e)
{
	static const char *k[] = { N_("內建項目"), N_("程式"), N_("指令"), N_("分隔線") };
	return _(k[e->kind]);
}

static void build_menu_list(PmUI *u)
{
	if (nme < 0) nme = pm_menu_load(me, PM_MENU_MAX);
	pm_ui_group(u, _("左上角的 Pomelo 選單"));
	for (int i = 0; i < nme; i++) {
		const char *sub = me[i].kind == PM_MENU_CMD ? me[i].arg : me[i].kind == PM_MENU_APP ? me[i].arg : NULL;
		const char *title = me[i].kind == PM_MENU_SEP ? _("──── 分隔線 ────") : pm_menu_label(&me[i]);
		if (pm_ui_item(u, NULL, title, sub, "›", 0)) me_open(u, i, 0);
	}
	if (pm_ui_button(u, _("加入程式、指令或分隔線"), _("加入…"))) {
		add_q[0] = add_name[0] = add_cmd[0] = 0;
		me_status[0] = 0;
		me_open(u, -1, 1);
	}
	if (pm_menu_customized() && pm_ui_button(u, _("回到預設的選單"), _("還原"))) {
		pm_menu_reset();
		nme = pm_menu_load(me, PM_MENU_MAX);
		u->again = 1;   /* 上面的清單是舊的，重畫一次 */
	}
	pm_ui_end(u);
	pm_ui_note(u, _("點一個項目可以改名稱、上下移動或移除；下次點開左上角的選單就會生效。在左上角的圖示上按右鍵也能來到這裡。"));
}

static void build_menu_edit(PmUI *u)
{
	if (pm_ui_back(u, _("選單列")) || me_sel >= nme) { me_open(u, -1, 0); me_status[0] = 0; return; }
	PmMenuEntry *e = &me[me_sel];
	pm_ui_title(u, e->kind == PM_MENU_SEP ? _("分隔線") : pm_menu_label(e), me_kind_name(e));
	if (e->kind != PM_MENU_SEP) {
		PmMenuEntry before = *e;
		pm_ui_group(u, NULL);
		pm_ui_entry(u, _("名稱"), e->label, sizeof e->label, 0);
		if (e->kind == PM_MENU_CMD) pm_ui_entry(u, _("指令"), e->arg, sizeof e->arg, 0);
		pm_ui_end(u);
		if (memcmp(&before, e, sizeof before)) me_save();   /* 打字就存，不用按儲存 */
		char note[400];
		if (e->kind == PM_MENU_CMD)
			pm_ui_note(u, _("指令用 sh 執行，例如 pomelo-open https://www.cwa.gov.tw 或 ~/bin/備份.sh。"));
		else {
			PmMenuEntry d = *e;
			d.label[0] = 0;
			snprintf(note, sizeof note, _("名稱留空就用原本的名稱：%s"), pm_menu_label(&d));
			pm_ui_note(u, note);
		}
	}
	pm_ui_group(u, NULL);
	if (me_sel > 0 && pm_ui_button(u, _("往上移一格"), _("上移"))) {
		PmMenuEntry t = me[me_sel]; me[me_sel] = me[me_sel - 1]; me[me_sel - 1] = t;
		me_sel--;
		me_save();
	}
	if (me_sel < nme - 1 && pm_ui_button(u, _("往下移一格"), _("下移"))) {
		PmMenuEntry t = me[me_sel]; me[me_sel] = me[me_sel + 1]; me[me_sel + 1] = t;
		me_sel++;
		me_save();
	}
	if (pm_ui_button(u, _("從選單移除"), _("移除"))) {
		memmove(&me[me_sel], &me[me_sel + 1], (size_t)(nme - me_sel - 1) * sizeof me[0]);
		nme--;
		me_save();
		me_open(u, -1, 0);
		snprintf(me_status, sizeof me_status, "%s", _("已移除"));
		pm_ui_end(u);
		return;
	}
	pm_ui_end(u);
	/* 目前在選單裡的位置 */
	char pos[64];
	snprintf(pos, sizeof pos, _("第 %d 項，共 %d 項"), me_sel + 1, nme);
	pm_ui_note(u, me_status[0] ? me_status : pos);
}

static void build_menu_add(PmUI *u)
{
	if (pm_ui_back(u, _("選單列"))) { me_open(u, -1, 0); return; }
	pm_ui_title(u, _("加入選單項目"), NULL);
	const char *kinds[] = { _("程式"), _("指令"), _("內建項目"), _("分隔線") };
	pm_ui_group(u, NULL);
	pm_ui_segment(u, _("種類"), kinds, 4, &add_kind);
	if (add_kind == 0) pm_ui_entry(u, _("搜尋"), add_q, sizeof add_q, 0);
	if (add_kind == 1) {
		pm_ui_entry(u, _("名稱"), add_name, sizeof add_name, 0);
		pm_ui_entry(u, _("指令"), add_cmd, sizeof add_cmd, 0);
		if (add_name[0] && add_cmd[0] && pm_ui_button(u, _("加到選單"), _("加入"))) { me_append(u, PM_MENU_CMD, add_cmd, add_name); pm_ui_end(u); return; }
	}
	if (add_kind == 3 && pm_ui_button(u, _("在選單最後加一條分隔線"), _("加入"))) { me_append(u, PM_MENU_SEP, NULL, NULL); pm_ui_end(u); return; }
	pm_ui_end(u);
	if (add_kind == 0) {
		int n, shown = 0;
		PmApp *v = pm_apps(&n);
		pm_ui_group(u, NULL);
		for (int i = 0; i < n && shown < 80; i++) {
			if (v[i].hidden || (add_q[0] && !pm_app_match(&v[i], add_q))) continue;
			shown++;
			if (pm_ui_item(u, v[i].icon ? v[i].icon : "application-x-executable", v[i].name, v[i].comment, NULL, 0)) {
				me_append(u, PM_MENU_APP, v[i].id, NULL);
				pm_ui_end(u);
				return;
			}
		}
		if (!shown) pm_ui_info(u, _("沒有符合的程式"), NULL);
		pm_ui_end(u);
	} else if (add_kind == 1) {
		pm_ui_note(u, _("例如名稱「看氣象」、指令 pomelo-open https://www.cwa.gov.tw；指令用 sh 執行。"));
	} else if (add_kind == 2) {
		pm_ui_group(u, NULL);
		for (int i = 0; i < pm_menu_nbuiltins; i++) {
			int have = 0;
			for (int j = 0; j < nme; j++) if (me[j].kind == PM_MENU_ITEM && !strcmp(me[j].arg, pm_menu_builtins[i].id)) have = 1;
			if (pm_ui_item(u, NULL, _(pm_menu_builtins[i].label), NULL, have ? _("已在選單裡") : NULL, 0)) {
				me_append(u, PM_MENU_ITEM, pm_menu_builtins[i].id, NULL);
				pm_ui_end(u);
				return;
			}
		}
		pm_ui_end(u);
	}
}

static void build(PmUI *u)
{
	char buf[64];
	if (section == S_MENUBAR && me_adding) { build_menu_add(u); return; }
	if (section == S_MENUBAR && me_sel >= 0) { build_menu_edit(u); return; }
	static int seen;
	if (seen != pm_conf_serial()) { seen = pm_conf_serial(); if (!dock_dirty) load_values(); }
	static const char *titles[] = { N_("外觀"), N_("外觀"), N_("背景圖片"), N_("桌面與 Dock"), N_("選單列"), N_("多桌面") };
	pm_ui_title(u, _(titles[section]), section == S_ALL ? _("調整整個 Pomelo 桌面的顏色、字型與桌布") : NULL);

	if (section == S_ALL || section == S_APPEARANCE) {
		pm_ui_group(u, section == S_ALL ? _("外觀") : NULL);
		if (pm_ui_segment(u, _("外觀模式"), modes, 2, &dark)) pm_conf_set_int("appearance.dark", dark);
		if (pm_ui_swatches(u, _("強調色"), accents, LEN(accents), &accent)) {
			snprintf(buf, sizeof buf, "#%06x", accents[accent]);
			pm_conf_set("appearance.accent", buf);
		}
		if (pm_ui_segment(u, _("文字大小"), sizes, 3, &fsize)) {
			snprintf(buf, sizeof buf, "%g", size_val[fsize]);
			pm_conf_set("appearance.font_size", buf);
		}
		if (nicon_themes && pm_ui_choice(u, _("圖示主題"), (const char **)icon_themes, nicon_themes, &icontheme))
			pm_conf_set("appearance.icon_theme", icon_themes[icontheme]);
		const char *sides[] = { _("左邊"), _("右邊") };
		if (pm_ui_segment(u, _("視窗按鈕的位置"), sides, 2, &btn_side)) pm_conf_set_int("appearance.buttons_right", btn_side);
		/* pomelo-compositor 自己畫陰影與淡入淡出、一直開著，這個開關跟 picom 只給舊的 pomelo-wm 用 */
		if (pm_under_compositor()) pm_ui_info(u, _("視窗陰影與動畫"), _("內建"));
		else if (comp_have) {
			if (pm_ui_switch(u, _("視窗陰影與動畫"), &compositor)) pm_conf_set_int("appearance.compositor", compositor);
		} else if (comp_job > 0) pm_ui_info(u, _("視窗陰影與動畫"), _("正在安裝 picom…"));
		else if (!pm_pkg_name("picom")) pm_ui_info(u, _("視窗陰影與動畫"), _("這個系統沒有 picom 套件"));
		else if (pm_ui_button(u, _("視窗陰影與動畫（要先安裝 picom）"), _("安裝…"))) comp_install();
		pm_ui_end(u);
		if (pm_under_compositor()) pm_ui_note(u, _("陰影、淡入淡出與視窗圓角由 Pomelo 的畫面核心直接畫，不用另外安裝 picom。"));
		else if (comp_msg[0]) pm_ui_note(u, comp_msg);
		else if (compositor && comp_have) pm_ui_note(u, _("陰影與淡入淡出由 picom 畫。比較舊的電腦或虛擬機如果覺得變慢，可以關掉。"));
		pm_ui_group(u, _("終端機"));
		if (pm_ui_choice(u, _("配色"), term_themes, 3, &termtheme)) pm_conf_set("terminal.theme", term_keys[termtheme]);
		pm_ui_end(u);
	}

	if (section == S_ALL || section == S_WALLPAPER) {
		pm_ui_group(u, section == S_ALL ? _("桌布") : NULL);
		if (pm_ui_segment(u, _("顯示方式"), fits, 3, &wpmode)) pm_conf_set_int("desktop.wallpaper_mode", wpmode);
		pm_ui_end(u);
		if (pm_ui_images(u, wp_thumb, wp_name, nwp, &wp, 150, 94)) pm_conf_set("desktop.wallpaper", wp_path[wp]);
		pm_ui_space(u, 10);
		if (section == S_WALLPAPER)
			pm_ui_note(u, _("清單包含 /usr/share/backgrounds 與「圖片」資料夾裡的圖片；「內建漸層」會跟著深色模式換顏色。"));
	}

	if (section == S_ALL || section == S_DOCK) {
		pm_ui_group(u, section == S_ALL ? _("Dock") : NULL);
		if (pm_ui_switch(u, _("顯示 Dock"), &dock_show)) pm_conf_set_int("dock.show", dock_show);
		if (pm_ui_switch(u, _("自動隱藏 Dock"), &dock_hide)) pm_conf_set_int("dock.autohide", dock_hide);
		if (pm_ui_slider(u, _("Dock 大小"), &dock_size, 32, 80)) dock_dirty = 1;
		/* 拖曳時只更新畫面，放開才寫入設定（避免面板一直重新載入圖示） */
		if (dock_dirty && !u->down) { pm_conf_set_int("dock.size", (int)(dock_size + 0.5)); dock_dirty = 0; }
		pm_ui_end(u);
		if (section == S_DOCK)
			pm_ui_note(u, _("在 Dock 或啟動器的圖示上按右鍵，可以把程式保留在 Dock 或移除。"));
	}

	if (section == S_ALL || section == S_MENUBAR) {
		static char examples[LEN(clock_fmts)][64];
		static const char *ex_ptr[LEN(clock_fmts)];
		time_t now = time(NULL);
		for (unsigned i = 0; i < LEN(clock_fmts); i++) {
			strftime(examples[i], sizeof examples[i], _(clock_fmts[i]), localtime(&now));
			ex_ptr[i] = examples[i];
		}
		pm_ui_group(u, section == S_ALL ? _("選單列") : NULL);
		if (pm_ui_choice(u, _("時鐘格式"), ex_ptr, LEN(clock_fmts), &clockfmt)) pm_conf_set("bar.clock_format", _(clock_fmts[clockfmt]));
		if (pm_ui_switch(u, _("顯示電量百分比"), &bar_batt)) pm_conf_set_int("bar.battery_percent", bar_batt);
		if (pm_ui_switch(u, _("顯示外接裝置圖示"), &bar_media)) pm_conf_set_int("bar.show_media", bar_media);
		pm_ui_end(u);
		if (section == S_MENUBAR) build_menu_list(u);
	}
	if (section == S_ALL || section == S_WORKSPACE) {
		static const char *counts[] = { "2", "3", "4", "5", "6", "7", "8", "9", "10" };
		static const char *foll[] = { N_("留在原本的桌面"), N_("跟著視窗過去") };
		static const char *newwin[] = { N_("目前的桌面"), N_("同一個程式的視窗所在的桌面") };
		static const char *fl[2], *nw[2];
		for (int i = 0; i < 2; i++) { fl[i] = _(foll[i]); nw[i] = _(newwin[i]); }
		int ci = ws_count - 2;
		pm_ui_group(u, section == S_ALL ? _("多桌面") : NULL);
		if (pm_ui_choice(u, _("桌面數量"), counts, 9, &ci)) { ws_count = ci + 2; pm_conf_set_int("workspace.count", ws_count); }
		if (pm_ui_switch(u, _("切換桌面時的滑動動畫"), &ws_anim)) pm_conf_set_int("workspace.animation", ws_anim);
		if (pm_ui_switch(u, _("觸控板三指或四指左右滑切換桌面"), &ws_gesture)) pm_conf_set_int("workspace.gesture", ws_gesture);
		if (pm_ui_choice(u, _("新開的視窗出現在"), nw, 2, &ws_newwin)) pm_conf_set_int("workspace.new_window", ws_newwin);
		int fi = ws_follow ? 1 : 0;
		if (pm_ui_segment(u, _("把視窗移到別的桌面（Super+Alt+數字）後"), fl, 2, &fi)) { ws_follow = fi; pm_conf_set_int("workspace.move_follow", ws_follow); }
		if (pm_ui_switch(u, _("Dock 顯示所有桌面的視窗"), &ws_dock_all)) pm_conf_set_int("workspace.dock_all", ws_dock_all);
		if (pm_ui_switch(u, _("Alt+Tab 也切換到其他桌面的視窗"), &ws_alt_all)) pm_conf_set_int("workspace.alt_tab_all", ws_alt_all);
		pm_ui_end(u);
		if (section == S_WORKSPACE)
			pm_ui_note(u, _("Ctrl+Alt+←／→ 切換桌面，Super+1～9 直接跳到某個桌面，Super+Alt+1～9 把目前的視窗移過去，Super+Tab 開桌面總覽。視窗標題列按右鍵可以「移到桌面」或「在所有桌面顯示」。"));
	}
	if (section == S_ALL)
		pm_ui_note(u, _("設定存在 ~/.config/pomelo/settings.conf，修改後所有 Pomelo 程式會立即套用。"));
}

/* 程式圖示：調色盤 */
static cairo_surface_t *icon(int size)
{
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
	cairo_t *cr = cairo_create(s);
	cairo_scale(cr, size / 128.0, size / 128.0);
	pm_rrect(cr, 10, 10, 108, 108, 26, 26);
	cairo_pattern_t *g = cairo_pattern_create_linear(0, 10, 0, 118);
	cairo_pattern_add_color_stop_rgb(g, 0, 0.98, 0.98, 1.0);
	cairo_pattern_add_color_stop_rgb(g, 1, 0.86, 0.87, 0.90);
	cairo_set_source(cr, g);
	cairo_fill(cr);
	cairo_pattern_destroy(g);
	static const unsigned cols[] = { 0xff5257, 0xf7821b, 0xffc600, 0x62ba46, 0x007aff, 0xa550a7 };
	for (int i = 0; i < 6; i++) {
		double a = i * M_PI / 3 - M_PI / 2;
		cairo_arc(cr, 64 + cos(a) * 28, 64 + sin(a) * 28, 13, 0, 2 * M_PI);
		pm_rgb(cr, cols[i]);
		cairo_fill(cr);
	}
	cairo_arc(cr, 64, 64, 12, 0, 2 * M_PI);
	pm_rgb(cr, 0xffffff);
	cairo_fill(cr);
	cairo_destroy(cr);
	return s;
}

int main(int argc, char *argv[])
{
	if (argc > 2 && !strcmp(argv[1], "--icon")) return pm_ui_write_icon(icon, argv[2]);
	pm_conf_load();
	static const char *names[] = { "", "appearance", "wallpaper", "dock", "menubar", "workspace" };
	for (int i = 1; i + 1 < argc; i++)
		if (!strcmp(argv[i], "--page"))
			for (unsigned k = 1; k < LEN(names); k++) if (!strcmp(argv[i + 1], names[k])) section = (int)k;
	scan_icon_themes();
	if (section == S_ALL || section == S_WALLPAPER) scan_wallpapers();
	load_values();
	bar_batt = pm_conf_int("bar.battery_percent", 1);
	comp_have = pm_have("picom");
	bar_media = pm_conf_int("bar.show_media", 1);
	static const char *wtitles[] = { N_("外觀"), N_("外觀"), N_("背景圖片"), N_("桌面與 Dock"), N_("選單列"), N_("多桌面") };
	int h = section == S_DOCK ? 360 : section == S_MENUBAR ? 640 : section == S_APPEARANCE ? 520 : section == S_WORKSPACE ? 560 : 760;
	pm_ui_native = 1;   /* build() 裡沒有直接呼叫 X：Wayland 工作階段直接當原生程式 */
	return pm_ui_run(_(wtitles[section]), "pomelo-theme", 720, h, build, tick, 500, icon);
}
