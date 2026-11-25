/* SPDX-License-Identifier: GPL-3.0-or-later */
/* layout.c — 見 layout.h */
#define _GNU_SOURCE
#include "layout.h"
#include <pomelo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#define TAB_STOP 36.0      /* 固定定位點：0.5 吋 */
#define LIST_INDENT 24.0   /* 每一層清單縮排 */
#define CELL_PAD 4.0       /* 表格儲存格內距 */
#define LINE_SPACING 1.15  /* 行高倍率 */
#define IMAGE_GAP 6.0      /* 圖片跟上下文字的間隔 */

/* ------------------------------------------------------------------ */
/* UTF-8 ／ 字型 小工具                                                  */
/* ------------------------------------------------------------------ */

static int utf8_len(unsigned char c)
{
	if (c < 0x80) return 1;
	if ((c & 0xe0) == 0xc0) return 2;
	if ((c & 0xf0) == 0xe0) return 3;
	if ((c & 0xf8) == 0xf0) return 4;
	return 1;
}

static unsigned utf8_decode(const char *s, int len)
{
	unsigned char c = (unsigned char)s[0];
	if (len <= 1) return c;
	if (len == 2) return (unsigned)((c & 0x1f) << 6) | ((unsigned char)s[1] & 0x3f);
	if (len == 3) return (unsigned)((c & 0x0f) << 12) | ((unsigned)((unsigned char)s[1] & 0x3f) << 6) | ((unsigned char)s[2] & 0x3f);
	return (unsigned)((c & 0x07) << 18) | ((unsigned)((unsigned char)s[1] & 0x3f) << 12) | ((unsigned)((unsigned char)s[2] & 0x3f) << 6) | ((unsigned char)s[3] & 0x3f);
}

static int is_cjk(unsigned cp)
{
	return (cp >= 0x2E80 && cp <= 0x9FFF) || (cp >= 0xA000 && cp <= 0xA4CF) || (cp >= 0xF900 && cp <= 0xFAFF)
	    || (cp >= 0xFF00 && cp <= 0xFFEF) || (cp >= 0x20000 && cp <= 0x2FFFF) || (cp >= 0x3000 && cp <= 0x303F);
}

/* p 可為 NULL（量測跟段落樣式無關的東西時，例如頁碼）；非 NULL 時套用段落樣式（標題）的預設字級／粗體，
 * run 自己有設定（size>0、bold）才蓋過去。 */
static void set_run_font(cairo_t *cr, const Run *r, const Para *p, const Document *d)
{
	const char *fam = r->font[0] ? r->font : d->default_font;
	PStyle style = p ? p->style : PSTYLE_BODY;
	double size = r->size > 0 ? r->size : pstyle_size(style, d->default_size);
	int bold = r->bold || pstyle_bold(style);
	cairo_select_font_face(cr, fam, r->italic ? CAIRO_FONT_SLANT_ITALIC : CAIRO_FONT_SLANT_NORMAL,
	                       bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cr, size);
}

/* ------------------------------------------------------------------ */
/* 圖片解碼（只認得 PNG、JPEG；圖片位元組來自文件模型，解碼結果交給 Layout 快取）     */
/* ------------------------------------------------------------------ */

typedef struct { const unsigned char *p; size_t n, off; } MemReader;

static cairo_status_t png_read_cb(void *closure, unsigned char *data, unsigned int length)
{
	MemReader *mr = closure;
	if (mr->off + length > mr->n) return CAIRO_STATUS_READ_ERROR;
	memcpy(data, mr->p + mr->off, length);
	mr->off += length;
	return CAIRO_STATUS_SUCCESS;
}

static cairo_surface_t *decode_image(const Image *im)
{
	if (im->is_png) {
		MemReader mr = { im->data, im->datalen, 0 };
		cairo_surface_t *s = cairo_image_surface_create_from_png_stream(png_read_cb, &mr);
		if (!s || cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) { if (s) cairo_surface_destroy(s); return NULL; }
		return s;
	}
	return pm_image_jpeg_mem(im->data, im->datalen, 0);
}

