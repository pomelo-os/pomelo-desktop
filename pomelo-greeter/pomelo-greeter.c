/* SPDX-License-Identifier: GPL-3.0-or-later */
/* pomelo-greeter — Pomelo 登入畫面（macOS 風格）
 *
 * 由 pomelo-dm 以沒有權限的系統帳號 pomelo-greeter 啟動，只負責畫面：
 * 上方是日期與大時鐘，下方是使用者頭像、名稱與密碼框，最下面是睡眠、重新開機、關機；
 * 背景是使用者上次用的桌布（pomelo-dm 在登入時幫忙存一份）。
 * 帳號密碼經由 fd 3（pomelo-dm 給的 socket）送出，驗證由 pomelo-dm 做；這裡碰不到任何密碼檔。
 *
 * 操作：直接打密碼按 Enter；沒打字時 ← → 或點頭像切換使用者；Esc 清除。
 *
 * 單獨執行（沒有 pomelo-dm，例如看畫面）：pomelo-greeter --preview，密碼 demo 會「登入成功」。
 */
#define _GNU_SOURCE
#include <pomelo.h>
#include <pwd.h>
#include <dirent.h>
#include <poll.h>
#include <sys/socket.h>
#include <X11/XKBlib.h>
#include <pomelo-wl.h>

#define MAXU 32

typedef struct {
	char name[64], full[128];
	cairo_surface_t *face;      /* 頭像（圓形裁切前的正方形），可能是 NULL */
	cairo_surface_t *bg;        /* 這位使用者的桌布（已縮放、加上暗角），用到才載入 */
	int dark;
} User;

static User users[MAXU];
static int nusers, cur, other_mode;   /* other_mode：手動輸入使用者名稱 */
static char sessions[16][64], session_names[16][128];
static int nsessions, cur_session;
static char pw[256], other_name[64];
static int pwlen, other_focus_pw;
static int dm_fd = -1, preview, busy, capslock;
static long fail_at, ok_at, anim_until;
static char fail_msg[256], info_msg[256];
static long info_at;
static Window win;
/* 原生 Wayland（layer-shell）模式：每個螢幕一張 overlay 層的 layer 視窗，主要顯示器那張獨佔鍵盤；沒有 X 連線 */
static int native;
static void wl_draw(void);
static int wl_main(void);
static PmWl *wl_primary(void);
static cairo_surface_t *default_bg;
static const char *state_dir, *sessions_dir;
static char last_user[64];

/* 各個可以點的區域 */
typedef struct { double x, y, w, h; } Box;
static Box box_users[MAXU + 1], box_field, box_go, box_power[3], box_session, box_other_name;

/* ------------------------------------------------------------------ */
/* 資料                                                                */
/* ------------------------------------------------------------------ */

static void state_get(const char *key, char *out, size_t n)
{
	out[0] = 0;
	char path[512], line[512];
	snprintf(path, sizeof path, "%s/state", state_dir);
	FILE *f = fopen(path, "r");
	size_t kl = strlen(key);
	while (f && fgets(line, sizeof line, f))
		if (!strncmp(line, key, kl) && line[kl] == '=') {
			line[strcspn(line, "\n")] = 0;
			snprintf(out, n, "%s", line + kl + 1);
		}
	if (f) fclose(f);
}

static char conf_hide[512];
static int conf_show_other = -1;
/* 外觀（/etc/pomelo/dm.conf，系統設定「登入畫面」改） */
static int conf_user_list = 1, conf_power = 1, conf_clock, conf_large, conf_bg;   /* clock：0 24h 1 12h 2 不顯示；bg：0 使用者 1 指定圖片 2 漸層 */
static char conf_msg[320];

static void load_conf(void)
{
	const char *p = getenv("POMELO_DM_CONF");
	FILE *f = fopen(p ? p : "/etc/pomelo/dm.conf", "r");
	char line[640];
	while (f && fgets(line, sizeof line, f)) {
		line[strcspn(line, "\n")] = 0;
		if (!strncmp(line, "hide-users=", 11)) snprintf(conf_hide, sizeof conf_hide, ",%s,", line + 11);
		if (!strncmp(line, "show-other-user=", 16)) conf_show_other = !strcmp(line + 16, "true") || !strcmp(line + 16, "1");
		if (!strncmp(line, "user-list=", 10)) conf_user_list = strcmp(line + 10, "false") != 0;
		if (!strncmp(line, "power-buttons=", 14)) conf_power = strcmp(line + 14, "false") != 0;
		if (!strncmp(line, "clock=", 6)) conf_clock = !strcmp(line + 6, "12h") ? 1 : !strcmp(line + 6, "off") ? 2 : 0;
		if (!strncmp(line, "text-size=", 10)) conf_large = !strcmp(line + 10, "large");
		if (!strncmp(line, "background=", 11)) conf_bg = !strcmp(line + 11, "image") ? 1 : !strcmp(line + 11, "gradient") ? 2 : 0;
		if (!strncmp(line, "message=", 8)) snprintf(conf_msg, sizeof conf_msg, "%s", line + 8);
	}
	if (f) fclose(f);
}

static int hidden_user(const char *name)
{
	char k[80];
	snprintf(k, sizeof k, ",%s,", name);
	return conf_hide[0] && strstr(conf_hide, k);
}

static int safe_mode;   /* pomelo-dm 發現登入畫面一直當掉：不載入使用者的圖片 */
static int users_truncated;   /* 帳號太多，有些沒列出來：一定要提供「其他使用者」 */

static cairo_surface_t *load_face(const char *name)
{
	if (safe_mode) return NULL;
	char p[600];
	snprintf(p, sizeof p, "%s/users/%s.face", state_dir, name);
	cairo_surface_t *s = pm_image_load_fit(p, 192, 192, 0);
	if (!s) {
		snprintf(p, sizeof p, "/var/lib/AccountsService/icons/%s", name);
		s = pm_image_load_fit(p, 192, 192, 0);
	}
	return s;
}

static int cmp_user(const void *a, const void *b)
{
	const User *x = a, *y = b;
	int lx = !strcmp(x->name, last_user), ly = !strcmp(y->name, last_user);
	if (lx != ly) return ly - lx;
	return strcasecmp(x->full[0] ? x->full : x->name, y->full[0] ? y->full : y->name);
}

