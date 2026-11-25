/* SPDX-License-Identifier: GPL-3.0-or-later */
/* pomelo-input — Pomelo 輸入法
 *
 * 一個很小的 XIM 伺服器：鍵盤事件由程式透過 XIM 協定送過來（libxcb-imdkit 處理協定），
 * 交給目前的輸入法引擎（engine.h：注音、韓文…），再把結果送回去。選字列是自己用 cairo 畫的小視窗，
 * 跟著游標（XNSpotLocation）走。Qt 程式沒有 XIM：另外用 IBus 的 D-Bus 協定服務它們（ibus.c），
 * 兩種連線共用同一套引擎與選字列（ic.h 的 IC）。不需要 fcitx、ibus、GTK。
 *
 * 用法：pomelo-input              （由 pomelo-session 啟動；程式要設 XMODIFIERS=@im=pomelo）
 *       pomelo-input --toggle     切換輸入法／英文
 *       pomelo-input --next       換下一個輸入法
 *       pomelo-input --engine ID  換成某個輸入法（選單列的選單用）
 *       pomelo-input --version    印出版本
 *
 * 操作：Shift 輕點或 Ctrl+Space 切換輸入法／英文；Ctrl+Shift+Space 換下一個輸入法（input.engines 裡有好幾個時）。
 * 選字列：滑鼠點選、滾輪翻頁、移過去時反白。
 * 狀態寫在 root：_POMELO_INPUT_MODE（zh／en；沒有啟用任何輸入法時是空的）、_POMELO_INPUT_LABEL（選單列上的字）、
 * _POMELO_INPUT_ENGINES（每行「id<Tab>字<Tab>名稱<Tab>1＝目前這個」）。
 */
#define _GNU_SOURCE
#include <pomelo.h>
#include <pomelo-settings.h>
#include <sys/select.h>
#include <xcb/xcb.h>
#include <xcb/xproto.h>
#include <xcb/shape.h>
#include <xcb/xcb_keysyms.h>
#include <xcb-imdkit/imdkit.h>
#include <xcb-imdkit/encoding.h>
#include <cairo-xcb.h>
#include <X11/keysym.h>
#include <signal.h>
#include "engine.h"
#include "ic.h"

#ifndef CHEWING_VERSION
#define CHEWING_VERSION "?"
#endif

static xcb_connection_t *conn;
static xcb_screen_t *screen;
static xcb_visualtype_t *visual;
static xcb_key_symbols_t *syms;
static xcb_im_t *im;
static xcb_window_t server_win, win;      /* 伺服器視窗（協定用，不顯示）、選字列 */
static int win_w, win_h, win_shown;
static xcb_atom_t a_settings, a_mode, a_ctl, a_label, a_engines, a_select, a_utf8;
static int native = 1;                    /* 全域：用輸入法（1）還是直接打英文（0） */
static int debug;                         /* POMELO_INPUT_DEBUG=1 時印出事件 */
static long shift_down_at = -1;           /* Shift 單獨按下的時間（輕點切換用） */
static int other_key_since_shift;

/* 所有的引擎；啟用的（input.engines，依序輪流）而且載入得了的才會用 */
static const Engine *const all_engines[] = { &eng_chewing, &eng_rime_pinyin, &eng_rime_cangjie, &eng_rime_quick, &eng_rime_jyutping, &eng_anthy, &eng_hangul, &eng_viet };
#define NENG (int)(sizeof all_engines / sizeof all_engines[0])
static int enabled[NENG], nenabled;       /* all_engines 的索引 */
static int cur_eng = -1;                  /* 目前的（all_engines 的索引；-1 = 沒有啟用任何輸入法） */

_Static_assert(NENG <= MAXENG, "ic.h 的 MAXENG 不夠");
static IC *cur_ic;                        /* 目前有焦點的輸入環境（XIM 或 IBus） */

/* 設定 */
char conf_kb[32] = "KB_DEFAULT", conf_hangul_kb[16] = "2", conf_viet[8] = "telex";
int conf_space_select = 1, conf_cand_n = 9;
static int conf_shift_toggle = 1;

static long now_ms(void) { return pm_now_ms(); }

/* ------------------------------------------------------------------ */
/* 設定                                                                */
/* ------------------------------------------------------------------ */

const char *tr(const char *s) { return _(s); }

void utf8_append(char *out, size_t n, const unsigned *s)
{
	size_t o = strlen(out);
	for (; *s && o + 5 < n; s++) {
		unsigned c = *s;
		if (c < 0x80) out[o++] = (char)c;
		else if (c < 0x800) { out[o++] = (char)(0xc0 | c >> 6); out[o++] = (char)(0x80 | (c & 63)); }
		else if (c < 0x10000) { out[o++] = (char)(0xe0 | c >> 12); out[o++] = (char)(0x80 | (c >> 6 & 63)); out[o++] = (char)(0x80 | (c & 63)); }
		else { out[o++] = (char)(0xf0 | c >> 18); out[o++] = (char)(0x80 | (c >> 12 & 63)); out[o++] = (char)(0x80 | (c >> 6 & 63)); out[o++] = (char)(0x80 | (c & 63)); }
	}
	out[o] = 0;
}

/* 介面語言預設的輸入法（還沒自己設定 input.engines 時）；英文、德文這些不需要輸入法的語言什麼都不啟用。
 * 跟 pomelo-language 的 default_engines 一樣。Rime 的字典還沒下載時載入不了，等「語言與輸入法」下載好 */
