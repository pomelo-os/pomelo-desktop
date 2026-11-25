/* SPDX-License-Identifier: GPL-3.0-or-later */
/* libpomelo — 設定程式用的介面元件（立即模式，macOS 系統設定風格） */
#define _GNU_SOURCE
#include "internal.h"
#include "pomelo-check.h"
#include <locale.h>
#include <sys/select.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>

#define ROW 44
#define PAD 16

static unsigned c_bg, c_card, c_line, c_text, c_dim, c_ctrl;
static int need_redraw;
static Atom a_delete;
static int pop_mode, pop_right, pop_top;
static int pop_max_h = 600;      /* 原生彈出式：最高多高（螢幕高度扣掉上面的位置） */
static int main_input_id;        /* pm_ui_input 的元件編號：它握著焦點時 Esc 仍然要能關閉彈出視窗 */
static int main_input_has_text;  /* 有打到一半的文字時，Esc 先清字，再按一次才關閉 */
int pm_ui_native;
static int ui_tick_override;       /* pm_ui_tick_ms：0 = 用 pm_ui_run 的 tick_ms */
static int ui_quit;              /* 原生 Wayland：pm_ask 按了按鈕，結束 pm_ui_run */
static int ime_id, ime_secret, ime_x, ime_y, ime_h;   /* 原生 Wayland：有焦點的輸入框與游標位置（給輸入法） */
static PmWl *wl_modal;           /* 原生 Wayland：開著的檔案對話窗（裡面跳出的 pm_ask 掛在它底下） */

/* 原生 Wayland 的程式要用到只有 X 版的東西（例如檔案對話窗）：臨時開 X 連線 */
static int need_x(void)
{
	if (!dpy && !(dpy = XOpenDisplay(NULL))) return 0;
	pm__init_x();
	return 1;
}

void pm_ui_popover(int right, int top) { pop_mode = 1; pop_right = right; pop_top = top; }
/* 原生 Wayland 的彈出式視窗暫時收起來（例如確認視窗開著時：獨佔鍵盤的 overlay 層會擋住它）。X 版沒有作用（程式自己處理） */
static int ui_hide_req;
void pm_ui_hide(int hide) { ui_hide_req = hide != 0; }

int pm_ui_write_icon(cairo_surface_t *(*icon)(int size), const char *path)
{
	cairo_surface_t *s = icon(256);
	int ok = cairo_surface_write_to_png(s, path) == CAIRO_STATUS_SUCCESS;
	cairo_surface_destroy(s);
	return ok ? 0 : 1;
}

char *pm_run(const char *cmd)
{
	char full[4096];
	snprintf(full, sizeof full, "LC_ALL=C.UTF-8 %s 2>/dev/null", cmd);   /* 英文欄位、保留 UTF-8 內容 */
	FILE *f = popen(full, "r");
	if (!f) return NULL;
	size_t cap = 4096, n = 0;
	char *out = malloc(cap);
	size_t k;
	while ((k = fread(out + n, 1, cap - n - 1, f)) > 0) {
		n += k;
		if (cap - n < 1024) out = realloc(out, cap *= 2);
	}
	out[n] = 0;
	pclose(f);
	return out;
}

int pm_have(const char *cmd)
{
	char *path = strdup(getenv("PATH") ? getenv("PATH") : "/usr/bin:/bin"), *save = NULL;
	int found = 0;
	for (char *d = strtok_r(path, ":", &save); d && !found; d = strtok_r(NULL, ":", &save)) {
		char full[PATH_MAX];
		snprintf(full, sizeof full, "%s/%s", d, cmd);
		found = !access(full, X_OK);
	}
	free(path);
	return found;
}

static void ui_colors(void)
{
	if (pm_dark) {
		c_bg = 0x1c1c1e; c_card = 0x2c2c2e; c_line = 0x3a3a3c; c_text = 0xf2f2f2; c_dim = 0x98989d; c_ctrl = 0x48484a;
	} else {
		c_bg = 0xf2f2f7; c_card = 0xffffff; c_line = 0xe5e5ea; c_text = 0x1d1d1f; c_dim = 0x8e8e93; c_ctrl = 0xe5e5ea;
	}
}

void pm_ui_tick_ms(int ms) { ui_tick_override = ms > 0 ? ms : 0; }

void pm_ui_redraw(PmUI *u) { (void)u; need_redraw = 1; }

static int clicked_in(PmUI *u, double x, double y, double w, double h)
{
	if (!u->clicked || u->claimed) return 0;
	if (u->mx < x || u->mx >= x + w || u->my < y || u->my >= y + h) return 0;
	u->claimed = 1;
	return 1;
}

/* 一行卡片背景，回傳這行的 y */
static int row(PmUI *u, int h)
{
	int y = u->y;
	if (!u->group_open) {   /* 沒有群組時自動開一個 */
		pm_ui_group(u, NULL);
		y = u->y;
	}
	pm_rgb(u->cr, c_card);
	cairo_rectangle(u->cr, u->x, y, u->cw, h);
	cairo_fill(u->cr);
	if (u->group_rows) {
		pm_rgb(u->cr, c_line);
		cairo_rectangle(u->cr, u->x + PAD, y, u->cw - PAD, 1);
		cairo_fill(u->cr);
	}
	u->group_rows++;
	u->y += h;
	return y;
}

/* 列左邊的標籤；right = 右邊控制項的寬度（0 = 不知道，最多用一半寬） */
static void label_left_w(PmUI *u, int y, int h, const char *label, double right)
{
	if (!label) return;
	pm_font(u->cr, pm_size, 0);
	pm_rgb(u->cr, c_text);
	char buf[256];
	double maxw = right > 0 ? u->cw - 2 * PAD - right - 16 : u->cw / 2 - PAD;
	pm_ellipsize(u->cr, pm_tr(label), maxw, buf, sizeof buf);
	pm_text(u->cr, u->x + PAD, y + h / 2.0, buf);
}

static void label_left(PmUI *u, int y, int h, const char *label) { label_left_w(u, y, h, label, 0); }

int pm_ui_back(PmUI *u, const char *label)
{
	u->id++;
	if (u->group_open) pm_ui_end(u);
	int y = u->y, h = 30;
	const char *t = pm_tr(label);
	pm_font(u->cr, pm_size, 0);
	double tw = pm_textw(u->cr, t);
	int hit = clicked_in(u, u->x - 6, y, tw + 30, h);
	double cx = u->x + 4, cy = y + h / 2.0;
	pm_rgb(u->cr, T->sel);
	cairo_new_path(u->cr);
	cairo_set_line_width(u->cr, 2.2);
	cairo_set_line_cap(u->cr, CAIRO_LINE_CAP_ROUND);
	cairo_set_line_join(u->cr, CAIRO_LINE_JOIN_ROUND);
	cairo_move_to(u->cr, cx + 5, cy - 7);
	cairo_line_to(u->cr, cx - 1, cy);
	cairo_line_to(u->cr, cx + 5, cy + 7);
	cairo_stroke(u->cr);
	pm_text(u->cr, u->x + 16, cy, t);
	u->y += h;
	/* 鍵盤：沒有在輸入文字時，Backspace 或 Alt+← 也是返回 */
	if (!u->focus && (u->key == XK_BackSpace || (u->key == XK_Left && (u->keystate & Mod1Mask)))) {
		u->key = 0;
		return 1;
	}
	return hit;
}

void pm_ui_title(PmUI *u, const char *title, const char *subtitle)
{
	pm_font(u->cr, 22, 1);
	pm_rgb(u->cr, c_text);
	pm_text(u->cr, u->x + 2, u->y + 16, pm_tr(title));
	u->y += 38;
	if (subtitle) {
		pm_font(u->cr, pm_size, 0);
		pm_rgb(u->cr, c_dim);
		pm_text(u->cr, u->x + 2, u->y + 6, pm_tr(subtitle));
		u->y += 24;
	}
	u->y += 4;
}

static int group_top;

void pm_ui_group(PmUI *u, const char *caption)
{
	if (u->group_open) pm_ui_end(u);
	if (caption) {
		pm_font(u->cr, 12, 1);
		pm_rgb(u->cr, c_dim);
		pm_text(u->cr, u->x + 4, u->y + 8, pm_tr(caption));
		u->y += 20;
	}
	u->group_open = 1;
	u->group_rows = 0;
	group_top = u->y;
}

/* 群組結束：把四個角裁成圓角 */
void pm_ui_end(PmUI *u)
{
	if (!u->group_open) return;
	u->group_open = 0;
	if (u->group_rows) {
		double r = 10, x = u->x, y = group_top, w = u->cw, h = u->y - group_top;
		double cx[4] = { x, x + w - r, x, x + w - r }, cy[4] = { y, y, y + h - r, y + h - r };
		double ox[4] = { x + r, x + w - r, x + r, x + w - r }, oy[4] = { y + r, y + r, y + h - r, y + h - r };
		for (int i = 0; i < 4; i++) {
			cairo_save(u->cr);
			cairo_rectangle(u->cr, cx[i], cy[i], r, r);
			cairo_clip(u->cr);
			cairo_rectangle(u->cr, cx[i], cy[i], r, r);
			cairo_arc(u->cr, ox[i], oy[i], r, 0, 2 * M_PI);
			cairo_set_fill_rule(u->cr, CAIRO_FILL_RULE_EVEN_ODD);
			pm_rgb(u->cr, c_bg);
			cairo_fill(u->cr);
			cairo_restore(u->cr);
		}
	}
	u->y += 20;
}

void pm_ui_note(PmUI *u, const char *text)
{
	pm_font(u->cr, 12, 0);
	pm_rgb(u->cr, c_dim);
	char line[512];
	const char *p = pm_tr(text);
	double maxw = u->cw - 12;
	u->y -= 12;
	while (*p) {
		/* 一行放不下就換行：英文在空白、中文在字與字之間（pm_text_fit） */
		size_t l = strcspn(p, "\n"), cut = pm_text_fit(u->cr, p, l, maxw);
		snprintf(line, sizeof line, "%.*s", (int)cut, p);
		pm_text(u->cr, u->x + 6, u->y + 8, line);
		u->y += 18;
		p += cut;
		if (*p == '\n') p++;
		else while (*p == ' ') p++;
	}
	u->y += 8;
}

void pm_ui_space(PmUI *u, int h) { u->y += h; }

int pm_ui_switch(PmUI *u, const char *label, int *v)
{
	u->id++;
	int y = row(u, ROW);
	label_left_w(u, y, ROW, label, 40);
	double w = 40, h = 24, x = u->x + u->cw - PAD - w, ty = y + (ROW - h) / 2;
	int changed = 0;
	if (clicked_in(u, u->x, y, u->cw, ROW)) { *v = !*v; changed = 1; }
	pm_rrect(u->cr, x, ty, w, h, h / 2, h / 2);
	pm_rgb(u->cr, *v ? T->sel : c_ctrl);
	cairo_fill(u->cr);
	double kx = *v ? x + w - h / 2 : x + h / 2;
	cairo_arc(u->cr, kx, ty + h / 2, h / 2 - 2, 0, 2 * M_PI);
	pm_rgb(u->cr, 0xffffff);
	cairo_fill_preserve(u->cr);
	pm_rgba(u->cr, 0x000000, 0.12);
	cairo_set_line_width(u->cr, 0.8);
	cairo_stroke(u->cr);
	return changed;
}

int pm_ui_slider(PmUI *u, const char *label, double *v, double lo, double hi)
{
	int id = ++u->id;
	int y = row(u, ROW);
	label_left(u, y, ROW, label);
	double tw = label ? MIN(260, u->cw / 2 - PAD) : u->cw - 2 * PAD;
	double tx = u->x + u->cw - PAD - tw, cy = y + ROW / 2.0;
	if (clicked_in(u, tx - 10, y, tw + 20, ROW)) u->active = id;
	int changed = 0;
	if (u->active == id && u->down) {
		double nv = lo + (hi - lo) * CLAMP((u->mx - tx) / tw, 0, 1);
		if (nv != *v) { *v = nv; changed = 1; }
	}
	double f = hi > lo ? CLAMP((*v - lo) / (hi - lo), 0, 1) : 0;
	pm_rrect(u->cr, tx, cy - 2, tw, 4, 2, 2);
	pm_rgb(u->cr, c_ctrl);
	cairo_fill(u->cr);
	pm_rrect(u->cr, tx, cy - 2, tw * f, 4, 2, 2);
	pm_rgb(u->cr, T->sel);
	cairo_fill(u->cr);
	cairo_arc(u->cr, tx + tw * f, cy, 9, 0, 2 * M_PI);
	pm_rgb(u->cr, 0xffffff);
	cairo_fill_preserve(u->cr);
	pm_rgba(u->cr, 0x000000, 0.18);
	cairo_set_line_width(u->cr, 1);
	cairo_stroke(u->cr);
	return changed;
}

int pm_ui_segment(PmUI *u, const char *label, const char **opts, int n, int *v)
{
	u->id++;
	int y = row(u, ROW);
	label_left(u, y, ROW, label);
	pm_font(u->cr, 12.5, 0);
	double sw0 = 0;
	for (int i = 0; i < n; i++) sw0 = MAX(sw0, pm_textw(u->cr, pm_tr(opts[i])) + 24);
	double h = 26, total = sw0 * n, ty = y + (ROW - h) / 2;
	/* 沒有標題時（分頁切換）置中 */
	double x0 = label ? u->x + u->cw - PAD - total : u->x + (u->cw - total) / 2;
	int changed = 0;
	for (int i = 0; i < n; i++)
		if (clicked_in(u, x0 + i * sw0, ty, sw0, h) && *v != i) { *v = i; changed = 1; }
	pm_rrect(u->cr, x0, ty, total, h, 7, 7);
	pm_rgb(u->cr, c_ctrl);
	cairo_fill(u->cr);
	for (int i = 0; i < n; i++) {
		double sx = x0 + i * sw0;
		if (i == *v) {
			pm_rrect(u->cr, sx + 2, ty + 2, sw0 - 4, h - 4, 6, 6);
			pm_rgb(u->cr, pm_dark ? 0x636366 : 0xffffff);
			cairo_fill(u->cr);
		}
		pm_rgb(u->cr, c_text);
		pm_text(u->cr, sx + (sw0 - pm_textw(u->cr, pm_tr(opts[i]))) / 2, ty + h / 2, pm_tr(opts[i]));
	}
	return changed;
}

static int choice_pick;
static void choice_cb(const Arg *a) { choice_pick = a->i; }

int pm_ui_choice(PmUI *u, const char *label, const char **opts, int n, int *v)
{
	u->id++;
	int y = row(u, ROW);
	label_left(u, y, ROW, label);
	pm_font(u->cr, pm_size, 0);
	const char *cur = *v >= 0 && *v < n ? pm_tr(opts[*v]) : "";
	char buf[256];
	pm_ellipsize(u->cr, cur, u->cw / 2 - 40, buf, sizeof buf);
	double tw = pm_textw(u->cr, buf), rx = u->x + u->cw - PAD;
	pm_rgb(u->cr, c_dim);
	pm_text(u->cr, rx - 18 - tw, y + ROW / 2.0, buf);
	/* 上下箭頭 */
	cairo_set_line_width(u->cr, 1.5);
	double ax = rx - 6, ay = y + ROW / 2.0;
	cairo_move_to(u->cr, ax - 4, ay - 2); cairo_line_to(u->cr, ax, ay - 6); cairo_line_to(u->cr, ax + 4, ay - 2);
	cairo_move_to(u->cr, ax - 4, ay + 2); cairo_line_to(u->cr, ax, ay + 6); cairo_line_to(u->cr, ax + 4, ay + 2);
	cairo_stroke(u->cr);
	if (!clicked_in(u, u->x + u->cw / 2, y, u->cw / 2, ROW)) return 0;
	MenuItem *m = calloc(n, sizeof *m);
	for (int i = 0; i < n; i++) { m[i].label = opts[i]; m[i].fn = choice_cb; m[i].arg.i = i; }
	choice_pick = -1;
	if (u->wl) {
		pmwl_menu(u->wl, m, n, (int)(rx - tw - 40), y + ROW - 4);
	} else {
		int rx0, ry0;
		Window child;
		XTranslateCoordinates(dpy, u->win, pm_root, (int)(rx - tw - 40), y + ROW - 4, &rx0, &ry0, &child);
		pm_popup(m, n, rx0, ry0);
	}
	free(m);
	if (choice_pick >= 0 && choice_pick != *v) {
		*v = choice_pick;
		u->again = 1;
		return 1;
	}
	return 0;
}