static void load_users(void)
{
	state_get("last-user", last_user, sizeof last_user);
	if (!conf_user_list) return;   /* 不列出使用者：只有「名稱與密碼」欄位 */
	setpwent();
	struct passwd *p;
	while ((p = getpwent())) {
		if (p->pw_uid < 1000 || p->pw_uid >= 60000) continue;
		const char *sh = p->pw_shell ? p->pw_shell : "";
		if (strstr(sh, "nologin") || strstr(sh, "/false") || !*sh) continue;
		if (hidden_user(p->pw_name)) continue;
		if (nusers >= MAXU) { users_truncated = 1; continue; }
		User *u = &users[nusers++];
		snprintf(u->name, sizeof u->name, "%s", p->pw_name);
		snprintf(u->full, sizeof u->full, "%s", p->pw_gecos ? p->pw_gecos : "");
		u->full[strcspn(u->full, ",")] = 0;
		char k[96], v[8];
		snprintf(k, sizeof k, "dark.%s", u->name);
		state_get(k, v, sizeof v);
		u->dark = v[0] == '1';
	}
	endpwent();
	qsort(users, nusers, sizeof *users, cmp_user);
	for (int i = 0; i < nusers; i++) users[i].face = load_face(users[i].name);
	if (getenv("POMELO_DM_TEST") && nusers > 1) {
		/* 測試時只留目前的使用者在最前面也沒關係，保持原樣 */
	}
}

/* 工作階段：/usr/share/xsessions 裡的 .desktop */
static void load_sessions(void)
{
	DIR *d = opendir(sessions_dir);
	struct dirent *e;
	const char *lang = getenv("LANG");
	char lname[32] = "", lshort[32] = "";
	if (lang) {
		snprintf(lname, sizeof lname, "Name[%.*s]=", (int)strcspn(lang, ".@"), lang);
		snprintf(lshort, sizeof lshort, "Name[%.*s]=", (int)strcspn(lang, "_.@"), lang);
	}
	while (d && (e = readdir(d)) && nsessions < 16) {
		size_t l = strlen(e->d_name);
		if (l < 9 || strcmp(e->d_name + l - 8, ".desktop") || e->d_name[0] == '.') continue;
		char path[600], line[512], name[128] = "", lname_v[128] = "", try_exec[256] = "";
		snprintf(path, sizeof path, "%s/%s", sessions_dir, e->d_name);
		FILE *f = fopen(path, "r");
		int hidden = 0;
		while (f && fgets(line, sizeof line, f)) {
			line[strcspn(line, "\n")] = 0;
			if (!strncmp(line, "Name=", 5)) snprintf(name, sizeof name, "%s", line + 5);
			else if ((lname[0] && !strncmp(line, lname, strlen(lname))) || (!lname_v[0] && lshort[0] && !strncmp(line, lshort, strlen(lshort))))
				snprintf(lname_v, sizeof lname_v, "%s", strchr(line, '=') + 1);
			else if (!strncmp(line, "TryExec=", 8)) snprintf(try_exec, sizeof try_exec, "%s", line + 8);
			else if (!strcmp(line, "Hidden=true") || !strcmp(line, "NoDisplay=true")) hidden = 1;
		}
		if (f) fclose(f);
		if (hidden || (try_exec[0] && !pm_have(try_exec))) continue;
		snprintf(sessions[nsessions], sizeof sessions[0], "%.*s", (int)(l - 8), e->d_name);
		snprintf(session_names[nsessions], sizeof session_names[0], "%s", lname_v[0] ? lname_v : name[0] ? name : sessions[nsessions]);
		nsessions++;
	}
	if (d) closedir(d);
}

static void pick_session_for_user(void)
{
	char want[64] = "";
	if (!other_mode && cur < nusers) {
		char k[96];
		snprintf(k, sizeof k, "session.%s", users[cur].name);
		state_get(k, want, sizeof want);
	}
	if (!want[0]) {
		const char *d = getenv("POMELO_DM_DEFAULT_SESSION");
		snprintf(want, sizeof want, "%s", d ? d : "pomelo");
	}
	cur_session = 0;
	for (int i = 0; i < nsessions; i++) if (!strcmp(sessions[i], want)) cur_session = i;
}

/* ------------------------------------------------------------------ */
/* 背景                                                                */
/* ------------------------------------------------------------------ */

/* 內建漸層（跟桌面的預設桌布一樣） */
static void paint_gradient(cairo_t *cr, int w, int h, int dark)
{
	cairo_pattern_t *g = cairo_pattern_create_linear(0, 0, w, h);
	if (dark) {
		cairo_pattern_add_color_stop_rgb(g, 0.0, 0.07, 0.09, 0.20);
		cairo_pattern_add_color_stop_rgb(g, 0.5, 0.20, 0.13, 0.32);
		cairo_pattern_add_color_stop_rgb(g, 1.0, 0.42, 0.20, 0.26);
	} else {
		cairo_pattern_add_color_stop_rgb(g, 0.0, 0.20, 0.36, 0.66);
		cairo_pattern_add_color_stop_rgb(g, 0.5, 0.52, 0.42, 0.74);
		cairo_pattern_add_color_stop_rgb(g, 1.0, 0.98, 0.62, 0.48);
	}
	cairo_set_source(cr, g);
	cairo_paint(cr);
	cairo_pattern_destroy(g);
	cairo_pattern_t *r = cairo_pattern_create_radial(w * 0.7, h * 0.35, 0, w * 0.7, h * 0.35, w * 0.55);
	cairo_pattern_add_color_stop_rgba(r, 0, 1, 1, 1, dark ? 0.10 : 0.22);
	cairo_pattern_add_color_stop_rgba(r, 1, 1, 1, 1, 0);
	cairo_set_source(cr, r);
	cairo_paint(cr);
	cairo_pattern_destroy(r);
}

