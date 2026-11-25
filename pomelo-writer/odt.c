/* SPDX-License-Identifier: GPL-3.0-or-later */
/* odt.c — 見 odt.h */
#define _GNU_SOURCE
#include "odt.h"
#include "zip.h"
#include "xml.h"
#include <cairo/cairo.h>
#include <pomelo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CM_TO_PT(cm) ((cm) * 72.0 / 2.54)
#define IN_TO_PT(in) ((in) * 72.0)

/* ------------------------------------------------------------------ */
/* 圖片：只用來量出像素尺寸，只認 PNG／JPEG（真正的解碼畫面交給 layout.c）       */
/* ------------------------------------------------------------------ */

typedef struct { const unsigned char *p; size_t n, off; } MemRd;

static cairo_status_t mem_read_cb(void *cl, unsigned char *data, unsigned int len)
{
	MemRd *m = cl;
	if (m->off + len > m->n) return CAIRO_STATUS_READ_ERROR;
	memcpy(data, m->p + m->off, len);
	m->off += len;
	return CAIRO_STATUS_SUCCESS;
}

static int sniff_is_png(const unsigned char *d, size_t n) { return n >= 8 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G'; }
static int sniff_is_jpeg(const unsigned char *d, size_t n) { return n >= 3 && d[0] == 0xff && d[1] == 0xd8 && d[2] == 0xff; }

static int image_dims(const unsigned char *data, size_t len, int is_png, int *pw, int *ph)
{
	cairo_surface_t *s;
	if (is_png) {
		MemRd m = { data, len, 0 };
		s = cairo_image_surface_create_from_png_stream(mem_read_cb, &m);
		if (!s || cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) { if (s) cairo_surface_destroy(s); return 0; }
	} else {
		s = pm_image_jpeg_mem(data, len, 0);
		if (!s) return 0;
	}
	*pw = cairo_image_surface_get_width(s);
	*ph = cairo_image_surface_get_height(s);
	cairo_surface_destroy(s);
	return *pw > 0 && *ph > 0;
}

/* 長度字串（"2.5cm"、"1in"、"12pt"）轉成 pt */
static double len_to_pt(const char *s, double def)
{
	if (!s) return def;
	char *end;
	double v = strtod(s, &end);
	if (end == s) return def;
	if (!strncmp(end, "cm", 2)) return CM_TO_PT(v);
	if (!strncmp(end, "mm", 2)) return CM_TO_PT(v / 10.0);
	if (!strncmp(end, "in", 2)) return IN_TO_PT(v);
	if (!strncmp(end, "pt", 2)) return v;
	return v;   /* 沒有單位：當作已經是 pt */
}

static unsigned parse_hex_color(const char *s, unsigned def)
{
	if (!s || s[0] != '#') return def;
	unsigned v = 0;
	if (sscanf(s + 1, "%6x", &v) != 1) return def;
	return v;
}

/* ------------------------------------------------------------------ */
/* 樣式表：style:name -> 段落／文字屬性                                    */
/* ------------------------------------------------------------------ */

typedef struct {
	char name[80];
	int is_para;             /* family=="paragraph" */
	int has_align; Align align;
	int has_bold, bold;
	int has_italic, italic;
	int has_underline, underline;
	int has_strike, strike;
	int has_color; unsigned color;
	int has_size; double size;
	char font[64];
} StyleInfo;

typedef struct { StyleInfo *s; int n; } StyleTable;

typedef struct { char name[80]; ListType type; } ListStyleInfo;
typedef struct { ListStyleInfo *s; int n; } ListStyleTable;

static Align align_from_str(const char *s)
{
	if (!s) return ALIGN_LEFT;
	if (!strcmp(s, "center")) return ALIGN_CENTER;
	if (!strcmp(s, "end") || !strcmp(s, "right")) return ALIGN_RIGHT;
	if (!strcmp(s, "justify")) return ALIGN_JUSTIFY;
	return ALIGN_LEFT;
}

/* 在任意節點底下（遞迴）收集所有 style:style，補進樣式表 */
static void collect_styles(const XmlNode *n, StyleTable *t)
{
	for (; n; n = n->next) {
		if (n->tag && !strcmp(n->tag, "style:style")) {
			const char *name = xml_attr(n, "style:name");
			const char *fam = xml_attr(n, "style:family");
			if (name && fam && (!strcmp(fam, "paragraph") || !strcmp(fam, "text"))) {
				t->s = realloc(t->s, (size_t)(t->n + 1) * sizeof(StyleInfo));
				StyleInfo *si = &t->s[t->n++];
				memset(si, 0, sizeof *si);
				snprintf(si->name, sizeof si->name, "%s", name);
				si->is_para = !strcmp(fam, "paragraph");
				XmlNode *pp = xml_child(n, "style:paragraph-properties");
				if (pp) {
					const char *al = xml_attr(pp, "fo:text-align");
					if (al) { si->has_align = 1; si->align = align_from_str(al); }
				}
				XmlNode *tp = xml_child(n, "style:text-properties");
				if (tp) {
					const char *w = xml_attr(tp, "fo:font-weight");
					if (w) { si->has_bold = 1; si->bold = strcmp(w, "normal") != 0; }
					const char *fs = xml_attr(tp, "fo:font-style");
					if (fs) { si->has_italic = 1; si->italic = strcmp(fs, "normal") != 0; }
					const char *ul = xml_attr(tp, "style:text-underline-style");
					if (ul) { si->has_underline = 1; si->underline = strcmp(ul, "none") != 0; }
					const char *st = xml_attr(tp, "style:text-line-through-style");
					if (st) { si->has_strike = 1; si->strike = strcmp(st, "none") != 0; }
					const char *col = xml_attr(tp, "fo:color");
					if (col) { si->has_color = 1; si->color = parse_hex_color(col, COLOR_AUTO); }
					const char *sz = xml_attr(tp, "fo:font-size");
					if (sz) { si->has_size = 1; si->size = len_to_pt(sz, 12); }
					const char *fn = xml_attr(tp, "style:font-name");
					if (!fn) fn = xml_attr(tp, "fo:font-family");
					if (fn) snprintf(si->font, sizeof si->font, "%s", fn);
				}
			}
		}
		collect_styles(n->children, t);
	}
}

static const StyleInfo *style_find(const StyleTable *t, const char *name)
{
	if (!name) return NULL;
	for (int i = 0; i < t->n; i++) if (!strcmp(t->s[i].name, name)) return &t->s[i];
	return NULL;
}

static void apply_style_to_run(Run *r, const StyleInfo *si, double default_size)
{
	r->size = default_size;
	r->color = COLOR_AUTO;
	if (!si) return;
	if (si->has_bold) r->bold = si->bold;
	if (si->has_italic) r->italic = si->italic;
	if (si->has_underline) r->underline = si->underline;
	if (si->has_strike) r->strike = si->strike;
	if (si->has_color) r->color = si->color;
	if (si->has_size) r->size = si->size;
	if (si->font[0]) snprintf(r->font, sizeof r->font, "%s", si->font);
}

/* text:list-style：看「第 1 層」是 bullet 還是 number，判斷整串清單（v1 不分層級）要當成項目符號還是編號。
 * 不能只看「有沒有 number 子項」：LibreOffice 常常一次定義 10 層，淺層是 bullet、深層卻換成 number
 * （例如從 Word 轉過來的清單樣式），要抓剛好 text:level="1" 的那一項才準。 */
static void collect_list_styles(const XmlNode *n, ListStyleTable *t)
{
	for (; n; n = n->next) {
		if (n->tag && !strcmp(n->tag, "text:list-style")) {
			const char *name = xml_attr(n, "style:name");
			if (name) {
				ListType type = LIST_BULLET;
				for (XmlNode *lvl = n->children; lvl; lvl = lvl->next) {
					const char *l = lvl->tag ? xml_attr(lvl, "text:level") : NULL;
					if (l && !strcmp(l, "1")) {
						type = !strcmp(lvl->tag, "text:list-level-style-number") ? LIST_DECIMAL : LIST_BULLET;
						break;
					}
				}
				t->s = realloc(t->s, (size_t)(t->n + 1) * sizeof(ListStyleInfo));
				ListStyleInfo *li = &t->s[t->n++];
				snprintf(li->name, sizeof li->name, "%s", name);
				li->type = type;
			}
		}
		collect_list_styles(n->children, t);
	}
}

static ListType list_style_find(const ListStyleTable *t, const char *name)
{
	if (!name) return LIST_BULLET;
	for (int i = 0; i < t->n; i++) if (!strcmp(t->s[i].name, name)) return t->s[i].type;
	return LIST_BULLET;
}

/* 段落（text:p／text:h）裡若恰好是一張圖片（沒有其他文字），回傳圖片資料；否則回傳 0。 */
static int parse_odt_para_image(const XmlNode *wp, Zip *z,
                                unsigned char **out_data, size_t *out_len, int *out_png, int *out_pw, int *out_ph, double *out_w, double *out_h)
{
	int saw_text = 0;
	const XmlNode *frame = NULL;
	for (const XmlNode *c = wp->children; c; c = c->next) {
		if (!c->tag) { if (*c->text) saw_text = 1; continue; }
		if (!strcmp(c->tag, "draw:frame")) { if (!frame) frame = c; }
		else if (!strcmp(c->tag, "text:span")) { char *t = xml_text(c); if (*t) saw_text = 1; free(t); }
	}
	if (!frame || saw_text) return 0;
	const XmlNode *img = xml_child(frame, "draw:image");
	const char *href = img ? xml_attr(img, "xlink:href") : NULL;
	if (!href) return 0;
	size_t len;
	unsigned char *data = zip_read(z, href, &len);
	if (!data) return 0;
	int is_png = sniff_is_png(data, len), is_jpeg = sniff_is_jpeg(data, len);
	if (!is_png && !is_jpeg) { free(data); return 0; }
	int pw, ph;
	if (!image_dims(data, len, is_png, &pw, &ph)) { free(data); return 0; }
	*out_data = data; *out_len = len; *out_png = is_png; *out_pw = pw; *out_ph = ph;
	*out_w = len_to_pt(xml_attr(frame, "svg:width"), pw * 0.75);
	*out_h = len_to_pt(xml_attr(frame, "svg:height"), ph * 0.75);
	return 1;
}

/* 在任意節點底下（遞迴）找第一個符合名稱的節點 */
static const XmlNode *find_tag(const XmlNode *n, const char *tag)
{
	for (; n; n = n->next) {
		if (n->tag && !strcmp(n->tag, tag)) return n;
		const XmlNode *found = find_tag(n->children, tag);
		if (found) return found;
	}
	return NULL;
}

/* 在任意節點底下（遞迴）找第一個 style:page-layout-properties（頁面大小／邊界） */
static const XmlNode *find_page_layout(const XmlNode *n)
{
	for (; n; n = n->next) {
		if (n->tag && !strcmp(n->tag, "style:page-layout-properties")) return n;
		const XmlNode *found = find_page_layout(n->children);
		if (found) return found;
	}
	return NULL;
}

/* ------------------------------------------------------------------ */
/* 讀取：段落、清單、表格                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
	const StyleTable *styles;
	double default_size;
} Ctx;

static void parse_odt_para_content(const XmlNode *wp, Para *para, const Ctx *ctx)
{
	const StyleInfo *pstyle = style_find(ctx->styles, xml_attr(wp, "text:style-name"));
	if (pstyle && pstyle->has_align) para->align = pstyle->align;
	Run base;
	memset(&base, 0, sizeof base);
	apply_style_to_run(&base, pstyle, ctx->default_size);   /* 段落樣式本身也可能帶字元屬性，當作沒包 span 的文字預設 */

	for (XmlNode *c = wp->children; c; c = c->next) {
		if (!c->tag) {
			if (*c->text) para_insert(para, (int)para_text_len(para), c->text, strlen(c->text), &base);
			continue;
		}
		if (!strcmp(c->tag, "text:span")) {
			const StyleInfo *si = style_find(ctx->styles, xml_attr(c, "text:style-name"));
			Run rs = base;
			apply_style_to_run(&rs, si, ctx->default_size);
			char *t = xml_text(c);
			if (*t) para_insert(para, (int)para_text_len(para), t, strlen(t), &rs);
			free(t);
		} else if (!strcmp(c->tag, "text:tab")) {
			para_insert(para, (int)para_text_len(para), "\t", 1, &base);
		} else if (!strcmp(c->tag, "text:line-break") || !strcmp(c->tag, "text:s")) {
			para_insert(para, (int)para_text_len(para), " ", 1, &base);
		}
	}
	if (para->nrun == 0) {
		para->runcap = 1;
		para->run = malloc(sizeof(Run));
		memset(&para->run[0], 0, sizeof(Run));
		para->run[0].text = strdup("");
		para->run[0].size = ctx->default_size;
		para->run[0].color = COLOR_AUTO;
		para->nrun = 1;
	}
}