int pm_ui_swatches(PmUI *u, const char *label, const unsigned *colors, int n, int *v)
{
	u->id++;
	int y = row(u, ROW);
	label_left(u, y, ROW, label);
	double step = 28, x0 = u->x + u->cw - PAD - step * n + 6, cy = y + ROW / 2.0;
	int changed = 0;
	for (int i = 0; i < n; i++) {
		double cx = x0 + i * step + 8;
		if (clicked_in(u, cx - 12, y, 24, ROW) && *v != i) { *v = i; changed = 1; }
		cairo_arc(u->cr, cx, cy, 9, 0, 2 * M_PI);
		pm_rgb(u->cr, colors[i]);
		cairo_fill(u->cr);
		if (i == *v) {
			cairo_arc(u->cr, cx, cy, 12.5, 0, 2 * M_PI);
			pm_rgb(u->cr, colors[i]);
			cairo_set_line_width(u->cr, 2);
			cairo_stroke(u->cr);
			cairo_arc(u->cr, cx, cy, 3, 0, 2 * M_PI);
			pm_rgb(u->cr, 0xffffff);
			cairo_fill(u->cr);
		}
	}
	return changed;
}

int pm_ui_button(PmUI *u, const char *label, const char *button)
{
	u->id++;
	int y = row(u, ROW);
	pm_font(u->cr, 12.5, 0);
	button = pm_tr(button);
	double bw = pm_textw(u->cr, button) + 28, bh = 26;
	label_left_w(u, y, ROW, label, bw);
	pm_font(u->cr, 12.5, 0);
	double bx = label ? u->x + u->cw - PAD - bw : u->x + (u->cw - bw) / 2, by = y + (ROW - bh) / 2;
	int hit = clicked_in(u, bx, by, bw, bh);
	pm_rrect(u->cr, bx, by, bw, bh, 6, 6);
	pm_rgb(u->cr, hit ? T->sel : c_ctrl);
	cairo_fill(u->cr);
	pm_rgb(u->cr, hit ? 0xffffff : c_text);
	pm_text(u->cr, bx + 14, by + bh / 2, button);
	return hit;
}

/* 對話窗底下一排橫的按鈕，靠右；def 是預設按鈕（藍色，按 Enter 也算），-1 = 沒有。回傳按下的 index，沒按回傳 -1 */
int pm_ui_buttons(PmUI *u, const char *const *labels, int n, int def)
{
	u->id++;
	if (u->group_open) pm_ui_end(u);
	enum { BH = 28, GAP = 8 };
	double w[8], total = 0;
	if (n > 8) n = 8;
	pm_font(u->cr, 13, 0);
	for (int i = 0; i < n; i++) {
		w[i] = MAX(76, pm_textw(u->cr, pm_tr(labels[i])) + 32);
		total += w[i] + (i ? GAP : 0);
	}
	double x = u->x + u->cw - total, y = u->y;
	int hit = -1;
	for (int i = 0; i < n; i++) {
		const char *t = pm_tr(labels[i]);
		if (clicked_in(u, x, y, w[i], BH)) hit = i;
		pm_rrect(u->cr, x, y, w[i], BH, 7, 7);
		pm_rgb(u->cr, i == def ? T->sel : c_ctrl);
		cairo_fill(u->cr);
		pm_rgb(u->cr, i == def ? 0xffffff : c_text);
		pm_text(u->cr, x + (w[i] - pm_textw(u->cr, t)) / 2, y + BH / 2.0, t);
		x += w[i] + GAP;
	}
	if (hit < 0 && def >= 0 && (u->key == XK_Return || u->key == XK_KP_Enter)) { hit = def; u->key = 0; }
	u->y += BH;
	return hit;
}

void pm_ui_menu(PmUI *u, const MenuItem *m, int n)
{
	int x = MAX(0, u->mx - 20), y = MAX(0, u->my - 10);
	if (u->wl) { pmwl_menu(u->wl, m, n, x, y); return; }
	int rx, ry;
	Window child;
	XTranslateCoordinates(dpy, u->win, pm_root, x, y, &rx, &ry, &child);
	pm_popup(m, n, rx, ry);
}


/* pm_ask：常見的詢問對話窗（一句問題，按鈕橫排靠右，Enter＝預設按鈕、Esc 或關掉視窗＝cancel） */
static int lines_of(cairo_t *cr, const char *p, double maxw);
static double draw_wrapped(cairo_t *cr, const char *p, double x, double y, double maxw, double lh);

static const char *ask_msg;
static const char *const *ask_btns;
static int ask_n, ask_def, ask_cancel, ask_result;
static PmUI *ask_ui;
static Window ask_win;   /* ask_ui 指到 pm_ui_run 裡的區域變數，跑完就失效，所以另外記視窗 */

static void ask_close(PmUI *u)
{
	if (u->wl) { ui_quit = 1; return; }
	XEvent e = { 0 };
	e.xclient.type = ClientMessage;
	e.xclient.window = u->win;
	e.xclient.message_type = XInternAtom(dpy, "WM_PROTOCOLS", False);
	e.xclient.format = 32;
	e.xclient.data.l[0] = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
	XSendEvent(dpy, u->win, False, NoEventMask, &e);
}

/* 從 build() 裡面關掉這個視窗（對話窗按了「儲存」之類的按鈕之後）；pm_ui_run 隨後回傳 */
void pm_ui_close(PmUI *u) { ask_close(u); }

/* 問題太長就換行；空一行之後的文字是補充說明（小一點的字）。視窗高度跟著文字（ask_height 先量好） */
enum { ASK_W = 420, ASK_TL = 22, ASK_DL = 19 };

static int ask_text_h(cairo_t *cr, const char *msg, double maxw)
{
	const char *sep = strstr(msg, "\n\n");
	char first[1024];
	snprintf(first, sizeof first, "%.*s", sep ? (int)(sep - msg) : (int)strlen(msg), msg);
	pm_font(cr, 15, 1);
	int h = lines_of(cr, first, maxw) * ASK_TL;
	if (sep) { pm_font(cr, 13, 0); h += 8 + lines_of(cr, sep + 2, maxw) * ASK_DL; }
	return h;
}

static void ask_build(PmUI *u)
{
	ask_ui = u;
	ask_win = u->win;
	const char *sep = strstr(ask_msg, "\n\n");
	char first[1024];
	snprintf(first, sizeof first, "%.*s", sep ? (int)(sep - ask_msg) : (int)strlen(ask_msg), ask_msg);
	double y = u->y + 1;
	pm_font(u->cr, 15, 1);
	pm_rgb(u->cr, c_text);
	y = draw_wrapped(u->cr, first, u->x, y, u->cw, ASK_TL);
	if (sep) {
		pm_font(u->cr, 13, 0);
		pm_rgb(u->cr, c_dim);
		y = draw_wrapped(u->cr, sep + 2, u->x, y + 8, u->cw, ASK_DL);
	}
	u->y = (int)y + 23;
	int r = pm_ui_buttons(u, ask_btns, ask_n, ask_def);
	if (r < 0 && u->key == XK_Escape) r = ask_cancel;
	if (r >= 0) { ask_result = r; ask_close(u); }
}

int pm_ask(const char *title, const char *wmclass, cairo_surface_t *(*icon)(int size),
           const char *msg, const char *const *buttons, int n, int def, int cancel)
{
	ask_msg = msg; ask_btns = buttons; ask_n = n; ask_def = def; ask_cancel = cancel;
	ask_result = cancel;
	ask_ui = NULL;
	ask_win = 0;
	/* 先量文字有幾行（內容寬度＝視窗寬減左右邊界） */
	cairo_surface_t *ms = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
	cairo_t *mcr = cairo_create(ms);
	int th = ask_text_h(mcr, msg, ASK_W - 2 * 24);
	cairo_destroy(mcr);
	cairo_surface_destroy(ms);
	pm_ui_run(title, wmclass, ASK_W, 126 + MAX(0, th - ASK_TL), ask_build, NULL, 0, icon);
	if (ask_ui && ask_win) { XDestroyWindow(dpy, ask_win); XFlush(dpy); }   /* 原生 Wayland 的視窗 pm_ui_run 自己收掉了 */
	return ask_result;
}

void pm_ui_info(PmUI *u, const char *label, const char *value)
{
	u->id++;
	int y = row(u, ROW);
	label_left(u, y, ROW, label);
	pm_font(u->cr, pm_size, 0);
	char buf[512];
	pm_ellipsize(u->cr, value ? value : "", u->cw * 0.6, buf, sizeof buf);
	pm_rgb(u->cr, c_dim);
	pm_text(u->cr, u->x + u->cw - PAD - pm_textw(u->cr, buf), y + ROW / 2.0, buf);
}

void pm_ui_graph(PmUI *u, const char *label, const char *const *legend, const double *const *data,
                 const unsigned *colors, int nseries, int n, double max)
{
	u->id++;
	int h = 150, y = row(u, h);
	cairo_t *cr = u->cr;
	if (label) {
		pm_font(cr, 12.5, 1);
		pm_rgb(cr, c_text);
		pm_text(cr, u->x + PAD, y + 18, pm_tr(label));
	}
	if (legend) {   /* 由右往左排：● 說明 */
		pm_font(cr, 12, 0);
		double lx = u->x + u->cw - PAD;
		for (int i = nseries - 1; i >= 0; i--) {
			if (!legend[i]) continue;
			lx -= pm_textw(cr, legend[i]);
			pm_rgb(cr, c_dim);
			pm_text(cr, lx, y + 18, legend[i]);
			cairo_arc(cr, lx - 9, y + 18, 4, 0, 2 * M_PI);
			pm_rgb(cr, colors[i]);
			cairo_fill(cr);
			lx -= 28;
		}
	}
	double gx = u->x + PAD, gy = y + 34, gw = u->cw - 2 * PAD, gh = h - 46;
	if (max <= 0) {   /* 自動縮放：取最大值再留一點空間 */
		for (int s = 0; s < nseries; s++)
			for (int i = 0; i < n; i++) max = MAX(max, data[s][i]);
		max = max > 0 ? max * 1.15 : 1;
	}
	pm_rrect(cr, gx, gy, gw, gh, 6, 6);
	pm_rgb(cr, pm_dark ? 0x1c1c1e : 0xf5f5f7);
	cairo_fill(cr);
	/* 格線 */
	pm_rgb(cr, c_line);
	for (int k = 1; k < 4; k++) {
		cairo_rectangle(cr, gx, (int)(gy + gh * k / 4), gw, 1);
		cairo_fill(cr);
	}
	cairo_save(cr);
	pm_rrect(cr, gx, gy, gw, gh, 6, 6);
	cairo_clip(cr);
	for (int s = 0; s < nseries; s++) {
		if (n < 2) break;
		cairo_new_path(cr);
		for (int i = 0; i < n; i++) {
			double v = MIN(MAX(data[s][i], 0), max);
			double x = gx + gw * i / (n - 1), yy = gy + gh - gh * v / max;
			if (i) cairo_line_to(cr, x, yy); else cairo_move_to(cr, x, yy);
		}
		cairo_path_t *line = cairo_copy_path(cr);
		cairo_line_to(cr, gx + gw, gy + gh);
		cairo_line_to(cr, gx, gy + gh);
		cairo_close_path(cr);
		pm_rgba(cr, colors[s], 0.18);
		cairo_fill(cr);
		cairo_append_path(cr, line);
		cairo_path_destroy(line);
		pm_rgb(cr, colors[s]);
		cairo_set_line_width(cr, 1.6);
		cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
		cairo_stroke(cr);
	}
	cairo_restore(cr);
}

void pm_ui_meter(PmUI *u, const char *label, const char *value, double frac, unsigned color)
{
	u->id++;
	int y = row(u, 36);
	cairo_t *cr = u->cr;
	frac = MIN(MAX(frac, 0), 1);
	pm_font(cr, 12.5, 0);
	pm_rgb(cr, c_text);
	if (label) pm_text(cr, u->x + PAD, y + 18, pm_tr(label));
	double vw = 0;
	if (value) {
		vw = pm_textw(cr, value);
		pm_rgb(cr, c_dim);
		pm_text(cr, u->x + u->cw - PAD - vw, y + 18, value);
	}
	double bx = u->x + PAD + MIN(150, u->cw / 4), bw = u->x + u->cw - PAD - vw - 14 - bx;
	if (bw < 20) return;
	pm_rrect(cr, bx, y + 14, bw, 8, 4, 4);
	pm_rgb(cr, c_ctrl);
	cairo_fill(cr);
	if (frac > 0) {
		pm_rrect(cr, bx, y + 14, MAX(8, bw * frac), 8, 4, 4);
		pm_rgb(cr, color);
		cairo_fill(cr);
	}
}

void pm_ui_bars(PmUI *u, const char *label, const double *v, const char *const *labels, int n, unsigned color, unsigned hot)
{
	u->id++;
	int h = label ? 112 : 90, y = row(u, h);
	cairo_t *cr = u->cr;
	if (label) {
		pm_font(cr, 12.5, 1);
		pm_rgb(cr, c_text);
		pm_text(cr, u->x + PAD, y + 18, pm_tr(label));
	}
	if (n < 1) return;
	double top = y + (label ? 34 : 12), bh = 52, gx = u->x + PAD, gw = u->cw - 2 * PAD;
	double step = gw / n, bw = MAX(2.0, MIN(22.0, step * 0.7));
	pm_font(cr, 10, 0);
	/* 編號只在放得下時顯示，太擠就每隔幾個顯示一次 */
	int every = 1;
	while (every < n && step * every < 22) every *= 2;
	for (int i = 0; i < n; i++) {
		double x = gx + step * i + (step - bw) / 2, f = MIN(MAX(v[i], 0), 1);
		pm_rrect(cr, x, top, bw, bh, 3, 3);
		pm_rgb(cr, c_ctrl);
		cairo_fill(cr);
		if (f > 0.005) {
			double fh = MAX(3.0, bh * f);
			pm_rrect(cr, x, top + bh - fh, bw, fh, 3, 3);
			pm_rgb(cr, f > 0.85 ? hot : color);
			cairo_fill(cr);
		}
		if (labels || i % every == 0) {
			char t[32];
			if (labels) snprintf(t, sizeof t, "%s", labels[i]);
			else snprintf(t, sizeof t, "%d", i + 1);
			pm_rgb(cr, c_dim);
			pm_text(cr, x + bw / 2 - pm_textw(cr, t) / 2, top + bh + 11, t);
		}
	}
}