/* 桌布（或漸層）加上上下的暗角，讓白字看得清楚 */
static cairo_surface_t *make_bg(const char *name, int dark)
{
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, sw, sh);
	cairo_t *cr = cairo_create(s);
	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_paint(cr);
	char p[600] = "";
	if (conf_bg == 1 && !safe_mode) {   /* 管理員指定的圖片（預覽時可以用環境變數換成還沒存的圖） */
		const char *pv = getenv("POMELO_DM_BACKGROUND");
		snprintf(p, sizeof p, "%s", pv && *pv ? pv : "");
		if (!p[0]) snprintf(p, sizeof p, "%s/background", state_dir);
	} else if (conf_bg == 0 && name && !safe_mode) snprintf(p, sizeof p, "%s/users/%s.wallpaper", state_dir, name);
	for (int i = 0; i < nmons; i++) {
		Rect m = mons[i];
		cairo_save(cr);
		cairo_rectangle(cr, m.x, m.y, m.w, m.h);
		cairo_clip(cr);
		cairo_translate(cr, m.x, m.y);
		cairo_surface_t *img = p[0] ? pm_wallpaper_load(p, m.w, m.h, 0) : NULL;
		if (img) {
			cairo_set_source_surface(cr, img, 0, 0);
			cairo_paint(cr);
			cairo_surface_destroy(img);
		} else paint_gradient(cr, m.w, m.h, dark);
		cairo_pattern_t *v = cairo_pattern_create_linear(0, 0, 0, m.h);
		cairo_pattern_add_color_stop_rgba(v, 0.0, 0, 0, 0, 0.30);
		cairo_pattern_add_color_stop_rgba(v, 0.30, 0, 0, 0, 0.0);
		cairo_pattern_add_color_stop_rgba(v, 0.60, 0, 0, 0, 0.0);
		cairo_pattern_add_color_stop_rgba(v, 1.0, 0, 0, 0, 0.45);
		cairo_set_source(cr, v);
		cairo_paint(cr);
		cairo_pattern_destroy(v);
		cairo_restore(cr);
	}
	cairo_destroy(cr);
	if (native) return s;   /* 原生：直接留記憶體影像，每次重畫貼上（pmwl_begin 的緩衝本來就在記憶體） */
	/* 放到 X 伺服器上（pixmap）：之後每次重畫只是伺服器內部複製，不用每次傳幾 MB 的圖 */
	cairo_surface_t *wsurf = cairo_xlib_surface_create(dpy, win, DefaultVisual(dpy, scr), sw, sh);
	cairo_surface_t *srv = cairo_surface_create_similar(wsurf, CAIRO_CONTENT_COLOR, sw, sh);
	cairo_t *c2 = cairo_create(srv);
	cairo_set_source_surface(c2, s, 0, 0);
	cairo_paint(c2);
	cairo_destroy(c2);
	cairo_surface_destroy(wsurf);
	cairo_surface_destroy(s);
	return srv;
}

static cairo_surface_t *current_bg(void)
{
	if (other_mode || cur >= nusers || conf_bg != 0) {   /* 指定圖片或漸層：每個人都一樣 */
		if (!default_bg) default_bg = make_bg(NULL, 1);
		return default_bg;
	}
	User *u = &users[cur];
	if (!u->bg) u->bg = make_bg(u->name, u->dark);
	return u->bg;
}

/* ------------------------------------------------------------------ */
/* 繪製                                                                */
/* ------------------------------------------------------------------ */

static double ease(double t) { t = t < 0 ? 0 : t > 1 ? 1 : t; return 1 - (1 - t) * (1 - t) * (1 - t); }

/* 圓形頭像：有照片就裁成圓形，沒有就畫 macOS 那種灰色人像 */
static void draw_avatar(cairo_t *cr, double cx, double cy, double r, cairo_surface_t *face, double alpha)
{
	cairo_save(cr);
	cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
	cairo_clip(cr);
	if (face) {
		int fw = cairo_image_surface_get_width(face), fh = cairo_image_surface_get_height(face);
		cairo_translate(cr, cx - r, cy - r);
		cairo_scale(cr, 2 * r / fw, 2 * r / fh);
		cairo_set_source_surface(cr, face, 0, 0);
		cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
		cairo_paint_with_alpha(cr, alpha);
	} else {
		cairo_pattern_t *g = cairo_pattern_create_linear(0, cy - r, 0, cy + r);
		cairo_pattern_add_color_stop_rgba(g, 0, 0.66, 0.68, 0.72, alpha);
		cairo_pattern_add_color_stop_rgba(g, 1, 0.45, 0.47, 0.52, alpha);
		cairo_set_source(cr, g);
		cairo_paint(cr);
		cairo_pattern_destroy(g);
		cairo_set_source_rgba(cr, 1, 1, 1, 0.92 * alpha);
		cairo_arc(cr, cx, cy - r * 0.18, r * 0.36, 0, 2 * M_PI);
		cairo_fill(cr);
		cairo_arc(cr, cx, cy + r * 0.95, r * 0.70, M_PI, 2 * M_PI);
		cairo_fill(cr);
	}
	cairo_restore(cr);
	cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
	cairo_set_source_rgba(cr, 1, 1, 1, 0.35 * alpha);
	cairo_set_line_width(cr, 1);
	cairo_stroke(cr);
}

/* 電源圖示：0 睡眠（月亮）、1 重新開機（圓箭頭）、2 關機（電源符號） */
static void draw_power_icon(cairo_t *cr, int kind, double cx, double cy, double s)
{
	cairo_set_line_width(cr, 2);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	if (kind == 0) {
		/* 月亮：實心圓挖掉右上角的另一個圓 */
		cairo_push_group(cr);
		cairo_arc(cr, cx, cy, s, 0, 2 * M_PI);
		cairo_fill(cr);
		cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
		cairo_arc(cr, cx + s * 0.55, cy - s * 0.40, s * 0.80, 0, 2 * M_PI);
		cairo_fill(cr);
		cairo_pop_group_to_source(cr);
		cairo_paint(cr);
	} else if (kind == 1) {
		cairo_arc(cr, cx, cy, s, -M_PI * 0.35, M_PI * 1.45);
		cairo_stroke(cr);
		double ax = cx + s * cos(-M_PI * 0.35), ay = cy + s * sin(-M_PI * 0.35);
		cairo_move_to(cr, ax - s * 0.55, ay - s * 0.05);
		cairo_line_to(cr, ax + s * 0.05, ay + s * 0.05);
		cairo_line_to(cr, ax + s * 0.05, ay - s * 0.6);
		cairo_stroke(cr);
	} else {
		cairo_arc(cr, cx, cy, s, -M_PI * 0.30, M_PI * 1.30);
		cairo_stroke(cr);
		cairo_move_to(cr, cx, cy - s * 1.15);
		cairo_line_to(cr, cx, cy - s * 0.15);
		cairo_stroke(cr);
	}
}

static int battery(int *charging)
{
	for (int i = 0; i < 4; i++) {
		char p[96], b[32] = "";
		snprintf(p, sizeof p, "/sys/class/power_supply/BAT%d/capacity", i);
		FILE *f = fopen(p, "r");
		if (!f) continue;
		int cap = -1;
		if (fscanf(f, "%d", &cap) != 1) cap = -1;
		fclose(f);
		snprintf(p, sizeof p, "/sys/class/power_supply/BAT%d/status", i);
		f = fopen(p, "r");
		if (f) { if (fgets(b, sizeof b, f)) b[strcspn(b, "\n")] = 0; fclose(f); }
		*charging = !strcmp(b, "Charging") || !strcmp(b, "Full");
		return cap;
	}
	return -1;
}

