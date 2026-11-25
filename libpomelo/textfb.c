/* SPDX-License-Identifier: GPL-3.0-or-later */
/* textfb.c — 泰文字型後援
 *
 * Pomelo 的介面字型固定是「Noto Sans CJK TC」。cairo 的 toy 文字 API（cairo_show_text）只用一個字型、沒有逐字後援，
 * 而 Noto Sans CJK 沒有泰文字形，所以泰文介面整個變成方塊。fontconfig 的後援在這裡不會自動發生。
 * 作法：pomelo.h 把 cairo_show_text／cairo_text_extents 轉到這裡；字串裡沒有泰文就直接呼叫原本的函式（只多掃一遍位元組），
 * 有泰文（U+0E00–U+0E7F）才把字串切成「泰文」與「其他」兩種段落，泰文段落暫時換成自由授權的泰文字型（Noto Sans Thai，
 * 沒有就找 Noto Sans Thai UI、Loma、Garuda…；用 cairo 的字形對照實際測有沒有泰文字形），畫完換回原本的字型。字級、粗細、斜體沿用目前的設定。
 * 限制：cairo 的 toy API 沒有 OpenType 排版（HarfBuzz），聲調符號疊在母音上的位置是字型的預設位置，少數組合會略擠。 */
#define POMELO_NO_TEXT_FALLBACK
#include "pomelo.h"

static int is_thai_lead(const char *s) { return (unsigned char)s[0] == 0xE0 && ((unsigned char)s[1] == 0xB8 || (unsigned char)s[1] == 0xB9); }

static int has_thai(const char *s)
{
	for (; *s; s++) if (is_thai_lead(s)) return 1;
	return 0;
}

static int cp_len(unsigned char c) { return c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1; }

/* 第一個真的有泰文字形（ก＋聲調符號 ้）的字型家族；測一次就記住 */
static const char *thai_family(void)
{
	static const char *fam;
	static int done;
	if (done) return fam;
	done = 1;
	static const char *const cand[] = { "Noto Sans Thai", "Noto Sans Thai UI", "Loma", "Garuda", "Waree", "Kinnari", "Noto Looped Thai", "Sarabun", NULL };
	cairo_surface_t *sf = cairo_image_surface_create(CAIRO_FORMAT_A8, 1, 1);
	cairo_t *cr = cairo_create(sf);
	for (int i = 0; cand[i] && !fam; i++) {
		cairo_select_font_face(cr, cand[i], CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
		cairo_set_font_size(cr, 12);
		cairo_glyph_t *g = NULL;
		int ng = 0;
		if (cairo_scaled_font_text_to_glyphs(cairo_get_scaled_font(cr), 0, 0, "\xe0\xb8\x81\xe0\xb9\x89", -1, &g, &ng, NULL, NULL, NULL) == CAIRO_STATUS_SUCCESS &&
		    ng == 2 && g[0].index && g[1].index)
			fam = cand[i];
		cairo_glyph_free(g);
	}
	cairo_destroy(cr);
	cairo_surface_destroy(sf);
	return fam;
}

/* 逐段處理：thai＝1 的段落用泰文字型，其餘用原本的字型 */
typedef struct { cairo_t *cr; cairo_font_face_t *orig, *th; } Fb;

static int fb_begin(Fb *f, cairo_t *cr)
{
	const char *fam = thai_family();
	f->cr = cr; f->orig = f->th = NULL;
	if (!fam) return 0;
	cairo_font_face_t *cur = cairo_get_font_face(cr);
	cairo_font_slant_t sl = CAIRO_FONT_SLANT_NORMAL;
	cairo_font_weight_t wt = CAIRO_FONT_WEIGHT_NORMAL;
	if (cairo_font_face_get_type(cur) == CAIRO_FONT_TYPE_TOY) { sl = cairo_toy_font_face_get_slant(cur); wt = cairo_toy_font_face_get_weight(cur); }
	f->orig = cairo_font_face_reference(cur);
	f->th = cairo_toy_font_face_create(fam, sl, wt);
	return 1;
}

static void fb_end(Fb *f)
{
	cairo_set_font_face(f->cr, f->orig);
	cairo_font_face_destroy(f->orig);
	cairo_font_face_destroy(f->th);
}

/* 取下一段：回傳長度；*thai 設成這段是不是泰文 */
static size_t next_run(const char *s, int *thai)
{
	size_t n = 0;
	*thai = is_thai_lead(s);
	while (s[n]) {
		if (is_thai_lead(s + n) != *thai) break;
		int l = cp_len((unsigned char)s[n]);
		for (int i = 0; i < l; i++) if (!s[n + i]) { l = i; break; }
		n += l;
	}
	return n ? n : 1;
}

void pm_show_text(cairo_t *cr, const char *s)
{
	Fb f;
	if (!s || !has_thai(s) || !fb_begin(&f, cr)) { cairo_show_text(cr, s); return; }
	char *buf = strdup(s);
	if (!buf) { fb_end(&f); cairo_show_text(cr, s); return; }
	for (const char *p = s; *p;) {
		int th;
		size_t n = next_run(p, &th);
		memcpy(buf, p, n); buf[n] = 0;
		cairo_set_font_face(cr, th ? f.th : f.orig);
		cairo_show_text(cr, buf);
		p += n;
	}
	free(buf);
	fb_end(&f);
}

void pm_text_extents(cairo_t *cr, const char *s, cairo_text_extents_t *e)
{
	Fb f;
	if (!s || !has_thai(s) || !fb_begin(&f, cr)) { cairo_text_extents(cr, s, e); return; }
	char *buf = strdup(s);
	if (!buf) { fb_end(&f); cairo_text_extents(cr, s, e); return; }
	double adv = 0, top = 1e9, bot = -1e9, left = 0, right = 0;
	int first = 1;
	for (const char *p = s; *p;) {
		int th;
		size_t n = next_run(p, &th);
		memcpy(buf, p, n); buf[n] = 0;
		cairo_set_font_face(cr, th ? f.th : f.orig);
		cairo_text_extents_t r;
		cairo_text_extents(cr, buf, &r);
		if (first) left = r.x_bearing;
		first = 0;
		if (r.y_bearing < top) top = r.y_bearing;
		if (r.y_bearing + r.height > bot) bot = r.y_bearing + r.height;
		right = adv + r.x_bearing + r.width;
		adv += r.x_advance;
		p += n;
	}
	free(buf);
	fb_end(&f);
	e->x_bearing = left; e->width = right - left;
	e->y_bearing = top < 1e9 ? top : 0; e->height = bot > -1e9 ? bot - e->y_bearing : 0;
	e->x_advance = adv; e->y_advance = 0;
}