void pm_ui_image(PmUI *u, cairo_surface_t *img, int w, int h)
{
	u->id++;
	int y = row(u, h + 2 * PAD);
	double x = u->x + (u->cw - w) / 2.0;
	cairo_t *cr = u->cr;
	pm_rrect(cr, x, y + PAD, w, h, 8, 8);
	if (!img) {
		pm_rgb(cr, pm_dark ? 0x1c1c1e : 0x2c2c2e);
		cairo_fill(cr);
		return;
	}
	cairo_save(cr);
	cairo_clip(cr);
	/* 等比例填滿（多出來的裁掉） */
	double iw = cairo_image_surface_get_width(img), ih = cairo_image_surface_get_height(img);
	double k = MAX(w / iw, h / ih);
	cairo_translate(cr, x + (w - iw * k) / 2, y + PAD + (h - ih * k) / 2);
	cairo_scale(cr, k, k);
	cairo_set_source_surface(cr, img, 0, 0);
	cairo_paint(cr);
	cairo_restore(cr);
}

/* 圖示快取（找不到的也記下來，避免每次重畫都去搜尋） */
static struct { char name[96]; cairo_surface_t *s; } icon_cache[96];
static int nicon_cache;

void pm_ui_add_icon(const char *name, cairo_surface_t *s)
{
	if (nicon_cache >= (int)LEN(icon_cache)) return;
	snprintf(icon_cache[nicon_cache].name, sizeof icon_cache[0].name, "%s", name);
	icon_cache[nicon_cache++].s = s;
}

static cairo_surface_t *item_icon(const char *name)
{
	for (int i = 0; i < nicon_cache; i++)
		if (!strcmp(icon_cache[i].name, name)) return icon_cache[i].s;
	cairo_surface_t *s = name[0] == '@' ? NULL : pm_icon_load(name, 56);
	if (nicon_cache < (int)LEN(icon_cache)) {
		snprintf(icon_cache[nicon_cache].name, sizeof icon_cache[0].name, "%s", name);
		icon_cache[nicon_cache++].s = s;
	}
	return s;
}

static int rclick, rclick_x, rclick_y;
static int item_y, item_h;   /* 剛畫的那一列（pm_ui_item_rclick 用） */
void (*pm_ui_on_drop)(const char *path);

/* 把檔案從這個視窗拖出去（原生 Wayland 才有；X11 回傳 0，那邊的程式用自己的 XDND）。要在剛按下滑鼠之後呼叫 */
int pm_ui_drag_files(PmUI *u, const char *const *paths, int n, cairo_surface_t *icon, int hot_x, int hot_y)
{
	return u && u->wl ? pmwl_drag_start_files(u->wl, paths, n, PMWL_DND_COPY | PMWL_DND_MOVE, icon, hot_x, hot_y) : 0;
}

/* 滾輪（自己畫畫面的程式用）與右鍵位置：只新增，不改既有行為 */
int pm_ui_wheel_steps;
int pm_ui_rclick_at(PmUI *u, int *x, int *y)
{
	(void)u;
	if (!rclick) return 0;
	if (x) *x = rclick_x;
	if (y) *y = rclick_y;
	rclick = 0;
	return 1;
}

int pm_ui_item_rclick(PmUI *u)
{
	if (!rclick) return 0;
	if (rclick_y < item_y || rclick_y >= item_y + item_h || rclick_x < u->x || rclick_x >= u->x + u->cw) return 0;
	u->mx = rclick_x + 22; u->my = rclick_y + 12;   /* pm_ui_menu 會往左上退 (20, 10)：選單左上角在游標右下 2 px，鬆開右鍵不會馬上選到第一項 */
	rclick = 0;
	return 1;
}

int pm_ui_item(PmUI *u, const char *icon, const char *title, const char *sub, const char *right, int checked)
{
	u->id++;
	int h = sub ? 52 : ROW;
	int y = row(u, h);
	item_y = y; item_h = h;
	int hit = clicked_in(u, u->x, y, u->cw, h);
	double tx = u->x + PAD;
	if (icon) {
		cairo_surface_t *s = item_icon(icon);
		if (s) {
			cairo_save(u->cr);
			cairo_translate(u->cr, tx, y + (h - 28) / 2.0);
			cairo_scale(u->cr, 0.5, 0.5);
			cairo_set_source_surface(u->cr, s, 0, 0);
			cairo_paint(u->cr);
			cairo_restore(u->cr);
		}
		tx += 38;
	}
	double rw = 0;
	char buf[256];
	if (right) {
		pm_font(u->cr, 12.5, 0);
		pm_ellipsize(u->cr, right, u->cw / 3, buf, sizeof buf);
		rw = pm_textw(u->cr, buf) + 8;
		pm_rgb(u->cr, c_dim);
		pm_text(u->cr, u->x + u->cw - PAD - (checked ? 22 : 0) - pm_textw(u->cr, buf), y + h / 2.0, buf);
	}
	if (checked) {
		double cx = u->x + u->cw - PAD - 8, cy = y + h / 2.0;
		pm_rgb(u->cr, T->sel);
		cairo_set_line_width(u->cr, 2.2);
		cairo_set_line_cap(u->cr, CAIRO_LINE_CAP_ROUND);
		cairo_move_to(u->cr, cx - 6, cy);
		cairo_line_to(u->cr, cx - 2, cy + 4);
		cairo_line_to(u->cr, cx + 6, cy - 5);
		cairo_stroke(u->cr);
		rw += 22;
	}
	pm_font(u->cr, pm_size, 0);
	pm_rgb(u->cr, c_text);
	pm_ellipsize(u->cr, title, u->x + u->cw - PAD - rw - tx, buf, sizeof buf);
	pm_text(u->cr, tx, sub ? y + 17 : y + h / 2.0, buf);
	if (sub) {
		pm_font(u->cr, 12, 0);
		pm_rgb(u->cr, c_dim);
		pm_ellipsize(u->cr, sub, u->x + u->cw - PAD - rw - tx, buf, sizeof buf);
		pm_text(u->cr, tx, y + 36, buf);
	}
	return hit;
}

/* ---- 健康檢查的一列狀態（pomelo-check.h）：燈號、結論、說明、建議、修正按鈕；文字太長自動換行 ---- */

static int lines_of(cairo_t *cr, const char *p, double maxw)
{
	int n = 0;
	while (*p) {
		size_t l = strcspn(p, "\n"), cut = pm_text_fit(cr, p, l, maxw);
		if (!cut) cut = l ? 1 : 0;
		n++;
		p += cut;
		if (*p == '\n') p++;
		else while (*p == ' ') p++;
	}
	return n;
}

static double draw_wrapped(cairo_t *cr, const char *p, double x, double y, double maxw, double lh)
{
	char line[1024];
	while (*p) {
		size_t l = strcspn(p, "\n"), cut = pm_text_fit(cr, p, l, maxw);
		if (!cut) cut = l ? 1 : 0;
		snprintf(line, sizeof line, "%.*s", (int)cut, p);
		pm_text(cr, x, y + lh / 2, line);
		y += lh;
		p += cut;
		if (*p == '\n') p++;
		else while (*p == ' ') p++;
	}
	return y;
}

static const unsigned status_col[] = { 0x34c759, 0x0a84ff, 0xff9f0a, 0xff3b30, 0x8e8e93 };

/* 燈號：圓點裡畫 ✓、i、!、×、–（r＝半徑） */
static void status_dot(cairo_t *cr, double cx, double cy, double r, int level)
{
	double k = r / 9;
	cairo_new_path(cr);
	cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
	pm_rgb(cr, status_col[level]);
	cairo_fill(cr);
	pm_rgb(cr, 0xffffff);
	cairo_set_line_width(cr, 2 * k);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	if (level == PM_CHK_OK) { cairo_move_to(cr, cx - 4 * k, cy); cairo_line_to(cr, cx - 1 * k, cy + 3 * k); cairo_line_to(cr, cx + 4 * k, cy - 3 * k); }
	else if (level == PM_CHK_INFO) { cairo_move_to(cr, cx, cy - 1 * k); cairo_line_to(cr, cx, cy + 4 * k); cairo_move_to(cr, cx, cy - 4.5 * k); cairo_line_to(cr, cx, cy - 4 * k); }
	else if (level == PM_CHK_WARN) { cairo_move_to(cr, cx, cy - 5 * k); cairo_line_to(cr, cx, cy + 1 * k); cairo_move_to(cr, cx, cy + 4.5 * k); cairo_line_to(cr, cx, cy + 5 * k); }
	else if (level == PM_CHK_BAD) { cairo_move_to(cr, cx - 3.5 * k, cy - 3.5 * k); cairo_line_to(cr, cx + 3.5 * k, cy + 3.5 * k); cairo_move_to(cr, cx + 3.5 * k, cy - 3.5 * k); cairo_line_to(cr, cx - 3.5 * k, cy + 3.5 * k); }
	else { cairo_move_to(cr, cx - 4 * k, cy); cairo_line_to(cr, cx + 4 * k, cy); }
	cairo_stroke(cr);
}

/* 圓角按鈕；primary＝藍底白字 */
static int round_button(PmUI *u, double x, double y, double w, double h, const char *label, int primary)
{
	int hit = clicked_in(u, x, y, w, h);
	pm_rrect(u->cr, x, y, w, h, 7, 7);
	pm_rgb(u->cr, primary || hit ? T->sel : c_ctrl);
	cairo_fill(u->cr);
	pm_font(u->cr, 12.5, primary);
	pm_rgb(u->cr, primary || hit ? 0xffffff : c_text);
	pm_text(u->cr, x + (w - pm_textw(u->cr, label)) / 2, y + h / 2, label);
	return hit;
}

int pm_ui_status(PmUI *u, int level, const char *title, const char *detail, const char *advice, const char *button)
{
	u->id++;
	if (level < 0 || level > 4) level = 4;
	cairo_t *cr = u->cr;
	double bw = 0;
	if (button) { pm_font(cr, 12.5, 1); bw = pm_textw(cr, button) + 30; }
	double tx = u->x + PAD + 32, maxw = u->x + u->cw - PAD - (bw ? bw + 14 : 0) - tx;
	double amaxw = u->x + u->cw - PAD - tx - 20;   /* 建議的框可以寬到右邊 */
	enum { TL = 21, DL = 17 };
	char adv[1200] = "";
	if (advice && *advice) snprintf(adv, sizeof adv, "%s%s", pm_tr("建議："), advice);
	pm_font(cr, pm_size + 0.5, 1);
	int nt = lines_of(cr, title ? title : "", maxw);
	pm_font(cr, 12, 0);
	int nd = detail && *detail ? lines_of(cr, detail, maxw) : 0;
	int na = adv[0] ? lines_of(cr, adv, amaxw) : 0;
	int h = 13 + nt * TL + (nd ? 3 + nd * DL : 0) + (na ? 9 + na * DL + 12 : 0) + 12;
	if (h < ROW) h = ROW;
	int y = row(u, h);
	/* 有問題、要注意的左邊一條顏色 */
	if (level == PM_CHK_BAD || level == PM_CHK_WARN) {
		pm_rgb(cr, status_col[level]);
		cairo_rectangle(cr, u->x, y + 1, 4, h - 1);
		cairo_fill(cr);
	}
	status_dot(cr, u->x + PAD + 11, y + 13 + TL / 2.0, 10, level);
	double ty = y + 13;
	pm_font(cr, pm_size + 0.5, 1);
	pm_rgb(cr, c_text);
	ty = draw_wrapped(cr, title ? title : "", tx, ty, maxw, TL);
	pm_font(cr, 12, 0);
	if (nd) { ty += 3; pm_rgb(cr, c_dim); ty = draw_wrapped(cr, detail, tx, ty, maxw, DL); }
	if (na) {
		/* 建議：淡藍色的框 */
		ty += 9;
		double bh = na * DL + 12;
		pm_rrect(cr, tx - 8, ty, amaxw + 16, bh, 7, 7);
		pm_rgba(cr, T->sel, pm_dark ? 0.18 : 0.08);
		cairo_fill(cr);
		pm_rgb(cr, c_text);
		draw_wrapped(cr, adv, tx + 2, ty + 6, amaxw, DL);
	}
	int hit = 0;
	if (button) {
		double bh = 28, bx = u->x + u->cw - PAD - bw, by = y + 13 + (TL - bh) / 2;
		hit = round_button(u, bx, by, bw, bh, button, level >= PM_CHK_WARN);
	}
	return hit;
}

/* 大的狀態卡（健康檢查的總覽）：左邊彩色圓圈、大標題、說明；右邊可以有按鈕 */
int pm_ui_hero(PmUI *u, int level, const char *title, const char *sub, const char *button)
{
	u->id++;
	if (level < 0 || level > 4) level = 4;
	cairo_t *cr = u->cr;
	if (!u->group_open) pm_ui_group(u, NULL);
	int h = 96, y = row(u, h);
	double cx = u->x + PAD + 32, cy = y + h / 2.0;
	/* 外圈淡色、內圈實心 */
	cairo_new_path(cr);
	cairo_arc(cr, cx, cy, 31, 0, 2 * M_PI);
	pm_rgba(cr, status_col[level], 0.18);
	cairo_fill(cr);
	status_dot(cr, cx, cy, 22, level);
	double bw = 0;
	if (button) { pm_font(cr, 12.5, 1); bw = pm_textw(cr, button) + 32; }
	double tx = cx + 48, maxw = u->x + u->cw - PAD - (bw ? bw + 14 : 0) - tx;
	char buf[300];
	pm_font(cr, 19, 1);
	pm_rgb(cr, c_text);
	pm_ellipsize(cr, title ? title : "", maxw, buf, sizeof buf);
	pm_text(cr, tx, sub && *sub ? cy - 11 : cy, buf);
	if (sub && *sub) {
		pm_font(cr, 12.5, 0);
		pm_rgb(cr, c_dim);
		pm_ellipsize(cr, sub, maxw, buf, sizeof buf);
		pm_text(cr, tx, cy + 15, buf);
	}
	int hit = 0;
	if (button) hit = round_button(u, u->x + u->cw - PAD - bw, cy - 15, bw, 30, button, 0);
	pm_ui_end(u);
	return hit;
}

/* 圖示卡片格（例如健康檢查的八類）：每張卡片有圖示、名稱、彩色的狀態字。點了回傳 index，否則 -1 */
int pm_ui_tiles(PmUI *u, const char *const *icons, const char *const *titles, const char *const *subs, const int *levels, int n, int cols)
{
	u->id++;
	if (u->group_open) pm_ui_end(u);
	cairo_t *cr = u->cr;
	if (cols < 1) cols = 1;
	enum { TH = 72, GAP = 12 };
	double tw = (u->cw - (cols - 1) * GAP) / (double)cols;
	int hit = -1;
	for (int i = 0; i < n; i++) {
		double x = u->x + (i % cols) * (tw + GAP), y = u->y + (i / cols) * (TH + GAP);
		if (clicked_in(u, x, y, tw, TH)) hit = i;
		pm_rrect(cr, x, y, tw, TH, 12, 12);
		pm_rgb(cr, c_card);
		cairo_fill(cr);
		cairo_surface_t *ic = icons && icons[i] ? item_icon(icons[i]) : NULL;
		double tx = x + 16;
		if (ic) {
			cairo_save(cr);
			cairo_translate(cr, x + 14, y + (TH - 40) / 2.0);
			cairo_scale(cr, 40.0 / 56, 40.0 / 56);
			cairo_set_source_surface(cr, ic, 0, 0);
			cairo_paint(cr);
			cairo_restore(cr);
			tx = x + 66;
		}
		char buf[200];
		double maxw = x + tw - 14 - tx;
		pm_font(cr, pm_size + 0.5, 1);
		pm_rgb(cr, c_text);
		pm_ellipsize(cr, titles[i], maxw, buf, sizeof buf);
		pm_text(cr, tx, y + TH / 2.0 - 10, buf);
		if (subs && subs[i]) {
			int lv = levels ? levels[i] : PM_CHK_SKIP;
			if (lv < 0 || lv > 4) lv = 4;
			double dx = tx;
			if (levels) {
				cairo_new_path(cr);
				cairo_arc(cr, tx + 4, y + TH / 2.0 + 12, 4, 0, 2 * M_PI);
				pm_rgb(cr, status_col[lv]);
				cairo_fill(cr);
				dx += 13;
			}
			pm_font(cr, 12.5, 0);
			pm_rgb(cr, lv == PM_CHK_BAD || lv == PM_CHK_WARN ? status_col[lv] : c_dim);
			pm_ellipsize(cr, subs[i], x + tw - 14 - dx, buf, sizeof buf);
			pm_text(cr, dx, y + TH / 2.0 + 12, buf);
		}
	}
	u->y += ((n + cols - 1) / cols) * (TH + GAP) + 8;
	return hit;
}