static const char *display_name(int i)
{
	return users[i].full[0] ? users[i].full : users[i].name;
}

static void draw_to(cairo_t *cr)
{
	long now = pm_now_ms();
	cairo_set_source_surface(cr, current_bg(), 0, 0);
	cairo_paint(cr);

	Rect m = mons[pmon];
	double cx = m.x + m.w / 2.0, k = MIN(m.w / 1440.0, m.h / 900.0);
	if (k < 0.75) k = 0.75;
	if (k > 1.6) k = 1.6;
	if (conf_large) k *= 1.18;   /* 文字大小：大 */

	/* 日期與大時鐘 */
	char buf[160];
	time_t t = time(NULL);
	struct tm *tm = localtime(&t);
	if (conf_clock != 2) {
		strftime(buf, sizeof buf, _("%-m月%-d日 %A"), tm);
		pm_font(cr, 22 * k, 1);
		pm_rgba(cr, 0xffffff, 0.88);
		pm_text(cr, cx - pm_textw(cr, buf) / 2, m.y + m.h * 0.11, buf);
		strftime(buf, sizeof buf, conf_clock == 1 ? "%-I:%M" : "%H:%M", tm);
		pm_font(cr, 110 * k, 1);
		pm_rgba(cr, 0xffffff, 0.92);
		pm_text(cr, cx - pm_textw(cr, buf) / 2, m.y + m.h * 0.11 + 88 * k, buf);
	}

	/* 右上角：電量 */
	int charging = 0, cap = battery(&charging);
	if (cap >= 0) {
		snprintf(buf, sizeof buf, "%d%%%s", cap, charging ? " ⚡" : "");
		pm_font(cr, 14 * k, 0);
		pm_rgba(cr, 0xffffff, 0.85);
		pm_text(cr, m.x + m.w - 24 * k - pm_textw(cr, buf), m.y + 22 * k, buf);
	}

	/* 使用者（一排頭像，選到的比較大） */
	double base_y = m.y + m.h * 0.66;
	int n = nusers + (conf_show_other == 1 || nusers == 0 ? 1 : 0);
	double big = 52 * k, small = 30 * k, gap = 34 * k;
	int sel = other_mode ? nusers : cur;
	/* 選到的使用者在正中間，其他人依序排在左右 */
	double xs[MAXU + 1];
	xs[sel] = cx;
	for (int i = sel - 1; i >= 0; i--) xs[i] = xs[i + 1] - ((i + 1 == sel ? big : small) + small + gap);
	for (int i = sel + 1; i < n; i++) xs[i] = xs[i - 1] + ((i - 1 == sel ? big : small) + small + gap);
	for (int i = 0; i < n; i++) {
		double r = i == sel ? big : small;
		double ux = xs[i], uy = base_y - (i == sel ? 0 : 10 * k);
		double alpha = i == sel ? 1.0 : 0.65;
		/* 密碼錯誤時整個選到的使用者左右晃一下 */
		if (i == sel && fail_at && now - fail_at < 450) ux += sin((now - fail_at) / 450.0 * M_PI * 6) * 10 * k * (1 - (now - fail_at) / 450.0);
		draw_avatar(cr, ux, uy, r, i < nusers ? users[i].face : NULL, alpha);
		box_users[i] = (Box){ ux - r, uy - r, 2 * r, 2 * r };
		if (i != sel && n > 1) {
			const char *nm = i < nusers ? display_name(i) : _("其他使用者");
			char el[128];
			pm_font(cr, 12 * k, 0);
			pm_ellipsize(cr, nm, 2 * r + gap - 6, el, sizeof el);
			pm_rgba(cr, 0xffffff, 0.75);
			pm_text(cr, ux - pm_textw(cr, el) / 2, uy + r + 14 * k, el);
		}
	}

	/* 選到的使用者：名稱與密碼框 */
	double sx = box_users[sel].x + box_users[sel].w / 2;
	if (sel < n) {
		const char *nm = other_mode || sel >= nusers ? _("其他使用者") : display_name(sel);
		pm_font(cr, 20 * k, 1);
		pm_rgb(cr, 0xffffff);
		pm_text(cr, sx - pm_textw(cr, nm) / 2, base_y + big + 26 * k, nm);
	}
	double fw = 230 * k, fh = 32 * k, fy = base_y + big + 48 * k;
	if (fail_at && now - fail_at < 450) sx += sin((now - fail_at) / 450.0 * M_PI * 6) * 10 * k * (1 - (now - fail_at) / 450.0);
	box_other_name = (Box){ 0, 0, 0, 0 };
	if (other_mode) {
		/* 其他使用者：先輸入名稱 */
		Box b = { sx - fw / 2, fy, fw, fh };
		box_other_name = b;
		pm_rrect(cr, b.x, b.y, b.w, b.h, fh / 2, fh / 2);
		pm_rgba(cr, 0xffffff, 0.22);
		cairo_fill_preserve(cr);
		pm_rgba(cr, 0xffffff, !other_focus_pw ? 0.75 : 0.30);
		cairo_set_line_width(cr, 1.2);
		cairo_stroke(cr);
		pm_font(cr, 14 * k, 0);
		if (other_name[0]) { pm_rgb(cr, 0xffffff); pm_text(cr, b.x + 14 * k, b.y + fh / 2, other_name); }
		else { pm_rgba(cr, 0xffffff, 0.6); pm_text(cr, b.x + 14 * k, b.y + fh / 2, _("使用者名稱")); }
		fy += fh + 10 * k;
	}
	box_field = (Box){ sx - fw / 2, fy, fw, fh };
	pm_rrect(cr, box_field.x, box_field.y, fw, fh, fh / 2, fh / 2);
	int failed = fail_at && now - fail_at < 4000 && !pwlen;
	pm_rgba(cr, 0xffffff, 0.22);
	cairo_fill_preserve(cr);
	pm_rgba(cr, failed ? 0xff6159 : 0xffffff, failed ? 0.85 : (!other_mode || other_focus_pw) ? 0.55 : 0.30);
	cairo_set_line_width(cr, 1.2);
	cairo_stroke(cr);
	/* 右邊的 → 按鈕（有打字時才出現）／驗證中轉圈 */
	double gr = fh / 2 - 4 * k, gx = box_field.x + fw - fh / 2, gy = box_field.y + fh / 2;
	box_go = (Box){ gx - gr, gy - gr, 2 * gr, 2 * gr };
	if (busy) {
		cairo_set_line_width(cr, 2);
		pm_rgba(cr, 0xffffff, 0.9);
		double a0 = now / 140.0;
		cairo_arc(cr, gx, gy, gr * 0.7, a0, a0 + M_PI * 1.4);
		cairo_stroke(cr);
	} else if (pwlen) {
		cairo_arc(cr, gx, gy, gr, 0, 2 * M_PI);
		pm_rgba(cr, 0xffffff, 0.85);
		cairo_fill(cr);
		pm_rgba(cr, 0x000000, 0.65);
		cairo_set_line_width(cr, 1.8 * k);
		cairo_move_to(cr, gx - gr * 0.45, gy);
		cairo_line_to(cr, gx + gr * 0.45, gy);
		cairo_move_to(cr, gx + gr * 0.05, gy - gr * 0.42);
		cairo_line_to(cr, gx + gr * 0.47, gy);
		cairo_line_to(cr, gx + gr * 0.05, gy + gr * 0.42);
		cairo_stroke(cr);
	}
	if (pwlen) {
		int cnt = 0;
		for (int i = 0; i < pwlen; i++) if (((unsigned char)pw[i] & 0xc0) != 0x80) cnt++;
		cnt = MIN(cnt, 18);
		double dx = 11 * k, start = box_field.x + 16 * k;
		pm_rgb(cr, 0xffffff);
		for (int i = 0; i < cnt; i++) {
			cairo_arc(cr, start + i * dx, box_field.y + fh / 2, 3.6 * k, 0, 2 * M_PI);
			cairo_fill(cr);
		}
	} else {
		pm_font(cr, 14 * k, 0);
		pm_rgba(cr, 0xffffff, 0.6);
		pm_text(cr, box_field.x + 16 * k, box_field.y + fh / 2, busy ? _("驗證中…") : _("輸入密碼"));
	}
	if (capslock && !busy) {
		pm_font(cr, 13 * k, 1);
		pm_rgba(cr, 0xffffff, 0.8);
		pm_text(cr, box_field.x + fw - fh - 10 * k, box_field.y + fh / 2, "⇪");
	}
	int info = info_at && now - info_at < 6000 && info_msg[0];
	const char *hint = ok_at ? _("登入中…") : failed ? (fail_msg[0] ? fail_msg : _("密碼錯誤，請再試一次")) :
	                   info ? info_msg : capslock && !busy ? _("大寫鎖定已開啟") : NULL;
	if (hint) {
		char el[300];
		pm_font(cr, 13 * k, 0);
		pm_ellipsize(cr, hint, m.w * 0.6, el, sizeof el);
		pm_rgba(cr, failed ? 0xffb3ad : 0xffffff, 0.92);
		pm_text(cr, cx - pm_textw(cr, el) / 2, box_field.y + fh + 22 * k, el);
	}

	/* 最下面：睡眠、重新開機、關機（可以在設定裡關掉） */
	const char *plabels[3] = { _("睡眠"), _("重新開機"), _("關機") };
	double pr = 20 * k, pg = 96 * k, py = m.y + m.h - 70 * k;
	/* 管理員設定的訊息：放在按鈕上方，墊一層半透明底色，任何桌布上都看得清楚 */
	if (conf_msg[0]) {
		char el[400];
		pm_font(cr, 14 * k, 0);
		pm_ellipsize(cr, conf_msg, m.w * 0.7, el, sizeof el);
		double tw = pm_textw(cr, el), my = conf_power ? py - pr - 40 * k : m.y + m.h - 56 * k;
		/* 下面放不下（例如「名稱與密碼」有兩個輸入框、螢幕又不高）：改放到時鐘下方，免得蓋住輸入框或它下面的提示 */
		if (my - 15 * k < box_field.y + box_field.h + 40 * k)
			my = conf_clock != 2 ? m.y + m.h * 0.11 + 88 * k + 90 * k : m.y + m.h * 0.11 + 40 * k;
		pm_rrect(cr, cx - tw / 2 - 16 * k, my - 15 * k, tw + 32 * k, 30 * k, 15 * k, 15 * k);
		pm_rgba(cr, 0x000000, 0.35);
		cairo_fill(cr);
		pm_rgba(cr, 0xffffff, 0.95);
		pm_text(cr, cx - tw / 2, my, el);
	}
	for (int i = 0; i < 3; i++) box_power[i] = (Box){ 0, 0, 0, 0 };
	for (int i = 0; i < 3 && conf_power; i++) {
		double px = cx + (i - 1) * pg;
		cairo_arc(cr, px, py, pr, 0, 2 * M_PI);
		pm_rgba(cr, 0xffffff, 0.18);
		cairo_fill(cr);
		pm_rgba(cr, 0xffffff, 0.92);
		draw_power_icon(cr, i, px, py, pr * 0.42);
		pm_font(cr, 12 * k, 0);
		pm_rgba(cr, 0xffffff, 0.85);
		pm_text(cr, px - pm_textw(cr, plabels[i]) / 2, py + pr + 14 * k, plabels[i]);
		box_power[i] = (Box){ px - pr, py - pr, 2 * pr, 2 * pr + 24 * k };
	}

	/* 左下角：工作階段（有兩個以上才顯示） */
	box_session = (Box){ 0, 0, 0, 0 };
	if (nsessions > 1) {
		snprintf(buf, sizeof buf, "%s", session_names[cur_session]);
		pm_font(cr, 13 * k, 0);
		double tw = pm_textw(cr, buf) + 18 * k, bx = m.x + 24 * k, bh = 28 * k, by = m.y + m.h - 24 * k - bh;
		pm_rrect(cr, bx, by, tw + 28 * k, bh, bh / 2, bh / 2);
		pm_rgba(cr, 0xffffff, 0.18);
		cairo_fill(cr);
		pm_rgba(cr, 0xffffff, 0.9);
		pm_text(cr, bx + 14 * k, by + bh / 2, buf);
		/* 往上的小箭頭（選單從上面打開） */
		double ax = bx + tw + 8 * k, ay = by + bh / 2;
		cairo_set_line_width(cr, 1.5 * k);
		cairo_move_to(cr, ax - 4 * k, ay + 2 * k);
		cairo_line_to(cr, ax, ay - 2 * k);
		cairo_line_to(cr, ax + 4 * k, ay + 2 * k);
		cairo_stroke(cr);
		box_session = (Box){ bx, by, tw + 28 * k, bh };
	}

	/* 登入成功：淡出成黑色 */
	if (ok_at) {
		double a = ease((now - ok_at) / 350.0);
		cairo_set_source_rgba(cr, 0, 0, 0, a);
		cairo_paint(cr);
	}
}

