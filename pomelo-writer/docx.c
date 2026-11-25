/* SPDX-License-Identifier: GPL-3.0-or-later */
/* docx.c — 見 docx.h */
#define _GNU_SOURCE
#include "docx.h"
#include "zip.h"
#include "xml.h"
#include <cairo/cairo.h>
#include <pomelo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* 圖片：只用來量出像素尺寸（真正的解碼畫面交給 layout.c），只認 PNG／JPEG       */
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

#define TWIP(pt) ((int)((pt) * 20 + 0.5))     /* pt -> twips（1pt = 20 twips） */
#define PT(twip) ((double)(twip) / 20.0)
#define HALFPT(pt) ((int)((pt) * 2 + 0.5))    /* pt -> 半點（w:sz 的單位） */
#define PTFROMHALF(h) ((double)(h) / 2.0)

static unsigned parse_hex_color(const char *s, unsigned def)
{
	if (!s || !strcmp(s, "auto")) return def;
	unsigned v = 0;
	if (sscanf(s, "%6x", &v) != 1) return def;
	return v;
}

static void hex_color(char *out, size_t n, unsigned c)
{
	if (c == COLOR_AUTO) snprintf(out, n, "auto");
	else snprintf(out, n, "%06X", c & 0xffffff);
}

/* ------------------------------------------------------------------ */
/* numbering.xml：numId -> abstractNumId -> (ilvl -> numFmt)            */
/* ------------------------------------------------------------------ */

typedef struct { int ilvl; ListType fmt; } LvlFmt;
typedef struct { int abstractId; LvlFmt lvl[9]; int nlvl; } AbstractNum;
typedef struct { int numId, abstractId; } NumMap;

typedef struct {
	AbstractNum *abs; int nabs;
	NumMap *map; int nmap;
} Numbering;

static ListType fmt_from_str(const char *s)
{
	if (!s) return LIST_NONE;
	if (!strcmp(s, "bullet")) return LIST_BULLET;
	if (!strcmp(s, "none")) return LIST_NONE;
	return LIST_DECIMAL;   /* decimal、lowerLetter、upperRoman… 一律當成編號清單 */
}

static Numbering *numbering_parse(const unsigned char *data, size_t len)
{
	if (!data) return NULL;
	XmlNode *root = xml_parse((const char *)data, len);
	if (!root) return NULL;
	Numbering *n = calloc(1, sizeof *n);
	for (XmlNode *an = xml_child(root, "w:abstractNum"); an; an = xml_next(an, "w:abstractNum")) {
		n->abs = realloc(n->abs, (size_t)(n->nabs + 1) * sizeof(AbstractNum));
		AbstractNum *a = &n->abs[n->nabs++];
		memset(a, 0, sizeof *a);
		const char *idstr = xml_attr(an, "w:abstractNumId");
		a->abstractId = idstr ? atoi(idstr) : -1;
		for (XmlNode *lvl = xml_child(an, "w:lvl"); lvl && a->nlvl < 9; lvl = xml_next(lvl, "w:lvl")) {
			const char *ilvlstr = xml_attr(lvl, "w:ilvl");
			XmlNode *fmtnode = xml_child(lvl, "w:numFmt");
			a->lvl[a->nlvl].ilvl = ilvlstr ? atoi(ilvlstr) : a->nlvl;
			a->lvl[a->nlvl].fmt = fmt_from_str(fmtnode ? xml_attr(fmtnode, "w:val") : NULL);
			a->nlvl++;
		}
	}
	for (XmlNode *num = xml_child(root, "w:num"); num; num = xml_next(num, "w:num")) {
		const char *idstr = xml_attr(num, "w:numId");
		XmlNode *absid = xml_child(num, "w:abstractNumId");
		if (!idstr || !absid) continue;
		n->map = realloc(n->map, (size_t)(n->nmap + 1) * sizeof(NumMap));
		n->map[n->nmap].numId = atoi(idstr);
		n->map[n->nmap].abstractId = atoi(xml_attr(absid, "w:val"));
		n->nmap++;
	}
	xml_free(root);
	return n;
}

static void numbering_free(Numbering *n)
{
	if (!n) return;
	free(n->abs);
	free(n->map);
	free(n);
}

static ListType numbering_lookup(const Numbering *n, int numId, int ilvl)
{
	if (!n) return LIST_NONE;
	int absId = -1;
	for (int i = 0; i < n->nmap; i++) if (n->map[i].numId == numId) { absId = n->map[i].abstractId; break; }
	if (absId < 0) return LIST_NONE;
	for (int i = 0; i < n->nabs; i++)
		if (n->abs[i].abstractId == absId)
			for (int j = 0; j < n->abs[i].nlvl; j++)
				if (n->abs[i].lvl[j].ilvl == ilvl) return n->abs[i].lvl[j].fmt;
	return LIST_DECIMAL;
}

/* ------------------------------------------------------------------ */
/* word/_rels/document.xml.rels：rId -> Target（圖片、頁尾）              */
/* ------------------------------------------------------------------ */

typedef struct { char id[16]; char target[256]; } Rel;
typedef struct { Rel *r; int n; } Rels;

static Rels *rels_parse(const unsigned char *data, size_t len)
{
	Rels *rs = calloc(1, sizeof *rs);
	if (!data) return rs;
	XmlNode *root = xml_parse((const char *)data, len);
	if (!root) return rs;
	for (XmlNode *n = xml_child(root, "Relationship"); n; n = xml_next(n, "Relationship")) {
		const char *id = xml_attr(n, "Id"), *tgt = xml_attr(n, "Target");
		if (!id || !tgt) continue;
		rs->r = realloc(rs->r, (size_t)(rs->n + 1) * sizeof(Rel));
		snprintf(rs->r[rs->n].id, sizeof rs->r[0].id, "%s", id);
		snprintf(rs->r[rs->n].target, sizeof rs->r[0].target, "%s", tgt);
		rs->n++;
	}
	xml_free(root);
	return rs;
}