static const char *default_engines(void)
{
	const char *l = pm_lang();
	if (!strncmp(l, "zh_CN", 5) || !strncmp(l, "zh_SG", 5)) return "rime-pinyin";
	if (!strncmp(l, "zh_HK", 5) || !strncmp(l, "zh_MO", 5)) return "rime-cangjie,rime-quick";
	if (!strncmp(l, "zh", 2)) return "chewing";
	if (!strncmp(l, "ja", 2)) return "anthy";
	if (!strncmp(l, "ko", 2)) return "hangul";
	if (!strncmp(l, "vi", 2)) return "viet";
	return "";
}

static void load_conf(void)
{
	pm_conf_load();
	snprintf(conf_kb, sizeof conf_kb, "%s", pm_conf_get("input.kb", "KB_DEFAULT"));
	snprintf(conf_hangul_kb, sizeof conf_hangul_kb, "%s", pm_conf_get("input.hangul_kb", "2"));
	snprintf(conf_viet, sizeof conf_viet, "%s", pm_conf_get("input.viet", "telex"));
	conf_space_select = pm_conf_int("input.space_select", 1);
	conf_shift_toggle = pm_conf_int("input.shift_toggle", 1);
	conf_cand_n = pm_conf_int("input.cand_per_page", 9);
	conf_cand_n = conf_cand_n < 4 ? 4 : conf_cand_n > 9 ? 9 : conf_cand_n;
	/* 啟用的輸入法：設定裡的順序，載入不了的略過（引擎載入時會用到上面的設定，例如 Rime 的每頁字數） */
	char list[512];
	snprintf(list, sizeof list, "%s", pm_conf_get("input.engines", default_engines()));
	nenabled = 0;
	for (char *save = NULL, *t = strtok_r(list, ", ", &save); t; t = strtok_r(NULL, ", ", &save))
		for (int i = 0; i < NENG; i++)
			if (!strcmp(all_engines[i]->id, t) && all_engines[i]->load()) {
				int dup = 0;
				for (int k = 0; k < nenabled; k++) dup |= enabled[k] == i;
				if (!dup && nenabled < NENG) enabled[nenabled++] = i;
			}
	const char *want = pm_conf_get("input.engine", "");
	cur_eng = nenabled ? enabled[0] : -1;
	for (int k = 0; k < nenabled; k++) if (!strcmp(all_engines[enabled[k]]->id, want)) cur_eng = enabled[k];
	pm_family = pm_conf_get("appearance.font", "Noto Sans CJK TC");
	pm_size = pm_conf_num("appearance.font_size", 13);
	pm_set_theme(pm_conf_int("appearance.dark", 0), pm_conf_color("appearance.accent", 0));
}

/* ------------------------------------------------------------------ */
/* 選字列視窗                                                          */
/* ------------------------------------------------------------------ */

static View view;
static int cand_x[CAND_MAX + 1];   /* 每個候選字的 x 範圍（點選用） */

static xcb_visualtype_t *find_visual(void)
{
	xcb_depth_iterator_t d = xcb_screen_allowed_depths_iterator(screen);
	for (; d.rem; xcb_depth_next(&d)) {
		xcb_visualtype_iterator_t v = xcb_depth_visuals_iterator(d.data);
		for (; v.rem; xcb_visualtype_next(&v))
			if (v.data->visual_id == screen->root_visual) return v.data;
	}
	return NULL;
}

static void shape_round(int w, int h, int r)
{
	xcb_pixmap_t pm = xcb_generate_id(conn);
	xcb_create_pixmap(conn, 1, pm, win, (uint16_t)w, (uint16_t)h);
	cairo_surface_t *s = cairo_xcb_surface_create_for_bitmap(conn, screen, pm, w, h);
	cairo_t *cr = cairo_create(s);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba(cr, 0, 0, 0, 0);
	cairo_paint(cr);
	cairo_set_source_rgba(cr, 1, 1, 1, 1);
	pm_rrect(cr, 0, 0, w, h, r, r);
	cairo_fill(cr);
	cairo_destroy(cr);
	cairo_surface_flush(s);
	cairo_surface_destroy(s);
	xcb_shape_mask(conn, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_BOUNDING, win, 0, 0, pm);
	xcb_free_pixmap(conn, pm);
}

/* 頁數：「2/5」；不知道總共幾頁時「2 ›」（後面還有）或「2」 */
static void page_label(char *pg, size_t n)
{
	if (view.npage > 0) snprintf(pg, n, "%d/%d", view.page + 1, view.npage);
	else snprintf(pg, n, "%d%s", view.page + 1, view.more ? " ›" : "");
}