static void draw(void)
{
	if (native) { wl_draw(); return; }
	Paint p;
	cairo_t *cr = pm_paint_begin(&p, win, sw, sh);
	draw_to(cr);
	pm_paint_end(&p, cr, 0, 0);
}

/* ------------------------------------------------------------------ */
/* 動作                                                                */
/* ------------------------------------------------------------------ */

static void clear_pw(void)
{
	explicit_bzero(pw, sizeof pw);
	pwlen = 0;
}

static void select_user(int i)
{
	int n = nusers + (conf_show_other == 1 || nusers == 0 ? 1 : 0);
	if (i < 0 || i >= n || busy) return;
	clear_pw();
	fail_at = 0;
	other_mode = i >= nusers;
	cur = other_mode ? cur : i;
	other_focus_pw = 0;
	if (other_mode) other_name[0] = 0;
	pick_session_for_user();
}

static void submit(void)
{
	if (busy || ok_at || !pwlen) return;
	const char *user = other_mode ? other_name : (cur < nusers ? users[cur].name : "");
	if (!*user) return;
	pw[pwlen] = 0;
	if (preview) {
		busy = 1;
		draw();
		if (dpy) XSync(dpy, False);
		usleep(600000);
		busy = 0;
		if (!strcmp(pw, "demo")) ok_at = pm_now_ms();
		else { fail_at = pm_now_ms(); fail_msg[0] = 0; }
		clear_pw();
		anim_until = pm_now_ms() + 500;
		return;
	}
	char msg[600];
	size_t k = 0;
	const char *f[4] = { "LOGIN", user, pw, nsessions ? sessions[cur_session] : "" };
	for (int i = 0; i < 4; i++) {
		size_t l = strlen(f[i]);
		if (k + l + 1 > sizeof msg) { clear_pw(); return; }
		memcpy(msg + k, f[i], l + 1);
		k += l + 1;
	}
	ssize_t r = send(dm_fd, msg, k, MSG_NOSIGNAL);
	explicit_bzero(msg, sizeof msg);
	clear_pw();
	if (r < 0) { fail_at = pm_now_ms(); snprintf(fail_msg, sizeof fail_msg, "%s", _("無法連線到登入管理員")); return; }
	busy = 1;
}