static void rels_free(Rels *rs) { if (rs) { free(rs->r); free(rs); } }

static const char *rels_target(const Rels *rs, const char *id)
{
	if (!rs || !id) return NULL;
	for (int i = 0; i < rs->n; i++) if (!strcmp(rs->r[i].id, id)) return rs->r[i].target;
	return NULL;
}

/* 在任意節點底下（遞迴）找第一個 a:blip，回傳它的 r:embed（沒有就回傳 NULL） */
static const char *find_blip_rid(const XmlNode *n)
{
	for (; n; n = n->next) {
		if (n->tag && !strcmp(n->tag, "a:blip")) {
			const char *v = xml_attr(n, "r:embed");
			if (v) return v;
		}
		const char *found = find_blip_rid(n->children);
		if (found) return found;
	}
	return NULL;
}

/* wp:extent 的 cx/cy 是 EMU（914400 = 1 吋 = 72pt） */
#define EMU_TO_PT(e) ((double)(e) / 12700.0)

/* 段落（w:p）裡若恰好是一張圖片（沒有其他文字），回傳圖片資料；否則回傳 0。
 * data 的擁有權轉移給呼叫端（成功時）。 */
static int parse_paragraph_image(const XmlNode *wp, Zip *z, const Rels *rels,
                                 unsigned char **out_data, size_t *out_len, int *out_png, int *out_pw, int *out_ph, double *out_w, double *out_h)
{
	const char *rid = NULL;
	double emu_w = 0, emu_h = 0;
	int saw_text = 0;
	for (XmlNode *c = wp->children; c && !rid; c = c->next) {
		if (!c->tag || strcmp(c->tag, "w:r")) continue;
		for (XmlNode *t = c->children; t; t = t->next) {
			if (t->tag && !strcmp(t->tag, "w:t")) { char *s = xml_text(t); if (*s) saw_text = 1; free(s); }
			else if (t->tag && !strcmp(t->tag, "w:drawing")) {
				rid = find_blip_rid(t);
				XmlNode *ext = NULL;
				for (XmlNode *e = t->children; e && !ext; e = e->next) if (e->tag) ext = e;   /* wp:inline 或 wp:anchor */
				XmlNode *extn = ext ? xml_child(ext, "wp:extent") : NULL;
				if (extn) {
					const char *cx = xml_attr(extn, "cx"), *cy = xml_attr(extn, "cy");
					if (cx) emu_w = atof(cx);
					if (cy) emu_h = atof(cy);
				}
			}
		}
	}
	if (!rid || saw_text) return 0;
	const char *target = rels_target(rels, rid);
	if (!target) return 0;
	char zpath[320];
	snprintf(zpath, sizeof zpath, "%s%s", target[0] == '/' ? target + 1 : "word/", target);
	size_t len;
	unsigned char *data = zip_read(z, zpath, &len);
	if (!data) return 0;
	int is_png = sniff_is_png(data, len), is_jpeg = sniff_is_jpeg(data, len);
	if (!is_png && !is_jpeg) { free(data); return 0; }
	int pw, ph;
	if (!image_dims(data, len, is_png, &pw, &ph)) { free(data); return 0; }
	*out_data = data; *out_len = len; *out_png = is_png; *out_pw = pw; *out_ph = ph;
	*out_w = emu_w > 0 ? EMU_TO_PT(emu_w) : pw * 0.75;
	*out_h = emu_h > 0 ? EMU_TO_PT(emu_h) : ph * 0.75;
	return 1;
}

static PStyle pstyle_from_docx_name(const char *v)
{
	if (!v) return PSTYLE_BODY;
	if (!strcmp(v, "Heading1") || !strcmp(v, "heading 1")) return PSTYLE_H1;
	if (!strcmp(v, "Heading2") || !strcmp(v, "heading 2")) return PSTYLE_H2;
	if (!strcmp(v, "Heading3") || !strcmp(v, "heading 3")) return PSTYLE_H3;
	return PSTYLE_BODY;
}

static const char *docx_style_id(PStyle s)
{
	switch (s) { case PSTYLE_H1: return "Heading1"; case PSTYLE_H2: return "Heading2"; case PSTYLE_H3: return "Heading3"; default: return NULL; }
}

/* ------------------------------------------------------------------ */
/* 讀取：w:rPr -> Run 樣式                                               */
/* ------------------------------------------------------------------ */

static int rpr_flag(const XmlNode *rpr, const char *tag)
{
	XmlNode *n = xml_child(rpr, tag);
	if (!n) return 0;
	const char *v = xml_attr(n, "w:val");
	return !v || (strcmp(v, "0") && strcmp(v, "false"));
}

static void parse_run_props(const XmlNode *rpr, Run *r, double default_size)
{
	r->size = default_size;
	r->color = COLOR_AUTO;
	if (!rpr) return;
	r->bold = rpr_flag(rpr, "w:b");
	r->italic = rpr_flag(rpr, "w:i");
	r->strike = rpr_flag(rpr, "w:strike");
	XmlNode *u = xml_child(rpr, "w:u");
	if (u) { const char *v = xml_attr(u, "w:val"); r->underline = !v || strcmp(v, "none"); }
	XmlNode *color = xml_child(rpr, "w:color");
	if (color) r->color = parse_hex_color(xml_attr(color, "w:val"), COLOR_AUTO);
	XmlNode *sz = xml_child(rpr, "w:sz");
	if (sz) { const char *v = xml_attr(sz, "w:val"); if (v) r->size = PTFROMHALF(atoi(v)); }
	XmlNode *fonts = xml_child(rpr, "w:rFonts");
	if (fonts) {
		const char *f = xml_attr(fonts, "w:ascii");
		if (!f) f = xml_attr(fonts, "w:hAnsi");
		if (!f) f = xml_attr(fonts, "w:eastAsia");
		if (f) snprintf(r->font, sizeof r->font, "%s", f);
	}
}