static void parse_odt_para(const XmlNode *wp, Para *para, const Ctx *ctx)
{
	memset(para, 0, sizeof *para);
	para->align = ALIGN_LEFT;
	parse_odt_para_content(wp, para, ctx);
}

static void parse_odt_cell(const XmlNode *tc, Para *cell, const Ctx *ctx)
{
	int first = 1;
	for (XmlNode *wp = xml_child(tc, "text:p"); wp; wp = xml_next(wp, "text:p")) {
		if (first) { parse_odt_para(wp, cell, ctx); first = 0; continue; }
		Para extra;
		parse_odt_para(wp, &extra, ctx);
		if (para_text_len(&extra) > 0) {
			if (para_text_len(cell) > 0) para_insert(cell, (int)para_text_len(cell), " ", 1, NULL);
			para_append(cell, &extra);
		} else
			para_clear(&extra);
	}
	if (first) parse_odt_para(tc, cell, ctx);   /* 沒有 text:p（不應該發生）：至少給個空段落 */
}

static void parse_odt_table(const XmlNode *tbl, Table *table, const Ctx *ctx)
{
	memset(table, 0, sizeof *table);
	int ncol = 0;
	for (XmlNode *gc = xml_child(tbl, "table:table-column"); gc; gc = xml_next(gc, "table:table-column")) ncol++;
	int nrow = 0;
	for (XmlNode *tr = xml_child(tbl, "table:table-row"); tr; tr = xml_next(tr, "table:table-row")) nrow++;
	if (!ncol) {
		XmlNode *tr0 = xml_child(tbl, "table:table-row");
		if (tr0) for (XmlNode *tc = xml_child(tr0, "table:table-cell"); tc; tc = xml_next(tc, "table:table-cell")) ncol++;
	}
	if (!nrow) nrow = 1;
	if (!ncol) ncol = 1;
	table->nrow = nrow;
	table->ncol = ncol;
	table->cell = malloc((size_t)nrow * ncol * sizeof(Para));
	for (int i = 0; i < nrow * ncol; i++) para_init(&table->cell[i]);
	int r = 0;
	for (XmlNode *tr = xml_child(tbl, "table:table-row"); tr && r < nrow; tr = xml_next(tr, "table:table-row"), r++) {
		int c = 0;
		for (XmlNode *tc = xml_child(tr, "table:table-cell"); tc && c < ncol; tc = xml_next(tc, "table:table-cell"), c++) {
			para_clear(&table->cell[r * ncol + c]);
			parse_odt_cell(tc, &table->cell[r * ncol + c], ctx);
		}
	}
}