/* 量尺寸＋畫（cr 為 NULL 時只量） */
static void draw_view(cairo_t *cr, int *out_w, int *out_h)
{
	const double pad = 10, fs = pm_size + 2, lh = fs * 1.5;
	cairo_surface_t *tmp = NULL;
	if (!cr) { tmp = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1); cr = cairo_create(tmp); }
	pm_font(cr, fs, 0);
	double w1 = pad + pm_textw(cr, view.pre) + pm_textw(cr, view.comp) + pm_textw(cr, view.post) + pad + 2;
	char lab[CAND_MAX][8];
	for (int i = 0; i < view.ncand; i++) {
		if (view.label[i][0]) snprintf(lab[i], sizeof lab[i], "%s", view.label[i]);
		else snprintf(lab[i], sizeof lab[i], "%d", (i + 1) % 10);
	}
	int has_pre = view.pre[0] || view.comp[0] || view.post[0];
	double w2 = 0;
	if (view.ncand) {
		w2 = pad;
		for (int i = 0; i < view.ncand; i++) {
			char t[128];
			snprintf(t, sizeof t, "%s %s", lab[i], view.cand[i]);
			cand_x[i] = (int)w2;
			w2 += pm_textw(cr, t) + 16;
			if (view.note[i][0]) { pm_font(cr, fs - 4, 0); w2 += pm_textw(cr, view.note[i]) + 8; pm_font(cr, fs, 0); }
		}
		cand_x[view.ncand] = (int)w2;
		if (view.npage > 1 || view.more || view.page > 0) {
			char pg[32];
			pm_font(cr, fs - 3, 0);
			page_label(pg, sizeof pg);
			w2 += pm_textw(cr, pg) + 4;
			pm_font(cr, fs, 0);
		}
		w2 += pad - 8;
	}
	double w3 = view.aux[0] ? pad * 2 + pm_textw(cr, view.aux) : 0;
	int W = (int)MAX(MAX(w1 * has_pre, w2), MAX(w3, 60.0));
	int H = (int)(lh * (has_pre + (view.ncand > 0) + (view.aux[0] != 0)) + 8);
	if (out_w) *out_w = W;
	if (out_h) *out_h = H;
	if (tmp) { cairo_destroy(cr); cairo_surface_destroy(tmp); return; }

	pm_rgb(cr, T->menu);
	cairo_paint(cr);
	pm_rrect(cr, 0.5, 0.5, W - 1, H - 1, 9, 9);
	pm_rgba(cr, 0x000000, pm_dark ? 0.55 : 0.18);
	cairo_set_line_width(cr, 1);
	cairo_stroke(cr);
	double y = 4;
	pm_font(cr, fs, 0);
	if (has_pre) {   /* 組字區：已轉好的字 + 正在拼的（強調色） + 游標 */
		double x = pad;
		pm_rgb(cr, T->menu_text);
		pm_text(cr, x, y + lh / 2, view.pre);
		x += pm_textw(cr, view.pre);
		if (view.comp[0]) {
			pm_rgb(cr, T->sel);
			pm_text(cr, x, y + lh / 2, view.comp);
			double bw = pm_textw(cr, view.comp);
			cairo_rectangle(cr, x, y + lh - 4, bw, 1.5);
			cairo_fill(cr);
			x += bw;
		} else {
			pm_rgb(cr, T->sel);
			cairo_rectangle(cr, x, y + 5, 1.5, lh - 10);
			cairo_fill(cr);
		}
		pm_rgb(cr, T->menu_text);
		pm_text(cr, x, y + lh / 2, view.post);
		/* 整段組字區畫一條淡淡的底線 */
		pm_rgba(cr, T->menu_text, 0.35);
		cairo_rectangle(cr, pad, y + lh - 2, x + pm_textw(cr, view.post) - pad, 1);
		cairo_fill(cr);
		y += lh;
	}
	if (view.ncand) {
		for (int i = 0; i < view.ncand; i++) {
			char t[128];
			snprintf(t, sizeof t, "%s %s", lab[i], view.cand[i]);
			double x = cand_x[i], tw = cand_x[i + 1] - cand_x[i];
			if (i == view.sel) {
				pm_rgb(cr, T->sel);
				pm_rrect(cr, x - 2, y + 3, tw - 8, lh - 6, 6, 6);
				cairo_fill(cr);
				pm_rgb(cr, 0xffffff);
			} else pm_rgb(cr, T->menu_text);
			pm_text(cr, x + 6, y + lh / 2, t);
			if (view.note[i][0]) {   /* 讀音、補碼提示：小一號的灰字 */
				double nx = x + 6 + pm_textw(cr, t) + 4;
				pm_font(cr, fs - 4, 0);
				if (i == view.sel) pm_rgba(cr, 0xffffff, 0.8); else pm_rgb(cr, T->dim);
				pm_text(cr, nx, y + lh / 2 + 1, view.note[i]);
				pm_font(cr, fs, 0);
			}
		}
		if (view.npage > 1 || view.more || view.page > 0) {
			char pg[32];
			page_label(pg, sizeof pg);
			pm_font(cr, fs - 3, 0);
			pm_rgb(cr, T->dim);
			pm_text(cr, cand_x[view.ncand] + 2, y + lh / 2, pg);
			pm_font(cr, fs, 0);
		}
		y += lh;
	}
	if (view.aux[0]) {
		pm_font(cr, fs - 2, 0);
		pm_rgb(cr, T->dim);
		pm_text(cr, pad, y + lh / 2, view.aux);
	}
}

static int wl_shown;   /* 原生 Wayland 程式的選字列（wl.c 的 popup）顯示中 */

static void hide_win(void)
{
	if (wl_shown) { wl_hide(); wl_shown = 0; }
	if (!win_shown) return;
	xcb_unmap_window(conn, win);
	win_shown = 0;
	xcb_flush(conn);
}

/* 焦點在原生 Wayland 視窗時，它的畫面在螢幕上的位置（pomelo-compositor 寫在根視窗 _POMELO_WL_ACTIVE 指的替身上）。
 * 原生 Wayland 的 Qt 程式用 IBus 協定時不知道自己在螢幕哪裡，給的游標座標是相對視窗的 */