/* 走訪一個 w:p 底下所有 w:r（包含藏在 w:hyperlink 裡的），依序加進 Para */
static void collect_runs(const XmlNode *container, Para *para, double default_size)
{
	for (XmlNode *c = container->children; c; c = c->next) {
		if (!c->tag) continue;
		if (!strcmp(c->tag, "w:r")) {
			XmlNode *rpr = xml_child(c, "w:rPr");
			Run style;
			memset(&style, 0, sizeof style);
			parse_run_props(rpr, &style, default_size);
			int any = 0;
			for (XmlNode *t = c->children; t; t = t->next) {
				if (!t->tag) continue;
				if (!strcmp(t->tag, "w:t")) {
					char *txt = xml_text(t);
					if (*txt) { para_insert(para, (int)para_text_len(para), txt, strlen(txt), &style); any = 1; }
					free(txt);
				} else if (!strcmp(t->tag, "w:tab")) {
					para_insert(para, (int)para_text_len(para), "\t", 1, &style);
					any = 1;
				} else if (!strcmp(t->tag, "w:br") || !strcmp(t->tag, "w:cr")) {
					/* 段落內換行：目前模型不支援，先用空格代替，已知限制 */
					para_insert(para, (int)para_text_len(para), " ", 1, &style);
					any = 1;
				}
			}
			(void)any;
		} else if (!strcmp(c->tag, "w:hyperlink")) {
			collect_runs(c, para, default_size);
		}
	}
}

static void parse_paragraph(const XmlNode *wp, Para *para, const Numbering *numb, double default_size)
{
	memset(para, 0, sizeof *para);   /* nrun=0、run=NULL：collect_runs 用 para_insert 補上內容，不用先放一個空 run 再丟掉 */
	para->align = ALIGN_LEFT;
	XmlNode *ppr = xml_child(wp, "w:pPr");
	if (ppr) {
		XmlNode *pstyle = xml_child(ppr, "w:pStyle");
		if (pstyle) para->style = pstyle_from_docx_name(xml_attr(pstyle, "w:val"));
		XmlNode *jc = xml_child(ppr, "w:jc");
		if (jc) {
			const char *v = xml_attr(jc, "w:val");
			if (v) {
				if (!strcmp(v, "center")) para->align = ALIGN_CENTER;
				else if (!strcmp(v, "end") || !strcmp(v, "right")) para->align = ALIGN_RIGHT;
				else if (!strcmp(v, "both") || !strcmp(v, "distribute")) para->align = ALIGN_JUSTIFY;
				else para->align = ALIGN_LEFT;
			}
		}
		XmlNode *numpr = xml_child(ppr, "w:numPr");
		if (numpr) {
			XmlNode *ilvln = xml_child(numpr, "w:ilvl");
			XmlNode *numidn = xml_child(numpr, "w:numId");
			int ilvl = ilvln && xml_attr(ilvln, "w:val") ? atoi(xml_attr(ilvln, "w:val")) : 0;
			int numid = numidn && xml_attr(numidn, "w:val") ? atoi(xml_attr(numidn, "w:val")) : 0;
			if (numid > 0) {
				para->list_type = numbering_lookup(numb, numid, ilvl);
				para->list_level = ilvl;
				para->list_id = numid;
			}
		}
	}
	double eff_size = pstyle_size(para->style, default_size);
	collect_runs(wp, para, eff_size);
	if (para->nrun == 0) {
		/* 整段沒有文字：補一個空 run，維持「段落至少有一個 run」的不變量 */
		para->runcap = 1;
		para->run = malloc(sizeof(Run));
		memset(&para->run[0], 0, sizeof(Run));
		para->run[0].text = strdup("");
		para->run[0].size = eff_size;
		para->run[0].color = COLOR_AUTO;
		para->nrun = 1;
	}
}

/* 把一個 table cell（w:tc）裡所有段落接成一段：第一段直接用，之後的段落整段（保留各自的文字樣式）
 * 接到後面，中間補一個空白（已知限制：多段落的儲存格會接在一起，看不出原本的分段）。 */
static void parse_cell(const XmlNode *tc, Para *cell, const Numbering *numb, double default_size)
{
	int first = 1;
	for (XmlNode *wp = xml_child(tc, "w:p"); wp; wp = xml_next(wp, "w:p")) {
		if (first) {
			parse_paragraph(wp, cell, numb, default_size);
			first = 0;
		} else {
			Para extra;
			parse_paragraph(wp, &extra, numb, default_size);
			if (para_text_len(&extra) > 0) {
				if (para_text_len(cell) > 0) para_insert(cell, (int)para_text_len(cell), " ", 1, NULL);
				para_append(cell, &extra);   /* 轉移 run 的擁有權，extra 用完變空殼 */
			} else
				para_clear(&extra);
		}
	}
	if (first) para_init(cell);   /* 根本沒有 w:p（不應該發生，保險而已） */
}

