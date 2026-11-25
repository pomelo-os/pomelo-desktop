/* SPDX-License-Identifier: GPL-3.0-or-later */
/* doc.c — 見 doc.h */
#define _GNU_SOURCE
#include "doc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* 區塊陣列的基本操作                                                     */
/* ------------------------------------------------------------------ */

static void doc_block_insert_raw(Document *d, int idx, BlockKind kind)
{
	if (d->nblock >= d->blockcap) {
		d->blockcap = d->blockcap ? d->blockcap * 2 : 8;
		d->block = realloc(d->block, (size_t)d->blockcap * sizeof(Block));
	}
	memmove(&d->block[idx + 1], &d->block[idx], (size_t)(d->nblock - idx) * sizeof(Block));
	memset(&d->block[idx], 0, sizeof(Block));
	d->block[idx].kind = kind;
	d->nblock++;
}

/* ------------------------------------------------------------------ */
/* 段落／run                                                            */
/* ------------------------------------------------------------------ */

void para_init(Para *p)
{
	memset(p, 0, sizeof *p);
	p->runcap = 4;
	p->run = malloc((size_t)p->runcap * sizeof(Run));
	p->nrun = 1;
	memset(&p->run[0], 0, sizeof(Run));
	p->run[0].text = strdup("");
	p->align = ALIGN_LEFT;
}

void para_clear(Para *p)
{
	for (int i = 0; i < p->nrun; i++) free(p->run[i].text);
	free(p->run);
	memset(p, 0, sizeof *p);
}

void para_copy_into(Para *dst, const Para *src)
{
	dst->nrun = dst->runcap = src->nrun > 0 ? src->nrun : 1;
	dst->run = malloc((size_t)dst->runcap * sizeof(Run));
	if (src->nrun <= 0) {
		memset(&dst->run[0], 0, sizeof(Run));
		dst->run[0].text = strdup("");
		dst->nrun = 1;
	} else {
		for (int i = 0; i < src->nrun; i++) {
			dst->run[i] = src->run[i];
			dst->run[i].text = malloc(src->run[i].len + 1);
			memcpy(dst->run[i].text, src->run[i].text, src->run[i].len);
			dst->run[i].text[src->run[i].len] = 0;
		}
	}
	dst->align = src->align;
	dst->list_type = src->list_type;
	dst->list_level = src->list_level;
	dst->list_id = src->list_id;
	dst->style = src->style;
}

size_t para_text_len(const Para *p)
{
	size_t n = 0;
	for (int i = 0; i < p->nrun; i++) n += p->run[i].len;
	return n;
}

char *para_text(const Para *p)
{
	size_t n = para_text_len(p);
	char *out = malloc(n + 1);
	size_t o = 0;
	for (int i = 0; i < p->nrun; i++) { memcpy(out + o, p->run[i].text, p->run[i].len); o += p->run[i].len; }
	out[n] = 0;
	return out;
}

int para_run_at(const Para *p, int offset, int *run_offset)
{
	int acc = 0;
	for (int i = 0; i < p->nrun; i++) {
		int rl = (int)p->run[i].len;
		if (offset <= acc + rl) { if (run_offset) *run_offset = offset - acc; return i; }
		acc += rl;
	}
	int last = p->nrun > 0 ? p->nrun - 1 : 0;
	if (run_offset) *run_offset = p->nrun > 0 ? (int)p->run[last].len : 0;
	return last;
}

static int style_eq(const Run *a, const Run *b)
{
	return a->bold == b->bold && a->italic == b->italic && a->underline == b->underline && a->strike == b->strike
	    && a->size == b->size && a->color == b->color && !strcmp(a->font, b->font);
}

static void style_copy(Run *dst, const Run *src)
{
	dst->bold = src->bold; dst->italic = src->italic; dst->underline = src->underline; dst->strike = src->strike;
	dst->size = src->size; dst->color = src->color;
	memcpy(dst->font, src->font, sizeof dst->font);
}