/* ------------------------------------------------------------------ */
/* 版面陣列管理                                                          */
/* ------------------------------------------------------------------ */

static Line *push_line(Layout *lo)
{
	if (lo->nline >= lo->linecap) { lo->linecap = lo->linecap ? lo->linecap * 2 : 64; lo->line = realloc(lo->line, (size_t)lo->linecap * sizeof(Line)); }
	Line *l = &lo->line[lo->nline++];
	memset(l, 0, sizeof *l);
	return l;
}

static TableGeom *push_tbl(Layout *lo)
{
	if (lo->ntbl >= lo->tblcap) { lo->tblcap = lo->tblcap ? lo->tblcap * 2 : 8; lo->tbl = realloc(lo->tbl, (size_t)lo->tblcap * sizeof(TableGeom)); }
	TableGeom *t = &lo->tbl[lo->ntbl++];
	memset(t, 0, sizeof *t);
	return t;
}

static ImageGeom *push_img(Layout *lo)
{
	if (lo->nimg >= lo->imgcap) { lo->imgcap = lo->imgcap ? lo->imgcap * 2 : 8; lo->img = realloc(lo->img, (size_t)lo->imgcap * sizeof(ImageGeom)); }
	ImageGeom *g = &lo->img[lo->nimg++];
	memset(g, 0, sizeof *g);
	return g;
}

/* 排一張圖片：置中，太寬就縮到放得下（依比例縮，不會超出插入時設定的大小） */
static void layout_image(Layout *lo, int block, const Image *im, double x0, double avail_w, int *page, double *y, double page_top, double page_bottom)
{
	double w = im->w, h = im->h;
	if (w > avail_w && w > 0) { h *= avail_w / w; w = avail_w; }
	if (*y + h > page_bottom && *y > page_top) { (*page)++; *y = page_top; }
	ImageGeom *g = push_img(lo);
	g->block = block; g->page = *page;
	g->x = x0 + (avail_w - w) / 2.0; g->y = *y; g->w = w; g->h = h;
	g->surf = decode_image(im);
	*y += h + IMAGE_GAP;
}

/* ------------------------------------------------------------------ */
/* 字元量測表：把一個段落的文字先量成一串 (寬度、run) 的陣列，換行與畫圖共用       */
/* ------------------------------------------------------------------ */

typedef struct { int off, len; double adv, ascent, descent; const Run *run; unsigned cp; } CharM;

static CharM *measure_para(cairo_t *cr, const Document *d, const Para *p, const char *text, int *out_n)
{
	size_t total = para_text_len(p);
	CharM *cm = malloc((total + 1) * sizeof(CharM));
	int n = 0, off = 0, ri = 0, run_start = 0;
	while ((size_t)off < total) {
		int len = utf8_len((unsigned char)text[off]);
		if (off + len > (int)total) len = (int)total - off;
		while (ri < p->nrun - 1 && off >= run_start + (int)p->run[ri].len) { run_start += (int)p->run[ri].len; ri++; }
		const Run *r = &p->run[ri];
		set_run_font(cr, r, p, d);
		char buf[8];
		memcpy(buf, text + off, (size_t)len);
		buf[len] = 0;
		double adv;
		if (text[off] == '\t') adv = TAB_STOP;   /* Tab 另外處理寬度，不量真的字 */
		else { cairo_text_extents_t te; cairo_text_extents(cr, buf, &te); adv = te.x_advance; }
		cairo_font_extents_t fe;
		cairo_font_extents(cr, &fe);
		cm[n].off = off; cm[n].len = len; cm[n].adv = adv; cm[n].ascent = fe.ascent; cm[n].descent = fe.descent;
		cm[n].run = r; cm[n].cp = utf8_decode(text + off, len);
		n++;
		off += len;
	}
	*out_n = n;
	return cm;
}

/* ------------------------------------------------------------------ */
/* 清單編號計數器（decimal 清單延續編號用）                                 */
/* ------------------------------------------------------------------ */

typedef struct { int id, count; } ListCounter;
typedef struct { ListCounter *c; int n; } Counters;