static void parse_table(const XmlNode *tbl, Table *table, const Numbering *numb, double default_size)
{
	memset(table, 0, sizeof *table);
	XmlNode *grid = xml_child(tbl, "w:tblGrid");
	int ncol = 0;
	double colw[64];
	if (grid) for (XmlNode *gc = xml_child(grid, "w:gridCol"); gc && ncol < 64; gc = xml_next(gc, "w:gridCol")) {
		const char *w = xml_attr(gc, "w:w");
		colw[ncol++] = w ? PT(atoi(w)) : 72;
	}
	int nrow = 0;
	for (XmlNode *tr = xml_child(tbl, "w:tr"); tr; tr = xml_next(tr, "w:tr")) nrow++;
	if (!ncol) {
		/* 沒有 tblGrid：用第一列的儲存格數當欄數 */
		XmlNode *tr0 = xml_child(tbl, "w:tr");
		if (tr0) for (XmlNode *tc = xml_child(tr0, "w:tc"); tc; tc = xml_next(tc, "w:tc")) ncol++;
	}
	if (!nrow) nrow = 1;
	if (!ncol) ncol = 1;
	table->nrow = nrow;
	table->ncol = ncol;
	table->cell = malloc((size_t)nrow * ncol * sizeof(Para));
	for (int i = 0; i < nrow * ncol; i++) para_init(&table->cell[i]);
	if (ncol > 0) {
		table->colw = malloc((size_t)ncol * sizeof(double));
		for (int i = 0; i < ncol; i++) table->colw[i] = i < 64 ? colw[i] : 72;
	}
	int r = 0;
	for (XmlNode *tr = xml_child(tbl, "w:tr"); tr && r < nrow; tr = xml_next(tr, "w:tr"), r++) {
		int c = 0;
		for (XmlNode *tc = xml_child(tr, "w:tc"); tc && c < ncol; tc = xml_next(tc, "w:tc"), c++) {
			para_clear(&table->cell[r * ncol + c]);   /* 丟掉上面預先放的空段落，換成真正剖析出來的內容 */
			parse_cell(tc, &table->cell[r * ncol + c], numb, default_size);
		}
	}
}

/* ------------------------------------------------------------------ */
/* docx_read                                                           */
/* ------------------------------------------------------------------ */

Document *docx_read(const char *path)
{
	Zip *z = zip_open(path);
	if (!z) return NULL;
	size_t dlen;
	unsigned char *docxml = zip_read(z, "word/document.xml", &dlen);
	if (!docxml) { zip_close(z); return NULL; }
	size_t nlen;
	unsigned char *numxml = zip_read(z, "word/numbering.xml", &nlen);
	Numbering *numb = numbering_parse(numxml, nlen);
	free(numxml);
	size_t rlen;
	unsigned char *relxml = zip_read(z, "word/_rels/document.xml.rels", &rlen);
	Rels *rels = rels_parse(relxml, rlen);
	free(relxml);

	XmlNode *root = xml_parse((const char *)docxml, dlen);
	free(docxml);
	if (!root) { zip_close(z); numbering_free(numb); rels_free(rels); return NULL; }

	Document *d = doc_new();
	para_clear(&d->block[0].para);   /* doc_new 放的那個預設空段落等一下就不要了，先釋放再蓋過去 */
	d->nblock = 0;
	double default_size = d->default_size;

	XmlNode *body = xml_child(root, "w:body");
	if (body) {
		for (XmlNode *c = body->children; c; c = c->next) {
			if (!c->tag) continue;
			if (!strcmp(c->tag, "w:p")) {
				unsigned char *idata; size_t ilen; int ipng, ipw, iph; double iw, ih;
				int is_img = parse_paragraph_image(c, z, rels, &idata, &ilen, &ipng, &ipw, &iph, &iw, &ih);
				if (d->nblock >= d->blockcap) { d->blockcap = d->blockcap ? d->blockcap * 2 : 8; d->block = realloc(d->block, (size_t)d->blockcap * sizeof(Block)); }
				Block *b = &d->block[d->nblock++];
				memset(b, 0, sizeof *b);
				if (is_img) {
					b->kind = BLOCK_IMAGE;
					b->image.data = idata; b->image.datalen = ilen; b->image.is_png = ipng;
					b->image.pw = ipw; b->image.ph = iph; b->image.w = iw; b->image.h = ih;
				} else {
					b->kind = BLOCK_PARA;
					parse_paragraph(c, &b->para, numb, default_size);
				}
			} else if (!strcmp(c->tag, "w:tbl")) {
				if (d->nblock >= d->blockcap) { d->blockcap = d->blockcap ? d->blockcap * 2 : 8; d->block = realloc(d->block, (size_t)d->blockcap * sizeof(Block)); }
				Block *b = &d->block[d->nblock++];
				memset(b, 0, sizeof *b);
				b->kind = BLOCK_TABLE;
				parse_table(c, &b->table, numb, default_size);
			} else if (!strcmp(c->tag, "w:sectPr")) {
				for (XmlNode *fr = xml_child(c, "w:footerReference"); fr; fr = xml_next(fr, "w:footerReference")) {
					const char *target = rels_target(rels, xml_attr(fr, "r:id"));
					if (!target) continue;
					char zpath[320];
					snprintf(zpath, sizeof zpath, "%s%s", target[0] == '/' ? target + 1 : "word/", target);
					size_t flen;
					unsigned char *fxml = zip_read(z, zpath, &flen);
					if (fxml && memmem(fxml, flen, "PAGE", 4)) d->page_numbers = 1;
					free(fxml);
				}
				XmlNode *pgsz = xml_child(c, "w:pgSz");
				if (pgsz) {
					const char *w = xml_attr(pgsz, "w:w"), *h = xml_attr(pgsz, "w:h");
					if (w) d->page_w = PT(atoi(w));
					if (h) d->page_h = PT(atoi(h));
				}
				XmlNode *mar = xml_child(c, "w:pgMar");
				if (mar) {
					const char *l = xml_attr(mar, "w:left"), *rr = xml_attr(mar, "w:right");
					const char *t = xml_attr(mar, "w:top"), *bo = xml_attr(mar, "w:bottom");
					if (l) d->margin_l = PT(atoi(l));
					if (rr) d->margin_r = PT(atoi(rr));
					if (t) d->margin_t = PT(atoi(t));
					if (bo) d->margin_b = PT(atoi(bo));
				}
			}
		}
	}
	if (d->nblock == 0) {
		doc_free(d);
		d = doc_new();
	} else {
		/* 讓 next_list_id 大於任何讀到的 numId，之後新增清單才不會撞號 */
		int maxid = 0;
		for (int i = 0; i < d->nblock; i++) if (d->block[i].kind == BLOCK_PARA && d->block[i].para.list_id > maxid) maxid = d->block[i].para.list_id;
		d->next_list_id = maxid;
	}

	xml_free(root);
	numbering_free(numb);
	rels_free(rels);
	zip_close(z);
	return d;
}