static void ensure_runcap(Para *p, int extra)
{
	if (p->nrun + extra > p->runcap) {
		p->runcap = (p->nrun + extra) * 2;
		p->run = realloc(p->run, (size_t)p->runcap * sizeof(Run));
	}
}

static void insert_run_at(Para *p, int at, Run r)
{
	ensure_runcap(p, 1);
	memmove(&p->run[at + 1], &p->run[at], (size_t)(p->nrun - at) * sizeof(Run));
	p->run[at] = r;
	p->nrun++;
}

static void para_coalesce(Para *p)
{
	for (int i = 0; i < p->nrun - 1;) {
		if (style_eq(&p->run[i], &p->run[i + 1])) {
			size_t nl = p->run[i].len + p->run[i + 1].len;
			char *nt = malloc(nl + 1);
			memcpy(nt, p->run[i].text, p->run[i].len);
			memcpy(nt + p->run[i].len, p->run[i + 1].text, p->run[i + 1].len);
			nt[nl] = 0;
			free(p->run[i].text);
			p->run[i].text = nt;
			p->run[i].len = nl;
			free(p->run[i + 1].text);
			memmove(&p->run[i + 1], &p->run[i + 2], (size_t)(p->nrun - i - 2) * sizeof(Run));
			p->nrun--;
		} else
			i++;
	}
}

/* 讓 offset 落在乾淨的 run 邊界上（必要時切開一個 run），回傳邊界處的 run 索引
 * （也就是「要在這裡插入新 run」的位置）。 */
static int ensure_boundary(Para *p, int offset)
{
	size_t total = para_text_len(p);
	if (offset <= 0) return 0;
	if ((size_t)offset >= total) return p->nrun;
	int ro, i = para_run_at(p, offset, &ro);
	if (ro == 0) return i;
	if (ro == (int)p->run[i].len) return i + 1;
	Run orig = p->run[i];
	size_t blen = (size_t)ro, alen = orig.len - blen;
	char *btext = malloc(blen + 1);
	memcpy(btext, orig.text, blen);
	btext[blen] = 0;
	char *atext = malloc(alen + 1);
	memcpy(atext, orig.text + blen, alen);
	atext[alen] = 0;
	free(p->run[i].text);
	p->run[i].text = btext;
	p->run[i].len = blen;
	Run after = orig;
	after.text = atext;
	after.len = alen;
	insert_run_at(p, i + 1, after);
	return i + 1;
}

void para_insert(Para *p, int offset, const char *text, size_t len, const Run *style)
{
	if (!len) return;
	size_t total = para_text_len(p);
	if (offset < 0) offset = 0;
	if ((size_t)offset > total) offset = (int)total;
	int idx = ensure_boundary(p, offset);
	Run newr;
	memset(&newr, 0, sizeof newr);
	if (style) style_copy(&newr, style);
	else if (idx > 0) style_copy(&newr, &p->run[idx - 1]);
	else if (idx < p->nrun) style_copy(&newr, &p->run[idx]);
	newr.text = malloc(len + 1);
	memcpy(newr.text, text, len);
	newr.text[len] = 0;
	newr.len = len;
	insert_run_at(p, idx, newr);
	para_coalesce(p);
}

void para_delete(Para *p, int start, int end)
{
	size_t total = para_text_len(p);
	if (start < 0) start = 0;
	if (end > (int)total) end = (int)total;
	if (start >= end) return;
	int i0 = ensure_boundary(p, start);
	int i1 = ensure_boundary(p, end);
	for (int i = i0; i < i1; i++) free(p->run[i].text);
	memmove(&p->run[i0], &p->run[i1], (size_t)(p->nrun - i1) * sizeof(Run));
	p->nrun -= (i1 - i0);
	if (p->nrun == 0) {
		p->nrun = 1;
		memset(&p->run[0], 0, sizeof(Run));
		p->run[0].text = strdup("");
	}
	para_coalesce(p);
}