static int counter_next(Counters *co, int list_id)
{
	for (int i = 0; i < co->n; i++) if (co->c[i].id == list_id) return ++co->c[i].count;
	co->c = realloc(co->c, (size_t)(co->n + 1) * sizeof(ListCounter));
	co->c[co->n].id = list_id;
	co->c[co->n].count = 1;
	co->n++;
	return 1;
}

/* ------------------------------------------------------------------ */
/* 排一個段落：在 (x0,avail_w) 範圍內換行，從 (*page,*y) 開始往下排；
 * 遇到頁尾放不下就換頁。回傳這個段落實際占用的行數。 */
/* ------------------------------------------------------------------ */

static int wrap_paragraph(Layout *lo, cairo_t *cr, const Document *d, int block, int row, int col,
                          const Para *p, double x0, double avail_w, int *page, double *y,
                          double page_top, double page_bottom, Counters *co, int record)
{
	char *text = para_text(p);
	int n;
	CharM *cm = measure_para(cr, d, p, text, &n);

	double indent = p->list_type != LIST_NONE ? LIST_INDENT * (p->list_level + 1) : 0;
	double lw = avail_w - indent;
	if (lw < 10) lw = 10;

	int nlines_out = 0;
	int line_start = 0;
	int i = 0;
	int first_line_of_para = 1;
	if (n == 0) {
		/* 空段落：至少佔一行（用段落預設字型量行高） */
		cairo_font_extents_t fe;
		static const Run empty_run;
		set_run_font(cr, p->nrun ? &p->run[0] : &empty_run, p, d);
		cairo_font_extents(cr, &fe);
		double h = (fe.ascent + fe.descent) * LINE_SPACING;
		if (*y + h > page_bottom && *y > page_top) { (*page)++; *y = page_top; }
		if (record) {
			Line *l = push_line(lo);
			l->block = block; l->row = row; l->col = col;
			l->off_start = 0; l->off_end = 0;
			l->page = *page; l->x = x0 + indent; l->y = *y; l->w = 0; l->h = h; l->ascent = fe.ascent * LINE_SPACING;
		}
		*y += h;
		free(cm);
		free(text);
		return 1;
	}
	while (line_start < n) {
		double pen = 0, max_asc = 0, max_desc = 0;
		int last_break = -1;   /* 斷行發生在這個索引「之後」 */
		i = line_start;
		while (i < n) {
			double adv = cm[i].adv;
			if (pen + adv > lw && i > line_start) break;
			pen += adv;
			if (cm[i].ascent > max_asc) max_asc = cm[i].ascent;
			if (cm[i].descent > max_desc) max_desc = cm[i].descent;
			if (cm[i].cp == ' ' || cm[i].cp == '\t' || is_cjk(cm[i].cp)) last_break = i;
			i++;
		}
		int line_end;          /* [line_start,line_end) 顯示在這一行 */
		int next_start;         /* 下一行從這裡開始 */
		if (i >= n) { line_end = n; next_start = n; }
		else if (last_break >= line_start) { line_end = last_break + 1; next_start = line_end; }
		else { line_end = i > line_start ? i : line_start + 1; next_start = line_end; }   /* 強制斷行（一個字都放不下也至少放一個） */
		/* 行尾若剛好是空白／Tab，不算進這一行的寬度／顯示範圍，但還是要跳過去 */
		int measured_end = line_end;
		if (measured_end > line_start && (cm[measured_end - 1].cp == ' ' || cm[measured_end - 1].cp == '\t')) measured_end--;

		double line_w = 0, asc = 0, desc = 0;
		for (int k = line_start; k < measured_end; k++) {
			line_w += cm[k].adv;
			if (cm[k].ascent > asc) asc = cm[k].ascent;
			if (cm[k].descent > desc) desc = cm[k].descent;
		}
		if (asc == 0 && desc == 0) { asc = max_asc; desc = max_desc; }   /* 這一行全是被裁掉的空白（罕見） */
		double h = (asc + desc) * LINE_SPACING;
		if (h <= 0) h = (d->default_size) * LINE_SPACING;

		if (*y + h > page_bottom && *y > page_top) { (*page)++; *y = page_top; }

		if (record) {
			Line *l = push_line(lo);
			l->block = block; l->row = row; l->col = col;
			l->off_start = cm[line_start].off;
			l->off_end = measured_end < n ? cm[measured_end].off : (int)para_text_len(p);
			l->page = *page;
			double lx = x0 + indent;
			if (p->align == ALIGN_CENTER) lx += (lw - line_w) / 2;
			else if (p->align == ALIGN_RIGHT) lx += lw - line_w;
			l->x = lx; l->y = *y; l->w = line_w; l->h = h; l->ascent = asc * LINE_SPACING;
			if (first_line_of_para && p->list_type != LIST_NONE) {
				/* 清單符號／編號：畫在縮排區（呼叫端畫圖時用 off_start==0 判斷要不要畫 marker） */
				if (p->list_type == LIST_DECIMAL) counter_next(co, p->list_id);
			}
		}
		*y += h;
		nlines_out++;
		first_line_of_para = 0;
		line_start = next_start;
	}
	free(cm);
	free(text);
	return nlines_out;
}