/* 焦點在密碼欄時不交給輸入法：不然注音會把按鍵吃掉（密碼欄一片空白），或把中文打進密碼裡，使用者自己都不知道密碼是什麼 */
static int focus_secret;

/* ---- 剪貼簿（輸入框的 Ctrl+C／V／X 與右鍵選單用）：原生 Wayland 交給 wl.c，X11 在這裡 ---- */
static char *xclip_text;   /* 我們放上 CLIPBOARD 的文字（X11：程式還開著時別的程式來要，由主迴圈回應） */
static Atom a_clip, a_utf8, a_targets, a_xclip;

static void xclip_atoms(void)
{
	if (a_clip) return;
	a_clip = XInternAtom(dpy, "CLIPBOARD", False);
	a_utf8 = XInternAtom(dpy, "UTF8_STRING", False);
	a_targets = XInternAtom(dpy, "TARGETS", False);
	a_xclip = XInternAtom(dpy, "_POMELO_CLIP", False);
}

/* 別的程式要我們放的文字 */
static void xclip_answer(XSelectionRequestEvent *r)
{
	xclip_atoms();
	XSelectionEvent se = { .type = SelectionNotify, .requestor = r->requestor, .selection = r->selection,
	                       .target = r->target, .property = None, .time = r->time };
	Atom prop = r->property ? r->property : r->target;
	if (xclip_text && r->selection == a_clip) {
		if (r->target == a_targets) {
			Atom t[] = { a_targets, a_utf8, XA_STRING };
			XChangeProperty(dpy, r->requestor, prop, XA_ATOM, 32, PropModeReplace, (unsigned char *)t, LEN(t));
			se.property = prop;
		} else if (r->target == a_utf8 || r->target == XA_STRING) {
			XChangeProperty(dpy, r->requestor, prop, r->target, 8, PropModeReplace,
			                (unsigned char *)xclip_text, (int)strlen(xclip_text));
			se.property = prop;
		}
	}
	XSendEvent(dpy, r->requestor, False, NoEventMask, (XEvent *)&se);
}

static void clip_set(Window w, PmWl *wl, const char *text)
{
	if (wl) { pmwl_clip_set(text); return; }
	xclip_atoms();
	free(xclip_text);
	xclip_text = strdup(text);
	XSetSelectionOwner(dpy, a_clip, w, CurrentTime);
}

/* 剪貼簿的文字（呼叫者 free）；別的程式最多等一秒 */
static char *clip_get(Window w, PmWl *wl)
{
	if (wl) return pmwl_clip_get();
	xclip_atoms();
	if (XGetSelectionOwner(dpy, a_clip) == w) return xclip_text ? strdup(xclip_text) : NULL;
	Atom targets[] = { a_utf8, XA_STRING };   /* 舊程式只給 STRING */
	for (unsigned k = 0; k < LEN(targets); k++) {
		XConvertSelection(dpy, a_clip, targets[k], a_xclip, w, CurrentTime);
		XFlush(dpy);
		XEvent ev;
		int got = 0;
		for (long until = pm_now_ms() + 1000; !got && pm_now_ms() < until; ) {
			if (XCheckTypedWindowEvent(dpy, w, SelectionNotify, &ev)) got = 1;
			else usleep(5000);
		}
		if (!got) return NULL;
		if (ev.xselection.property == None) continue;
		Atom type;
		int fmt;
		unsigned long nitems, after;
		unsigned char *data = NULL;
		char *r = NULL;
		if (XGetWindowProperty(dpy, w, a_xclip, 0, 1 << 18, True, AnyPropertyType, &type, &fmt, &nitems, &after, &data) == Success && data) {
			if (fmt == 8 && type != XInternAtom(dpy, "INCR", False)) r = strndup((char *)data, nitems);
			XFree(data);
		}
		return r;
	}
	return NULL;
}

/* 貼到單行的輸入框：換行、Tab 變成空白，結尾的空白與換行拿掉（從網頁複製密碼常常多帶一個換行） */
static void paste_into(char *buf, size_t n, const char *text)
{
	if (!text) return;
	size_t len = strlen(buf), tl = strlen(text);
	while (tl && (text[tl - 1] == '\n' || text[tl - 1] == '\r' || text[tl - 1] == ' ' || text[tl - 1] == '\t')) tl--;
	for (size_t i = 0; i < tl && len + 1 < n; i++) {
		char c = text[i];
		if (c == '\r' && text[i + 1] == '\n') continue;
		buf[len++] = c == '\n' || c == '\r' || c == '\t' ? ' ' : c;
	}
	while (len && ((unsigned char)buf[len - 1] & 0xc0) == 0xc0) len--;   /* 裝不下時不要切在 UTF-8 字元中間 */
	if (len && ((unsigned char)buf[len - 1] & 0x80)) {
		size_t st = len - 1;
		while (st && ((unsigned char)buf[st] & 0xc0) == 0x80) st--;
		unsigned char lead = (unsigned char)buf[st];
		size_t need = lead >= 0xf0 ? 4 : lead >= 0xe0 ? 3 : lead >= 0xc0 ? 2 : 1;
		if (len - st < need) len = st;
	}
	buf[len] = 0;
}

/* 右鍵：輸入框的選單（剪下、複製、貼上、全部清除） */
static PmUI *em_u;
static char *em_buf;
static size_t em_n;
static void em_cut(const Arg *a) { (void)a; clip_set(em_u->win, em_u->wl, em_buf); em_buf[0] = 0; }
static void em_copy(const Arg *a) { (void)a; clip_set(em_u->win, em_u->wl, em_buf); }
static void em_paste(const Arg *a) { (void)a; char *t = clip_get(em_u->win, em_u->wl); paste_into(em_buf, em_n, t); free(t); }
static void em_clear(const Arg *a) { (void)a; em_buf[0] = 0; }

static void entry_menu(PmUI *u, char *buf, size_t n, int secret, int x, int y)
{
	em_u = u; em_buf = buf; em_n = n;
	MenuItem m[5];
	int k = 0, has = buf[0] != 0;
	m[k++] = (MenuItem){ _("剪下"), has && !secret ? em_cut : pm_menu_off, { 0 } };   /* 密碼不能複製出去 */
	m[k++] = (MenuItem){ _("複製"), has && !secret ? em_copy : pm_menu_off, { 0 } };
	m[k++] = (MenuItem){ _("貼上"), em_paste, { 0 } };
	m[k++] = (MenuItem){ NULL, NULL, { 0 } };
	m[k++] = (MenuItem){ _("全部清除"), has ? em_clear : pm_menu_off, { 0 } };
	if (u->wl) {
		pmwl_menu(u->wl, m, k, x, y);
	} else {
		int rx, ry;
		Window child;
		XTranslateCoordinates(dpy, u->win, pm_root, x, y, &rx, &ry, &child);
		pm_popup(m, k, rx, ry);
	}
}

static int entry_impl(PmUI *u, const char *label, const char *hint, char *buf, size_t n, int secret, int autofocus)
{
	int id = ++u->id;
	/* 主要輸入框（例如對話視窗）：一律保持焦點。
	 * 不能只在 focus 為 0 時才設定：前面的內容變多時元件編號會變，記住的編號會指到別的元件 */
	if (autofocus) { u->focus = id; main_input_id = id; main_input_has_text = buf[0] != 0; }
	int y = row(u, ROW);
	label_left(u, y, ROW, label);
	double bw = label ? MIN(280, u->cw / 2) : u->cw - 2 * PAD, bh = 28;
	double bx = u->x + u->cw - PAD - bw, by = y + (ROW - bh) / 2;
	if (clicked_in(u, bx, by, bw, bh) ||
	    (rclick && rclick_x >= bx && rclick_x < bx + bw && rclick_y >= by && rclick_y < by + bh)) {
		u->focus = id;
		focus_secret = secret;
		if (u->xic) { if (secret) XUnsetICFocus(u->xic); else XSetICFocus(u->xic); }
		if (rclick) {
			rclick = 0;
			entry_menu(u, buf, n, secret, rclick_x, rclick_y);
			u->again = 1;
		}
	}
	int enter = 0;
	if (u->focus == id && (u->key || u->keylen)) {
		size_t len = strlen(buf);
		int ctrl = u->keystate & ControlMask;
		KeySym lk = u->key >= XK_A && u->key <= XK_Z ? u->key + (XK_a - XK_A) : u->key;
		if ((ctrl && lk == XK_v) || ((u->keystate & ShiftMask) && u->key == XK_Insert)) {   /* 貼上 */
			char *t = clip_get(u->win, u->wl);
			paste_into(buf, n, t);
			free(t);
		} else if (ctrl && (lk == XK_c || lk == XK_x)) {   /* 複製、剪下：整段文字（密碼不行） */
			if (!secret && buf[0]) {
				clip_set(u->win, u->wl, buf);
				if (lk == XK_x) buf[0] = 0;
			}
		} else if (u->key == XK_Return || u->key == XK_KP_Enter) {
			enter = 1;
		} else if (u->key == XK_BackSpace) {
			while (len && ((unsigned char)buf[len - 1] & 0xc0) == 0x80) buf[--len] = 0;
			if (len) buf[--len] = 0;
		} else if (u->key == XK_Escape) {
			if (autofocus) { buf[0] = 0; main_input_has_text = 0; }   /* 主要輸入框：Esc 清掉文字，再按一次由主迴圈關閉 */
			else u->focus = 0;
		} else if (u->keylen > 0 && (unsigned char)u->keybuf[0] >= 32 && u->keybuf[0] != 127 &&
		           !(u->keystate & ControlMask) && len + u->keylen < n) {
			memcpy(buf + len, u->keybuf, u->keylen);
			buf[len + u->keylen] = 0;
		}
		u->key = 0;
		u->keylen = 0;
	}
	pm_rrect(u->cr, bx, by, bw, bh, 6, 6);
	pm_rgb(u->cr, pm_dark ? 0x1c1c1e : 0xffffff);
	cairo_fill_preserve(u->cr);
	pm_rgb(u->cr, u->focus == id ? T->sel : c_line);
	cairo_set_line_width(u->cr, u->focus == id ? 2 : 1);
	cairo_stroke(u->cr);
	char shown[1024];
	if (secret) {
		size_t k = 0;
		for (const char *p = buf; *p && k + 4 < sizeof shown; p++)
			if (((unsigned char)*p & 0xc0) != 0x80) { memcpy(shown + k, "•", 3); k += 3; }
		shown[k] = 0;
	} else {
		snprintf(shown, sizeof shown, "%s", buf);
	}
	pm_font(u->cr, pm_size, 0);
	cairo_save(u->cr);
	cairo_rectangle(u->cr, bx + 4, by, bw - 8, bh);
	cairo_clip(u->cr);
	double tw = pm_textw(u->cr, shown), shift = MAX(0, tw - (bw - 20));
	pm_rgb(u->cr, c_text);
	pm_text(u->cr, bx + 8 - shift, by + bh / 2, shown);
	if (!buf[0] && hint) {   /* 空的時候顯示提示文字 */
		pm_rgb(u->cr, c_dim);
		pm_text(u->cr, bx + 12, by + bh / 2, pm_tr(hint));
	}
	if (u->focus == id) {
		pm_rgb(u->cr, T->sel);
		cairo_rectangle(u->cr, bx + 9 + tw - shift, by + 6, 1.5, bh - 12);
		cairo_fill(u->cr);
		ime_id = id; ime_secret = secret;
		ime_x = (int)(bx + 9 + tw - shift); ime_y = (int)by; ime_h = (int)bh;
	}
	cairo_restore(u->cr);
	return enter;
}

int pm_ui_entry(PmUI *u, const char *label, char *buf, size_t n, int secret)
{
	return entry_impl(u, label, NULL, buf, n, secret, 0);
}

int pm_ui_input(PmUI *u, const char *hint, char *buf, size_t n)
{
	return entry_impl(u, NULL, hint, buf, n, 0, 1);
}

/* 把文字依寬度切成好幾行（斷在哪裡見 pm_text_fit），回傳行數 */
static int wrap_text(cairo_t *cr, const char *text, double maxw, char lines[][400], int max)
{
	int nl = 0;
	const char *p = text;
	while (*p && nl < max) {
		size_t l = strcspn(p, "\n"), cut = pm_text_fit(cr, p, MIN(l, (size_t)399), maxw);
		snprintf(lines[nl++], 400, "%.*s", (int)cut, p);
		p += cut;
		if (*p == '\n') p++;
		else while (*p == ' ') p++;
	}
	return nl;
}

void pm_ui_bubble(PmUI *u, const char *text, int mine)
{
	u->id++;
	if (u->group_open) pm_ui_end(u);
	static char lines[64][400];
	pm_font(u->cr, pm_size, 0);
	double maxw = u->cw * 0.78 - 24;
	int nl = wrap_text(u->cr, text, maxw, lines, 64);
	double w = 0;
	for (int i = 0; i < nl; i++) w = MAX(w, pm_textw(u->cr, lines[i]));
	double lh = pm_size + 7, bw = w + 24, bh = nl * lh + 16;
	double bx = mine ? u->x + u->cw - bw : u->x, by = u->y;
	pm_rrect(u->cr, bx, by, bw, bh, 14, 14);
	pm_rgb(u->cr, mine ? T->sel : c_card);
	cairo_fill(u->cr);
	pm_rgb(u->cr, mine ? 0xffffff : c_text);
	for (int i = 0; i < nl; i++) pm_text(u->cr, bx + 12, by + 8 + lh * i + lh / 2, lines[i]);
	u->y += (int)bh + 8;
}

int pm_ui_images(PmUI *u, cairo_surface_t **imgs, const char **names, int n, int *v, int tw, int th)
{
	if (u->group_open) pm_ui_end(u);
	int gap = 14, cols = MAX(1, (u->cw + gap) / (tw + gap));
	int lh = names ? 20 : 0, changed = 0;
	double x0 = u->x + (u->cw - (cols * (tw + gap) - gap)) / 2.0;
	for (int i = 0; i < n; i++) {
		u->id++;
		double x = x0 + (i % cols) * (tw + gap), y = u->y + (i / cols) * (th + gap + lh);
		if (clicked_in(u, x, y, tw, th + lh) && *v != i) { *v = i; changed = 1; }
		cairo_save(u->cr);
		pm_rrect(u->cr, x, y, tw, th, 8, 8);
		cairo_clip(u->cr);
		if (imgs[i]) {
			cairo_set_source_surface(u->cr, imgs[i], x, y);
			cairo_paint(u->cr);
		} else {
			pm_rgb(u->cr, c_ctrl);
			cairo_paint(u->cr);
		}
		cairo_restore(u->cr);
		if (i == *v) {
			pm_rrect(u->cr, x - 3, y - 3, tw + 6, th + 6, 10, 10);
			pm_rgb(u->cr, T->sel);
			cairo_set_line_width(u->cr, 3);
			cairo_stroke(u->cr);
		}
		if (names && names[i]) {
			char buf[128];
			pm_font(u->cr, 12, 0);
			pm_ellipsize(u->cr, names[i], tw, buf, sizeof buf);
			pm_rgb(u->cr, c_dim);
			pm_text(u->cr, x + (tw - pm_textw(u->cr, buf)) / 2, y + th + 12, buf);
		}
	}
	u->y += ((n + cols - 1) / cols) * (th + gap + lh) + 10;
	return changed;
}