void para_apply_style(Para *p, int start, int end, void (*fn)(Run *r, void *ctx), void *ctx)
{
	size_t total = para_text_len(p);
	if (start < 0) start = 0;
	if (end > (int)total) end = (int)total;
	if (start >= end) {
		/* 沒有選取範圍：套到整段（常見用法：游標停在段落中沒選字時，按粗體套到接下來要打的字——
		 * 這裡簡化成套到整個段落，呼叫端若要「只影響接下來打的字」要自己另外處理預設樣式） */
		return;
	}
	int i0 = ensure_boundary(p, start);
	int i1 = ensure_boundary(p, end);
	for (int i = i0; i < i1; i++) fn(&p->run[i], ctx);
	para_coalesce(p);
}

Para para_split(Para *p, int offset)
{
	int idx = ensure_boundary(p, offset);
	Para tail;
	memset(&tail, 0, sizeof tail);
	int moved = p->nrun - idx;
	if (moved <= 0) {
		tail.runcap = tail.nrun = 1;
		tail.run = malloc(sizeof(Run));
		memset(&tail.run[0], 0, sizeof(Run));
		tail.run[0].text = strdup("");
	} else {
		tail.runcap = tail.nrun = moved;
		tail.run = malloc((size_t)moved * sizeof(Run));
		memcpy(tail.run, &p->run[idx], (size_t)moved * sizeof(Run));
	}
	p->nrun = idx;
	if (p->nrun == 0) {
		p->nrun = 1;
		memset(&p->run[0], 0, sizeof(Run));
		p->run[0].text = strdup("");
	}
	tail.align = p->align;
	tail.list_type = p->list_type;
	tail.list_level = p->list_level;
	tail.list_id = p->list_id;
	tail.style = p->style;
	return tail;
}

void para_append(Para *dst, Para *src)
{
	if (src->nrun > 0) {
		ensure_runcap(dst, src->nrun);
		for (int i = 0; i < src->nrun; i++) dst->run[dst->nrun++] = src->run[i];
	}
	free(src->run);
	memset(src, 0, sizeof *src);
	para_coalesce(dst);
}

/* ------------------------------------------------------------------ */
/* 位址存取                                                              */
/* ------------------------------------------------------------------ */

Para *doc_para_at(Document *d, Pos p)
{
	if (p.block < 0 || p.block >= d->nblock) return NULL;
	Block *b = &d->block[p.block];
	if (b->kind == BLOCK_PARA) return &b->para;
	if (b->kind == BLOCK_IMAGE) return NULL;    /* 圖片沒有文字 */
	if (p.row < 0 || p.row >= b->table.nrow || p.col < 0 || p.col >= b->table.ncol) return NULL;
	return &b->table.cell[p.row * b->table.ncol + p.col];
}

const Para *doc_para_at_c(const Document *d, Pos p) { return doc_para_at((Document *)d, p); }

Pos doc_unit_start(const Document *d, int block, int row, int col) { (void)d; return (Pos){ block, row, col, 0 }; }

Pos doc_unit_end(const Document *d, int block, int row, int col)
{
	Pos p = { block, row, col, 0 };
	const Para *pa = doc_para_at_c(d, p);
	p.offset = pa ? (int)para_text_len(pa) : 0;
	return p;
}

int doc_next_unit(const Document *d, int block, int row, int col, int *nb, int *nr, int *nc)
{
	if (block < 0 || block >= d->nblock) return 0;
	const Block *b = &d->block[block];
	if (b->kind == BLOCK_TABLE) {
		int idx = row * b->table.ncol + col;
		int total = b->table.nrow * b->table.ncol;
		if (idx + 1 < total) { *nb = block; *nr = (idx + 1) / b->table.ncol; *nc = (idx + 1) % b->table.ncol; return 1; }
	}
	if (block + 1 < d->nblock) { *nb = block + 1; *nr = 0; *nc = 0; return 1; }
	return 0;
}