static void power(int i)
{
	static const char *what[3] = { "suspend", "reboot", "poweroff" };
	if (preview) { fprintf(stderr, "pomelo-greeter: %s\n", what[i]); return; }
	char msg[32];
	int k = snprintf(msg, sizeof msg, "POWER%c%s", 0, what[i]);
	if (send(dm_fd, msg, (size_t)k + 1, MSG_NOSIGNAL) < 0) fprintf(stderr, "pomelo-greeter: 無法送出電源指令\n");
}

static void session_cb(const Arg *a) { cur_session = a->i; }

/* 抓住鍵盤。只靠 XSetInputFocus 不可靠：pomelo-compositor（wlroots）的視窗管理器看到不受管理的
 * 視窗自己換焦點，會把 X 的焦點改回「沒有」，按鍵就丟掉了、密碼打不進去——要看時序，有時會有時
 * 不會。鎖定畫面、啟動器也都是抓鍵盤。 */
static void grab_keyboard(void)
{
	for (int i = 0; i < 50; i++) {
		if (XGrabKeyboard(dpy, win, True, GrabModeAsync, GrabModeAsync, CurrentTime) == GrabSuccess) return;
		usleep(20000);
	}
	fputs("pomelo-greeter: 抓不到鍵盤\n", stderr);
}

static void session_menu(void)
{
	MenuItem items[16];
	for (int i = 0; i < nsessions; i++) items[i] = (MenuItem){ session_names[i], session_cb, { .i = i } };
	if (native) { pmwl_menu(wl_primary(), items, nsessions, (int)box_session.x - mons[pmon].x, (int)(box_session.y - nsessions * 30 - 20) - mons[pmon].y); return; }
	pm_popup(items, nsessions, (int)box_session.x, (int)(box_session.y - nsessions * 30 - 20));
	grab_keyboard();   /* 選單關掉時會放開鍵盤，要再抓回來 */
}

static int in(Box b, int x, int y) { return b.w > 0 && x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h; }

/* 點了哪裡（全域座標）：X 與原生共用 */
static void click(int x, int y, int btn)
{
	if (btn != 1 || ok_at) return;
	int n = nusers + (conf_show_other == 1 || nusers == 0 ? 1 : 0);
	for (int i = 0; i < n; i++) if (in(box_users[i], x, y)) { select_user(i); return; }
	for (int i = 0; i < 3; i++) if (in(box_power[i], x, y)) { power(i); return; }
	if (in(box_session, x, y)) { session_menu(); return; }
	if (in(box_go, x, y)) { submit(); return; }
	if (other_mode && in(box_other_name, x, y)) other_focus_pw = 0;
	else if (other_mode && in(box_field, x, y)) other_focus_pw = 1;
}

static void button(XButtonEvent *e) { click(e->x, e->y, (int)e->button); }

/* 按鍵（keysym、打出來的字、修飾鍵）：X 與原生共用 */
static void key_sym(KeySym ks, const char *buf, int len, unsigned state)
{
	if (busy || ok_at) return;
	int typing_name = other_mode && !other_focus_pw;
	switch (ks) {
	case XK_Return: case XK_KP_Enter:
		if (typing_name) { if (other_name[0]) other_focus_pw = 1; return; }
		submit();
		return;
	case XK_Tab:
		if (other_mode) { other_focus_pw = !other_focus_pw; return; }
		select_user((cur + 1) % MAX(nusers, 1));
		return;
	case XK_Escape:
		if (preview && !pwlen && !typing_name) exit(0);   /* 預覽：Esc 離開 */
		clear_pw();
		fail_at = 0;
		return;
	case XK_Left: case XK_Right:
		if (!pwlen && !typing_name) select_user((other_mode ? nusers : cur) + (ks == XK_Left ? -1 : 1));
		return;
	case XK_BackSpace:
		if (typing_name) { size_t l = strlen(other_name); if (l) other_name[l - 1] = 0; return; }
		while (pwlen && ((unsigned char)pw[pwlen - 1] & 0xc0) == 0x80) pwlen--;
		if (pwlen) pwlen--;
		pw[pwlen] = 0;
		return;
	default:
		if ((state & ControlMask) && (ks == XK_u || ks == XK_U)) { if (typing_name) other_name[0] = 0; else clear_pw(); return; }
		if (len <= 0 || (unsigned char)buf[0] < 32 || buf[0] == 127) return;
		if (typing_name) {
			size_t l = strlen(other_name);
			/* 帳號名稱只收一般字元 */
			if (len == 1 && (isalnum((unsigned char)buf[0]) || strchr("._-", buf[0])) && l + 1 < sizeof other_name) { other_name[l] = buf[0]; other_name[l + 1] = 0; }
			return;
		}
		if (pwlen + len < (int)sizeof pw - 1) {
			memcpy(pw + pwlen, buf, (size_t)len);
			pwlen += len;
			fail_at = 0;
		}
	}
}