/* ------------------------------------------------------------------ */
/* 視窗與事件迴圈                                                      */
/* ------------------------------------------------------------------ */

static PmUI *cur_ui;
static PmBuild cur_build;

static void frame(PmUI *u)
{
	for (int pass = 0; pass < 3; pass++) {
		u->again = 0;
		Paint p;
		/* 原生：大小以 compositor 最後給的為準（modal 視窗開著時錯過的改大小也會跟上） */
		u->cr = u->wl ? pmwl_begin(u->wl, &u->w, &u->h) : pm_paint_begin(&p, u->win, u->w, u->h);
		ime_id = 0;
		if (u->wl && pop_mode) {   /* 原生彈出式：圓角外面透明（X 版是 Shape 裁的） */
			cairo_set_operator(u->cr, CAIRO_OPERATOR_CLEAR);
			cairo_paint(u->cr);
			cairo_set_operator(u->cr, CAIRO_OPERATOR_OVER);
			pm_rrect(u->cr, 0, 0, u->w, u->h, 12, 12);
			cairo_clip(u->cr);
		}
		pm_rgb(u->cr, c_bg);
		cairo_paint(u->cr);
		u->id = 0;
		u->claimed = 0;
		u->cw = pop_mode ? u->w - 24 : MIN(u->w - 48, 640);
		u->x = (u->w - u->cw) / 2;
		u->y = (pop_mode ? 12 : 24) - u->scroll;
		u->group_open = 0;
		cur_build(u);
		if (u->group_open) pm_ui_end(u);
		u->content_h = u->y + u->scroll + 8;
		/* 捲軸 */
		if (u->content_h > u->h) {
			double bh = MAX(30.0, (double)u->h * u->h / u->content_h);
			double by = (u->h - bh) * u->scroll / (u->content_h - u->h);
			pm_rrect(u->cr, u->w - 9, by + 2, 6, bh - 4, 3, 3);
			pm_rgba(u->cr, pm_dark ? 0xffffff : 0x000000, 0.25);
			cairo_fill(u->cr);
		}
		if (u->wl && pop_mode) {   /* layer-shell 沒有 compositor 畫的陰影：描一圈細邊，跟後面分得開 */
			pm_rrect(u->cr, 0.5, 0.5, u->w - 1, u->h - 1, 12, 12);
			pm_rgba(u->cr, pm_dark ? 0xffffff : 0x000000, pm_dark ? 0.18 : 0.16);
			cairo_set_line_width(u->cr, 1);
			cairo_stroke(u->cr);
		}
		cairo_t *cr = u->cr;
		if (!u->wl) pm_paint_end(&p, cr, 0, 0);
		u->cr = NULL;
		if (u->clicked && !u->claimed) u->focus = 0;   /* 點到空白處：取消輸入焦點 */
		u->clicked = 0;
		u->key = 0;
		u->keylen = 0;
		if (u->released) { u->active = 0; u->released = 0; }
		/* 彈出式：高度跟著內容；內容比螢幕高時停在螢幕高度（不然每次重畫都會多畫兩遍） */
		int pop_h = pop_mode ? MIN(u->content_h, u->wl ? pop_max_h : sh - pop_top - 10) : u->h;
		if (pop_mode && pop_h != u->h) {
			u->h = pop_h;
			if (u->wl) {
				pmwl_layer_resize(u->wl, u->w, u->h);
			} else {
				XMoveResizeWindow(dpy, u->win, pop_right - u->w, pop_top, u->w, u->h);
				pm_shape_round(u->win, u->w, u->h, 12, 12);
			}
			u->again = 1;
		}
		int maxs = MAX(0, u->content_h - u->h);
		if (u->scroll > maxs) { u->scroll = maxs; u->again = 1; }
		/* 原生 Wayland：還要再畫一次的話這次先不送出（不然一下子用掉三張畫面緩衝） */
		if (u->wl) { if (u->again && pass < 2) pmwl_cancel(u->wl, cr); else pmwl_end(u->wl, cr); }
		if (!u->again) break;
	}
	need_redraw = 0;
	rclick = 0;   /* 沒有點在輸入框上的右鍵 */
	if (u->wl) pmwl_ime(u->wl, u->focus && u->focus == ime_id && !ime_secret, ime_x, ime_y, ime_h);
}

static void apply_conf(void)
{
	pm_size = pm_conf_num("appearance.font_size", 13);
	pm_family = pm_conf_get("appearance.font", "Noto Sans CJK TC");
	pm_icon_theme = pm_conf_get("appearance.icon_theme", "Yaru");
	pm_set_theme(pm_conf_int("appearance.dark", 0), pm_conf_color("appearance.accent", 0));
	ui_colors();
}

static void modal_dispatch(XEvent *ev)
{
	if (ev->type == Expose) need_redraw = 1;
	else if (ev->type == SelectionRequest) xclip_answer(&ev->xselectionrequest);
}

static void set_icon(Window w, cairo_surface_t *(*icon)(int))
{
	int sizes[] = { 32, 64 };
	size_t total = 0;
	for (unsigned i = 0; i < LEN(sizes); i++) total += 2 + (size_t)sizes[i] * sizes[i];
	unsigned long *d = malloc(total * sizeof *d), *p = d;
	for (unsigned i = 0; i < LEN(sizes); i++) {
		int n = sizes[i];
		cairo_surface_t *s = icon(n);
		cairo_surface_flush(s);
		unsigned char *px = cairo_image_surface_get_data(s);
		int stride = cairo_image_surface_get_stride(s);
		*p++ = n; *p++ = n;
		for (int y = 0; y < n; y++)
			for (int x = 0; x < n; x++) {
				uint32_t v = ((uint32_t *)(px + y * stride))[x];
				unsigned a = v >> 24, r = (v >> 16) & 255, g = (v >> 8) & 255, b = v & 255;
				if (a && a < 255) { r = r * 255 / a; g = g * 255 / a; b = b * 255 / a; }
				*p++ = ((unsigned long)a << 24) | (r << 16) | (g << 8) | b;
			}
		cairo_surface_destroy(s);
	}
	XChangeProperty(dpy, w, XInternAtom(dpy, "_NET_WM_ICON", False), XA_CARDINAL, 32, PropModeReplace,
	                (unsigned char *)d, (int)total);
	free(d);
}

/* 原生 Wayland 版的 pm_ui_run：一樣的 build()／tick()，事件來自 wl.c。視窗在這裡就收掉（pm_ask 包在別的視窗裡用也一樣）。
 * 開不了視窗回傳 -1（改走 X11） */
static int ui_run_wl(const char *title, const char *wmclass, int w, int h, PmBuild build,
                     void (*tick)(PmUI *u), int tick_ms)
{
	struct sigaction sa = { .sa_handler = SIG_IGN, .sa_flags = SA_NOCLDWAIT };
	sigaction(SIGCHLD, &sa, NULL);
	pm_conf_load();
	pm_init(pm_conf_get("appearance.font", "Noto Sans CJK TC"), pm_conf_num("appearance.font_size", 13),
	        pm_conf_int("appearance.dark", 0), pm_conf_get("appearance.icon_theme", "Yaru"));
	apply_conf();

	PmUI *u = calloc(1, sizeof *u);
	if (!u) return -1;
	u->w = w; u->h = h; u->native = 1;
	PmWl *parent = wl_modal ? wl_modal : cur_ui ? cur_ui->wl : NULL;   /* pm_ask：掛在叫它的視窗底下 */
	PmWl *catcher = NULL;
	int pop_oi = 0, pop_mt = 0, pop_mr = 0, hidden = 0;
	ui_hide_req = 0;
	if (pop_mode) {
		/* 彈出式（控制中心）：overlay 層、貼螢幕右上角，獨佔鍵盤。下面墊一張全螢幕透明的 overlay：
		 * 點到它（也就是點在外面）就關閉，跟 X 版搶住滑鼠的效果一樣。pop_right／pop_top 是全域座標 */
		Rect o;
		int oi = pmwl_output_at(pop_right - 1, pop_top, &o);
		int mr = oi >= 0 ? o.x + o.w - pop_right : 8, mt = oi >= 0 ? pop_top - o.y : pop_top;
		if (oi >= 0) pop_max_h = o.h - mt - 10;
		catcher = pmwl_layer("pomelo-popover-outside", 3, oi, PMWL_ANCHOR_TOP | PMWL_ANCHOR_BOTTOM | PMWL_ANCHOR_LEFT | PMWL_ANCHOR_RIGHT,
		                     0, 0, 0, 0, 0, 0, -1, 0);
		u->wl = pmwl_layer("pomelo-popover", 3, oi, PMWL_ANCHOR_TOP | PMWL_ANCHOR_RIGHT, w, h, mt, mr, 0, 0, -1, 1);
		if (!u->wl) { pmwl_destroy(catcher); free(u); return -1; }
		pmwl_clear(catcher);
		pop_oi = oi; pop_mt = mt; pop_mr = mr;
	} else if (!(u->wl = pmwl_window(pm_tr(title), wmclass, w, h, MIN(420, w), MIN(300, h), parent))) { free(u); return -1; }
	PmUI *prev_ui = cur_ui;
	PmBuild prev_build = cur_build;
	cur_ui = u;
	cur_build = build;
	need_redraw = 1;
	ui_quit = 0;
	if (pm_ui_on_drop && !pop_mode) {   /* 別的程式拖檔案進來：text/uri-list，每個本機檔案呼叫一次 pm_ui_on_drop */
		static const char *const uri[] = { "text/uri-list" };
		pmwl_dnd_target(u->wl, uri, 1, PMWL_DND_COPY);
	}
	long next_tick = tick ? pm_now_ms() : 0;
	while (!ui_quit) {
		long wait = tick ? MAX(0, next_tick - pm_now_ms()) : -1;
		PmwEvent ev;
		if (pop_mode && ui_hide_req != hidden) {   /* 彈出式收起來／回來：視窗整個拆掉重開（layer 視窗沒有「藏起來」） */
			hidden = ui_hide_req;
			if (hidden) {
				pmwl_destroy(u->wl); pmwl_destroy(catcher);
				u->wl = catcher = NULL;
			} else {
				catcher = pmwl_layer("pomelo-popover-outside", 3, pop_oi, PMWL_ANCHOR_TOP | PMWL_ANCHOR_BOTTOM | PMWL_ANCHOR_LEFT | PMWL_ANCHOR_RIGHT,
				                     0, 0, 0, 0, 0, 0, -1, 0);
				u->wl = pmwl_layer("pomelo-popover", 3, pop_oi, PMWL_ANCHOR_TOP | PMWL_ANCHOR_RIGHT, u->w, u->h, pop_mt, pop_mr, 0, 0, -1, 1);
				if (!u->wl) break;
				pmwl_clear(catcher);
			}
			need_redraw = 1;
		}
		int want_draw = need_redraw && !hidden;
		/* 先把手上的事件都處理完再畫（要重畫時不等） */
		if (pmwl_next(&ev, want_draw ? 0 : wait)) {
			if (catcher && ev.win == catcher) {   /* 彈出式：點在外面就關閉 */
				if (ev.type == PMW_DOWN || ev.type == PMW_CLOSE) ui_quit = 1;
				else if (ev.type == PMW_SIZE) pmwl_clear(catcher);
				continue;
			}
			if (ev.win != u->wl && ev.type != PMW_CONF) continue;
			switch (ev.type) {
			case PMW_EXPOSE: case PMW_FOCUS_IN: need_redraw = 1; break;   /* 拿到焦點：輸入法要重新打開 */
			case PMW_SIZE: u->w = ev.w; u->h = ev.h; need_redraw = 1; break;
			case PMW_CONF: apply_conf(); need_redraw = 1; break;
			case PMW_CLOSE: ui_quit = 1; break;
			case PMW_DND_DROP:
				if (pm_ui_on_drop) {
					char **dp = NULL;
					int dn = pmwl_dnd_files(&dp);
					for (int i = 0; i < dn; i++) pm_ui_on_drop(dp[i]);
					if (dn > 0) pm_dnd_free(dp, dn);
					need_redraw = 1;
				}
				break;
			case PMW_DOWN:
				if (ev.button == 3) { rclick = 1; rclick_x = ev.x; rclick_y = ev.y; need_redraw = 1; break; }
				if (ev.button != 1) break;
				if (u->clicked) frame(u);   /* 上一下還沒畫（點很快）：先處理掉 */
				u->mx = ev.x; u->my = ev.y;
				u->down = 1; u->clicked = 1;
				u->dbl = (Time)ev.time - u->last_click < 400;
				u->last_click = ev.time;
				need_redraw = 1;
				break;
			case PMW_UP:
				if (ev.button == 1) { u->down = 0; u->released = 1; need_redraw = 1; }
				break;
			case PMW_MOTION:
				u->mx = ev.x; u->my = ev.y;
				if (u->active) need_redraw = 1;
				break;
			case PMW_SCROLL:
				u->scroll = MAX(0, u->scroll + (ev.button == 4 ? -40 : 40));
				pm_ui_wheel_steps += ev.button == 4 ? -1 : 1;
				need_redraw = 1;
				break;
			case PMW_KEY:
				if ((ev.state & (ControlMask | Mod4Mask)) && (ev.key == XK_w || ev.key == XK_q)) { ui_quit = 1; break; }
				if (pop_mode && ev.key == XK_Escape && (!u->focus || (u->focus == main_input_id && !main_input_has_text))) { ui_quit = 1; break; }
				if (u->key || u->keylen) frame(u);   /* 上一個鍵還沒處理（打很快、輸入法一次送好幾段）：不要蓋過去 */
				u->key = ev.key;
				memcpy(u->keybuf, ev.text, sizeof u->keybuf);
				u->keylen = ev.textlen;
				u->keystate = ev.state;
				need_redraw = 1;
				break;
			}
			continue;
		}
		if (want_draw) frame(u);
		if (tick && !ui_quit && pm_now_ms() >= next_tick) { tick(u); next_tick = pm_now_ms() + (ui_tick_override > 0 ? ui_tick_override : tick_ms); }
	}
	pmwl_destroy(u->wl);
	pmwl_destroy(catcher);
	if (ask_ui == u) ask_ui = NULL;
	cur_ui = prev_ui;
	cur_build = prev_build;
	ui_quit = 0;
	need_redraw = 1;
	free(u);
	return 0;
}

static int ui_run_impl(const char *title, const char *wmclass, int w, int h, PmBuild build,
                       void (*tick)(PmUI *u), int tick_ms, cairo_surface_t *(*icon)(int size));

/* 可以在另一個 pm_ui_run 的 build()／tick() 裡面再開一個（例如 pm_ask 問「確定要刪除？」）：
 * 裡面那個用自己的 PmUI，結束後外面的 cur_ui／cur_build 還原、整個視窗重畫一次 */