/* 遞迴走訪 office:text（或 text:list-item）底下的內容，把段落／清單／表格依序加進 d->block */
static void walk_body(Document *d, const XmlNode *container, Zip *z, const Ctx *ctx, const ListStyleTable *lstyles, int list_level, int list_id, ListType list_type)
{
	for (XmlNode *c = container->children; c; c = c->next) {
		if (!c->tag) continue;
		if (!strcmp(c->tag, "text:p") || !strcmp(c->tag, "text:h")) {
			unsigned char *idata; size_t ilen; int ipng, ipw, iph; double iw, ih;
			int is_img = parse_odt_para_image(c, z, &idata, &ilen, &ipng, &ipw, &iph, &iw, &ih);
			if (d->nblock >= d->blockcap) { d->blockcap = d->blockcap ? d->blockcap * 2 : 8; d->block = realloc(d->block, (size_t)d->blockcap * sizeof(Block)); }
			Block *b = &d->block[d->nblock++];
			memset(b, 0, sizeof *b);
			if (is_img) {
				b->kind = BLOCK_IMAGE;
				b->image.data = idata; b->image.datalen = ilen; b->image.is_png = ipng;
				b->image.pw = ipw; b->image.ph = iph; b->image.w = iw; b->image.h = ih;
				continue;
			}
			b->kind = BLOCK_PARA;
			parse_odt_para(c, &b->para, ctx);
			if (!strcmp(c->tag, "text:h")) {
				const char *lvl = xml_attr(c, "text:outline-level");
				int l = lvl ? atoi(lvl) : 1;
				b->para.style = l <= 1 ? PSTYLE_H1 : l == 2 ? PSTYLE_H2 : PSTYLE_H3;
			}
			if (list_level >= 0) { b->para.list_type = list_type; b->para.list_level = list_level; b->para.list_id = list_id; }
		} else if (!strcmp(c->tag, "text:list")) {
			ListType lt = list_style_find(lstyles, xml_attr(c, "text:style-name"));
			int new_id = list_id > 0 ? list_id : (d->next_list_id = d->next_list_id + 1);
			for (XmlNode *li = xml_child(c, "text:list-item"); li; li = xml_next(li, "text:list-item"))
				walk_body(d, li, z, ctx, lstyles, list_level + 1, new_id, lt);
		} else if (!strcmp(c->tag, "table:table")) {
			if (d->nblock >= d->blockcap) { d->blockcap = d->blockcap ? d->blockcap * 2 : 8; d->block = realloc(d->block, (size_t)d->blockcap * sizeof(Block)); }
			Block *b = &d->block[d->nblock++];
			memset(b, 0, sizeof *b);
			b->kind = BLOCK_TABLE;
			parse_odt_table(c, &b->table, ctx);
		}
	}
}