/* ------------------------------------------------------------------ */
/* 排一張表格                                                            */
/* ------------------------------------------------------------------ */

static void layout_table(Layout *lo, cairo_t *cr, const Document *d, int block, const Table *t,
                         double x0, double avail_w, int *page, double *y, double page_top, double page_bottom, Counters *co)
{
	int ncol = t->ncol;
	double *colw = malloc((size_t)ncol * sizeof(double));
	double sum = 0;
	for (int c = 0; c < ncol; c++) { colw[c] = t->colw ? t->colw[c] : avail_w / ncol; sum += colw[c]; }
	double scale = sum > 0 ? avail_w / sum : 1;
	for (int c = 0; c < ncol; c++) colw[c] *= scale;

	/* 先量每一列的高度（不實際記錄 Line，record=0），算出整張表格要多高，決定要不要先換頁 */
	double *rowh = malloc((size_t)t->nrow * sizeof(double));
	double table_h = 0;
	for (int r = 0; r < t->nrow; r++) {
		double maxh = 0;
		for (int c = 0; c < ncol; c++) {
			double yy = 0;
			int dummy_page = 0;
			wrap_paragraph(lo, cr, d, block, r, c, &t->cell[r * ncol + c], 0, colw[c] - 2 * CELL_PAD, &dummy_page, &yy, 0, 1e9, co, 0);
			if (yy + 2 * CELL_PAD > maxh) maxh = yy + 2 * CELL_PAD;
		}
		rowh[r] = maxh;
		table_h += maxh;
	}

	if (*y + table_h > page_bottom && *y > page_top) { (*page)++; *y = page_top; }
	/* 表格前面可能因為上面段落換頁，這裡再檢查一次（整張表格不拆列的政策：即使還是放不下也不再換頁，避免無窮換頁） */

	TableGeom *tg = push_tbl(lo);
	tg->block = block; tg->page = *page; tg->x = x0; tg->y = *y; tg->w = avail_w; tg->h = table_h;
	tg->nrow = t->nrow; tg->ncol = ncol;
	tg->colx = malloc((size_t)(ncol + 1) * sizeof(double));
	tg->rowy = malloc((size_t)(t->nrow + 1) * sizeof(double));
	double cx = 0;
	for (int c = 0; c <= ncol; c++) { tg->colx[c] = cx; if (c < ncol) cx += colw[c]; }
	double ry = 0;
	for (int r = 0; r <= t->nrow; r++) { tg->rowy[r] = ry; if (r < t->nrow) ry += rowh[r]; }

	double row_y = *y;
	for (int r = 0; r < t->nrow; r++) {
		for (int c = 0; c < ncol; c++) {
			double cell_y = row_y + CELL_PAD;
			int cell_page = *page;
			wrap_paragraph(lo, cr, d, block, r, c, &t->cell[r * ncol + c], x0 + tg->colx[c] + CELL_PAD, colw[c] - 2 * CELL_PAD,
			              &cell_page, &cell_y, row_y, row_y + rowh[r], co, 1);
		}
		row_y += rowh[r];
	}
	*y = row_y;
	free(colw);
	free(rowh);
}