int doc_prev_unit(const Document *d, int block, int row, int col, int *nb, int *nr, int *nc)
{
	if (block < 0 || block >= d->nblock) return 0;
	const Block *b = &d->block[block];
	if (b->kind == BLOCK_TABLE) {
		int idx = row * b->table.ncol + col;
		if (idx > 0) { *nb = block; *nr = (idx - 1) / b->table.ncol; *nc = (idx - 1) % b->table.ncol; return 1; }
	}
	if (block - 1 < 0) return 0;
	const Block *pb = &d->block[block - 1];
	if (pb->kind == BLOCK_TABLE) { *nb = block - 1; *nr = pb->table.nrow - 1; *nc = pb->table.ncol - 1; return 1; }
	*nb = block - 1; *nr = 0; *nc = 0;
	return 1;
}

Pos doc_start(const Document *d) { return doc_unit_start(d, 0, 0, 0); }

Pos doc_end(const Document *d)
{
	int b = d->nblock - 1;
	if (b < 0) return (Pos){ 0, 0, 0, 0 };
	const Block *blk = &d->block[b];
	if (blk->kind == BLOCK_TABLE) return doc_unit_end(d, b, blk->table.nrow - 1, blk->table.ncol - 1);
	return doc_unit_end(d, b, 0, 0);
}

int doc_pos_cmp(const Document *d, Pos a, Pos b)
{
	(void)d;
	if (a.block != b.block) return a.block < b.block ? -1 : 1;
	if (a.row != b.row) return a.row < b.row ? -1 : 1;
	if (a.col != b.col) return a.col < b.col ? -1 : 1;
	if (a.offset != b.offset) return a.offset < b.offset ? -1 : 1;
	return 0;
}

Pos doc_pos_clamp(const Document *d, Pos p)
{
	if (d->nblock == 0) return (Pos){ 0, 0, 0, 0 };
	if (p.block < 0) p.block = 0;
	if (p.block >= d->nblock) p.block = d->nblock - 1;
	Block *b = &d->block[p.block];
	if (b->kind == BLOCK_TABLE) {
		if (p.row < 0) p.row = 0;
		if (p.row >= b->table.nrow) p.row = b->table.nrow - 1;
		if (p.col < 0) p.col = 0;
		if (p.col >= b->table.ncol) p.col = b->table.ncol - 1;
	} else {
		p.row = p.col = 0;
	}
	const Para *pa = doc_para_at_c(d, p);
	int len = pa ? (int)para_text_len(pa) : 0;
	if (p.offset < 0) p.offset = 0;
	if (p.offset > len) p.offset = len;
	return p;
}

/* ------------------------------------------------------------------ */
/* 文件層級的編輯                                                        */
/* ------------------------------------------------------------------ */

Pos doc_insert_text(Document *d, Pos at, const char *text, size_t len, const Run *style)
{
	Para *p = doc_para_at(d, at);
	if (!p) return at;
	para_insert(p, at.offset, text, len, style);
	Pos np = at;
	np.offset = at.offset + (int)len;
	return np;
}

Pos doc_insert_para_break(Document *d, Pos at)
{
	Block *b = &d->block[at.block];
	if (b->kind == BLOCK_TABLE) {
		int nb, nr, nc;
		if (doc_next_unit(d, at.block, at.row, at.col, &nb, &nr, &nc)) return doc_unit_start(d, nb, nr, nc);
		return at;
	}
	if (b->kind == BLOCK_IMAGE) return at;   /* 圖片沒有文字可以切，忽略（已知限制） */
	Para tail = para_split(&b->para, at.offset);
	doc_block_insert_raw(d, at.block + 1, BLOCK_PARA);
	d->block[at.block + 1].para = tail;
	return (Pos){ at.block + 1, 0, 0, 0 };
}