int pm_ui_run(const char *title, const char *wmclass, int w, int h, PmBuild build,
              void (*tick)(PmUI *u), int tick_ms, cairo_surface_t *(*icon)(int size))
{
	PmUI *outer = cur_ui;
	PmBuild outer_build = cur_build;
	int r = ui_run_impl(title, wmclass, w, h, build, tick, tick_ms, icon);
	cur_ui = outer;
	cur_build = outer_build;
	if (outer) need_redraw = 1;
	return r;
}

static int ui_run_impl(const char *title, const char *wmclass, int w, int h, PmBuild build,
                       void (*tick)(PmUI *u), int tick_ms, cairo_surface_t *(*icon)(int size))
{
	setlocale(LC_ALL, "");
	if (pm_ui_native && !dpy && pmwl_connect() && (!pop_mode || pmwl_has_layer())) {   /* 彈出式要 layer-shell */
		int r = ui_run_wl(title, wmclass, w, h, build, tick, tick_ms);
		if (r >= 0) return r;
	}
	if (!dpy && !(dpy = XOpenDisplay(NULL))) {
		fprintf(stderr, _("%s: 無法開啟 X display\n"), wmclass);
		return 1;
	}
	struct sigaction sa = { .sa_handler = SIG_IGN, .sa_flags = SA_NOCLDWAIT };
	sigaction(SIGCHLD, &sa, NULL);
	pm_conf_load();
	pm_init(pm_conf_get("appearance.font", "Noto Sans CJK TC"), pm_conf_num("appearance.font_size", 13),
	        pm_conf_int("appearance.dark", 0), pm_conf_get("appearance.icon_theme", "Yaru"));
	apply_conf();
	pm_conf_watch(dpy);
	pm_dispatch = modal_dispatch;

	PmUI u = { 0 };   /* 不能是 static：pm_ask 等會在 build() 裡面再跑一個 */
	u.w = w; u.h = h;
	cur_ui = &u;
	cur_build = build;
	XSetWindowAttributes wa = {
		.background_pixel = c_bg, .bit_gravity = NorthWestGravity,
		.event_mask = ExposureMask | KeyPressMask | ButtonPressMask | ButtonReleaseMask | ButtonMotionMask |
		              StructureNotifyMask | FocusChangeMask,
	};
	wa.override_redirect = pop_mode;
	wa.backing_store = WhenMapped;   /* 被別的視窗（例如走過去的小柚）蓋住再露出來時，X 伺服器自己補回來，不用重畫 */
	unsigned long mask = CWBackPixel | CWBitGravity | CWBackingStore | CWEventMask | CWOverrideRedirect;
	u.win = pop_mode ? pm_round_window(pop_right - w, pop_top, w, h, mask, &wa)
	                 : XCreateWindow(dpy, pm_root, 0, 0, w, h, 0, CopyFromParent, InputOutput, CopyFromParent, mask, &wa);
	XClassHint ch = { (char *)wmclass, (char *)wmclass };
	XSizeHints *sz = XAllocSizeHints();
	sz->flags = PMinSize;
	sz->min_width = MIN(420, w); sz->min_height = MIN(300, h);
	XWMHints wm = { .flags = InputHint, .input = True };
	XSetWMProperties(dpy, u.win, NULL, NULL, NULL, 0, sz, &wm, &ch);
	XFree(sz);
	title = pm_tr(title);
	XStoreName(dpy, u.win, title);
	XChangeProperty(dpy, u.win, XInternAtom(dpy, "_NET_WM_NAME", False), XInternAtom(dpy, "UTF8_STRING", False),
	                8, PropModeReplace, (const unsigned char *)title, strlen(title));
	a_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
	XSetWMProtocols(dpy, u.win, &a_delete, 1);
	if (icon) set_icon(u.win, icon);

	static XIM xim;
	pm_xim_attach(dpy, &xim, &u.xic, u.win);   /* 輸入法重新啟動也不會當掉 */
	if (pm_ui_on_drop) pm_dnd_accept(dpy, u.win);
	XMapRaised(dpy, u.win);
	if (pop_mode) {
		/* 彈出式：抓住滑鼠與鍵盤，點外面就關閉 */
		pm_shape_round(u.win, w, h, 12, 12);
		XSync(dpy, False);
		for (int i = 0; i < 100; i++) {
			if (XGrabPointer(dpy, u.win, True, ButtonPressMask | ButtonReleaseMask | ButtonMotionMask,
			                 GrabModeAsync, GrabModeAsync, None, pm_cursor, CurrentTime) == GrabSuccess)
				break;
			usleep(2000);
		}
		XGrabKeyboard(dpy, u.win, True, GrabModeAsync, GrabModeAsync, CurrentTime);
		XSetInputFocus(dpy, u.win, RevertToPointerRoot, CurrentTime);
	}

	int fd = ConnectionNumber(dpy);
	long next_tick = tick ? pm_now_ms() : 0;
	long redraw_at[2] = { 0, 0 };
	for (;;) {
		while (XPending(dpy)) {
			XEvent ev;
			XNextEvent(dpy, &ev);
			if (XFilterEvent(&ev, None)) continue;
			if (pm_ui_on_drop) {   /* 拖進來的檔案（只有 X11；原生 Wayland 還沒有） */
				char **dp = NULL;
				int dn = pm_dnd_event(dpy, &ev, &dp);
				if (dn >= 0) {
					for (int i = 0; i < dn; i++) pm_ui_on_drop(dp[i]);
					if (dn > 0) pm_dnd_free(dp, dn);
					need_redraw = 1;
					continue;
				}
			}
			switch (ev.type) {
			case Expose: need_redraw = 1; break;
			/* 剛顯示時，視窗管理員可能還在把視窗放進外框，第一次畫的內容會被丟掉，
			 * 而且之後不一定有 Expose：顯示後再補畫兩次 */
			case MapNotify:
				need_redraw = 1;
				redraw_at[0] = pm_now_ms() + 120;
				redraw_at[1] = pm_now_ms() + 500;
				break;
			case ConfigureNotify:
				if (ev.xconfigure.width != u.w || ev.xconfigure.height != u.h) {
					u.w = ev.xconfigure.width; u.h = ev.xconfigure.height;
					need_redraw = 1;
				}
				break;
			case ButtonPress:
				if (ev.xbutton.button == 4 || ev.xbutton.button == 5) {
					u.scroll = MAX(0, u.scroll + (ev.xbutton.button == 4 ? -40 : 40));
					pm_ui_wheel_steps += ev.xbutton.button == 4 ? -1 : 1;
				} else if (pop_mode && (ev.xbutton.x < 0 || ev.xbutton.y < 0 ||
				                        ev.xbutton.x >= u.w || ev.xbutton.y >= u.h)) {
					return 0;   /* 點到視窗外 */
				} else if (ev.xbutton.button == Button3) {   /* 右鍵：給輸入框的選單 */
					rclick = 1; rclick_x = ev.xbutton.x; rclick_y = ev.xbutton.y;
				} else if (ev.xbutton.button == Button1) {
					u.mx = ev.xbutton.x; u.my = ev.xbutton.y;
					u.down = 1; u.clicked = 1;
					u.dbl = ev.xbutton.time - u.last_click < 400;
					u.last_click = ev.xbutton.time;
				}
				need_redraw = 1;
				break;
			case ButtonRelease:
				if (ev.xbutton.button == Button1) { u.down = 0; u.released = 1; need_redraw = 1; }
				break;
			case MotionNotify:
				u.mx = ev.xmotion.x; u.my = ev.xmotion.y;
				if (u.active) need_redraw = 1;
				break;
			case KeyPress: {
				Status st = 0;
				u.keylen = u.xic ? Xutf8LookupString(u.xic, &ev.xkey, u.keybuf, sizeof u.keybuf - 1, &u.key, &st)
				                 : XLookupString(&ev.xkey, u.keybuf, sizeof u.keybuf - 1, &u.key, NULL);
				if (u.keylen < 0 || st == XBufferOverflow) u.keylen = 0;
				u.keybuf[u.keylen] = 0;
				u.keystate = ev.xkey.state;
				if ((ev.xkey.state & (ControlMask | Mod4Mask)) && (u.key == XK_w || u.key == XK_q)) return 0;
				if (pop_mode && u.key == XK_Escape && (!u.focus || (u.focus == main_input_id && !main_input_has_text))) return 0;
				need_redraw = 1;
				break;
			}
			case FocusIn: if (u.xic && !focus_secret) XSetICFocus(u.xic); break;
			case FocusOut: if (u.xic) XUnsetICFocus(u.xic); break;
			case ClientMessage:
				if ((Atom)ev.xclient.data.l[0] == a_delete) return 0;
				break;
			case SelectionRequest: xclip_answer(&ev.xselectionrequest); break;
			case SelectionClear: if (ev.xselectionclear.selection == a_clip) { free(xclip_text); xclip_text = NULL; } break;
			case PropertyNotify:
				if (pm_conf_changed(dpy, &ev)) { apply_conf(); need_redraw = 1; }
				break;
			}
		}
		for (int k = 0; k < 2; k++)
			if (redraw_at[k] && pm_now_ms() >= redraw_at[k]) { redraw_at[k] = 0; need_redraw = 1; }
		if (need_redraw) frame(&u);
		long wait = -1;
		for (int k = 0; k < 2; k++)
			if (redraw_at[k]) {
				long w2 = MAX(0, redraw_at[k] - pm_now_ms());
				wait = wait < 0 ? w2 : MIN(wait, w2);
			}
		if (tick) {
			long now = pm_now_ms();
			if (now >= next_tick) { tick(&u); next_tick = now + (ui_tick_override > 0 ? ui_tick_override : tick_ms); if (need_redraw) frame(&u); }
			long w2 = MAX(0, next_tick - pm_now_ms());
			wait = wait < 0 ? w2 : MIN(wait, w2);
		}
		/* 畫好的東西要先送出去再睡（不然最後一批繪圖會留在 Xlib 的緩衝區，等下一個事件才出現） */
		XFlush(dpy);
		if (XPending(dpy)) continue;
		fd_set fds;
		FD_ZERO(&fds);
		FD_SET(fd, &fds);
		struct timeval tv = { wait / 1000, (wait % 1000) * 1000 };
		select(fd + 1, &fds, NULL, NULL, wait >= 0 ? &tv : NULL);
	}
}

/* ------------------------------------------------------------------ */
/* 開啟／另存新檔對話窗（pm_file_dialog）                              */
/* ------------------------------------------------------------------ */
/* 常見的排法：上面是目前的資料夾（↑回上一層），左邊是常用位置，中間是資料夾與檔案，
 * 下面是名稱（另存新檔才有）、檔案類型，「取消」跟「開啟／儲存」靠右。 */

typedef struct { char name[256]; int dir; time_t mtime; } FdEnt;

enum { FD_TOP = 50, FD_SIDE = 168, FD_ROW = 30, FD_HEAD = 26, FD_MAXPLACE = 8 };

static struct {
	Window win;
	PmWl *wl;                 /* 原生 Wayland 時用這個，win 是 0 */
	int ime_x, ime_y;         /* 名稱欄的游標（給輸入法擺選字列） */
	int w, h, save, done, result;
	const char *title, *wmclass;
	cairo_surface_t *(*icon)(int size);
	char dir[PATH_MAX];
	FdEnt *ent;
	int nent, cap, sel, scroll;
	char name[256];
	int cur, selall;
	const PmFileType *types;
	int ntype, type;
	char plabel[FD_MAXPLACE][96], ppath[FD_MAXPLACE][PATH_MAX];
	cairo_surface_t *picon[FD_MAXPLACE], *ic_folder, *ic_file;
	int nplace;
	XIC xic;
	Time last_click;
	int last_idx;
	Rect r_up, r_place[FD_MAXPLACE], r_list, r_name, r_type, r_cancel, r_ok;
	char out[PATH_MAX];
} fd;

static int fd_bottom(void) { return fd.save ? 96 : 56; }

/* 副檔名符不符合 exts（不含點，多個用分號隔開） */
static int fd_ext_match(const char *name, const char *exts)
{
	const char *dot = strrchr(name, '.');
	if (!dot || dot == name || !exts) return 0;
	dot++;
	size_t dl = strlen(dot);
	for (const char *p = exts; *p; ) {
		size_t l = strcspn(p, ";");
		if (l == dl && !strncasecmp(p, dot, l)) return 1;
		p += l;
		if (*p == ';') p++;
	}
	return 0;
}

static int fd_cmp(const void *a, const void *b)
{
	const FdEnt *x = a, *y = b;
	if (x->dir != y->dir) return y->dir - x->dir;   /* 資料夾在前面 */
	return strcoll(x->name, y->name);
}

static void fd_scan(void)
{
	fd.nent = 0;
	fd.sel = -1;
	fd.scroll = 0;
	DIR *d = opendir(fd.dir);
	if (!d) return;
	struct dirent *de;
	while ((de = readdir(d))) {
		if (de->d_name[0] == '.') continue;
		char full[PATH_MAX + 260];
		snprintf(full, sizeof full, "%s/%s", fd.dir, de->d_name);
		struct stat st;
		if (stat(full, &st)) continue;
		int isdir = S_ISDIR(st.st_mode);
		if (!isdir && fd.ntype > 0 && !fd_ext_match(de->d_name, fd.types[fd.type].ext)) continue;
		if (fd.nent == fd.cap) {
			fd.cap = fd.cap ? fd.cap * 2 : 128;
			fd.ent = realloc(fd.ent, (size_t)fd.cap * sizeof *fd.ent);
		}
		FdEnt *e = &fd.ent[fd.nent++];
		snprintf(e->name, sizeof e->name, "%s", de->d_name);
		e->dir = isdir;
		e->mtime = st.st_mtime;
	}
	closedir(d);
	qsort(fd.ent, (size_t)fd.nent, sizeof *fd.ent, fd_cmp);
}

static void fd_chdir(const char *dir)
{
	char real[PATH_MAX];
	struct stat st;
	if (!realpath(dir, real) || stat(real, &st) || !S_ISDIR(st.st_mode)) return;
	snprintf(fd.dir, sizeof fd.dir, "%s", real);
	fd_scan();
}

static void fd_up(void)
{
	char d[PATH_MAX];
	snprintf(d, sizeof d, "%s", fd.dir);
	char *slash = strrchr(d, '/');
	if (!slash) return;
	if (slash == d) d[1] = 0; else *slash = 0;
	char was[PATH_MAX];
	snprintf(was, sizeof was, "%s", slash == d ? fd.dir + 1 : slash + 1);
	fd_chdir(d);
	for (int i = 0; i < fd.nent; i++)   /* 回上一層時選到剛剛那個資料夾 */
		if (!strcmp(fd.ent[i].name, was)) fd.sel = i;
}

/* 常用位置：家目錄、桌面、文件、下載、圖片（使用者資料夾設定裡有的才列）、電腦 */
static void fd_add_place(const char *label, const char *path, const char *icon)
{
	if (fd.nplace >= FD_MAXPLACE) return;
	snprintf(fd.plabel[fd.nplace], sizeof fd.plabel[0], "%s", label);
	snprintf(fd.ppath[fd.nplace], sizeof fd.ppath[0], "%s", path);
	fd.picon[fd.nplace] = pm_icon_load(icon, 16);
	fd.nplace++;
}