Document *odt_read(const char *path)
{
	Zip *z = zip_open(path);
	if (!z) return NULL;
	size_t clen;
	unsigned char *content = zip_read(z, "content.xml", &clen);
	if (!content) { zip_close(z); return NULL; }
	size_t slen;
	unsigned char *styles_xml = zip_read(z, "styles.xml", &slen);

	XmlNode *croot = xml_parse((const char *)content, clen);
	free(content);
	if (!croot) { free(styles_xml); zip_close(z); return NULL; }
	XmlNode *sroot = styles_xml ? xml_parse((const char *)styles_xml, slen) : NULL;
	free(styles_xml);

	StyleTable st = { 0 };
	collect_styles(croot, &st);
	if (sroot) collect_styles(sroot, &st);
	ListStyleTable lst = { 0 };
	collect_list_styles(croot, &lst);
	if (sroot) collect_list_styles(sroot, &lst);

	Document *d = doc_new();
	para_clear(&d->block[0].para);
	d->nblock = 0;

	/* 頁面大小／邊界：styles.xml 裡隨便找第一個 style:page-layout-properties */
	if (sroot) {
		const XmlNode *found = find_page_layout(sroot->children);
		if (found) {
			const char *w = xml_attr(found, "fo:page-width"), *h = xml_attr(found, "fo:page-height");
			const char *ml = xml_attr(found, "fo:margin-left"), *mr = xml_attr(found, "fo:margin-right");
			const char *mt = xml_attr(found, "fo:margin-top"), *mb = xml_attr(found, "fo:margin-bottom");
			if (w) d->page_w = len_to_pt(w, d->page_w);
			if (h) d->page_h = len_to_pt(h, d->page_h);
			if (ml) d->margin_l = len_to_pt(ml, d->margin_l);
			if (mr) d->margin_r = len_to_pt(mr, d->margin_r);
			if (mt) d->margin_t = len_to_pt(mt, d->margin_t);
			if (mb) d->margin_b = len_to_pt(mb, d->margin_b);
		}
	}

	Ctx ctx = { &st, d->default_size };
	XmlNode *body = xml_child(croot, "office:body");
	XmlNode *text = body ? xml_child(body, "office:text") : NULL;
	if (text) walk_body(d, text, z, &ctx, &lst, -1, 0, LIST_NONE);

	/* 頁碼：styles.xml 裡隨便哪裡有 text:page-number 這個節點就算有（只會出現在頁尾） */
	if (sroot && find_tag(sroot->children, "text:page-number")) d->page_numbers = 1;

	if (d->nblock == 0) { doc_free(d); d = doc_new(); }

	free(st.s);
	free(lst.s);
	xml_free(croot);
	if (sroot) xml_free(sroot);
	zip_close(z);
	return d;
}