void doc_delete_block(Document *d, int block)
{
	if (block < 0 || block >= d->nblock) return;
	Block *b = &d->block[block];
	if (b->kind == BLOCK_PARA) para_clear(&b->para);
	else if (b->kind == BLOCK_IMAGE) free(b->image.data);
	else {
		int n = b->table.nrow * b->table.ncol;
		for (int i = 0; i < n; i++) para_clear(&b->table.cell[i]);
		free(b->table.cell);
		free(b->table.colw);
	}
	memmove(&d->block[block], &d->block[block + 1], (size_t)(d->nblock - block - 1) * sizeof(Block));
	d->nblock--;
	if (d->nblock == 0) {
		doc_block_insert_raw(d, 0, BLOCK_PARA);
		para_init(&d->block[0].para);
	}
}

Pos doc_delete_range(Document *d, Pos a, Pos b)
{
	if (doc_pos_cmp(d, a, b) > 0) { Pos t = a; a = b; b = t; }
	if (doc_pos_cmp(d, a, b) == 0) return a;

	if (a.block == b.block && a.row == b.row && a.col == b.col) {
		Para *p = doc_para_at(d, a);
		if (p) para_delete(p, a.offset, b.offset);
		return a;
	}

	Block *ba = &d->block[a.block];
	if (a.block == b.block && ba->kind == BLOCK_TABLE) {
		/* 同一張表格、不同儲存格：清掉頭尾兩格以及中間（row-major 次序）經過的格子，
		 * 表格結構本身不變（已知限制：不會把多格合併成一格） */
		int ncol = ba->table.ncol;
		int ia = a.row * ncol + a.col, ib = b.row * ncol + b.col;
		Para *pa2 = &ba->table.cell[ia], *pb2 = &ba->table.cell[ib];
		para_delete(pa2, a.offset, (int)para_text_len(pa2));
		para_delete(pb2, 0, b.offset);
		for (int idx = ia + 1; idx < ib; idx++) {
			Para *pc = &ba->table.cell[idx];
			para_delete(pc, 0, (int)para_text_len(pc));
		}
		return a;
	}

	Block *bb = &d->block[b.block];
	int a_is_cell = (ba->kind == BLOCK_TABLE);
	int b_is_cell_other_block = (bb->kind == BLOCK_TABLE);

	Para *pa = doc_para_at(d, a);
	if (pa) para_delete(pa, a.offset, (int)para_text_len(pa));
	Para *pb = doc_para_at(d, b);
	if (pb) para_delete(pb, 0, b.offset);

	/* 只有「起點不是表格儲存格、終點所在區塊也不是表格」時，才能把尾段文字接過來、
	 * 把中間與終點的區塊整個刪掉；否則（牽涉到表格邊界）只清文字，保留表格結構，
	 * 這是已知限制：跨進／跨出表格的選取不會把段落接在一起。 */
	int merge_tail = !a_is_cell && !b_is_cell_other_block;
	if (merge_tail && pa && pb) para_append(pa, pb);

	int remove_from = a.block + 1;
	int remove_to = merge_tail ? b.block : b.block - 1;
	for (int i = remove_to; i >= remove_from; i--) doc_delete_block(d, i);

	return a;
}

/* ------------------------------------------------------------------ */
/* 表格                                                                 */
/* ------------------------------------------------------------------ */

int doc_insert_table(Document *d, int block, int nrow, int ncol)
{
	int idx = block + 1;
	doc_block_insert_raw(d, idx, BLOCK_TABLE);
	Block *b = &d->block[idx];
	b->table.nrow = nrow;
	b->table.ncol = ncol;
	b->table.cell = malloc((size_t)nrow * ncol * sizeof(Para));
	for (int i = 0; i < nrow * ncol; i++) para_init(&b->table.cell[i]);
	b->table.colw = NULL;
	return idx;
}