static void key(XKeyEvent *e)
{
	char buf[32];
	KeySym ks;
	int len = XLookupString(e, buf, sizeof buf - 1, &ks, NULL);
	unsigned st = 0;
	XkbGetIndicatorState(dpy, XkbUseCoreKbd, &st);
	capslock = st & 1;
	key_sym(ks, buf, len, e->state);
}

/* pomelo-dm 的回覆 */
static void dm_reply(void)
{
	char buf[600];
	ssize_t n = recv(dm_fd, buf, sizeof buf - 1, 0);
	if (n <= 0) {   /* pomelo-dm 關掉了 socket：登入成功就播完淡出再結束 */
		if (!ok_at) exit(1);
		close(dm_fd);
		dm_fd = -1;
		return;
	}
	buf[n] = 0;
	if (strcmp(buf, "INFO")) busy = 0;   /* 提示訊息不影響「驗證中」 */
	if (!strcmp(buf, "OK")) { ok_at = pm_now_ms(); anim_until = ok_at + 400; return; }
	const char *m0 = (size_t)n > strlen(buf) + 1 ? buf + strlen(buf) + 1 : "";
	if (!strcmp(buf, "INFO")) {   /* 提示（例如不能關機），不算密碼錯誤：不晃動 */
		info_at = pm_now_ms();
		snprintf(info_msg, sizeof info_msg, "%s", *m0 ? _(m0) : "");
		return;
	}
	fail_at = pm_now_ms();
	anim_until = fail_at + 500;
	const char *m = (size_t)n > strlen(buf) + 1 ? buf + strlen(buf) + 1 : "";
	snprintf(fail_msg, sizeof fail_msg, "%s", *m ? _(m) : "");
}

/* ------------------------------------------------------------------ */
/* 原生 Wayland（layer-shell；Wayland 轉移階段 3 第 9 步）                      */
/* ------------------------------------------------------------------ */

#define WLMAX 8
static PmWl *wg[WLMAX];
static int wl_nout;

static PmWl *wl_primary(void) { return pmon >= 0 && pmon < WLMAX && wg[pmon] ? wg[pmon] : wg[0]; }

/* 螢幕清單：自己填 libpomelo 的 mons／pmon／sw／sh */
static void wl_sync_outputs(void)
{
	nmons = pmwl_outputs(mons, (int)LEN(mons));
	if (nmons <= 0) { nmons = 1; mons[0] = (Rect){ 0, 0, 1280, 800 }; }
	wl_nout = MIN(nmons, WLMAX);
	pmon = 0;
	for (int i = 0; i < nmons; i++)
		if (mons[i].x <= 0 && mons[i].y <= 0 && mons[i].x + mons[i].w > 0 && mons[i].y + mons[i].h > 0) pmon = i;
	int x1 = 0, y1 = 0;
	for (int i = 0; i < nmons; i++) { x1 = MAX(x1, mons[i].x + mons[i].w); y1 = MAX(y1, mons[i].y + mons[i].h); }
	sw = x1; sh = y1;
}

/* 每個螢幕一張貼滿的 overlay 視窗；主要顯示器那張獨佔鍵盤（密碼打在這） */
static void wl_surfaces(void)
{
	for (int i = 0; i < WLMAX; i++) {
		if (i < wl_nout && !wg[i])
			wg[i] = pmwl_layer("pomelo-greeter", 3, i, PMWL_ANCHOR_TOP | PMWL_ANCHOR_BOTTOM | PMWL_ANCHOR_LEFT | PMWL_ANCHOR_RIGHT,
			                   0, 0, 0, 0, 0, 0, -1, i == pmon ? 1 : 0);
		else if (i >= wl_nout && wg[i]) { pmwl_destroy(wg[i]); wg[i] = NULL; }
	}
}

static void wl_draw(void)
{
	for (int i = 0; i < wl_nout; i++) {
		if (!wg[i]) continue;
		int W, H;
		cairo_t *cr = pmwl_begin(wg[i], &W, &H);
		if (!cr) continue;
		cairo_translate(cr, -mons[i].x, -mons[i].y);   /* draw_to 用全域座標畫整個桌面，這張只露出這個螢幕的那塊 */
		draw_to(cr);
		pmwl_end(wg[i], cr);
	}
}

static int wl_index(PmWl *w) { for (int i = 0; i < WLMAX; i++) if (w && wg[i] == w) return i; return -1; }

static void wl_outputs_changed(void)
{
	wl_sync_outputs();
	wl_surfaces();
	for (int i = 0; i < nusers; i++) if (users[i].bg) { cairo_surface_destroy(users[i].bg); users[i].bg = NULL; }
	if (default_bg) { cairo_surface_destroy(default_bg); default_bg = NULL; }
}

static int wl_main(void)
{
	wl_surfaces();
	draw();
	int dirty = 0;
	for (;;) {
		int animating = busy || (anim_until && pm_now_ms() < anim_until) || ok_at;   /* 登入成功後一直到結束都要醒著 */
		struct pollfd pf = { dm_fd, POLLIN, 0 };
		PmwEvent ev;
		int r = pmwl_next_fds(&ev, animating ? 16 : 1000, &pf, dm_fd >= 0 ? 1 : 0);
		for (int more = r; more; more = pmwl_next(&ev, 0)) {
			int oi = wl_index(ev.win);
			switch (ev.type) {
			case PMW_EXPOSE: case PMW_SIZE: dirty = 1; break;
			case PMW_KEY:
				capslock = (ev.state & LockMask) != 0;
				key_sym(ev.key, ev.text, ev.textlen, ev.state);
				dirty = 1;
				break;
			case PMW_DOWN: if (oi >= 0) { click(mons[oi].x + ev.x, mons[oi].y + ev.y, ev.button); dirty = 1; } break;
			case PMW_OUTPUTS: wl_outputs_changed(); dirty = 1; break;   /* 接上或拔掉螢幕 */
			case PMW_CLOSE: if (oi >= 0) { pmwl_destroy(wg[oi]); wg[oi] = NULL; } break;
			}
		}
		if (dm_fd >= 0 && (pf.revents & (POLLIN | POLLHUP))) { dm_reply(); dirty = 1; }
		if (ok_at && pm_now_ms() - ok_at > 420 && (preview || dm_fd < 0)) { wl_draw(); return 0; }
		if (fail_at && pm_now_ms() - fail_at > 4000) { fail_at = 0; dirty = 1; }   /* 失敗訊息四秒後自動消失 */
		if (info_at && pm_now_ms() - info_at > 6000) { info_at = 0; dirty = 1; }
		if (dirty || r == 0 || animating) { wl_draw(); dirty = 0; }
	}
}