/* ------------------------------------------------------------------ */
/* docx_write                                                          */
/* ------------------------------------------------------------------ */

static void write_run(XmlWriter *w, const Run *r, double default_size)
{
	xml_open(w, "w:r");
	int need_rpr = r->bold || r->italic || r->underline || r->strike || r->color != COLOR_AUTO || r->font[0] || r->size != default_size;
	if (need_rpr) {
		xml_open(w, "w:rPr");
		if (r->font[0]) { xml_open(w, "w:rFonts"); xml_attr_set(w, "w:ascii", r->font); xml_attr_set(w, "w:hAnsi", r->font); xml_attr_set(w, "w:eastAsia", r->font); xml_close(w); }
		if (r->bold) { xml_open(w, "w:b"); xml_close(w); }
		if (r->italic) { xml_open(w, "w:i"); xml_close(w); }
		if (r->underline) { xml_open(w, "w:u"); xml_attr_set(w, "w:val", "single"); xml_close(w); }
		if (r->strike) { xml_open(w, "w:strike"); xml_close(w); }
		if (r->color != COLOR_AUTO) { char hc[8]; hex_color(hc, sizeof hc, r->color); xml_open(w, "w:color"); xml_attr_set(w, "w:val", hc); xml_close(w); }
		if (r->size != default_size && r->size > 0) { char buf[16]; snprintf(buf, sizeof buf, "%d", HALFPT(r->size)); xml_open(w, "w:sz"); xml_attr_set(w, "w:val", buf); xml_close(w); xml_open(w, "w:szCs"); xml_attr_set(w, "w:val", buf); xml_close(w); }
		xml_close(w);   /* w:rPr */
	}
	xml_open(w, "w:t");
	xml_attr_set(w, "xml:space", "preserve");
	xml_text_out(w, r->text);
	xml_close(w);   /* w:t */
	xml_close(w);   /* w:r */
}

static void write_para(XmlWriter *w, const Para *p, double default_size)
{
	xml_open(w, "w:p");
	int need_ppr = p->align != ALIGN_LEFT || p->list_type != LIST_NONE || p->style != PSTYLE_BODY;
	if (need_ppr) {
		xml_open(w, "w:pPr");
		if (p->style != PSTYLE_BODY) { xml_open(w, "w:pStyle"); xml_attr_set(w, "w:val", docx_style_id(p->style)); xml_close(w); }
		if (p->list_type != LIST_NONE) {
			xml_open(w, "w:numPr");
			char buf[16];
			xml_open(w, "w:ilvl"); snprintf(buf, sizeof buf, "%d", p->list_level); xml_attr_set(w, "w:val", buf); xml_close(w);
			xml_open(w, "w:numId"); snprintf(buf, sizeof buf, "%d", p->list_id > 0 ? p->list_id : 1); xml_attr_set(w, "w:val", buf); xml_close(w);
			xml_close(w);   /* w:numPr */
		}
		if (p->align != ALIGN_LEFT) {
			const char *jc = p->align == ALIGN_CENTER ? "center" : p->align == ALIGN_RIGHT ? "end" : "both";
			xml_open(w, "w:jc"); xml_attr_set(w, "w:val", jc); xml_close(w);
		}
		xml_close(w);   /* w:pPr */
	}
	double eff_size = pstyle_size(p->style, default_size);
	for (int i = 0; i < p->nrun; i++) write_run(w, &p->run[i], eff_size);
	xml_close(w);   /* w:p */
}