void table_insert_row(Table *t, int at)
{
	if (at < 0) at = 0;
	if (at > t->nrow) at = t->nrow;
	Para *nc = malloc((size_t)(t->nrow + 1) * (size_t)t->ncol * sizeof(Para));
	memcpy(nc, t->cell, (size_t)at * (size_t)t->ncol * sizeof(Para));
	for (int c = 0; c < t->ncol; c++) para_init(&nc[at * t->ncol + c]);
	memcpy(nc + (size_t)(at + 1) * t->ncol, t->cell + (size_t)at * t->ncol, (size_t)(t->nrow - at) * (size_t)t->ncol * sizeof(Para));
	free(t->cell);
	t->cell = nc;
	t->nrow++;
}

void table_delete_row(Table *t, int at)
{
	if (at < 0 || at >= t->nrow || t->nrow <= 1) return;
	for (int c = 0; c < t->ncol; c++) para_clear(&t->cell[at * t->ncol + c]);
	memmove(t->cell + (size_t)at * t->ncol, t->cell + (size_t)(at + 1) * t->ncol, (size_t)(t->nrow - at - 1) * (size_t)t->ncol * sizeof(Para));
	t->nrow--;
}

void table_insert_col(Table *t, int at)
{
	int oldcol = t->ncol;
	if (at < 0) at = 0;
	if (at > oldcol) at = oldcol;
	int newcol = oldcol + 1;
	Para *nc = malloc((size_t)t->nrow * (size_t)newcol * sizeof(Para));
	for (int r = 0; r < t->nrow; r++) {
		memcpy(nc + (size_t)r * newcol, t->cell + (size_t)r * oldcol, (size_t)at * sizeof(Para));
		para_init(&nc[r * newcol + at]);
		memcpy(nc + (size_t)r * newcol + at + 1, t->cell + (size_t)r * oldcol + at, (size_t)(oldcol - at) * sizeof(Para));
	}
	free(t->cell);
	t->cell = nc;
	if (t->colw) {
		double *ncw = realloc(t->colw, (size_t)newcol * sizeof(double));
		memmove(ncw + at + 1, ncw + at, (size_t)(oldcol - at) * sizeof(double));
		ncw[at] = 72;
		t->colw = ncw;
	}
	t->ncol = newcol;
}

void table_delete_col(Table *t, int at)
{
	int oldcol = t->ncol;
	if (at < 0 || at >= oldcol || oldcol <= 1) return;
	int newcol = oldcol - 1;
	Para *nc = malloc((size_t)t->nrow * (size_t)newcol * sizeof(Para));
	for (int r = 0; r < t->nrow; r++)
		for (int c = 0; c < oldcol; c++) {
			if (c == at) { para_clear(&t->cell[r * oldcol + c]); continue; }
			int dc = c < at ? c : c - 1;
			nc[r * newcol + dc] = t->cell[r * oldcol + c];
		}
	free(t->cell);
	t->cell = nc;
	if (t->colw) {
		memmove(t->colw + at, t->colw + at + 1, (size_t)(newcol - at) * sizeof(double));
		t->colw = realloc(t->colw, (size_t)newcol * sizeof(double));
	}
	t->ncol = newcol;
}

int doc_new_list_id(Document *d) { return ++d->next_list_id; }

/* ------------------------------------------------------------------ */
/* 圖片                                                                 */
/* ------------------------------------------------------------------ */

int doc_insert_image(Document *d, int block, unsigned char *data, size_t datalen, int is_png, int pw, int ph, double w, double h)
{
	int idx = block + 1;
	doc_block_insert_raw(d, idx, BLOCK_IMAGE);
	Block *b = &d->block[idx];
	b->image.data = data;
	b->image.datalen = datalen;
	b->image.is_png = is_png;
	b->image.pw = pw;
	b->image.ph = ph;
	b->image.w = w;
	b->image.h = h;
	return idx;
}

/* ------------------------------------------------------------------ */
/* 段落樣式                                                             */
/* ------------------------------------------------------------------ */

double pstyle_size(PStyle s, double default_size)
{
	switch (s) {
	case PSTYLE_H1: return default_size + 8;
	case PSTYLE_H2: return default_size + 4;
	case PSTYLE_H3: return default_size + 1;
	default: return default_size;
	}
}