static int native_origin(int *ox, int *oy)
{
	static xcb_atom_t a_active, a_origin;
	if (!a_active) {
		xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(conn, xcb_intern_atom(conn, 0, 17, "_POMELO_WL_ACTIVE"), NULL);
		a_active = r ? r->atom : XCB_ATOM_NONE; free(r);
		r = xcb_intern_atom_reply(conn, xcb_intern_atom(conn, 0, 17, "_POMELO_WL_ORIGIN"), NULL);
		a_origin = r ? r->atom : XCB_ATOM_NONE; free(r);
	}
	if (!a_active || !a_origin) return 0;
	xcb_get_property_reply_t *r = xcb_get_property_reply(conn, xcb_get_property(conn, 0, screen->root, a_active, XCB_ATOM_WINDOW, 0, 1), NULL);
	xcb_window_t w = r && xcb_get_property_value_length(r) >= 4 ? *(xcb_window_t *)xcb_get_property_value(r) : 0;
	free(r);
	if (!w) return 0;
	r = xcb_get_property_reply(conn, xcb_get_property(conn, 0, w, a_origin, XCB_ATOM_CARDINAL, 0, 2), NULL);
	int ok = r && xcb_get_property_value_length(r) >= 8;
	if (ok) { int32_t *v = xcb_get_property_value(r); *ox = v[0]; *oy = v[1]; }
	free(r);
	return ok;
}