static void fd_places(void)
{
	const char *home = getenv("HOME") ? getenv("HOME") : "/";
	fd.nplace = 0;
	fd_add_place(_("家目錄"), home, "user-home");
	static const struct { const char *key, *icon; } xdg[] = {
		{ "XDG_DESKTOP_DIR", "user-desktop" }, { "XDG_DOCUMENTS_DIR", "folder-documents" },
		{ "XDG_DOWNLOAD_DIR", "folder-download" }, { "XDG_PICTURES_DIR", "folder-pictures" },
	};
	char cfg[PATH_MAX], line[PATH_MAX], found[LEN(xdg)][PATH_MAX];
	memset(found, 0, sizeof found);
	snprintf(cfg, sizeof cfg, "%s/.config/user-dirs.dirs", home);
	FILE *f = fopen(cfg, "r");
	while (f && fgets(line, sizeof line, f))
		for (unsigned i = 0; i < LEN(xdg); i++) {
			size_t kl = strlen(xdg[i].key);
			if (strncmp(line, xdg[i].key, kl) || line[kl] != '=') continue;
			char *v = line + kl + 1;
			v[strcspn(v, "\n")] = 0;
			if (*v == '"') { v++; v[strcspn(v, "\"")] = 0; }
			if (!strncmp(v, "$HOME", 5)) snprintf(found[i], PATH_MAX, "%s%s", home, v + 5);
			else snprintf(found[i], PATH_MAX, "%s", v);
		}
	if (f) fclose(f);
	for (unsigned i = 0; i < LEN(xdg); i++) {
		struct stat st;
		if (!found[i][0] || !strcmp(found[i], home) || stat(found[i], &st) || !S_ISDIR(st.st_mode)) continue;
		const char *base = strrchr(found[i], '/') ? strrchr(found[i], '/') + 1 : found[i];
		fd_add_place(base, found[i], xdg[i].icon);
	}
	fd_add_place(_("電腦"), "/", "drive-harddisk");
}

/* 換檔案類型時，名稱的副檔名跟著換 */
static void fd_name_set_ext(void)
{
	if (!fd.ntype) return;
	const char *ext = fd.types[fd.type].ext;
	char *dot = strrchr(fd.name, '.');
	if (dot && dot != fd.name) *dot = 0;
	size_t n = strlen(fd.name);
	snprintf(fd.name + n, sizeof fd.name - n, ".%.*s", (int)strcspn(ext, ";"), ext);
	fd.cur = (int)strlen(fd.name);
}

static void fd_button(cairo_t *cr, Rect r, const char *label, int def, int enabled)
{
	pm_rrect(cr, r.x, r.y, r.w, r.h, 7, 7);
	if (def) pm_rgba(cr, T->sel, enabled ? 1 : 0.45); else pm_rgb(cr, c_ctrl);
	cairo_fill(cr);
	pm_font(cr, 13, 0);
	pm_rgb(cr, def ? 0xffffff : c_text);
	pm_text(cr, r.x + (r.w - pm_textw(cr, label)) / 2, r.y + r.h / 2.0, label);
}

static int fd_can_ok(void) { return fd.save ? fd.name[0] != 0 : fd.sel >= 0; }

static void fd_draw(void)
{
	Paint p;
	cairo_t *cr = fd.wl ? pmwl_begin(fd.wl, &fd.w, &fd.h) : pm_paint_begin(&p, fd.win, fd.w, fd.h);
	int W = fd.w, H = fd.h, B = fd_bottom();
	pm_rgb(cr, c_bg);
	cairo_paint(cr);

	/* 上面：↑ 跟目前的資料夾（家目錄底下的顯示成「家目錄 › 文件」） */
	fd.r_up = (Rect){ 12, 11, 30, 28 };
	pm_rrect(cr, 12, 11, 30, 28, 6, 6);
	pm_rgb(cr, c_ctrl);
	cairo_fill(cr);
	pm_rgb(cr, c_text);
	cairo_set_line_width(cr, 1.6);
	cairo_move_to(cr, 27, 18); cairo_line_to(cr, 27, 32);
	cairo_move_to(cr, 21, 24); cairo_line_to(cr, 27, 18); cairo_line_to(cr, 33, 24);
	cairo_stroke(cr);
	const char *home = getenv("HOME") ? getenv("HOME") : "/";
	size_t hl = strlen(home);
	char path[PATH_MAX + 100], shown[PATH_MAX + 100];
	if (hl > 1 && !strncmp(fd.dir, home, hl) && (fd.dir[hl] == '/' || !fd.dir[hl])) {
		snprintf(path, sizeof path, "%s", _("家目錄"));
		for (const char *s = fd.dir + hl; *s; ) {
			s++;
			size_t l = strcspn(s, "/");
			size_t n = strlen(path);
			snprintf(path + n, sizeof path - n, " › %.*s", (int)l, s);
			s += l;
		}
	} else snprintf(path, sizeof path, "%s", fd.dir);
	pm_font(cr, 13.5, 1);
	/* 太長的話從左邊截掉，保留目前所在的資料夾 */
	double room = W - 60 - 16;
	const char *show = path;
	while (pm_textw(cr, show) > room && strchr(show, ' ')) show = strchr(show, ' ') + 1;
	snprintf(shown, sizeof shown, "%s%s", show == path ? "" : "… ", show);
	pm_text(cr, 54, 25, shown);
	pm_rgb(cr, c_line);
	cairo_rectangle(cr, 0, FD_TOP - 1, W, 1);
	cairo_fill(cr);

	/* 左邊：常用位置 */
	pm_font(cr, 13, 0);
	for (int i = 0; i < fd.nplace; i++) {
		int y = FD_TOP + 8 + i * FD_ROW;
		fd.r_place[i] = (Rect){ 8, y, FD_SIDE - 16, FD_ROW - 2 };
		if (!strcmp(fd.dir, fd.ppath[i])) {
			pm_rrect(cr, 8, y, FD_SIDE - 16, FD_ROW - 2, 6, 6);
			pm_rgba(cr, T->sel, 0.18);
			cairo_fill(cr);
		}
		if (fd.picon[i]) {
			cairo_set_source_surface(cr, fd.picon[i], 18, y + (FD_ROW - 2 - 16) / 2.0);
			cairo_paint(cr);
		}
		char buf[96];
		pm_ellipsize(cr, fd.plabel[i], FD_SIDE - 60, buf, sizeof buf);
		pm_rgb(cr, c_text);
		pm_text(cr, 42, y + (FD_ROW - 2) / 2.0, buf);
	}

	/* 中間：資料夾與檔案 */
	int lx = FD_SIDE, ly = FD_TOP, lw = W - FD_SIDE, lh = H - B - FD_TOP;
	pm_rgb(cr, c_card);
	cairo_rectangle(cr, lx, ly, lw, lh);
	cairo_fill(cr);
	pm_rgb(cr, c_line);
	cairo_rectangle(cr, lx, ly, 1, lh);
	cairo_rectangle(cr, lx, ly + FD_HEAD - 1, lw, 1);
	cairo_fill(cr);
	double date_x = lx + lw - 150;
	pm_font(cr, 12, 0);
	pm_rgb(cr, c_dim);
	pm_text(cr, lx + 40, ly + FD_HEAD / 2.0, _("名稱"));
	pm_text(cr, date_x, ly + FD_HEAD / 2.0, _("修改日期"));
	fd.r_list = (Rect){ lx + 1, ly + FD_HEAD, lw - 1, lh - FD_HEAD };
	int maxs = MAX(0, fd.nent * FD_ROW - fd.r_list.h);
	fd.scroll = CLAMP(fd.scroll, 0, maxs);
	cairo_save(cr);
	cairo_rectangle(cr, fd.r_list.x, fd.r_list.y, fd.r_list.w, fd.r_list.h);
	cairo_clip(cr);
	pm_font(cr, 13, 0);
	for (int i = 0; i < fd.nent; i++) {
		double y = fd.r_list.y + i * FD_ROW - fd.scroll;
		if (y + FD_ROW < fd.r_list.y || y > fd.r_list.y + fd.r_list.h) continue;
		FdEnt *e = &fd.ent[i];
		int on = i == fd.sel;
		if (on) {
			cairo_rectangle(cr, fd.r_list.x, y, fd.r_list.w, FD_ROW);
			pm_rgb(cr, T->sel);
			cairo_fill(cr);
		}
		cairo_surface_t *ic = e->dir ? fd.ic_folder : fd.ic_file;
		if (ic) {
			cairo_set_source_surface(cr, ic, lx + 14, y + (FD_ROW - 18) / 2.0);
			cairo_paint(cr);
		}
		char buf[300];
		pm_ellipsize(cr, e->name, date_x - lx - 52, buf, sizeof buf);
		pm_rgb(cr, on ? T->sel_text : c_text);
		pm_text(cr, lx + 40, y + FD_ROW / 2.0, buf);
		char date[32];
		strftime(date, sizeof date, "%Y/%m/%d %H:%M", localtime(&e->mtime));
		if (on) pm_rgba(cr, T->sel_text, 0.85); else pm_rgb(cr, c_dim);
		pm_text(cr, date_x, y + FD_ROW / 2.0, date);
	}
	cairo_restore(cr);
	if (maxs > 0) {   /* 捲軸 */
		double bh = MAX(24.0, (double)fd.r_list.h * fd.r_list.h / (fd.nent * FD_ROW));
		double by = fd.r_list.y + (fd.r_list.h - bh) * fd.scroll / maxs;
		pm_rrect(cr, W - 8, by + 2, 5, bh - 4, 2.5, 2.5);
		pm_rgba(cr, pm_dark ? 0xffffff : 0x000000, 0.25);
		cairo_fill(cr);
	}

	/* 下面：名稱、類型、按鈕 */
	int by0 = H - B;
	pm_rgb(cr, c_line);
	cairo_rectangle(cr, 0, by0, W, 1);
	cairo_fill(cr);
	int bw_ok = 92, bw_cancel = 84, row2 = fd.save ? by0 + 54 : by0 + 14;
	fd.r_ok = (Rect){ W - 16 - bw_ok, row2, bw_ok, 28 };
	fd.r_cancel = (Rect){ fd.r_ok.x - 8 - bw_cancel, row2, bw_cancel, 28 };
	pm_font(cr, 13, 0);
	double labw = MAX(pm_textw(cr, _("名稱")), pm_textw(cr, _("類型"))) + 26;
	if (fd.save) {
		int y = by0 + 14;
		pm_rgb(cr, c_text);
		pm_text(cr, 16, y + 14, _("名稱"));
		fd.r_name = (Rect){ (int)labw, y, W - 16 - (int)labw, 28 };
		pm_rrect(cr, fd.r_name.x, y, fd.r_name.w, 28, 6, 6);
		pm_rgb(cr, c_card);
		cairo_fill_preserve(cr);
		pm_rgb(cr, T->sel);
		cairo_set_line_width(cr, 1.5);
		cairo_stroke(cr);
		cairo_save(cr);
		cairo_rectangle(cr, fd.r_name.x + 4, y, fd.r_name.w - 8, 28);
		cairo_clip(cr);
		char head[256];
		snprintf(head, sizeof head, "%.*s", fd.cur, fd.name);
		double tw = pm_textw(cr, fd.name), cw = pm_textw(cr, head);
		double shift = MAX(0, cw - (fd.r_name.w - 24));
		if (fd.selall && fd.name[0]) {
			cairo_rectangle(cr, fd.r_name.x + 9 - shift, y + 6, tw + 2, 16);
			pm_rgba(cr, T->sel, 0.3);
			cairo_fill(cr);
		}
		pm_rgb(cr, c_text);
		pm_text(cr, fd.r_name.x + 10 - shift, y + 14, fd.name);
		if (!fd.selall) {
			cairo_rectangle(cr, fd.r_name.x + 10 - shift + cw, y + 6, 1.5, 16);
			pm_rgb(cr, T->sel);
			cairo_fill(cr);
		}
		cairo_restore(cr);
		fd.ime_x = (int)(fd.r_name.x + 10 - shift + (fd.selall ? 0 : cw));
		fd.ime_y = y;
		if (fd.xic) {   /* 輸入法的選字窗跟著游標，不然會跑到螢幕角落 */
			XPoint spot = { (short)fd.ime_x, (short)(y + 24) };
			XVaNestedList attr = XVaCreateNestedList(0, XNSpotLocation, &spot, NULL);
			XSetICValues(fd.xic, XNPreeditAttributes, attr, NULL);
			XFree(attr);
		}
	}
	if (fd.ntype > 0) {
		pm_rgb(cr, c_text);
		pm_text(cr, 16, row2 + 14, _("類型"));
		const char *tl = pm_tr(fd.types[fd.type].label);
		double tw = MAX(200, pm_textw(cr, tl) + 44);
		tw = MIN(tw, fd.r_cancel.x - 16 - labw);
		fd.r_type = (Rect){ (int)labw, row2, (int)tw, 28 };
		pm_rrect(cr, labw, row2, tw, 28, 6, 6);
		pm_rgb(cr, c_ctrl);
		cairo_fill(cr);
		char buf[200];
		pm_ellipsize(cr, tl, tw - 34, buf, sizeof buf);
		pm_rgb(cr, c_text);
		pm_text(cr, labw + 10, row2 + 14, buf);
		cairo_set_line_width(cr, 1.4);
		double ax = labw + tw - 16, ay = row2 + 12;
		cairo_move_to(cr, ax - 4, ay); cairo_line_to(cr, ax, ay + 4); cairo_line_to(cr, ax + 4, ay);
		cairo_stroke(cr);
	} else fd.r_type = (Rect){ 0, 0, 0, 0 };
	fd_button(cr, fd.r_cancel, _("取消"), 0, 1);
	fd_button(cr, fd.r_ok, fd.save ? _("儲存") : _("開啟"), 1, fd_can_ok());
	if (fd.wl) {
		pmwl_end(fd.wl, cr);
		pmwl_ime(fd.wl, fd.save, fd.ime_x, fd.ime_y, 28);   /* 另存新檔：名稱欄一直拿著鍵盤 */
	} else {
		pm_paint_end(&p, cr, 0, 0);
	}
}

static void fd_finish(const char *path)
{
	snprintf(fd.out, sizeof fd.out, "%s", path);
	fd.result = 1;
	fd.done = 1;
}

static void fd_confirm(void);

/* 按兩下或 Enter：資料夾就進去，檔案就選定 */
static void fd_activate(int i)
{
	if (i < 0 || i >= fd.nent) return;
	char full[PATH_MAX + 260];
	snprintf(full, sizeof full, "%s%s%s", fd.dir, strcmp(fd.dir, "/") ? "/" : "", fd.ent[i].name);
	if (fd.ent[i].dir) { fd_chdir(full); return; }
	if (!fd.save) { fd_finish(full); return; }
	snprintf(fd.name, sizeof fd.name, "%s", fd.ent[i].name);
	fd.cur = (int)strlen(fd.name);
	fd_confirm();
}