/* 圖片：寫成「一個段落裡只有一個 drawing」的最小合法 OOXML（wp:inline） */
static void write_image_para(XmlWriter *w, const Image *im, int ridnum, int docpr_id)
{
	xml_open(w, "w:p");
	xml_open(w, "w:r");
	xml_open(w, "w:drawing");
	xml_open(w, "wp:inline");
	xml_attr_set(w, "distT", "0"); xml_attr_set(w, "distB", "0"); xml_attr_set(w, "distL", "0"); xml_attr_set(w, "distR", "0");
	char buf[32];
	xml_open(w, "wp:extent");
	snprintf(buf, sizeof buf, "%d", (int)(im->w * 12700)); xml_attr_set(w, "cx", buf);
	snprintf(buf, sizeof buf, "%d", (int)(im->h * 12700)); xml_attr_set(w, "cy", buf);
	xml_close(w);   /* wp:extent */
	xml_open(w, "wp:docPr");
	snprintf(buf, sizeof buf, "%d", docpr_id); xml_attr_set(w, "id", buf);
	xml_attr_set(w, "name", "Picture");
	xml_close(w);   /* wp:docPr */
	xml_open(w, "a:graphic");
	xml_open(w, "a:graphicData");
	xml_attr_set(w, "uri", "http://schemas.openxmlformats.org/drawingml/2006/picture");
	xml_open(w, "pic:pic");
	xml_open(w, "pic:nvPicPr");
	xml_open(w, "pic:cNvPr"); xml_attr_set(w, "id", "0"); xml_attr_set(w, "name", "Picture"); xml_close(w);
	xml_open(w, "pic:cNvPicPr"); xml_close(w);
	xml_close(w);   /* pic:nvPicPr */
	xml_open(w, "pic:blipFill");
	xml_open(w, "a:blip");
	snprintf(buf, sizeof buf, "rId%d", ridnum);
	xml_attr_set(w, "r:embed", buf);
	xml_close(w);   /* a:blip */
	xml_open(w, "a:stretch");
	xml_open(w, "a:fillRect"); xml_close(w);
	xml_close(w);   /* a:stretch */
	xml_close(w);   /* pic:blipFill */
	xml_open(w, "pic:spPr");
	xml_open(w, "a:xfrm");
	xml_open(w, "a:off"); xml_attr_set(w, "x", "0"); xml_attr_set(w, "y", "0"); xml_close(w);
	xml_open(w, "a:ext");
	snprintf(buf, sizeof buf, "%d", (int)(im->w * 12700)); xml_attr_set(w, "cx", buf);
	snprintf(buf, sizeof buf, "%d", (int)(im->h * 12700)); xml_attr_set(w, "cy", buf);
	xml_close(w);   /* a:ext */
	xml_close(w);   /* a:xfrm */
	xml_open(w, "a:prstGeom"); xml_attr_set(w, "prst", "rect"); xml_close(w);
	xml_close(w);   /* pic:spPr */
	xml_close(w);   /* pic:pic */
	xml_close(w);   /* a:graphicData */
	xml_close(w);   /* a:graphic */
	xml_close(w);   /* wp:inline */
	xml_close(w);   /* w:drawing */
	xml_close(w);   /* w:r */
	xml_close(w);   /* w:p */
}

static void write_table(XmlWriter *w, const Table *t, double default_size)
{
	xml_open(w, "w:tbl");
	xml_open(w, "w:tblPr");
	xml_open(w, "w:tblBorders");
	static const char *sides[] = { "w:top", "w:start", "w:bottom", "w:end", "w:insideH", "w:insideV" };
	for (size_t i = 0; i < sizeof sides / sizeof *sides; i++) {
		xml_open(w, sides[i]);
		xml_attr_set(w, "w:val", "single");
		xml_attr_set(w, "w:sz", "4");
		xml_attr_set(w, "w:space", "0");
		xml_attr_set(w, "w:color", "999999");
		xml_close(w);
	}
	xml_close(w);   /* w:tblBorders */
	xml_close(w);   /* w:tblPr */
	xml_open(w, "w:tblGrid");
	for (int c = 0; c < t->ncol; c++) {
		xml_open(w, "w:gridCol");
		char buf[16];
		snprintf(buf, sizeof buf, "%d", TWIP(t->colw ? t->colw[c] : 100));
		xml_attr_set(w, "w:w", buf);
		xml_close(w);
	}
	xml_close(w);   /* w:tblGrid */
	for (int r = 0; r < t->nrow; r++) {
		xml_open(w, "w:tr");
		for (int c = 0; c < t->ncol; c++) {
			xml_open(w, "w:tc");
			xml_open(w, "w:tcPr");
			xml_open(w, "w:tcW");
			char buf[16];
			snprintf(buf, sizeof buf, "%d", TWIP(t->colw ? t->colw[c] : 100));
			xml_attr_set(w, "w:w", buf);
			xml_attr_set(w, "w:type", "dxa");
			xml_close(w);
			xml_close(w);   /* w:tcPr */
			write_para(w, &t->cell[r * t->ncol + c], default_size);
			xml_close(w);   /* w:tc */
		}
		xml_close(w);   /* w:tr */
	}
	xml_close(w);   /* w:tbl */
}

static void write_numbering_zip(ZipWriter *zw, const Document *d)
{
	/* 收集文件裡用到的清單群組（list_id），各自需要一個 abstractNum＋num 對照
	 * （bullet 用固定符號、decimal 用阿拉伯數字；層級縮排用固定公式，不逐一還原原本縮排） */
	int ids[256], nids = 0;
	ListType types[256];
	for (int i = 0; i < d->nblock && nids < 256; i++) {
		if (d->block[i].kind != BLOCK_PARA) continue;
		const Para *p = &d->block[i].para;
		if (p->list_type == LIST_NONE) continue;
		int found = 0;
		for (int j = 0; j < nids; j++) if (ids[j] == p->list_id) { found = 1; break; }
		if (!found) { ids[nids] = p->list_id; types[nids] = p->list_type; nids++; }
	}
	XmlWriter *w = xml_writer_new();
	xml_open(w, "w:numbering");
	xml_attr_set(w, "xmlns:w", "http://schemas.openxmlformats.org/wordprocessingml/2006/main");
	for (int i = 0; i < nids; i++) {
		xml_open(w, "w:abstractNum");
		char buf[16];
		snprintf(buf, sizeof buf, "%d", ids[i]);
		xml_attr_set(w, "w:abstractNumId", buf);
		for (int lvl = 0; lvl < 9; lvl++) {
			xml_open(w, "w:lvl");
			snprintf(buf, sizeof buf, "%d", lvl);
			xml_attr_set(w, "w:ilvl", buf);
			xml_open(w, "w:numFmt");
			xml_attr_set(w, "w:val", types[i] == LIST_BULLET ? "bullet" : "decimal");
			xml_close(w);
			xml_open(w, "w:lvlText");
			xml_attr_set(w, "w:val", types[i] == LIST_BULLET ? "•" : "%1.");
			xml_close(w);
			xml_open(w, "w:ind");
			snprintf(buf, sizeof buf, "%d", 720 * (lvl + 1));
			xml_attr_set(w, "w:start", buf);
			xml_attr_set(w, "w:hanging", "360");
			xml_close(w);
			xml_close(w);   /* w:lvl */
		}
		xml_close(w);   /* w:abstractNum */
	}
	for (int i = 0; i < nids; i++) {
		xml_open(w, "w:num");
		char buf[16];
		snprintf(buf, sizeof buf, "%d", ids[i]);
		xml_attr_set(w, "w:numId", buf);
		xml_open(w, "w:abstractNumId");
		xml_attr_set(w, "w:val", buf);
		xml_close(w);
		xml_close(w);   /* w:num */
	}
	xml_close(w);   /* w:numbering */
	size_t len;
	char *out = xml_writer_finish(w, &len);
	zip_writer_add(zw, "word/numbering.xml", (unsigned char *)out, len, 0);
	free(out);
	xml_writer_free(w);
}