/* ------------------------------------------------------------------ */
/* 寫入                                                                 */
/* ------------------------------------------------------------------ */

static int style_seq;

static void write_run_odt(XmlWriter *aw, XmlWriter *bw, const Run *r, double default_size)
{
	int need_style = r->bold || r->italic || r->underline || r->strike || r->color != COLOR_AUTO || r->font[0] || (r->size > 0 && r->size != default_size);
	if (!need_style) {
		xml_text_out(bw, r->text);
		return;
	}
	char name[32];
	snprintf(name, sizeof name, "T%d", ++style_seq);
	xml_open(aw, "style:style");
	xml_attr_set(aw, "style:name", name);
	xml_attr_set(aw, "style:family", "text");
	xml_open(aw, "style:text-properties");
	if (r->bold) { xml_attr_set(aw, "fo:font-weight", "bold"); xml_attr_set(aw, "style:font-weight-asian", "bold"); }
	if (r->italic) { xml_attr_set(aw, "fo:font-style", "italic"); xml_attr_set(aw, "style:font-style-asian", "italic"); }
	if (r->underline) { xml_attr_set(aw, "style:text-underline-style", "solid"); xml_attr_set(aw, "style:text-underline-width", "auto"); xml_attr_set(aw, "style:text-underline-color", "font-color"); }
	if (r->strike) xml_attr_set(aw, "style:text-line-through-style", "solid");
	if (r->color != COLOR_AUTO) { char hc[8]; snprintf(hc, sizeof hc, "#%06X", r->color & 0xffffff); xml_attr_set(aw, "fo:color", hc); }
	if (r->size > 0 && r->size != default_size) { char sz[16]; snprintf(sz, sizeof sz, "%.1fpt", r->size); xml_attr_set(aw, "fo:font-size", sz); xml_attr_set(aw, "style:font-size-asian", sz); }
	if (r->font[0]) { xml_attr_set(aw, "style:font-name", r->font); xml_attr_set(aw, "style:font-name-asian", r->font); }
	xml_close(aw);   /* text-properties */
	xml_close(aw);   /* style:style */

	xml_open(bw, "text:span");
	xml_attr_set(bw, "text:style-name", name);
	xml_text_out(bw, r->text);
	xml_close(bw);
}

static int outline_level(PStyle s) { return s == PSTYLE_H1 ? 1 : s == PSTYLE_H2 ? 2 : 3; }
/* ODF 把具名樣式名稱裡的空白編成 _20_；用這三個固定名稱（而不是每段各自一個自動樣式），
 * LibreOffice 自己存的標題也是這樣命名，這樣存出去的檔案被 LibreOffice 轉成 .docx 時
 * 才會正確辨認出「這是標題」，換成 Word 的 Heading 樣式（而不是全部掉回 Normal）。 */