/* 把選字列放到游標下面 */
static void place_win(IC *c, int w, int h)
{
	int x = 0, y = 0, have_spot = 0, line = (int)(pm_size * 1.6);
	xcb_im_input_context_t *ic = c->xic;
	xcb_window_t fw = 0;
	if (c->ibus) {
		/* Qt 程式：游標位置由 SetCursorLocation 告訴我們；還不知道就放在畫面下方中間。
		 * 原生 Wayland 的 Qt 給的是相對視窗的座標：焦點在原生視窗時加上視窗的位置 */
		int ox = 0, oy = 0, native = c->has_cursor && native_origin(&ox, &oy);
		if (c->has_cursor && (native || !c->rel_cursor)) {
			x = c->cx + ox; y = c->cy + oy + c->ch + 4; line = MAX(c->ch, 8); have_spot = 1;
		} else { x = (screen->width_in_pixels - w) / 2; y = screen->height_in_pixels - h - 80; }
	} else {
		fw = xcb_im_input_context_get_focus_window(ic);
		if (!fw) fw = xcb_im_input_context_get_client_window(ic);
	}
	if (fw) {
		xcb_get_geometry_reply_t *g = xcb_get_geometry_reply(conn, xcb_get_geometry(conn, fw), NULL);
		xcb_translate_coordinates_reply_t *t = xcb_translate_coordinates_reply(conn, xcb_translate_coordinates(conn, fw, screen->root, 0, 0), NULL);
		if (g && t) {
			const xcb_im_preedit_attr_t *pa = xcb_im_input_context_get_preedit_attr(ic);
			if ((xcb_im_input_context_get_preedit_attr_mask(ic) & XCB_XIM_XNSpotLocation_MASK) && pa && (pa->spot_location.x || pa->spot_location.y)) {
				x = t->dst_x + pa->spot_location.x;
				y = t->dst_y + pa->spot_location.y + 4;
				have_spot = 1;
			} else {
				x = t->dst_x + 8;
				y = t->dst_y + g->height - h - 8;
			}
		}
		free(g);
		free(t);
	}
	int sw_ = screen->width_in_pixels, sh_ = screen->height_in_pixels;
	if (x + w > sw_) x = sw_ - w;
	if (x < 0) x = 0;
	if (y + h > sh_) y = have_spot ? y - line - h - 8 : sh_ - h;
	if (y < 0) y = 0;
	uint32_t vals[] = { (uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h, XCB_STACK_MODE_ABOVE };
	xcb_configure_window(conn, win, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT | XCB_CONFIG_WINDOW_STACK_MODE, vals);
}

static void paint_win(void)
{
	cairo_surface_t *s = cairo_xcb_surface_create(conn, win, visual, win_w, win_h);
	cairo_t *cr = cairo_create(s);
	draw_view(cr, NULL, NULL);
	cairo_destroy(cr);
	cairo_surface_flush(s);
	cairo_surface_destroy(s);
	xcb_flush(conn);
}

static void show_win(IC *ic)
{
	int w, h;
	draw_view(NULL, &w, &h);
	int has_pre = view.pre[0] || view.comp[0] || view.post[0];
	if (!has_pre && !view.ncand && !view.aux[0]) { hide_win(); return; }
	if (ic && ic->wl) {   /* 原生 Wayland 程式：compositor 把 popup 擺在文字游標下面 */
		if (win_shown) { xcb_unmap_window(conn, win); win_shown = 0; }
		win_w = w; win_h = h;
		wl_show(w, h);
		wl_shown = 1;
		return;
	}
	if (wl_shown) { wl_hide(); wl_shown = 0; }
	place_win(ic, w, h);
	if (w != win_w || h != win_h) { win_w = w; win_h = h; shape_round(w, h, 9); }
	if (!win_shown) { xcb_map_window(conn, win); win_shown = 1; }
	paint_win();
}

/* ------------------------------------------------------------------ */
/* 引擎                                                                */
/* ------------------------------------------------------------------ */

IC *ic_new(void) { return calloc(1, sizeof(IC)); }

static void free_ic(void *p)
{
	IC *d = p;
	if (!d) return;
	for (int i = 0; i < NENG; i++) if (d->ctx[i]) all_engines[i]->destroy(d->ctx[i]);
	if (cur_ic == d) cur_ic = NULL;
	free(d);
}

/* XIM 的輸入環境對應的 IC（建立 IC 時就放好；保險起見沒有就補一個） */
static IC *xim_ic(xcb_im_input_context_t *xic)
{
	IC *c = xcb_im_input_context_get_data(xic);
	if (!c && (c = ic_new())) { c->xic = xic; xcb_im_input_context_set_data(xic, c, free_ic); }
	return c;
}

/* 這個輸入環境裡目前引擎的狀態（第一次用時才建立） */
static void *ectx(IC *ic)
{
	if (cur_eng < 0 || !ic) return NULL;
	if (!ic->ctx[cur_eng]) ic->ctx[cur_eng] = all_engines[cur_eng]->create();
	return ic->ctx[cur_eng];
}

static void commit_utf8(IC *ic, const char *s)
{
	if (!s || !*s) return;
	if (ic->ibus) { ibus_commit(ic, s); return; }
	if (ic->wl) { wl_commit(ic, s); return; }
	size_t n = 0;
	char *ct = xcb_utf8_to_compound_text(s, strlen(s), &n);
	if (ct) {
		xcb_im_commit_string(im, ic->xic, XCB_XIM_LOOKUP_CHARS, ct, (uint32_t)n, 0);
		free(ct);
	}
}

/* 引擎有要送出的字就送出 */
static void take_commit(IC *ic)
{
	void *c = ectx(ic);
	char out[2048];
	if (c && all_engines[cur_eng]->commit(c, out, sizeof out)) commit_utf8(ic, out);
}

static void update_view(IC *ic)
{
	memset(&view, 0, sizeof view);
	void *c = native ? ectx(ic) : NULL;
	if (c) all_engines[cur_eng]->view(c, &view);
}

static void after_key(IC *ic)
{
	take_commit(ic);
	update_view(ic);
	show_win(ic);
}

/* 選單列看的狀態 */
static void publish(void)
{
	const char *m = cur_eng < 0 ? "" : native ? "zh" : "en";
	xcb_change_property(conn, XCB_PROP_MODE_REPLACE, screen->root, a_mode, XCB_ATOM_STRING, 8, (uint32_t)strlen(m), m);
	const char *l = cur_eng < 0 ? "" : all_engines[cur_eng]->badge;
	xcb_change_property(conn, XCB_PROP_MODE_REPLACE, screen->root, a_label, a_utf8, 8, (uint32_t)strlen(l), l);
	char list[1024] = "";
	for (int k = 0; k < nenabled; k++) {
		const Engine *e = all_engines[enabled[k]];
		size_t o = strlen(list);
		snprintf(list + o, sizeof list - o, "%s\t%s\t%s\t%d\n", e->id, e->badge, e->name, enabled[k] == cur_eng);
	}
	xcb_change_property(conn, XCB_PROP_MODE_REPLACE, screen->root, a_engines, a_utf8, 8, (uint32_t)strlen(list), list);
	xcb_flush(conn);
}

/* 目前的組字區送出、清掉（切換輸入法、切到英文前） */
static void flush_current(void)
{
	if (!cur_ic || cur_eng < 0) return;
	void *c = ectx(cur_ic);
	if (!c) return;
	all_engines[cur_eng]->flush(c);
	take_commit(cur_ic);
	all_engines[cur_eng]->reset(c);
}

/* 切換輸入法／英文：切到英文時先把組字區送出去 */
static void set_native(int on)
{
	if (native == on || cur_eng < 0) return;
	if (!on) flush_current();
	native = on;
	if (cur_ic) { update_view(cur_ic); show_win(cur_ic); }
	publish();
}

/* 換成某個輸入法（all_engines 的索引；同時切回輸入法模式） */
static void select_engine(int idx)
{
	if (idx < 0 || idx >= NENG) return;
	if (idx != cur_eng) {
		flush_current();
		cur_eng = idx;
		pm_conf_set("input.engine", all_engines[idx]->id);
	}
	native = 1;
	if (cur_ic) { update_view(cur_ic); show_win(cur_ic); }
	publish();
}

static void next_engine(void)
{
	if (nenabled < 2) { set_native(1); return; }
	int k = 0;
	while (k < nenabled && enabled[k] != cur_eng) k++;
	select_engine(enabled[(k + 1) % nenabled]);
}

static xcb_keysym_t keysym_of(xcb_key_press_event_t *e)
{
	xcb_keysym_t k0 = xcb_key_symbols_get_keysym(syms, e->detail, 0), k1 = xcb_key_symbols_get_keysym(syms, e->detail, 1);
	int shift = !!(e->state & XCB_MOD_MASK_SHIFT), lock = !!(e->state & XCB_MOD_MASK_LOCK);
	if (k0 >= 'a' && k0 <= 'z') return (shift ^ lock) ? k1 : k0;
	return shift && k1 != XCB_NO_SYMBOL ? k1 : k0;
}

/* 回傳 1 表示吃掉了，0 表示要原樣轉給程式。ks 是 keysym，state 是 X 的修飾鍵 */
static int handle_key(IC *ic, int press, xcb_keysym_t ks, unsigned state)
{
	int ctrl = !!(state & XCB_MOD_MASK_CONTROL), alt = !!(state & XCB_MOD_MASK_1), super = !!(state & XCB_MOD_MASK_4);
	int shift = !!(state & XCB_MOD_MASK_SHIFT);
	int is_shift = ks == XK_Shift_L || ks == XK_Shift_R;
	if (cur_eng < 0) return 0;   /* 沒有啟用任何輸入法 */

	/* Shift 輕點：按下放開之間沒按別的鍵 */
	if (is_shift) {
		if (press) { shift_down_at = now_ms(); other_key_since_shift = 0; }
		else if (conf_shift_toggle && shift_down_at >= 0 && !other_key_since_shift && now_ms() - shift_down_at < 400 && !ctrl && !alt && !super) {
			shift_down_at = -1;
			set_native(!native);
		}
		return 0;
	}
	if (press) other_key_since_shift = 1;
	if (!press) return 0;   /* 放開鍵一律轉回去 */

	if (ctrl && ks == XK_space) { if (shift) next_engine(); else set_native(!native); return 1; }
	if (ks == XK_Hangul || ks == XK_Zenkaku_Hankaku || ks == XK_Kanji) { set_native(!native); return 1; }   /* 韓文鍵盤的「한/영」、日文鍵盤的「半角/全角」 */
	if (!native || alt || super || ks == XK_Caps_Lock) return 0;

	void *c = ectx(ic);
	if (!c) return 0;
	int ate = all_engines[cur_eng]->key(c, ks, state);
	/* 不管吃不吃，先把要送出的字送出，再讓沒吃掉的鍵轉給程式（順序才對） */
	after_key(ic);
	return ate;
}

/* ------------------------------------------------------------------ */
/* XIM 事件                                                            */
/* ------------------------------------------------------------------ */

static void im_callback(xcb_im_t *im_, xcb_im_client_t *client, xcb_im_input_context_t *ic,
                        const xcb_im_packet_header_fr_t *hdr, void *frame, void *arg, void *user)
{
	(void)im_; (void)client; (void)frame; (void)user;
	if (debug) fprintf(stderr, "xim: opcode %d ic=%p\n", hdr->major_opcode, (void *)ic);
	switch (hdr->major_opcode) {
	case XCB_XIM_CREATE_IC: {
		IC *c = ic_new();
		if (c) { c->xic = ic; xcb_im_input_context_set_data(ic, c, free_ic); }
		break;
	}
	case XCB_XIM_DESTROY_IC:
		if (cur_ic && cur_ic->xic == ic) { cur_ic = NULL; hide_win(); }
		break;
	case XCB_XIM_SET_IC_FOCUS:
		ic_focus(xim_ic(ic), 1);
		break;
	case XCB_XIM_UNSET_IC_FOCUS:
		ic_focus(xim_ic(ic), 0);
		break;
	case XCB_XIM_SET_IC_VALUES:   /* 游標位置可能變了 */
		ic_cursor_moved(xim_ic(ic));
		break;
	case XCB_XIM_RESET_IC:
		ic_reset(xim_ic(ic));
		break;
	case XCB_XIM_FORWARD_EVENT: {   /* 按鍵事件在 arg（frame 是協定的封包） */
		xcb_key_press_event_t *e = arg;
		IC *c = xim_ic(ic);
		if (!c) { xcb_im_forward_event(im, ic, e); break; }
		cur_ic = c;
		int ate = handle_key(c, (e->response_type & 0x7f) == XCB_KEY_PRESS, keysym_of(e), e->state);
		if (debug) fprintf(stderr, "key: type=%d code=%d state=%#x -> %s\n", e->response_type & 0x7f, e->detail, e->state, ate ? "吃掉" : "轉回");
		if (!ate) xcb_im_forward_event(im, ic, e);
		break;
	}
	}
}

/* ---- 給 XIM 與 IBus（ibus.c）共用的動作 ---- */

void ic_focus(IC *ic, int in)
{
	if (!ic) return;
	if (in) { cur_ic = ic; update_view(ic); show_win(ic); }
	else if (cur_ic == ic) { cur_ic = NULL; hide_win(); }
}

void ic_reset(IC *ic)
{
	if (!ic) return;
	if (cur_eng >= 0 && ic->ctx[cur_eng]) all_engines[cur_eng]->reset(ic->ctx[cur_eng]);
	if (cur_ic == ic) hide_win();
}

void ic_cursor_moved(IC *ic)
{
	if (ic && cur_ic == ic && win_shown) show_win(ic);
}

int ic_key(IC *ic, unsigned keysym, unsigned state, int press)
{
	if (!ic) return 0;
	cur_ic = ic;
	int ate = handle_key(ic, press, keysym, state);
	if (debug) fprintf(stderr, "%s key: %#x state=%#x %s -> %s\n", ic->wl ? "wayland" : "ibus", keysym, state, press ? "按下" : "放開", ate ? "吃掉" : "轉回");
	return ate;
}

void ic_destroy(IC *ic)
{
	if (!ic) return;
	if (cur_ic == ic) hide_win();
	free_ic(ic);
}

/* 選字列上的滑鼠：點選、滾輪翻頁、移過去時反白 */
static int cand_at(int x)
{
	for (int i = 0; i < view.ncand; i++) if (x >= cand_x[i] && x < cand_x[i + 1]) return i;
	return -1;
}

static void click_cand(int x)
{
	int i = cand_at(x);
	if (!cur_ic || i < 0) return;
	void *c = ectx(cur_ic);
	if (c) all_engines[cur_eng]->choose(c, i);
	after_key(cur_ic);
}

/* 送一個按鍵給引擎（滾輪翻頁用） */
static void engine_key(unsigned ks)
{
	void *c = cur_ic ? ectx(cur_ic) : NULL;
	if (!c) return;
	all_engines[cur_eng]->key(c, ks, 0);
	after_key(cur_ic);
}

/* ---- 給 wl.c：原生程式的選字列跟 X 的共用畫法與滑鼠操作 ---- */

void ui_draw(void *cr) { draw_view(cr, NULL, NULL); }
void ui_click(int x) { click_cand(x); }
void ui_scroll(int down) { if (view.ncand) engine_key(down ? XK_Page_Down : XK_Page_Up); }
void ui_hover(int x, int y)
{
	int i = y > win_h - (int)((pm_size + 2) * 1.5) - 8 ? cand_at(x) : -1;
	if (i >= 0 && i != view.sel && wl_shown && cur_ic) { view.sel = i; show_win(cur_ic); }
}

static void on_term(int sig) { (void)sig; exit(0); }   /* 讓 atexit 刪掉 IBus 的位址檔 */

/* --toggle、--next、--engine ID：改 root 的屬性，伺服器本來就在聽 root 的 PropertyNotify */
static int ctl_from_cli(const char *atom, const char *value)
{
	xcb_connection_t *c = xcb_connect(NULL, NULL);
	if (!c || xcb_connection_has_error(c)) return 1;
	xcb_screen_t *s = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
	xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(c, xcb_intern_atom(c, 0, (uint16_t)strlen(atom), atom), NULL);
	if (!r) return 1;
	if (value) xcb_change_property(c, XCB_PROP_MODE_REPLACE, s->root, r->atom, XCB_ATOM_STRING, 8, (uint32_t)strlen(value), value);
	else {
		uint32_t v = (uint32_t)time(NULL);
		xcb_change_property(c, XCB_PROP_MODE_REPLACE, s->root, r->atom, XCB_ATOM_CARDINAL, 32, 1, &v);
	}
	/* 等伺服器處理完再斷線 */
	free(xcb_get_input_focus_reply(c, xcb_get_input_focus(c), NULL));
	free(r);
	xcb_disconnect(c);
	return 0;
}

int main(int argc, char *argv[])
{
	if (argc > 1 && !strcmp(argv[1], "--toggle")) return ctl_from_cli("_POMELO_INPUT_TOGGLE", NULL);
	if (argc > 1 && !strcmp(argv[1], "--next")) return ctl_from_cli("_POMELO_INPUT_SELECT", "+");
	if (argc > 2 && !strcmp(argv[1], "--engine")) return ctl_from_cli("_POMELO_INPUT_SELECT", argv[2]);
	if (argc > 1 && (!strcmp(argv[1], "--version") || !strcmp(argv[1], "-v"))) {
		puts("pomelo-input " POMELO_VERSION " (libchewing " CHEWING_VERSION ")");
		return 0;
	}
	if (argc > 1) { puts(_("用法：pomelo-input [--toggle | --next | --engine 輸入法 | --version]")); return 0; }
	setlocale(LC_ALL, "");
	debug = getenv("POMELO_INPUT_DEBUG") != NULL;
	int scrn = 0;
	conn = xcb_connect(NULL, &scrn);
	if (!conn || xcb_connection_has_error(conn)) { fputs("pomelo-input: 無法開啟 X display\n", stderr); return 2; }
	xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(conn));
	for (int i = 0; i < scrn; i++) xcb_screen_next(&it);
	screen = it.data;
	visual = find_visual();
	syms = xcb_key_symbols_alloc(conn);
	xcb_compound_text_init();
	load_conf();

	/* 原子 */
	const char *names[] = { "_POMELO_SETTINGS", "_POMELO_INPUT_MODE", "_POMELO_INPUT_TOGGLE", "_POMELO_INPUT_LABEL",
	                        "_POMELO_INPUT_ENGINES", "_POMELO_INPUT_SELECT", "UTF8_STRING" };
	xcb_atom_t *outs[] = { &a_settings, &a_mode, &a_ctl, &a_label, &a_engines, &a_select, &a_utf8 };
	for (int i = 0; i < 7; i++) {
		xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(conn, xcb_intern_atom(conn, 0, (uint16_t)strlen(names[i]), names[i]), NULL);
		*outs[i] = r ? r->atom : XCB_ATOM_NONE;
		free(r);
	}
	/* 伺服器視窗（不顯示）與選字列 */
	server_win = xcb_generate_id(conn);
	xcb_create_window(conn, XCB_COPY_FROM_PARENT, server_win, screen->root, 0, 0, 1, 1, 0,
	                  XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, 0, NULL);
	win = xcb_generate_id(conn);
	uint32_t wvals[] = { screen->white_pixel, 1, XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_LEAVE_WINDOW };
	xcb_create_window(conn, XCB_COPY_FROM_PARENT, win, screen->root, 0, 0, 60, 30, 0,
	                  XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
	                  XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK, wvals);
	xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8, 25, "pomelo-input\0pomelo-input");

	/* XIM：不登記觸發鍵，所有按鍵都轉過來，由我們決定吃不吃 */
	uint32_t styles[] = { XCB_IM_PreeditPosition | XCB_IM_StatusNothing, XCB_IM_PreeditNothing | XCB_IM_StatusNothing,
	                      XCB_IM_PreeditPosition | XCB_IM_StatusNone, XCB_IM_PreeditNothing | XCB_IM_StatusNone,
	                      XCB_IM_PreeditNone | XCB_IM_StatusNone };
	xcb_im_styles_t im_styles = { 5, styles };
	xcb_im_encoding_t encs[] = { "COMPOUND_TEXT" };
	xcb_im_encodings_t im_encs = { 1, encs };
	xcb_im_trigger_keys_t nokeys = { 0, NULL };
	im = xcb_im_create(conn, scrn, server_win, "pomelo", XCB_IM_ALL_LOCALES, &im_styles, &nokeys, &nokeys, &im_encs,
	                   XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_KEY_RELEASE, im_callback, NULL);
	xcb_im_set_use_sync_mode(im, true);
	if (!xcb_im_open_im(im)) { fputs("pomelo-input: 無法註冊 XIM 伺服器（已經有一個在跑？）\n", stderr); return 1; }
	/* 聽 root 的屬性變動（設定、切換中英）；要在 open_im 之後設，不然會被它蓋掉 */
	uint32_t rmask = XCB_EVENT_MASK_PROPERTY_CHANGE;
	xcb_change_window_attributes(conn, screen->root, XCB_CW_EVENT_MASK, &rmask);
	native = 1;
	publish();
	/* Qt 程式：IBus 協定（開不起來也沒關係，XIM 照常） */
	ibus_start(getenv("DISPLAY"));
	/* 原生 Wayland 程式：compositor 的 input-method-v2（不是 Wayland 工作階段就略過） */
	if (wl_start() == 0 && debug) fputs("pomelo-input: 已接上 compositor 的 input-method-v2\n", stderr);
	atexit(ibus_stop);
	signal(SIGTERM, on_term);
	signal(SIGINT, on_term);
	signal(SIGPIPE, SIG_IGN);

	int fd = xcb_get_file_descriptor(conn);
	for (;;) {
		xcb_generic_event_t *ev;
		while ((ev = xcb_poll_for_event(conn))) {
			if (xcb_im_filter_event(im, ev)) { free(ev); continue; }
			switch (ev->response_type & 0x7f) {
			case XCB_EXPOSE: if (((xcb_expose_event_t *)ev)->window == win && win_shown) paint_win(); break;
			case XCB_BUTTON_PRESS: {
				xcb_button_press_event_t *b = (xcb_button_press_event_t *)ev;
				if (b->event == win && b->detail == 1) click_cand(b->event_x);
				else if (b->event == win && (b->detail == 4 || b->detail == 5) && view.ncand) engine_key(b->detail == 4 ? XK_Page_Up : XK_Page_Down);
				break;
			}
			case XCB_MOTION_NOTIFY: {   /* 滑鼠移過去的候選字反白 */
				xcb_motion_notify_event_t *m = (xcb_motion_notify_event_t *)ev;
				int i = m->event == win && m->event_y > win_h - (int)((pm_size + 2) * 1.5) - 8 ? cand_at(m->event_x) : -1;
				if (i >= 0 && i != view.sel && win_shown) { view.sel = i; paint_win(); }
				break;
			}
			case XCB_PROPERTY_NOTIFY: {
				xcb_property_notify_event_t *p = (xcb_property_notify_event_t *)ev;
				if (debug) fprintf(stderr, "property %u on %#x (settings=%u toggle=%u)\n", p->atom, p->window, a_settings, a_ctl);
				if (p->window == screen->root && p->atom == a_settings) {
					if (cur_ic && cur_eng >= 0) flush_current();
					load_conf();
					if (cur_ic)   /* 設定改了：每個引擎都套用 */
						for (int i = 0; i < NENG; i++) if (cur_ic->ctx[i]) all_engines[i]->conf(cur_ic->ctx[i]);
					if (cur_eng < 0) hide_win();
					else if (win_shown) { win_w = 0; show_win(cur_ic); }
					publish();
				} else if (p->window == screen->root && p->atom == a_ctl && p->state == XCB_PROPERTY_NEW_VALUE) {
					set_native(!native);   /* 選單列的指示器或 --toggle */
				} else if (p->window == screen->root && p->atom == a_select && p->state == XCB_PROPERTY_NEW_VALUE) {
					/* --next 或 --engine ID */
					xcb_get_property_reply_t *r = xcb_get_property_reply(conn, xcb_get_property(conn, 1, screen->root, a_select, XCB_ATOM_STRING, 0, 64), NULL);
					char want[64] = "";
					if (r) snprintf(want, sizeof want, "%.*s", xcb_get_property_value_length(r), (char *)xcb_get_property_value(r));
					free(r);
					if (!strcmp(want, "+")) next_engine();
					else for (int k = 0; k < nenabled; k++) if (!strcmp(all_engines[enabled[k]]->id, want)) select_engine(enabled[k]);
				}
				break;
			}
			case XCB_MAPPING_NOTIFY: xcb_refresh_keyboard_mapping(syms, (xcb_mapping_notify_event_t *)ev); break;
			}
			free(ev);
		}
		xcb_flush(conn);
		if (xcb_connection_has_error(conn)) break;
		fd_set rf, wf;
		FD_ZERO(&rf);
		FD_ZERO(&wf);
		FD_SET(fd, &rf);
		int maxfd = ibus_fds(&rf, &wf, fd);
		maxfd = wl_fds(&rf, maxfd);
		/* 引擎在準備（例如 Rime 第一次部署）：每半秒更新一次選字列，好了提示就會消失 */
		int waiting = (win_shown || wl_shown) && view.aux[0] && cur_ic;
		int ms = waiting ? 500 : -1, rep = wl_timeout();
		if (rep >= 0 && (ms < 0 || rep < ms)) ms = rep;
		struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
		int n = select(maxfd + 1, &rf, &wf, NULL, ms >= 0 ? &tv : NULL);
		if (n < 0) { FD_ZERO(&rf); FD_ZERO(&wf); }
		wl_handle(&rf);   /* 每次 select 之後都要（wl_fds 開始的讀取要結束） */
		wl_tick();
		if (n == 0 && waiting) { update_view(cur_ic); show_win(cur_ic); }
		if (n > 0) { ibus_handle(&rf, &wf); xcb_flush(conn); }
	}
	return 0;
}