int docx_write(const Document *d, const char *path)
{
	int use_numbering = 0, use_headings = 0, nimages = 0;
	for (int i = 0; i < d->nblock; i++) {
		const Block *b = &d->block[i];
		if (b->kind == BLOCK_PARA) {
			if (b->para.list_type != LIST_NONE) use_numbering = 1;
			if (b->para.style != PSTYLE_BODY) use_headings = 1;
		} else if (b->kind == BLOCK_IMAGE) nimages++;
	}
	int use_footer = d->page_numbers;

	/* rId 配置：1=styles，接著視情況給 numbering／footer，再接著每張圖片各一個 */
	int rid_num = use_numbering ? 2 : 0;
	int rid_footer = use_footer ? (use_numbering ? 3 : 2) : 0;
	int rid_img0 = 2 + (use_numbering ? 1 : 0) + (use_footer ? 1 : 0);
	int *img_rid = nimages ? malloc((size_t)nimages * sizeof(int)) : NULL;

	XmlWriter *w = xml_writer_new();
	xml_open(w, "w:document");
	xml_attr_set(w, "xmlns:w", "http://schemas.openxmlformats.org/wordprocessingml/2006/main");
	if (nimages) {
		xml_attr_set(w, "xmlns:wp", "http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing");
		xml_attr_set(w, "xmlns:a", "http://schemas.openxmlformats.org/drawingml/2006/main");
		xml_attr_set(w, "xmlns:pic", "http://schemas.openxmlformats.org/drawingml/2006/picture");
		xml_attr_set(w, "xmlns:r", "http://schemas.openxmlformats.org/officeDocument/2006/relationships");
	}
	xml_open(w, "w:body");
	int imgidx = 0;
	for (int i = 0; i < d->nblock; i++) {
		const Block *b = &d->block[i];
		if (b->kind == BLOCK_PARA) write_para(w, &b->para, d->default_size);
		else if (b->kind == BLOCK_IMAGE) {
			img_rid[imgidx] = rid_img0 + imgidx;
			write_image_para(w, &b->image, img_rid[imgidx], imgidx + 1);
			imgidx++;
		} else write_table(w, &b->table, d->default_size);
	}
	xml_open(w, "w:sectPr");
	char buf[32];
	if (use_footer) {
		xml_open(w, "w:footerReference");
		xml_attr_set(w, "w:type", "default");
		snprintf(buf, sizeof buf, "rId%d", rid_footer);
		xml_attr_set(w, "r:id", buf);
		xml_close(w);
	}
	xml_open(w, "w:pgSz");
	snprintf(buf, sizeof buf, "%d", TWIP(d->page_w));
	xml_attr_set(w, "w:w", buf);
	snprintf(buf, sizeof buf, "%d", TWIP(d->page_h));
	xml_attr_set(w, "w:h", buf);
	xml_close(w);   /* w:pgSz */
	xml_open(w, "w:pgMar");
	snprintf(buf, sizeof buf, "%d", TWIP(d->margin_t)); xml_attr_set(w, "w:top", buf);
	snprintf(buf, sizeof buf, "%d", TWIP(d->margin_r)); xml_attr_set(w, "w:right", buf);
	snprintf(buf, sizeof buf, "%d", TWIP(d->margin_b)); xml_attr_set(w, "w:bottom", buf);
	snprintf(buf, sizeof buf, "%d", TWIP(d->margin_l)); xml_attr_set(w, "w:left", buf);
	xml_attr_set(w, "w:header", "0");
	xml_attr_set(w, "w:footer", use_footer ? "360" : "0");
	xml_attr_set(w, "w:gutter", "0");
	xml_close(w);   /* w:pgMar */
	xml_close(w);   /* w:sectPr */
	xml_close(w);   /* w:body */
	xml_close(w);   /* w:document */
	size_t doclen;
	char *docxml = xml_writer_finish(w, &doclen);
	xml_writer_free(w);

	ZipWriter *zw = zip_writer_new();

	/* [Content_Types].xml */
	{
		int has_png = 0, has_jpeg = 0;
		for (int i = 0; i < d->nblock; i++) if (d->block[i].kind == BLOCK_IMAGE) { if (d->block[i].image.is_png) has_png = 1; else has_jpeg = 1; }
		size_t cap = 2048;
		char *ctbuf = malloc(cap);
		int n = snprintf(ctbuf, cap,
		    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
		    "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
		    "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
		    "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
		    "%s%s"
		    "<Override PartName=\"/word/document.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml\"/>"
		    "<Override PartName=\"/word/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml\"/>"
		    "%s%s"
		    "</Types>",
		    has_png ? "<Default Extension=\"png\" ContentType=\"image/png\"/>" : "",
		    has_jpeg ? "<Default Extension=\"jpeg\" ContentType=\"image/jpeg\"/>" : "",
		    use_numbering ? "<Override PartName=\"/word/numbering.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.numbering+xml\"/>" : "",
		    use_footer ? "<Override PartName=\"/word/footer1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.footer+xml\"/>" : "");
		zip_writer_add(zw, "[Content_Types].xml", (unsigned char *)ctbuf, (size_t)n, 0);
		free(ctbuf);
	}

	const char *rels =
	    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
	    "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
	    "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"word/document.xml\"/>"
	    "</Relationships>";
	zip_writer_add(zw, "_rels/.rels", (const unsigned char *)rels, strlen(rels), 0);

	/* word/_rels/document.xml.rels */
	{
		size_t cap = 512 + (size_t)nimages * 160;
		char *drbuf = malloc(cap);
		size_t n = (size_t)snprintf(drbuf, cap,
		    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
		    "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
		    "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/>");
		if (use_numbering) n += (size_t)snprintf(drbuf + n, cap - n,
		    "<Relationship Id=\"rId%d\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/numbering\" Target=\"numbering.xml\"/>", rid_num);
		if (use_footer) n += (size_t)snprintf(drbuf + n, cap - n,
		    "<Relationship Id=\"rId%d\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/footer\" Target=\"footer1.xml\"/>", rid_footer);
		for (int i = 0; i < nimages; i++) {
			const Block *b = NULL; int seen = 0;
			for (int j = 0; j < d->nblock; j++) if (d->block[j].kind == BLOCK_IMAGE) { if (seen == i) { b = &d->block[j]; break; } seen++; }
			n += (size_t)snprintf(drbuf + n, cap - n,
			    "<Relationship Id=\"rId%d\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/image\" Target=\"media/image%d.%s\"/>",
			    img_rid[i], i + 1, b && b->image.is_png ? "png" : "jpeg");
		}
		n += (size_t)snprintf(drbuf + n, cap - n, "</Relationships>");
		zip_writer_add(zw, "word/_rels/document.xml.rels", (unsigned char *)drbuf, n, 0);
		free(drbuf);
	}

	size_t cap_styles = 2048;
	char *stylesbuf = malloc(cap_styles);
	int sn = snprintf(stylesbuf, cap_styles,
	    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
	    "<w:styles xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
	    "<w:docDefaults><w:rPrDefault><w:rPr><w:rFonts w:ascii=\"%s\" w:hAnsi=\"%s\" w:eastAsia=\"%s\"/>"
	    "<w:sz w:val=\"%d\"/></w:rPr></w:rPrDefault></w:docDefaults>"
	    "<w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\"><w:name w:val=\"Normal\"/></w:style>"
	    "%s"
	    "</w:styles>",
	    d->default_font, d->default_font, d->default_font, HALFPT(d->default_size),
	    use_headings ?
	    "<w:style w:type=\"paragraph\" w:styleId=\"Heading1\"><w:name w:val=\"heading 1\"/><w:basedOn w:val=\"Normal\"/>"
	    "<w:pPr><w:outlineLvl w:val=\"0\"/></w:pPr><w:rPr><w:b/></w:rPr></w:style>"
	    "<w:style w:type=\"paragraph\" w:styleId=\"Heading2\"><w:name w:val=\"heading 2\"/><w:basedOn w:val=\"Normal\"/>"
	    "<w:pPr><w:outlineLvl w:val=\"1\"/></w:pPr><w:rPr><w:b/></w:rPr></w:style>"
	    "<w:style w:type=\"paragraph\" w:styleId=\"Heading3\"><w:name w:val=\"heading 3\"/><w:basedOn w:val=\"Normal\"/>"
	    "<w:pPr><w:outlineLvl w:val=\"2\"/></w:pPr><w:rPr><w:b/></w:rPr></w:style>" : "");
	zip_writer_add(zw, "word/styles.xml", (const unsigned char *)stylesbuf, (size_t)sn, 0);
	free(stylesbuf);

	if (use_footer) {
		char ftr[512];
		int fn = snprintf(ftr, sizeof ftr,
		    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
		    "<w:ftr xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
		    "<w:p><w:pPr><w:jc w:val=\"center\"/></w:pPr>"
		    "<w:fldSimple w:instr=\" PAGE \"><w:r><w:t>1</w:t></w:r></w:fldSimple>"
		    "</w:p></w:ftr>");
		zip_writer_add(zw, "word/footer1.xml", (const unsigned char *)ftr, (size_t)fn, 0);
	}

	if (use_numbering) write_numbering_zip(zw, d);

	imgidx = 0;
	for (int i = 0; i < d->nblock; i++) {
		if (d->block[i].kind != BLOCK_IMAGE) continue;
		const Image *im = &d->block[i].image;
		char mpath[64];
		snprintf(mpath, sizeof mpath, "word/media/image%d.%s", imgidx + 1, im->is_png ? "png" : "jpeg");
		zip_writer_add(zw, mpath, im->data, im->datalen, 1);
		imgidx++;
	}
	free(img_rid);

	zip_writer_add(zw, "word/document.xml", (unsigned char *)docxml, doclen, 0);
	free(docxml);

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