static const char *heading_style_name(PStyle s)
{
	switch (s) { case PSTYLE_H1: return "Heading_20_1"; case PSTYLE_H2: return "Heading_20_2"; case PSTYLE_H3: return "Heading_20_3"; default: return NULL; }
}

static void write_para_odt(XmlWriter *aw, XmlWriter *bw, const Para *p, double default_size)
{
	char pname[32] = "";
	double eff_size = pstyle_size(p->style, default_size);
	const char *base = heading_style_name(p->style);   /* NULL 表示內文（沒有具名樣式） */
	if (p->align != ALIGN_LEFT) {
		/* 標題又有自訂對齊：另開一個自動樣式，繼承具名的標題樣式（字級、粗體跟著來） */
		snprintf(pname, sizeof pname, "P%d", ++style_seq);
		xml_open(aw, "style:style");
		xml_attr_set(aw, "style:name", pname);
		xml_attr_set(aw, "style:family", "paragraph");
		if (base) xml_attr_set(aw, "style:parent-style-name", base);
		xml_open(aw, "style:paragraph-properties");
		xml_attr_set(aw, "fo:text-align", p->align == ALIGN_CENTER ? "center" : p->align == ALIGN_RIGHT ? "end" : "justify");
		xml_close(aw);   /* paragraph-properties */
		xml_close(aw);   /* style:style */
	}
	int heading = p->style != PSTYLE_BODY;
	xml_open(bw, heading ? "text:h" : "text:p");
	if (heading) { char lvl[4]; snprintf(lvl, sizeof lvl, "%d", outline_level(p->style)); xml_attr_set(bw, "text:outline-level", lvl); }
	if (pname[0]) xml_attr_set(bw, "text:style-name", pname);
	else if (base) xml_attr_set(bw, "text:style-name", base);
	for (int i = 0; i < p->nrun; i++) write_run_odt(aw, bw, &p->run[i], eff_size);
	xml_close(bw);
}

/* 圖片：draw:frame 包著 draw:image，要包在一個段落裡才合法 */
static void write_image_odt(XmlWriter *bw, const Image *im, int imgidx)
{
	xml_open(bw, "text:p");
	xml_open(bw, "draw:frame");
	char sz[16];
	snprintf(sz, sizeof sz, "%.2fpt", im->w); xml_attr_set(bw, "svg:width", sz);
	snprintf(sz, sizeof sz, "%.2fpt", im->h); xml_attr_set(bw, "svg:height", sz);
	xml_open(bw, "draw:image");
	char href[64];
	snprintf(href, sizeof href, "Pictures/image%d.%s", imgidx + 1, im->is_png ? "png" : "jpeg");
	xml_attr_set(bw, "xlink:href", href);
	xml_attr_set(bw, "xlink:type", "simple");
	xml_attr_set(bw, "xlink:show", "embed");
	xml_attr_set(bw, "xlink:actuate", "onLoad");
	xml_close(bw);   /* draw:image */
	xml_close(bw);   /* draw:frame */
	xml_close(bw);   /* text:p */
}

static void write_table_odt(XmlWriter *aw, XmlWriter *bw, const Table *t, double default_size)
{
	char tname[32], cellstyle[32];
	snprintf(tname, sizeof tname, "Table%d", ++style_seq);
	snprintf(cellstyle, sizeof cellstyle, "TC%d", ++style_seq);
	xml_open(aw, "style:style");
	xml_attr_set(aw, "style:name", cellstyle);
	xml_attr_set(aw, "style:family", "table-cell");
	xml_open(aw, "style:table-cell-properties");
	xml_attr_set(aw, "fo:border", "0.5pt solid #999999");
	xml_attr_set(aw, "fo:padding", "0.1cm");
	xml_close(aw);
	xml_close(aw);   /* style:style */

	xml_open(bw, "table:table");
	xml_attr_set(bw, "table:name", tname);
	for (int c = 0; c < t->ncol; c++) { xml_open(bw, "table:table-column"); xml_close(bw); }
	for (int r = 0; r < t->nrow; r++) {
		xml_open(bw, "table:table-row");
		for (int c = 0; c < t->ncol; c++) {
			xml_open(bw, "table:table-cell");
			xml_attr_set(bw, "table:style-name", cellstyle);
			xml_attr_set(bw, "office:value-type", "string");
			write_para_odt(aw, bw, &t->cell[r * t->ncol + c], default_size);
			xml_close(bw);
		}
		xml_close(bw);
	}
	xml_close(bw);
}