static void fd_confirm(void)
{
	if (!fd.save) { fd_activate(fd.sel); return; }
	if (!fd.name[0]) return;
	char path[PATH_MAX + 260];
	const char *home = getenv("HOME") ? getenv("HOME") : "/";
	if (fd.name[0] == '/') snprintf(path, sizeof path, "%s", fd.name);
	else if (fd.name[0] == '~' && fd.name[1] == '/') snprintf(path, sizeof path, "%s%s", home, fd.name + 1);
	else snprintf(path, sizeof path, "%s%s%s", fd.dir, strcmp(fd.dir, "/") ? "/" : "", fd.name);
	struct stat st;
	if (!stat(path, &st) && S_ISDIR(st.st_mode)) {   /* 打的是資料夾：進去 */
		fd_chdir(path);
		fd.name[0] = 0;
		fd.cur = 0;
		return;
	}
	for (int k = 0; k < fd.ntype; k++)   /* 自己打了別種類型的副檔名（例如 .docx）：就存成那種 */
		if (!fd_ext_match(path, fd.types[fd.type].ext) && fd_ext_match(path, fd.types[k].ext)) fd.type = k;
	if (fd.ntype > 0 && !fd_ext_match(path, fd.types[fd.type].ext)) {
		const char *ext = fd.types[fd.type].ext;
		size_t n = strlen(path);
		snprintf(path + n, sizeof path - n, ".%.*s", (int)strcspn(ext, ";"), ext);
	}
	if (!stat(path, &st)) {
		const char *base = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
		char msg[600];
		snprintf(msg, sizeof msg, _("「%s」已經存在，要取代嗎？"), base);
		const char *b[2] = { "取消", "取代" };
		if (pm_ask(fd.title, fd.wmclass, fd.icon, msg, b, 2, 0, 0) != 1) return;
	}
	fd_finish(path);
}

static void fd_ensure_visible(void)
{
	if (fd.sel < 0) return;
	if (fd.sel * FD_ROW < fd.scroll) fd.scroll = fd.sel * FD_ROW;
	else if ((fd.sel + 1) * FD_ROW > fd.scroll + fd.r_list.h) fd.scroll = (fd.sel + 1) * FD_ROW - fd.r_list.h;
}

static int fd_in(Rect r, int x, int y) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }

static int fd_picked;
static void fd_pick(const Arg *a) { fd_picked = a->i; }

/* button：1 左鍵、4／5 滾輪；time 毫秒（連點兩下用） */
static void fd_click(int button, int x, int y, long time)
{
	struct { int x, y; long time; } ev = { x, y, time }, *e = &ev;
	if (button == Button4 || button == Button5) {
		if (fd_in(fd.r_list, e->x, e->y)) fd.scroll += button == Button4 ? -3 * FD_ROW : 3 * FD_ROW;
		return;
	}
	if (button != Button1) return;
	if (fd_in(fd.r_up, e->x, e->y)) { fd_up(); return; }
	for (int i = 0; i < fd.nplace; i++)
		if (fd_in(fd.r_place[i], e->x, e->y)) { fd_chdir(fd.ppath[i]); return; }
	if (fd_in(fd.r_list, e->x, e->y)) {
		int i = (e->y - fd.r_list.y + fd.scroll) / FD_ROW;
		if (i < 0 || i >= fd.nent) { fd.sel = -1; return; }
		int dbl = i == fd.last_idx && (Time)e->time - fd.last_click < 400;
		fd.last_click = e->time;
		fd.last_idx = i;
		fd.sel = i;
		if (fd.save && !fd.ent[i].dir) {   /* 點了現有的檔案：名稱換成它 */
			snprintf(fd.name, sizeof fd.name, "%s", fd.ent[i].name);
			fd.cur = (int)strlen(fd.name);
			fd.selall = 0;
		}
		if (dbl) { fd.last_click = 0; fd_activate(i); }
		return;
	}
	if (fd.save && fd_in(fd.r_name, e->x, e->y)) {
		/* 點在名稱裡：取消全選，游標放到最後 */
		fd.selall = 0;
		fd.cur = (int)strlen(fd.name);
		return;
	}
	if (fd.ntype > 0 && fd_in(fd.r_type, e->x, e->y)) {
		MenuItem m[16];
		int n = MIN(fd.ntype, 16);
		char lab[16][200];
		for (int i = 0; i < n; i++) {
			snprintf(lab[i], sizeof lab[i], "%c%s", i == fd.type ? '\1' : '\2', fd.types[i].label);
			m[i] = (MenuItem){ lab[i], fd_pick, { .i = i } };
		}
		fd_picked = -1;
		if (fd.wl) {
			pmwl_menu(fd.wl, m, n, fd.r_type.x, fd.r_type.y + fd.r_type.h);
		} else {
			int rx, ry;
			Window ch;
			XTranslateCoordinates(dpy, fd.win, pm_root, fd.r_type.x, fd.r_type.y + fd.r_type.h, &rx, &ry, &ch);
			pm_popup(m, n, rx, ry);
		}
		if (fd_picked >= 0 && fd_picked != fd.type) {
			fd.type = fd_picked;
			if (fd.save) fd_name_set_ext();
			fd_scan();
		}
		return;
	}
	if (fd_in(fd.r_cancel, e->x, e->y)) { fd.done = 1; return; }
	if (fd_in(fd.r_ok, e->x, e->y) && fd_can_ok()) fd_confirm();
}

/* ks：keysym（輸入法打好的字是 NoSymbol，只有 buf）；state：X 的修飾鍵位元 */
static void fd_key(KeySym ks, const char *buf, int len, unsigned state)
{
	int ctrl = state & ControlMask, alt = state & Mod1Mask;
	if (ks == XK_Escape) { fd.done = 1; return; }
	if (alt && ks == XK_Up) { fd_up(); return; }
	if (ks == XK_Return || ks == XK_KP_Enter) {
		/* 選到資料夾、而且名稱沒在改（或是開啟）：進資料夾；不然就確定 */
		if (fd.sel >= 0 && fd.ent[fd.sel].dir && (!fd.save || fd.selall || !fd.name[0])) fd_activate(fd.sel);
		else fd_confirm();
		return;
	}
	if (ks == XK_Up || ks == XK_Down) {
		if (!fd.nent) return;
		fd.sel = fd.sel < 0 ? 0 : CLAMP(fd.sel + (ks == XK_Up ? -1 : 1), 0, fd.nent - 1);
		fd_ensure_visible();
		if (fd.save && !fd.ent[fd.sel].dir) { snprintf(fd.name, sizeof fd.name, "%s", fd.ent[fd.sel].name); fd.cur = (int)strlen(fd.name); fd.selall = 1; }
		return;
	}
	if (!fd.save) {
		if (ks == XK_BackSpace) fd_up();
		return;
	}
	/* 名稱欄位 */
	size_t n = strlen(fd.name);
	if (ctrl && (ks == XK_a || ks == XK_A)) { fd.selall = 1; return; }
	if (ctrl && (ks == XK_v || ks == XK_V)) {   /* 貼上：插在游標的位置（全選時換掉整個名稱） */
		char *t = clip_get(fd.win, fd.wl), add[256] = "";
		paste_into(add, sizeof add, t);
		free(t);
		if (fd.selall) { fd.name[0] = 0; fd.cur = 0; n = 0; fd.selall = 0; }
		size_t al = strlen(add);
		if (al && n + al < sizeof fd.name) {
			memmove(fd.name + fd.cur + al, fd.name + fd.cur, n - (size_t)fd.cur + 1);
			memcpy(fd.name + fd.cur, add, al);
			fd.cur += (int)al;
		}
		return;
	}
	if (ks == XK_BackSpace || ks == XK_Delete) {
		if (fd.selall) { fd.name[0] = 0; fd.cur = 0; fd.selall = 0; return; }
		if (ks == XK_BackSpace && fd.cur > 0) {
			int s = fd.cur;
			do s--; while (s > 0 && ((unsigned char)fd.name[s] & 0xc0) == 0x80);
			memmove(fd.name + s, fd.name + fd.cur, n - (size_t)fd.cur + 1);
			fd.cur = s;
		} else if (ks == XK_Delete && (size_t)fd.cur < n) {
			int s = fd.cur;
			do s++; while ((size_t)s < n && ((unsigned char)fd.name[s] & 0xc0) == 0x80);
			memmove(fd.name + fd.cur, fd.name + s, n - (size_t)s + 1);
		}
		return;
	}
	if (ks == XK_Left || ks == XK_Right || ks == XK_Home || ks == XK_End) {
		if (fd.selall) { fd.selall = 0; fd.cur = ks == XK_Left || ks == XK_Home ? 0 : (int)n; return; }
		if (ks == XK_Home) fd.cur = 0;
		else if (ks == XK_End) fd.cur = (int)n;
		else if (ks == XK_Left && fd.cur > 0) { do fd.cur--; while (fd.cur > 0 && ((unsigned char)fd.name[fd.cur] & 0xc0) == 0x80); }
		else if (ks == XK_Right && (size_t)fd.cur < n) { do fd.cur++; while ((size_t)fd.cur < n && ((unsigned char)fd.name[fd.cur] & 0xc0) == 0x80); }
		return;
	}
	if (len > 0 && (unsigned char)buf[0] >= 0x20 && !ctrl) {
		if (fd.selall) { fd.name[0] = 0; fd.cur = 0; n = 0; fd.selall = 0; }
		if (n + (size_t)len >= sizeof fd.name) return;
		memmove(fd.name + fd.cur + len, fd.name + fd.cur, n - (size_t)fd.cur + 1);
		memcpy(fd.name + fd.cur, buf, (size_t)len);
		fd.cur += len;
	}
}

static void fd_run_x(const char *title, const char *wmclass, cairo_surface_t *(*icon)(int size), int save)
{
	XSetWindowAttributes wa = {
		.background_pixel = c_bg, .bit_gravity = NorthWestGravity, .backing_store = WhenMapped,
		.event_mask = ExposureMask | KeyPressMask | ButtonPressMask | StructureNotifyMask | FocusChangeMask,
	};
	fd.win = XCreateWindow(dpy, pm_root, 0, 0, fd.w, fd.h, 0, CopyFromParent, InputOutput, CopyFromParent,
	                       CWBackPixel | CWBitGravity | CWBackingStore | CWEventMask, &wa);
	XClassHint ch = { (char *)wmclass, (char *)wmclass };
	XSizeHints *sz = XAllocSizeHints();
	sz->flags = PMinSize;
	sz->min_width = 560; sz->min_height = 360;
	XWMHints wm = { .flags = InputHint, .input = True };
	XSetWMProperties(dpy, fd.win, NULL, NULL, NULL, 0, sz, &wm, &ch);
	XFree(sz);
	const char *t = pm_tr(title);
	XStoreName(dpy, fd.win, t);
	XChangeProperty(dpy, fd.win, XInternAtom(dpy, "_NET_WM_NAME", False), XInternAtom(dpy, "UTF8_STRING", False),
	                8, PropModeReplace, (const unsigned char *)t, (int)strlen(t));
	Atom del = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
	XSetWMProtocols(dpy, fd.win, &del, 1);
	if (icon) set_icon(fd.win, icon);
	static XIM xim;
	fd.xic = NULL;
	if (save) pm_xim_attach(dpy, &xim, &fd.xic, fd.win);   /* 名稱可以打中文 */
	XMapRaised(dpy, fd.win);

	while (!fd.done) {
		XEvent ev;
		XNextEvent(dpy, &ev);
		if (XFilterEvent(&ev, None)) continue;
		if (ev.xany.window != fd.win) continue;
		switch (ev.type) {
		case Expose: if (ev.xexpose.count) continue; break;
		case ConfigureNotify: fd.w = ev.xconfigure.width; fd.h = ev.xconfigure.height; break;
		case ButtonPress: fd_click((int)ev.xbutton.button, ev.xbutton.x, ev.xbutton.y, (long)ev.xbutton.time); break;
		case KeyPress: {
			char buf[64];
			KeySym ks = NoSymbol;
			Status st = 0;
			int len = fd.xic ? Xutf8LookupString(fd.xic, &ev.xkey, buf, sizeof buf - 1, &ks, &st)
			                 : XLookupString(&ev.xkey, buf, sizeof buf - 1, &ks, NULL);
			if (len < 0 || st == XBufferOverflow) len = 0;
			buf[len] = 0;
			fd_key(ks, buf, len, ev.xkey.state);
			break;
		}
		case FocusIn: if (fd.xic) XSetICFocus(fd.xic); continue;
		case FocusOut: if (fd.xic) XUnsetICFocus(fd.xic); continue;
		case ClientMessage: if ((Atom)ev.xclient.data.l[0] == del) fd.done = 1; continue;
		default: continue;
		}
		if (!fd.done) fd_draw();
	}
	XDestroyWindow(dpy, fd.win);
	XFlush(dpy);
}

/* 原生 Wayland 版：視窗用 wl.c，畫面、點擊、按鍵跟 X 版共用 fd_draw／fd_click／fd_key。
 * 從原生的 pm_ui 程式叫出來時掛在它的視窗底下（compositor 會擺在上面、一起最小化） */
static void fd_run_wl(const char *title, const char *wmclass)
{
	fd.win = 0;
	fd.wl = pmwl_window(pm_tr(title), wmclass, fd.w, fd.h, 560, 360, cur_ui ? cur_ui->wl : NULL);
	if (!fd.wl) return;
	wl_modal = fd.wl;
	fd_draw();
	while (!fd.done) {
		PmwEvent ev;
		if (!pmwl_next(&ev, -1)) break;
		if (ev.win != fd.wl && ev.type != PMW_CONF) continue;   /* 對話窗開著時主視窗的事件先不管（跟 X 版一樣是 modal） */
		switch (ev.type) {
		case PMW_EXPOSE: case PMW_SIZE: case PMW_FOCUS_IN: break;
		case PMW_CONF: apply_conf(); break;
		case PMW_CLOSE: fd.done = 1; continue;
		case PMW_DOWN: case PMW_SCROLL: fd_click(ev.button, ev.x, ev.y, ev.time); break;
		case PMW_KEY: fd_key(ev.key, ev.text, ev.textlen, ev.state); break;
		default: continue;
		}
		if (!fd.done) fd_draw();
	}
	wl_modal = NULL;
	pmwl_destroy(fd.wl);
	fd.wl = NULL;
}

int pm_file_dialog(const char *title, const char *wmclass, cairo_surface_t *(*icon)(int size), int save,
                   const char *dir, const char *name, const PmFileType *types, int ntype, int *type,
                   char *out, size_t outn)
{
	/* 原生 Wayland：跟 pm_ui_run 同一個規則（程式設了 pm_ui_native、而且還沒開 X 連線） */
	int native = pm_ui_native && !dpy && pmwl_connect();
	if (!native && !need_x()) return 0;
	fd.title = title; fd.wmclass = wmclass; fd.icon = icon;
	fd.save = save;
	fd.done = fd.result = 0;
	fd.types = types;
	fd.ntype = types ? ntype : 0;
	fd.type = type && *type >= 0 && *type < fd.ntype ? *type : 0;
	fd.last_click = 0;
	fd.last_idx = -1;
	apply_conf();
	fd_places();
	fd.ic_folder = pm_icon_load("folder", 18);
	fd.ic_file = pm_icon_load("text-x-generic", 18);
	const char *home = getenv("HOME") ? getenv("HOME") : "/";
	fd.dir[0] = 0;
	if (dir && *dir) fd_chdir(dir);
	if (!fd.dir[0]) fd_chdir(home);
	snprintf(fd.name, sizeof fd.name, "%s", save && name ? name : "");
	fd.cur = (int)strlen(fd.name);
	fd.selall = fd.name[0] != 0;   /* 一開始整個選起來：直接打就換掉 */

	fd.w = 760; fd.h = 480;
	if (native) fd_run_wl(title, wmclass);
	else fd_run_x(title, wmclass, icon, save);
	for (int i = 0; i < fd.nplace; i++) if (fd.picon[i]) cairo_surface_destroy(fd.picon[i]);
	if (fd.ic_folder) cairo_surface_destroy(fd.ic_folder);
	if (fd.ic_file) cairo_surface_destroy(fd.ic_file);
	fd.nplace = 0;
	if (fd.result) {
		snprintf(out, outn, "%s", fd.out);
		if (type) *type = fd.type;
	}
	return fd.result;
}