/* ------------------------------------------------------------------ */
/* layout_build                                                        */
/* ------------------------------------------------------------------ */

Layout *layout_build(cairo_t *cr, const Document *d)
{
	Layout *lo = calloc(1, sizeof *lo);
	Counters co = { 0 };
	int page = 0;
	double y = d->margin_t;
	double page_top = d->margin_t, page_bottom = d->page_h - d->margin_b;
	double x0 = d->margin_l, avail_w = d->page_w - d->margin_l - d->margin_r;

	for (int i = 0; i < d->nblock; i++) {
		const Block *b = &d->block[i];
		if (b->kind == BLOCK_PARA) {
			wrap_paragraph(lo, cr, d, i, 0, 0, &b->para, x0, avail_w, &page, &y, page_top, page_bottom, &co, 1);
		} else if (b->kind == BLOCK_IMAGE) {
			layout_image(lo, i, &b->image, x0, avail_w, &page, &y, page_top, page_bottom);
		} else {
			layout_table(lo, cr, d, i, &b->table, x0, avail_w, &page, &y, page_top, page_bottom, &co);
		}
	}
	lo->npage = page + 1;
	free(co.c);
	return lo;
}

void layout_free(Layout *lo)
{
	if (!lo) return;
	free(lo->line);
	for (int i = 0; i < lo->ntbl; i++) { free(lo->tbl[i].colx); free(lo->tbl[i].rowy); }
	free(lo->tbl);
	for (int i = 0; i < lo->nimg; i++) if (lo->img[i].surf) cairo_surface_destroy(lo->img[i].surf);
	free(lo->img);
	free(lo);
}

/* ------------------------------------------------------------------ */
/* 查詢：點擊定位、游標座標                                                */
/* ------------------------------------------------------------------ */

static int same_unit(const Line *l, int block, int row, int col) { return l->block == block && l->row == row && l->col == col; }

Pos layout_hit_test(const Document *d, const Layout *lo, int page, double x, double y)
{
	const Line *best = NULL;
	double best_dy = 1e18;
	for (int i = 0; i < lo->nline; i++) {
		const Line *l = &lo->line[i];
		if (l->page != page) continue;
		double dy = y < l->y ? l->y - y : (y > l->y + l->h ? y - (l->y + l->h) : 0);
		if (dy < best_dy || (dy == best_dy && best && l->y < best->y)) { best_dy = dy; best = l; }
	}
	const ImageGeom *best_img = NULL;
	double best_img_dy = 1e18;
	for (int i = 0; i < lo->nimg; i++) {
		const ImageGeom *g = &lo->img[i];
		if (g->page != page) continue;
		double dy = y < g->y ? g->y - y : (y > g->y + g->h ? y - (g->y + g->h) : 0);
		if (dy < best_img_dy) { best_img_dy = dy; best_img = g; }
	}
	if (best_img && best_img_dy <= best_dy) return (Pos){ best_img->block, 0, 0, 0 };
	if (!best) return doc_start(d);
	Pos p = { best->block, best->row, best->col, best->off_start };
	const Para *para = doc_para_at_c(d, p);
	if (!para) return p;
	if (x <= best->x) { p.offset = best->off_start; return p; }
	if (x >= best->x + best->w) { p.offset = best->off_end; return p; }

	/* 在這一行裡找最接近的字元邊界 */
	char *text = para_text(para);
	cairo_surface_t *ssurf = cairo_image_surface_create(CAIRO_FORMAT_A8, 1, 1);
	cairo_t *scratch = cairo_create(ssurf);
	double pen = best->x;
	int off = best->off_start, ri, run_start;
	{ int ro; ri = para_run_at(para, off, &ro); run_start = off - ro; }
	while (off < best->off_end) {
		int len = utf8_len((unsigned char)text[off]);
		while (ri < para->nrun - 1 && off >= run_start + (int)para->run[ri].len) { run_start += (int)para->run[ri].len; ri++; }
		set_run_font(scratch, &para->run[ri], para, d);
		double adv;
		if (text[off] == '\t') adv = TAB_STOP;
		else { char buf[8]; memcpy(buf, text + off, (size_t)len); buf[len] = 0; cairo_text_extents_t te; cairo_text_extents(scratch, buf, &te); adv = te.x_advance; }
		if (x < pen + adv / 2) { p.offset = off; goto done; }
		pen += adv;
		off += len;
	}
	p.offset = best->off_end;
done:
	cairo_destroy(scratch);
	cairo_surface_destroy(ssurf);
	free(text);
	return p;
}