int pstyle_bold(PStyle s) { return s != PSTYLE_BODY; }

/* ------------------------------------------------------------------ */
/* 文件建立／釋放／複製                                                    */
/* ------------------------------------------------------------------ */

Document *doc_new(void)
{
	Document *d = calloc(1, sizeof *d);
	d->page_w = 595.0;    /* A4，單位 pt（1/72 吋） */
	d->page_h = 842.0;
	d->margin_l = d->margin_r = d->margin_t = d->margin_b = 72.0;   /* 1 吋 */
	snprintf(d->default_font, sizeof d->default_font, "Noto Sans CJK TC");
	d->default_size = 12;
	doc_block_insert_raw(d, 0, BLOCK_PARA);
	para_init(&d->block[0].para);
	return d;
}

void doc_free(Document *d)
{
	if (!d) return;
	for (int i = 0; i < d->nblock; i++) {
		Block *b = &d->block[i];
		if (b->kind == BLOCK_PARA) para_clear(&b->para);
		else if (b->kind == BLOCK_IMAGE) free(b->image.data);
		else {
			int n = b->table.nrow * b->table.ncol;
			for (int j = 0; j < n; j++) para_clear(&b->table.cell[j]);
			free(b->table.cell);
			free(b->table.colw);
		}
	}
	free(d->block);
	free(d);
}

Document *doc_clone(const Document *src)
{
	Document *d = calloc(1, sizeof *d);
	d->page_w = src->page_w; d->page_h = src->page_h;
	d->margin_l = src->margin_l; d->margin_r = src->margin_r; d->margin_t = src->margin_t; d->margin_b = src->margin_b;
	memcpy(d->default_font, src->default_font, sizeof d->default_font);
	d->default_size = src->default_size;
	d->next_list_id = src->next_list_id;
	d->page_numbers = src->page_numbers;
	for (int i = 0; i < src->nblock; i++) {
		const Block *sb = &src->block[i];
		doc_block_insert_raw(d, i, sb->kind);
		Block *db = &d->block[i];
		if (sb->kind == BLOCK_PARA) {
			para_copy_into(&db->para, &sb->para);
		} else if (sb->kind == BLOCK_IMAGE) {
			db->image = sb->image;
			db->image.data = malloc(sb->image.datalen);
			memcpy(db->image.data, sb->image.data, sb->image.datalen);
		} else {
			db->table.nrow = sb->table.nrow;
			db->table.ncol = sb->table.ncol;
			int n = sb->table.nrow * sb->table.ncol;
			db->table.cell = malloc((size_t)(n > 0 ? n : 1) * sizeof(Para));
			for (int j = 0; j < n; j++) para_copy_into(&db->table.cell[j], &sb->table.cell[j]);
			if (sb->table.colw) {
				db->table.colw = malloc((size_t)sb->table.ncol * sizeof(double));
				memcpy(db->table.colw, sb->table.colw, (size_t)sb->table.ncol * sizeof(double));
			}
		}
	}
	return d;
}

size_t doc_size_estimate(const Document *d)
{
	size_t n = sizeof(Document);
	for (int i = 0; i < d->nblock; i++) {
		const Block *b = &d->block[i];
		if (b->kind == BLOCK_PARA) {
			n += sizeof(Para);
			for (int j = 0; j < b->para.nrun; j++) n += sizeof(Run) + b->para.run[j].len;
		} else if (b->kind == BLOCK_IMAGE) {
			n += sizeof(Image) + b->image.datalen;
		} else {
			int cnt = b->table.nrow * b->table.ncol;
			n += sizeof(Table) + (size_t)cnt * sizeof(Para);
			for (int j = 0; j < cnt; j++) {
				const Para *p = &b->table.cell[j];
				for (int k = 0; k < p->nrun; k++) n += sizeof(Run) + p->run[k].len;
			}
		}
	}
	return n;
}