int odt_write(const Document *d, const char *path)
{
	style_seq = 0;
	XmlWriter *aw = xml_writer_new();   /* automatic-styles 的內容（之後接到 content.xml 前段） */
	XmlWriter *bw = xml_writer_new();   /* office:text 的內容（本文） */

	/* 先把清單結構包起來：連續同一個 list_id 的段落包成一個 text:list（巢狀層級先不處理，一律當第 0 層） */
	int i = 0, list_counter = 0, imgidx = 0;
	while (i < d->nblock) {
		const Block *b = &d->block[i];
		if (b->kind == BLOCK_IMAGE) {
			write_image_odt(bw, &b->image, imgidx++);
			i++;
		} else if (b->kind == BLOCK_PARA && b->para.list_type != LIST_NONE) {
			int lid = b->para.list_id;
			ListType lt = b->para.list_type;
			char lsname[32];
			snprintf(lsname, sizeof lsname, "L%d", ++list_counter);
			xml_open(aw, "text:list-style");
			xml_attr_set(aw, "style:name", lsname);
			xml_open(aw, lt == LIST_BULLET ? "text:list-level-style-bullet" : "text:list-level-style-number");
			xml_attr_set(aw, "text:level", "1");
			if (lt == LIST_BULLET) xml_attr_set(aw, "text:bullet-char", "•");
			else xml_attr_set(aw, "style:num-format", "1");
			xml_close(aw);
			xml_close(aw);   /* text:list-style */

			xml_open(bw, "text:list");
			xml_attr_set(bw, "text:style-name", lsname);
			while (i < d->nblock && d->block[i].kind == BLOCK_PARA && d->block[i].para.list_type != LIST_NONE && d->block[i].para.list_id == lid) {
				xml_open(bw, "text:list-item");
				write_para_odt(aw, bw, &d->block[i].para, d->default_size);
				xml_close(bw);
				i++;
			}
			xml_close(bw);   /* text:list */
		} else if (b->kind == BLOCK_PARA) {
			write_para_odt(aw, bw, &b->para, d->default_size);
			i++;
		} else {
			write_table_odt(aw, bw, &b->table, d->default_size);
			i++;
		}
	}

	size_t alen, blen;
	char *astr = xml_writer_finish(aw, &alen);
	char *bstr = xml_writer_finish(bw, &blen);
	xml_writer_free(aw);
	xml_writer_free(bw);
	/* astr／bstr 各自帶了一個 <?xml ...?> 開頭，組合時要跳過 */
	const char *abody = strchr(astr, '>') ? strchr(astr, '>') + 1 : astr;
	const char *bbody = strchr(bstr, '>') ? strchr(bstr, '>') + 1 : bstr;

	char *content = malloc(alen + blen + 4096);
	int n = sprintf(content,
	    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
	    "<office:document-content xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\""
	    " xmlns:text=\"urn:oasis:names:tc:opendocument:xmlns:text:1.0\""
	    " xmlns:style=\"urn:oasis:names:tc:opendocument:xmlns:style:1.0\""
	    " xmlns:fo=\"urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0\""
	    " xmlns:table=\"urn:oasis:names:tc:opendocument:xmlns:table:1.0\""
	    " xmlns:draw=\"urn:oasis:names:tc:opendocument:xmlns:drawing:1.0\""
	    " xmlns:svg=\"urn:oasis:names:tc:opendocument:xmlns:svg-compatible:1.0\""
	    " xmlns:xlink=\"http://www.w3.org/1999/xlink\""
	    " office:version=\"1.2\">"
	    "<office:automatic-styles>%s</office:automatic-styles>"
	    "<office:body><office:text>%s</office:text></office:body>"
	    "</office:document-content>", abody, bbody);
	free(astr);
	free(bstr);

	int nimages = 0, use_headings = 0;
	for (int k = 0; k < d->nblock; k++) {
		if (d->block[k].kind == BLOCK_IMAGE) nimages++;
		else if (d->block[k].kind == BLOCK_PARA && d->block[k].para.style != PSTYLE_BODY) use_headings = 1;
	}

	char headbuf[1024] = "";
	if (use_headings) {
		size_t hn = 0;
		for (int si = PSTYLE_H1; si <= PSTYLE_H3; si++) {
			PStyle s = (PStyle)si;
			char sz[16];
			snprintf(sz, sizeof sz, "%.1fpt", pstyle_size(s, d->default_size));
			hn += (size_t)snprintf(headbuf + hn, sizeof headbuf - hn,
			    "<style:style style:name=\"%s\" style:display-name=\"Heading %d\" style:family=\"paragraph\""
			    " style:parent-style-name=\"Standard\" style:default-outline-level=\"%d\" style:class=\"text\">"
			    "<style:text-properties fo:font-size=\"%s\" style:font-size-asian=\"%s\" fo:font-weight=\"bold\" style:font-weight-asian=\"bold\"/>"
			    "</style:style>",
			    heading_style_name(s), outline_level(s), outline_level(s), sz, sz);
		}
	}

	size_t cap_styles = 2048 + strlen(headbuf);
	char *stylesbuf = malloc(cap_styles);
	int sn = snprintf(stylesbuf, cap_styles,
	    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
	    "<office:document-styles xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\""
	    " xmlns:style=\"urn:oasis:names:tc:opendocument:xmlns:style:1.0\""
	    " xmlns:text=\"urn:oasis:names:tc:opendocument:xmlns:text:1.0\""
	    " xmlns:fo=\"urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0\">"
	    "<office:styles><style:default-style style:family=\"paragraph\">"
	    "<style:text-properties style:font-name=\"%s\" fo:font-size=\"%.1fpt\"/>"
	    "</style:default-style>%s</office:styles>"
	    "<office:master-styles><style:master-page style:name=\"Standard\" style:page-layout-name=\"PL1\">%s</style:master-page></office:master-styles>"
	    "<office:automatic-styles><style:page-layout style:name=\"PL1\"><style:page-layout-properties"
	    " fo:page-width=\"%.2fpt\" fo:page-height=\"%.2fpt\" fo:margin-left=\"%.2fpt\" fo:margin-right=\"%.2fpt\""
	    " fo:margin-top=\"%.2fpt\" fo:margin-bottom=\"%.2fpt\"/></style:page-layout></office:automatic-styles>"
	    "</office:document-styles>",
	    d->default_font, d->default_size, headbuf,
	    d->page_numbers ? "<style:footer><text:p><text:page-number>1</text:page-number></text:p></style:footer>" : "",
	    d->page_w, d->page_h, d->margin_l, d->margin_r, d->margin_t, d->margin_b);

	size_t mani_cap = 1024 + (size_t)nimages * 128;
	char *manifest = malloc(mani_cap);
	size_t mn = (size_t)snprintf(manifest, mani_cap,
	    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
	    "<manifest:manifest xmlns:manifest=\"urn:oasis:names:tc:opendocument:xmlns:manifest:1.0\" manifest:version=\"1.2\">"
	    "<manifest:file-entry manifest:full-path=\"/\" manifest:version=\"1.2\" manifest:media-type=\"application/vnd.oasis.opendocument.text\"/>"
	    "<manifest:file-entry manifest:full-path=\"content.xml\" manifest:media-type=\"text/xml\"/>"
	    "<manifest:file-entry manifest:full-path=\"styles.xml\" manifest:media-type=\"text/xml\"/>"
	    "<manifest:file-entry manifest:full-path=\"meta.xml\" manifest:media-type=\"text/xml\"/>");
	{
		int ii = 0;
		for (int k = 0; k < d->nblock; k++) {
			if (d->block[k].kind != BLOCK_IMAGE) continue;
			mn += (size_t)snprintf(manifest + mn, mani_cap - mn,
			    "<manifest:file-entry manifest:full-path=\"Pictures/image%d.%s\" manifest:media-type=\"image/%s\"/>",
			    ii + 1, d->block[k].image.is_png ? "png" : "jpeg", d->block[k].image.is_png ? "png" : "jpeg");
			ii++;
		}
	}
	mn += (size_t)snprintf(manifest + mn, mani_cap - mn, "</manifest:manifest>");

	const char *meta =
	    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
	    "<office:document-meta xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\">"
	    "<office:meta><meta:generator xmlns:meta=\"urn:oasis:names:tc:opendocument:xmlns:meta:1.0\">Pomelo Writer</meta:generator></office:meta>"
	    "</office:document-meta>";

	ZipWriter *zw = zip_writer_new();
	const char *mime = "application/vnd.oasis.opendocument.text";
	zip_writer_add(zw, "mimetype", (const unsigned char *)mime, strlen(mime), 1);   /* 一定要存檔、放第一個 */
	zip_writer_add(zw, "META-INF/manifest.xml", (unsigned char *)manifest, mn, 0);
	free(manifest);
	zip_writer_add(zw, "meta.xml", (const unsigned char *)meta, strlen(meta), 0);
	zip_writer_add(zw, "content.xml", (unsigned char *)content, (size_t)n, 0);
	zip_writer_add(zw, "styles.xml", (unsigned char *)stylesbuf, (size_t)sn, 0);
	free(content);
	free(stylesbuf);

	{
		int ii = 0;
		for (int k = 0; k < d->nblock; k++) {
			if (d->block[k].kind != BLOCK_IMAGE) continue;
			const Image *im = &d->block[k].image;
			char ipath[64];
			snprintf(ipath, sizeof ipath, "Pictures/image%d.%s", ii + 1, im->is_png ? "png" : "jpeg");
			zip_writer_add(zw, ipath, im->data, im->datalen, 1);
			ii++;
		}
	}

	size_t outlen;
	unsigned char *out = zip_writer_finish(zw, &outlen);
	zip_writer_free(zw);

	FILE *f = fopen(path, "wb");
	if (!f) { free(out); return 0; }
	size_t written = fwrite(out, 1, outlen, f);
	fclose(f);
	free(out);
	return written == outlen;
}