int layout_pos_xy(const Document *d, const Layout *lo, Pos p, int *page, double *x, double *y, double *h)
{
	if (p.block >= 0 && p.block < d->nblock && d->block[p.block].kind == BLOCK_IMAGE) {
		for (int i = 0; i < lo->nimg; i++) {
			const ImageGeom *g = &lo->img[i];
			if (g->block != p.block) continue;
			*page = g->page; *x = g->x; *y = g->y; *h = g->h;
			return 1;
		}
		return 0;
	}
	for (int i = 0; i < lo->nline; i++) {
		const Line *l = &lo->line[i];
		if (!same_unit(l, p.block, p.row, p.col)) continue;
		if (p.offset < l->off_start || p.offset > l->off_end) continue;
		if (p.offset == l->off_end && l->off_end < l->off_start) continue;
		/* 這一行涵蓋這個位置；但如果同一個 offset 剛好也是下一行的開頭（斷行點），
		 * 優先算在「這一行的結尾」而不是「下一行的開頭」，游標會停在行尾比較符合直覺，
		 * 除非它就是段落真正的結尾（那就沒有下一行了，直接用這行） */
		*page = l->page;
		*h = l->h;
		*y = l->y;
		if (p.offset == l->off_start) { *x = l->x; return 1; }
		/* 算出 offset 在這一行內的 x 位置 */
		const Para *para = doc_para_at_c(d, p);
		char *text = para_text(para);
		cairo_surface_t *ssurf = cairo_image_surface_create(CAIRO_FORMAT_A8, 1, 1);
		cairo_t *scratch = cairo_create(ssurf);
		double pen = l->x;
		int off = l->off_start, ri, run_start;
		{ int ro; ri = para_run_at(para, off, &ro); run_start = off - ro; }
		while (off < p.offset) {
			int len = utf8_len((unsigned char)text[off]);
			while (ri < para->nrun - 1 && off >= run_start + (int)para->run[ri].len) { run_start += (int)para->run[ri].len; ri++; }
			set_run_font(scratch, &para->run[ri], para, d);
			double adv;
			if (text[off] == '\t') adv = TAB_STOP;
			else { char buf[8]; memcpy(buf, text + off, (size_t)len); buf[len] = 0; cairo_text_extents_t te; cairo_text_extents(scratch, buf, &te); adv = te.x_advance; }
			pen += adv;
			off += len;
		}
		*x = pen;
		cairo_destroy(scratch);
		cairo_surface_destroy(ssurf);
		free(text);
		return 1;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* 畫圖                                                                 */
/* ------------------------------------------------------------------ */

static void set_rgb(cairo_t *cr, unsigned c, double a)
{
	cairo_set_source_rgba(cr, ((c >> 16) & 0xff) / 255.0, ((c >> 8) & 0xff) / 255.0, (c & 0xff) / 255.0, a);
}

static int pos_in_range(const Document *d, Pos p, Pos a, Pos b)
{
	if (doc_pos_cmp(d, a, b) > 0) { Pos t = a; a = b; b = t; }
	return doc_pos_cmp(d, a, p) <= 0 && doc_pos_cmp(d, p, b) <= 0;
}

static void draw_list_marker(cairo_t *cr, const Document *d, const Para *p, const Line *l, unsigned text_color, Counters *co)
{
	char buf[16];
	if (p->list_type == LIST_BULLET) snprintf(buf, sizeof buf, "•");
	else { int n = counter_next(co, p->list_id); snprintf(buf, sizeof buf, "%d.", n); }
	static const Run empty_run;
	set_run_font(cr, p->nrun ? &p->run[0] : &empty_run, p, d);
	set_rgb(cr, text_color, 1);
	double w;
	{ cairo_text_extents_t te; cairo_text_extents(cr, buf, &te); w = te.x_advance; }
	double indent = LIST_INDENT * (p->list_level + 1);
	cairo_move_to(cr, l->x - indent + (indent - w) * 0.5, l->y + l->ascent);
	cairo_show_text(cr, buf);
}

void layout_draw_page(cairo_t *cr, const Document *d, const Layout *lo, int page,
                      const Pos *sel_a, const Pos *sel_b, const Pos *cursor, int cursor_on,
                      unsigned text_color, unsigned sel_color, unsigned cursor_color, unsigned grid_color)
{
	Counters list_co = { 0 };   /* 畫 decimal 清單編號用，從頭數到這一頁（跨頁的清單編號在 draw 時重新數一次，
	                              * 確保第 2 頁起的清單接續第 1 頁的號碼：這裡偷懶用「畫每一頁都從頭整份文件重算一次」） */
	for (int pass_page = 0; pass_page <= page; pass_page++) {
		for (int i = 0; i < lo->nline; i++) {
			const Line *l = &lo->line[i];
			if (l->page != pass_page) continue;
			if (l->off_start != 0) continue;   /* 只在段落第一行時數號碼 */
			Pos pp = { l->block, l->row, l->col, 0 };
			const Para *para = doc_para_at_c(d, pp);
			if (para && para->list_type == LIST_DECIMAL && pass_page < page) counter_next(&list_co, para->list_id);
		}
	}

	for (int i = 0; i < lo->nline; i++) {
		const Line *l = &lo->line[i];
		if (l->page != page) continue;
		Pos pp = { l->block, l->row, l->col, 0 };
		const Para *para = doc_para_at_c(d, pp);
		if (!para) continue;

		if (l->off_start == 0 && para->list_type != LIST_NONE) draw_list_marker(cr, d, para, l, text_color, &list_co);

		/* 選取反白 */
		if (sel_a && sel_b) {
			Pos ls = { l->block, l->row, l->col, l->off_start }, le = { l->block, l->row, l->col, l->off_end };
			Pos a = *sel_a, b = *sel_b;
			if (doc_pos_cmp(d, a, b) > 0) { Pos t = a; a = b; b = t; }
			if (doc_pos_cmp(d, a, le) <= 0 && doc_pos_cmp(d, ls, b) <= 0) {
				Pos cs = doc_pos_cmp(d, a, ls) > 0 ? a : ls;
				Pos ce = doc_pos_cmp(d, b, le) < 0 ? b : le;
				double sx = l->x, ex = l->x + l->w;
				if (cs.offset > l->off_start || ce.offset < l->off_end) {
					/* 需要精確算選取範圍在這行內的 x：用 layout_pos_xy 風格的量測 */
					int pg; double xx, yy, hh;
					if (cs.offset > l->off_start && layout_pos_xy(d, lo, cs, &pg, &xx, &yy, &hh)) sx = xx;
					if (ce.offset < l->off_end && layout_pos_xy(d, lo, ce, &pg, &xx, &yy, &hh)) ex = xx;
				}
				set_rgb(cr, sel_color, 0.35);
				cairo_rectangle(cr, sx, l->y, MAX(1.0, ex - sx), l->h);
				cairo_fill(cr);
			}
		}

		/* 文字本身 */
		if (l->off_end > l->off_start) {
			char *text = para_text(para);
			double pen = l->x;
			int off = l->off_start, ri, run_start;
			{ int ro; ri = para_run_at(para, off, &ro); run_start = off - ro; }
			while (off < l->off_end) {
				int len = utf8_len((unsigned char)text[off]);
				while (ri < para->nrun - 1 && off >= run_start + (int)para->run[ri].len) { run_start += (int)para->run[ri].len; ri++; }
				const Run *r = &para->run[ri];
				set_run_font(cr, r, para, d);
				double adv;
				if (text[off] == '\t') adv = TAB_STOP;
				else {
					char buf[8];
					memcpy(buf, text + off, (size_t)len);
					buf[len] = 0;
					cairo_text_extents_t te;
					cairo_text_extents(cr, buf, &te);
					adv = te.x_advance;
					set_rgb(cr, r->color == COLOR_AUTO ? text_color : r->color, 1);
					cairo_move_to(cr, pen, l->y + l->ascent);
					cairo_show_text(cr, buf);
					if (r->underline || r->strike) {
						cairo_font_extents_t fe;
						cairo_font_extents(cr, &fe);
						double uy = r->underline ? l->y + l->ascent + fe.descent * 0.3 : l->y + l->ascent - fe.ascent * 0.3;
						cairo_set_line_width(cr, MAX(0.6, r->size * 0.05));
						cairo_move_to(cr, pen, uy);
						cairo_line_to(cr, pen + adv, uy);
						cairo_stroke(cr);
					}
				}
				pen += adv;
				off += len;
			}
			free(text);
		}

		/* 游標 */
		if (cursor && cursor_on && cursor->block == l->block && cursor->row == l->row && cursor->col == l->col
		    && cursor->offset >= l->off_start && cursor->offset <= l->off_end
		    && !(cursor->offset == l->off_end && l->off_end > l->off_start && l->off_end < (int)para_text_len(para))) {
			int pg; double cx, cy, ch;
			if (layout_pos_xy(d, lo, *cursor, &pg, &cx, &cy, &ch)) {
				set_rgb(cr, cursor_color, 1);
				cairo_set_line_width(cr, 1.2);
				cairo_move_to(cr, cx, cy + 1);
				cairo_line_to(cr, cx, cy + ch - 1);
				cairo_stroke(cr);
			}
		}
	}

	/* 表格格線 */
	for (int i = 0; i < lo->ntbl; i++) {
		const TableGeom *t = &lo->tbl[i];
		if (t->page != page) continue;
		set_rgb(cr, grid_color, 1);
		cairo_set_line_width(cr, 0.75);
		for (int r = 0; r <= t->nrow; r++) { cairo_move_to(cr, t->x, t->y + t->rowy[r]); cairo_line_to(cr, t->x + t->w, t->y + t->rowy[r]); }
		for (int c = 0; c <= t->ncol; c++) { cairo_move_to(cr, t->x + t->colx[c], t->y); cairo_line_to(cr, t->x + t->colx[c], t->y + t->h); }
		cairo_stroke(cr);
	}

	/* 圖片 */
	for (int i = 0; i < lo->nimg; i++) {
		const ImageGeom *g = &lo->img[i];
		if (g->page != page) continue;
		if (!g->surf) continue;   /* 解碼失敗（檔案壞掉）：這個位置就空白，不擋住其他內容 */
		cairo_surface_t *surf = g->surf;
		int sw = cairo_image_surface_get_width(surf), sh = cairo_image_surface_get_height(surf);
		if (sw <= 0 || sh <= 0) continue;
		cairo_save(cr);
		cairo_translate(cr, g->x, g->y);
		cairo_scale(cr, g->w / sw, g->h / sh);
		cairo_set_source_surface(cr, surf, 0, 0);
		cairo_paint(cr);
		cairo_restore(cr);
		if (cursor && cursor_on && cursor->block == g->block) {
			set_rgb(cr, cursor_color, 1);
			cairo_set_line_width(cr, 1.5);
			cairo_rectangle(cr, g->x - 2, g->y - 2, g->w + 4, g->h + 4);
			cairo_stroke(cr);
		}
	}

	/* 頁碼：頁尾正中間 */
	if (d->page_numbers) {
		char buf[16];
		snprintf(buf, sizeof buf, "%d", page + 1);
		cairo_select_font_face(cr, d->default_font, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
		cairo_set_font_size(cr, d->default_size * 0.9);
		cairo_text_extents_t te;
		cairo_text_extents(cr, buf, &te);
		set_rgb(cr, text_color, 1);
		cairo_move_to(cr, (d->page_w - te.width) / 2.0 - te.x_bearing, d->page_h - d->margin_b / 2.0);
		cairo_show_text(cr, buf);
	}

	free(list_co.c);
	(void)pos_in_range;
}