static int xerror(Display *d, XErrorEvent *e) { (void)d; (void)e; return 0; }

int main(int argc, char *argv[])
{
	if (argc > 1 && !strcmp(argv[1], "--preview")) preview = 1;
	else if (argc > 1) { puts(_("用法：pomelo-greeter（由 pomelo-dm 啟動）；pomelo-greeter --preview 只看畫面，密碼 demo")); return 0; }
	setlocale(LC_ALL, "");
	safe_mode = getenv("POMELO_GREETER_SAFE") != NULL;
	const char *fd = getenv("POMELO_DM_FD");
	if (!preview) {
		if (!fd) { fputs("pomelo-greeter: 要由 pomelo-dm 啟動（只看畫面請用 --preview）\n", stderr); return 2; }
		dm_fd = atoi(fd);
		/* 確認真的是 pomelo-dm 給的 socket（不然之後打開 X 時可能拿到同一個號碼，把訊息寫進 X 的連線） */
		int type = 0;
		socklen_t tl = sizeof type;
		if (dm_fd < 3 || getsockopt(dm_fd, SOL_SOCKET, SO_TYPE, &type, &tl) || type != SOCK_SEQPACKET) {
			fputs("pomelo-greeter: POMELO_DM_FD 不是 pomelo-dm 的連線\n", stderr);
			return 2;
		}
	}
	state_dir = getenv("POMELO_DM_STATE");
	if (!state_dir) state_dir = "/var/lib/pomelo-dm";
	sessions_dir = getenv("POMELO_DM_SESSIONS");
	if (!sessions_dir) sessions_dir = "/usr/share/xsessions";
	/* Wayland 工作階段（pomelo-dm 已設 WAYLAND_DISPLAY、compositor 有 layer-shell）就用原生模式，不開 X */
	native = pmwl_has_layer();
	if (!native && !(dpy = XOpenDisplay(NULL))) { fputs("pomelo-greeter: 無法開啟 X display\n", stderr); return 1; }
	if (dpy) XSetErrorHandler(xerror);
	pm_init("Noto Sans CJK TC", 13, 1, "Yaru");
	pm_set_theme(1, 0);
	if (native) wl_sync_outputs(); else pm_update_monitors();
	load_conf();
	load_users();
	load_sessions();
	if (!nusers) other_mode = 1;
	if (users_truncated && conf_show_other != 1) conf_show_other = 1;
	pick_session_for_user();
	if (native) return wl_main();

	XSetWindowAttributes wa = {
		.override_redirect = True, .background_pixel = BlackPixel(dpy, scr),
		.event_mask = ExposureMask | KeyPressMask | ButtonPressMask,
	};
	win = XCreateWindow(dpy, pm_root, 0, 0, sw, sh, 0, CopyFromParent, InputOutput, CopyFromParent,
	                    CWOverrideRedirect | CWBackPixel | CWEventMask, &wa);
	XStoreName(dpy, win, "pomelo-greeter");
	XDefineCursor(dpy, win, pm_cursor);
	XDefineCursor(dpy, pm_root, pm_cursor);
	XMapRaised(dpy, win);
	XSync(dpy, False);
	XSetInputFocus(dpy, win, RevertToPointerRoot, CurrentTime);
	grab_keyboard();
	if (rr_base) XRRSelectInput(dpy, pm_root, RRScreenChangeNotifyMask);
	unsigned st = 0;
	XkbGetIndicatorState(dpy, XkbUseCoreKbd, &st);
	capslock = st & 1;
	draw();

	int xfd = ConnectionNumber(dpy), dirty = 0;
	for (;;) {
		while (XPending(dpy)) {
			XEvent ev;
			XNextEvent(dpy, &ev);
			if (ev.type == NoExpose || ev.type == GraphicsExpose) continue;   /* 複製畫面時 X 回的通知，不用重畫 */
			if (ev.type == KeyPress) key(&ev.xkey);
			else if (ev.type == ButtonPress) button(&ev.xbutton);
			else if (rr_base && ev.type == rr_base + RRScreenChangeNotify) {
				/* 接上或拔掉螢幕：重新排版、重做背景 */
				XRRUpdateConfiguration(&ev);
				pm_update_monitors();
				sw = DisplayWidth(dpy, scr); sh = DisplayHeight(dpy, scr);
				XMoveResizeWindow(dpy, win, 0, 0, sw, sh);
				for (int i = 0; i < nusers; i++) if (users[i].bg) { cairo_surface_destroy(users[i].bg); users[i].bg = NULL; }
				if (default_bg) { cairo_surface_destroy(default_bg); default_bg = NULL; }
			}
			dirty = 1;
		}
		if (dirty) { draw(); dirty = 0; }
		if (ok_at && pm_now_ms() - ok_at > 420 && (preview || dm_fd < 0)) { draw(); XSync(dpy, False); return 0; }
		XFlush(dpy);   /* 畫好的東西馬上送出去，不要等到下一輪 */
		if (XEventsQueued(dpy, QueuedAlready)) continue;   /* 送出時 Xlib 可能已經讀進新事件，先處理，不要睡 */
		int animating = busy || (anim_until && pm_now_ms() < anim_until) || ok_at;   /* 登入成功後一直到結束都要醒著 */
		struct pollfd p[2] = { { xfd, POLLIN, 0 }, { dm_fd, POLLIN, 0 } };
		int r = poll(p, dm_fd >= 0 ? 2 : 1, animating ? 16 : 1000);
		if (r > 0 && dm_fd >= 0 && (p[1].revents & (POLLIN | POLLHUP))) { dm_reply(); draw(); }
		/* 每兩秒確認一次還在最上層（不要每輪都做：會產生事件） */
		static long raised_at;
		if (pm_now_ms() - raised_at > 2000) { XRaiseWindow(dpy, win); raised_at = pm_now_ms(); }
		if (r == 0 || animating) draw();
		/* 失敗訊息四秒後自動消失 */
		if (fail_at && pm_now_ms() - fail_at > 4000) { fail_at = 0; draw(); }
		if (info_at && pm_now_ms() - info_at > 6000) { info_at = 0; draw(); }
		XFlush(dpy);
	}
}
