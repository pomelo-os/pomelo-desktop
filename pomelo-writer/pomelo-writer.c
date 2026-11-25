/* SPDX-License-Identifier: GPL-3.0-or-later */
/* pomelo-writer — Pomelo 文書處理器
 *
 * 開啟、編輯、存檔 .docx（Word）與 .odt（OpenDocument Text）。文件模型、排版、檔案格式
 * 都是自己寫的（見 doc.h / layout.h / docx.h / odt.h），不依賴 LibreOffice 或其他外部程式。
 *
 * 用法：pomelo-writer [檔案]
 * 快捷鍵：Ctrl+N 新檔、Ctrl+O 開啟、Ctrl+S 儲存、Ctrl+Shift+S 另存新檔、Ctrl+P 列印、
 *   Ctrl+F 尋找、Ctrl+H 取代、Ctrl+Z/Y 復原／重做、Ctrl+X/C/V 剪下／拷貝／貼上、
 *   Ctrl+B/I/U 粗體／斜體／底線、Ctrl+加號／減號 放大縮小、Tab／Shift+Tab 在表格儲存格間移動。
 *
 * 已知限制：不支援頁首、追蹤修訂、真正的齊行（撐開字距）；字型清單是常見的幾種，
 * 不是列出系統全部字型；表格儲存格裡只能有一個段落、不能再分段；
 * 剪貼簿只能貼純文字（格式不會保留）；圖片只能自己佔一整行、只認得 PNG／JPEG；
 * 頁碼只能整份文件開關，不能自訂格式或位置；開啟檔案的瀏覽視窗只列出資料夾跟 .docx／.odt 檔案。
 */
#define _GNU_SOURCE
#include "doc.h"
#include "layout.h"
#include "docx.h"
#include "odt.h"
#include <pomelo.h>
#include <pomelo-wl.h>
#include <pomelo-dnd.h>
#include <pomelo-widgets.h>
#include <limits.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <math.h>
#include <cairo/cairo-pdf.h>

#define PAGE_GAP 16.0        /* 頁與頁之間的視覺間隔（畫面像素，隨縮放調整） */
#define RULER_MARGIN 24       /* 畫布左右留白（畫面像素） */
#define MENU_H 30
#define TOOL_H 40
#define RULER_H 20
#define RULER_Y0 (MENU_H + TOOL_H)
#define TOP_H (MENU_H + TOOL_H + RULER_H)
#define STATUS_H 26           /* 最下面的狀態列：第幾頁、字數、檢視方式、縮放 */
#define MAX_UNDO 60
#define UNDO_BYTES (16 * 1024 * 1024)

/* ------------------------------------------------------------------ */
/* 全域狀態                                                              */
/* ------------------------------------------------------------------ */

static Document *doc;
static Layout *lo;
static int layout_dirty = 1;
static char cur_path[PATH_MAX];
static int cur_is_odt;        /* 目前檔案是不是 .odt（否則是 .docx）；新檔預設存成 .odt */
static int modified;
static Window win;
static PmWl *wl;   /* 原生 Wayland 的視窗（NULL＝X11） */
static int ww = 900, wh = 700;
static double scroll_y;        /* 捲動位置（畫面像素，連續版面座標：每一頁含間隔疊起來） */
static double view_scale = 1.0;   /* pt -> 畫面像素 的比例，開檔或縮放時重算 */
static int zoom_locked;         /* 使用者自己用 Ctrl+滾輪／+- 調過縮放後，視窗大小改變就不要再自動套用「符合寬度」 */
/* 檢視方式（狀態列右邊）：頁寬＝跟視窗一樣寬、單頁＝整頁放得下、多頁＝兩頁並排。沒有自己調過縮放時，
 * 縮放跟著視窗大小用這個算（見 layout_view_scale） */
enum { VIEW_WIDTH, VIEW_PAGE, VIEW_MULTI };
static int view_mode = VIEW_WIDTH;

static Pos cursor_pos, sel_anchor;
static int has_sel;
static long cursor_blink_at;
static int cursor_visible = 1;
static int want_x_valid;        /* 上下移動游標時，記住原本的 x 座標 */
static double want_x;

static Run typing_style;        /* 游標處、還沒選取任何文字時，下一個打的字要用的樣式 */

static Document *undo_stack[MAX_UNDO], *redo_stack[MAX_UNDO];
static Pos undo_cursor[MAX_UNDO], redo_cursor[MAX_UNDO];
static int nundo, nredo;
static size_t undo_bytes;
static long last_edit_ms;
static int last_edit_kind;      /* 1 打字、2 刪除、0 其他 */

static char *clipbuf;
static Atom a_delete, a_utf8, a_targets, a_clipboard, a_paste, a_netname;
static Rect menu_r[5];
static int hover_menu = -1;
static int drag_sel;
static int outline_open;        /* 左邊的大綱面板開著嗎 */
static int ruler_drag;          /* 尺標上拖曳邊界：0 沒有、1 拖左邊界、2 拖右邊界 */
#define OUTLINE_W 200

static cairo_surface_t *(*app_icon)(int);

static void rebuild_layout(void);
static void set_title(void);
static void scroll_to_cursor(void);
static void layout_view_scale(void);

/* ------------------------------------------------------------------ */
/* 小工具                                                               */
/* ------------------------------------------------------------------ */

static long now_ms(void) { return pm_now_ms(); }

static int is_odt_path(const char *path)
{
	const char *ext = strrchr(path, '.');
	return ext && !strcasecmp(ext, ".odt");
}

/* ------------------------------------------------------------------ */
/* undo／redo：整份文件快照（文件通常不大，複製整份最簡單也最不容易出錯）        */
/* ------------------------------------------------------------------ */

static void undo_drop_oldest(Document **stack, int *n, size_t *bytes)
{
	if (bytes) *bytes -= doc_size_estimate(stack[0]);
	doc_free(stack[0]);
	memmove(stack, stack + 1, (size_t)(*n - 1) * sizeof(Document *));
	(*n)--;
}

static void clear_redo(void)
{
	for (int i = 0; i < nredo; i++) doc_free(redo_stack[i]);
	nredo = 0;
}

/* 在真正修改文件「之前」呼叫：把目前的狀態存進復原堆疊。kind 用來把連續同類型的編輯
 * （例如連續打字）合併成一步復原，跟 pomelo-edit 的 push_undo 同一個想法。 */
static void push_undo(int kind)
{
	long t = now_ms();
	if (kind && kind == last_edit_kind && t - last_edit_ms < 800 && nundo) { last_edit_ms = t; return; }
	last_edit_kind = kind;
	last_edit_ms = t;
	if (nundo == MAX_UNDO) undo_drop_oldest(undo_stack, &nundo, &undo_bytes);
	undo_stack[nundo] = doc_clone(doc);
	undo_cursor[nundo] = cursor_pos;
	undo_bytes += doc_size_estimate(undo_stack[nundo]);
	nundo++;
	while (undo_bytes > UNDO_BYTES && nundo > 1) undo_drop_oldest(undo_stack, &nundo, &undo_bytes);
	clear_redo();
}

static void do_undo(void)
{
	if (!nundo) return;
	if (nredo == MAX_UNDO) undo_drop_oldest(redo_stack, &nredo, NULL);
	redo_stack[nredo] = doc_clone(doc);
	redo_cursor[nredo] = cursor_pos;
	nredo++;
	doc_free(doc);
	doc = undo_stack[--nundo];
	cursor_pos = doc_pos_clamp(doc, undo_cursor[nundo]);
	undo_bytes -= doc_size_estimate(doc);
	has_sel = 0;
	last_edit_kind = 0;
	layout_dirty = 1;
	modified = 1;
}

static void do_redo(void)
{
	if (!nredo) return;
	if (nundo == MAX_UNDO) undo_drop_oldest(undo_stack, &nundo, &undo_bytes);
	undo_stack[nundo] = doc_clone(doc);
	undo_cursor[nundo] = cursor_pos;
	undo_bytes += doc_size_estimate(undo_stack[nundo]);
	nundo++;
	doc_free(doc);
	nredo--;
	doc = redo_stack[nredo];
	cursor_pos = doc_pos_clamp(doc, redo_cursor[nredo]);
	has_sel = 0;
	last_edit_kind = 0;
	layout_dirty = 1;
	modified = 1;
}

/* ------------------------------------------------------------------ */
/* 版面與捲動座標                                                        */
/* ------------------------------------------------------------------ */

static void rebuild_layout(void)
{
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_A8, 1, 1);
	cairo_t *cr = cairo_create(s);
	if (lo) layout_free(lo);
	lo = layout_build(cr, doc);
	cairo_destroy(cr);
	cairo_surface_destroy(s);
	layout_dirty = 0;
}

static double canvas_x0(void) { return outline_open ? OUTLINE_W : 0; }
static double canvas_h(void) { return wh - TOP_H - STATUS_H; }   /* 頁面區的高度（工具列與狀態列中間） */

/* 一排放幾頁：「多頁」或自己縮小到放得下兩頁以上時並排，跟一般文書軟體一樣；頁寬、單頁一排一頁 */
static int page_cols(void)
{
	if (view_mode != VIEW_MULTI && !zoom_locked) return 1;
	double avail = ww - canvas_x0() - 2 * RULER_MARGIN, pw = doc->page_w * view_scale;
	int c = (int)((avail + PAGE_GAP + 0.5) / (pw + PAGE_GAP));
	return CLAMP(c, 1, MAX(1, lo ? lo->npage : 1));
}

static double page_left_px(int page)
{
	int cols = page_cols();
	double pw = doc->page_w * view_scale, total = cols * pw + (cols - 1) * PAGE_GAP;
	return canvas_x0() + (ww - canvas_x0() - total) / 2.0 + (page % cols) * (pw + PAGE_GAP);
}

static double page_top_px(int page) { return (double)(page / page_cols()) * (doc->page_h * view_scale + PAGE_GAP); }
static double content_height_px(void)
{
	int cols = page_cols(), rows = (lo->npage + cols - 1) / cols;
	return rows * (doc->page_h * view_scale + PAGE_GAP) - PAGE_GAP;
}

/* 螢幕座標（視窗內，y 已經扣掉工具列高度）轉成「第幾頁＋頁面座標系(pt)」；頁面外也會夾回最近的頁 */
static void screen_to_page(double sx, double sy, int *page, double *px, double *py)
{
	double cy = sy + scroll_y, pw = doc->page_w * view_scale;
	int cols = page_cols(), rows = (lo->npage + cols - 1) / cols;
	int row = (int)floor(cy / (doc->page_h * view_scale + PAGE_GAP));
	row = CLAMP(row, 0, rows - 1);
	int col = (int)floor((sx - page_left_px(row * cols) + PAGE_GAP / 2) / (pw + PAGE_GAP));
	col = CLAMP(col, 0, cols - 1);
	int p = MIN(row * cols + col, lo->npage - 1);
	*page = p;
	*px = (sx - page_left_px(p)) / view_scale;
	*py = (cy - page_top_px(p)) / view_scale;
}

static void page_to_screen(int page, double px, double py, double *sx, double *sy)
{
	*sx = page_left_px(page) + px * view_scale;
	*sy = page_top_px(page) + py * view_scale - scroll_y;
}

static int cursor_page(void)
{
	int page; double x, y, h;
	return layout_pos_xy(doc, lo, cursor_pos, &page, &x, &y, &h) ? page : 0;
}

static void scroll_to_cursor(void)
{
	/* 剛打的字、剛按的 Enter 要先排好版才找得到游標在哪一頁（不然打字時畫面不會跟著捲） */
	if (layout_dirty) { layout_view_scale(); rebuild_layout(); }
	int page; double x, y, h;
	if (!layout_pos_xy(doc, lo, cursor_pos, &page, &x, &y, &h)) return;
	double top = page_top_px(page) + y * view_scale;
	double bot = top + h * view_scale;
	if (top < scroll_y) scroll_y = MAX(0, top - 20);
	else if (bot > scroll_y + canvas_h()) scroll_y = bot - canvas_h() + 20;
}

/* ------------------------------------------------------------------ */
/* 文件編輯動作（統一在這裡 push_undo，呼叫端不用自己管）                      */
/* ------------------------------------------------------------------ */

static void set_modified(void) { modified = 1; layout_dirty = 1; }

/* doc_delete_range 假設範圍兩端的段落「留下來、被接到別的段落」；圖片沒有文字可以這樣接，
 * 如果範圍的起點剛好在圖片上，要先把圖片整塊刪掉、範圍往後移一個區塊，再交給 doc_delete_range
 * 處理剩下的部分（範圍終點在圖片上則不用特別處理，doc_delete_range 本來就會連它一起刪掉）。 */
static Pos delete_range_safe(Pos a, Pos b)
{
	if (doc_pos_cmp(doc, a, b) > 0) { Pos t = a; a = b; b = t; }
	while (doc_pos_cmp(doc, a, b) < 0 && doc->block[a.block].kind == BLOCK_IMAGE) {
		doc_delete_block(doc, a.block);
		if (b.block > a.block) b.block--;
		a = doc_pos_clamp(doc, a);
	}
	if (doc_pos_cmp(doc, a, b) == 0) return a;
	return doc_delete_range(doc, a, b);
}

/* 游標直接停在一張圖片上（block 本身就是 BLOCK_IMAGE）時，Backspace／Delete 都直接刪掉那張圖片。 */
static void delete_image_at(int bi)
{
	push_undo(2);
	doc_delete_block(doc, bi);
	int nb = bi < doc->nblock ? bi : doc->nblock - 1;
	cursor_pos = doc_pos_clamp(doc, (Pos){ nb, 0, 0, 0 });
	has_sel = 0;
	set_modified();
	want_x_valid = 0;
}

static void type_text(const char *s, size_t n)
{
	push_undo(1);
	if (has_sel) { cursor_pos = delete_range_safe(sel_anchor, cursor_pos); has_sel = 0; }
	if (doc->block[cursor_pos.block].kind == BLOCK_IMAGE) { set_modified(); return; }   /* 游標停在圖片上：打字先不處理 */
	cursor_pos = doc_insert_text(doc, cursor_pos, s, n, &typing_style);
	set_modified();
	want_x_valid = 0;
}

static void delete_backward(void)
{
	if (has_sel) { push_undo(0); cursor_pos = delete_range_safe(sel_anchor, cursor_pos); has_sel = 0; set_modified(); return; }
	Pos a = cursor_pos;
	if (doc->block[a.block].kind == BLOCK_IMAGE) { delete_image_at(a.block); return; }
	int nb, nr, nc;
	if (a.offset > 0) {
		char *text = para_text(doc_para_at(doc, a));
		int off = a.offset - 1;
		while (off > 0 && ((unsigned char)text[off] & 0xc0) == 0x80) off--;   /* 退到 UTF-8 字元開頭，不要切到一半 */
		free(text);
		push_undo(2);
		Pos b = a; b.offset = off;
		cursor_pos = doc_delete_range(doc, b, a);
	} else if (doc_prev_unit(doc, a.block, a.row, a.col, &nb, &nr, &nc)) {
		if (doc->block[nb].kind == BLOCK_IMAGE) { delete_image_at(nb); return; }
		push_undo(2);
		Pos b = doc_unit_end(doc, nb, nr, nc);
		cursor_pos = doc_delete_range(doc, b, a);
	} else return;
	set_modified();
	want_x_valid = 0;
}

static void delete_forward(void)
{
	if (has_sel) { push_undo(0); cursor_pos = delete_range_safe(sel_anchor, cursor_pos); has_sel = 0; set_modified(); return; }
	Pos a = cursor_pos;
	if (doc->block[a.block].kind == BLOCK_IMAGE) { delete_image_at(a.block); return; }
	const Para *p = doc_para_at_c(doc, a);
	int len = p ? (int)para_text_len(p) : 0;
	int nb, nr, nc;
	if (a.offset < len) {
		char *text = para_text(p);
		int off = a.offset + 1;
		while (off < len && ((unsigned char)text[off] & 0xc0) == 0x80) off++;
		free(text);
		push_undo(2);
		Pos b = a; b.offset = off;
		cursor_pos = doc_delete_range(doc, a, b);
	} else if (doc_next_unit(doc, a.block, a.row, a.col, &nb, &nr, &nc)) {
		if (doc->block[nb].kind == BLOCK_IMAGE) { delete_image_at(nb); return; }
		push_undo(2);
		Pos b = doc_unit_start(doc, nb, nr, nc);
		cursor_pos = doc_delete_range(doc, a, b);
	} else return;
	set_modified();
	want_x_valid = 0;
}

static void insert_newline(void)
{
	push_undo(0);
	if (has_sel) { cursor_pos = delete_range_safe(sel_anchor, cursor_pos); has_sel = 0; }
	cursor_pos = doc_insert_para_break(doc, cursor_pos);
	set_modified();
	want_x_valid = 0;
}

static void insert_tab_or_move(void)
{
	Block *b = &doc->block[cursor_pos.block];
	if (b->kind == BLOCK_TABLE) {
		int nb, nr, nc;
		if (doc_next_unit(doc, cursor_pos.block, cursor_pos.row, cursor_pos.col, &nb, &nr, &nc)) {
			Pos np = doc_unit_start(doc, nb, nr, nc);
			if (doc->block[nb].kind == BLOCK_TABLE) { cursor_pos = np; has_sel = 1; sel_anchor = np; cursor_pos = doc_unit_end(doc, nb, nr, nc); }
			else cursor_pos = np;
		}
		return;
	}
	push_undo(1);
	if (has_sel) { cursor_pos = delete_range_safe(sel_anchor, cursor_pos); has_sel = 0; }
	cursor_pos = doc_insert_text(doc, cursor_pos, "\t", 1, &typing_style);
	set_modified();
}

/* 套用字元樣式到選取範圍；沒有選取時改成「接下來要打的字」的樣式 */
static void apply_char_style(void (*fn)(Run *, void *), void *ctx)
{
	if (has_sel) {
		push_undo(0);
		Pos a = sel_anchor, b = cursor_pos;
		if (doc_pos_cmp(doc, a, b) > 0) { Pos t = a; a = b; b = t; }
		if (a.block == b.block && a.row == b.row && a.col == b.col) {
			Para *p = doc_para_at(doc, a);
			if (p) para_apply_style(p, a.offset, b.offset, fn, ctx);
		} else {
			/* 跨多個段落：每個涵蓋到的段落各自套用（已知限制：跨表格儲存格的選取只處理頭尾兩格） */
			int bi = a.block, bj = b.block;
			for (int i = bi; i <= bj; i++) {
				if (doc->block[i].kind != BLOCK_PARA) continue;
				Para *p = &doc->block[i].para;
				int s = i == bi ? a.offset : 0;
				int e = i == bj ? b.offset : (int)para_text_len(p);
				para_apply_style(p, s, e, fn, ctx);
			}
		}
		set_modified();
	} else {
		fn(&typing_style, ctx);
	}
}

static void fn_toggle_bold(Run *r, void *ctx) { (void)ctx; r->bold = !r->bold; }
static void fn_toggle_italic(Run *r, void *ctx) { (void)ctx; r->italic = !r->italic; }
static void fn_toggle_underline(Run *r, void *ctx) { (void)ctx; r->underline = !r->underline; }
static void fn_set_font(Run *r, void *ctx) { snprintf(r->font, sizeof r->font, "%s", (const char *)ctx); }
static void fn_set_size(Run *r, void *ctx) { r->size = *(double *)ctx; }
static void fn_set_color(Run *r, void *ctx) { r->color = *(unsigned *)ctx; }

static void set_para_align(Align align)
{
	push_undo(0);
	int bi = cursor_pos.block, bj = has_sel ? sel_anchor.block : cursor_pos.block;
	if (bi > bj) { int t = bi; bi = bj; bj = t; }
	for (int i = bi; i <= bj; i++) if (doc->block[i].kind == BLOCK_PARA) doc->block[i].para.align = align;
	set_modified();
}

static void set_para_style(PStyle style)
{
	push_undo(0);
	int bi = cursor_pos.block, bj = has_sel ? sel_anchor.block : cursor_pos.block;
	if (bi > bj) { int t = bi; bi = bj; bj = t; }
	for (int i = bi; i <= bj; i++) if (doc->block[i].kind == BLOCK_PARA) doc->block[i].para.style = style;
	set_modified();
}

static void toggle_list(ListType lt)
{
	push_undo(0);
	int bi = cursor_pos.block, bj = has_sel ? sel_anchor.block : cursor_pos.block;
	if (bi > bj) { int t = bi; bi = bj; bj = t; }
	/* 如果整段範圍已經是這種清單，取消；否則套用（新開一個清單群組） */
	int all_on = 1;
	for (int i = bi; i <= bj; i++) if (doc->block[i].kind == BLOCK_PARA && doc->block[i].para.list_type != lt) all_on = 0;
	int id = all_on ? 0 : doc_new_list_id(doc);
	for (int i = bi; i <= bj; i++) {
		if (doc->block[i].kind != BLOCK_PARA) continue;
		doc->block[i].para.list_type = all_on ? LIST_NONE : lt;
		doc->block[i].para.list_id = all_on ? 0 : id;
		if (!all_on && doc->block[i].para.list_level == 0 && lt != LIST_NONE) doc->block[i].para.list_level = 0;
	}
	set_modified();
}

static void insert_table_here(int nrow, int ncol)
{
	push_undo(0);
	int bi = doc_insert_table(doc, cursor_pos.block, nrow, ncol);
	cursor_pos = doc_unit_start(doc, bi, 0, 0);
	has_sel = 0;
	set_modified();
}

/* ------------------------------------------------------------------ */
/* 剪貼簿                                                               */
/* ------------------------------------------------------------------ */

static char *selection_text(void)
{
	if (!has_sel) return NULL;
	Pos a = sel_anchor, b = cursor_pos;
	if (doc_pos_cmp(doc, a, b) > 0) { Pos t = a; a = b; b = t; }
	if (a.block == b.block && a.row == b.row && a.col == b.col) {
		const Para *pa = doc_para_at(doc, a);
		if (!pa) return NULL;
		char *t = para_text(pa);
		size_t total = strlen(t);
		size_t s = MIN((size_t)a.offset, total), e = MIN((size_t)b.offset, total);
		char *out = malloc(e - s + 1);
		memcpy(out, t + s, e - s);
		out[e - s] = 0;
		free(t);
		return out;
	}
	/* 跨段落：把涵蓋到的段落文字用換行接起來（已知限制：跨儲存格只接頭尾兩格間的頂層段落） */
	size_t cap = 256, len = 0;
	char *out = malloc(cap);
	out[0] = 0;
	for (int i = a.block; i <= b.block; i++) {
		if (doc->block[i].kind != BLOCK_PARA) continue;
		char *t = para_text(&doc->block[i].para);
		size_t tl = strlen(t);
		size_t s = i == a.block ? MIN((size_t)a.offset, tl) : 0;
		size_t e = i == b.block ? MIN((size_t)b.offset, tl) : tl;
		size_t seg = e > s ? e - s : 0;
		if (len + seg + 2 > cap) { cap = (len + seg + 2) * 2; out = realloc(out, cap); }
		memcpy(out + len, t + s, seg);
		len += seg;
		if (i < b.block) out[len++] = '\n';
		out[len] = 0;
		free(t);
	}
	return out;
}

static void copy_sel(int cut)
{
	char *t = selection_text();
	if (!t) return;
	free(clipbuf);
	clipbuf = t;
	if (wl) pmwl_clip_set(clipbuf); else XSetSelectionOwner(dpy, a_clipboard, win, CurrentTime);
	if (cut) { push_undo(0); cursor_pos = delete_range_safe(sel_anchor, cursor_pos); has_sel = 0; set_modified(); }
}

static void on_selrequest(XSelectionRequestEvent *e)
{
	XSelectionEvent r = { .type = SelectionNotify, .display = e->display, .requestor = e->requestor,
	                      .selection = e->selection, .target = e->target, .property = None, .time = e->time };
	const char *data = clipbuf ? clipbuf : "";
	Atom prop = e->property ? e->property : e->target;
	if (e->target == a_targets) {
		Atom ts[] = { a_targets, a_utf8, XA_STRING };
		XChangeProperty(dpy, e->requestor, prop, XA_ATOM, 32, PropModeReplace, (unsigned char *)ts, 3);
		r.property = prop;
	} else if (e->target == a_utf8 || e->target == XA_STRING) {
		XChangeProperty(dpy, e->requestor, prop, e->target, 8, PropModeReplace, (const unsigned char *)data, (int)strlen(data));
		r.property = prop;
	}
	XSendEvent(dpy, e->requestor, False, 0, (XEvent *)&r);
}

static void insert_pasted(const char *data, size_t n);
static void paste(void)
{
	if (wl) { char *t = pmwl_clip_get(); if (t) { insert_pasted(t, strlen(t)); free(t); } return; }
	XConvertSelection(dpy, a_clipboard, a_utf8, a_paste, win, CurrentTime);
}

static void insert_pasted(const char *data, size_t n)
{
	push_undo(0);
	if (has_sel) { cursor_pos = delete_range_safe(sel_anchor, cursor_pos); has_sel = 0; }
	/* 貼上的文字裡如果有換行，當成好幾段分開插入 */
	size_t i = 0;
	while (i < n) {
		size_t j = i;
		while (j < n && data[j] != '\n') j++;
		size_t seg = j - i;
		if (seg && data[i + seg - 1] == '\r') seg--;
		if (seg) cursor_pos = doc_insert_text(doc, cursor_pos, data + i, seg, &typing_style);
		if (j < n) cursor_pos = doc_insert_para_break(doc, cursor_pos);
		i = j + 1;
	}
	set_modified();
}

static void on_selnotify(XSelectionEvent *e)
{
	if (e->property == None) return;
	Atom type;
	int fmt;
	unsigned long n, extra;
	unsigned char *d = NULL;
	if (XGetWindowProperty(dpy, win, a_paste, 0, 1 << 22, True, AnyPropertyType, &type, &fmt, &n, &extra, &d) == Success && d) {
		if (n) insert_pasted((char *)d, n);
		XFree(d);
	}
}

/* ------------------------------------------------------------------ */
/* 檔案 I/O                                                             */
/* ------------------------------------------------------------------ */

static void load_file(const char *path)
{
	Document *nd = is_odt_path(path) ? odt_read(path) : docx_read(path);
	if (!nd) return;
	doc_free(doc);
	doc = nd;
	cur_is_odt = is_odt_path(path);
	snprintf(cur_path, sizeof cur_path, "%s", path);
	cursor_pos = doc_start(doc);
	has_sel = 0;
	modified = 0;
	layout_dirty = 1;
	scroll_y = 0;
	for (int i = 0; i < nundo; i++) doc_free(undo_stack[i]);
	nundo = 0;
	undo_bytes = 0;
	clear_redo();
	set_title();
}

static int save_file(const char *path, int as_odt)
{
	int ok = as_odt ? odt_write(doc, path) : docx_write(doc, path);
	if (ok) { snprintf(cur_path, sizeof cur_path, "%s", path); cur_is_odt = as_odt; modified = 0; set_title(); }
	return ok;
}

static void new_document(void)
{
	doc_free(doc);
	doc = doc_new();
	cur_path[0] = 0;
	cur_is_odt = 1;
	cursor_pos = doc_start(doc);
	has_sel = 0;
	modified = 0;
	layout_dirty = 1;
	scroll_y = 0;
	for (int i = 0; i < nundo; i++) doc_free(undo_stack[i]);
	nundo = 0;
	undo_bytes = 0;
	clear_redo();
	set_title();
}

static void set_title(void)
{
	char t[PATH_MAX + 32];
	const char *name = cur_path[0] ? strrchr(cur_path, '/') : NULL;
	name = name ? name + 1 : (cur_path[0] ? cur_path : _("未命名文件"));
	snprintf(t, sizeof t, "%s%s — %s", modified ? "• " : "", name, _("文書處理器"));
	if (wl) { pmwl_set_title(wl, t); return; }
	XStoreName(dpy, win, t);
	XChangeProperty(dpy, win, a_netname, a_utf8, 8, PropModeReplace, (unsigned char *)t, (int)strlen(t));
}

/* ------------------------------------------------------------------ */
/* 小對話框（借用 libpomelo 的設定元件開一個小視窗，跟 pomelo-remote 同一招） */
/* ------------------------------------------------------------------ */

static PmUI *dlg_ui;
static int dlg_result;
static char dlg_path[PATH_MAX];
static int dlg_table_rows = 2, dlg_table_cols = 2;
static unsigned dlg_color;
static double dlg_size;
static char dlg_printers[16][128];
static int dlg_nprinters, dlg_printer_sel;
static double dlg_copies = 1;
static char dlg_message[256];
static int dlg_mode;   /* 2=插入表格 3=顏色 4=字型大小 7=列印 8=訊息 9=版面設定 10=尋找取代
                        * （開啟、另存新檔、插入圖片改用 pm_file_dialog，確認關閉改用 pm_ask）*/

/* ---- 版面設定 ---- */
static int dlg_paper_size;        /* 0=A4 1=US Letter 2=US Legal 3=自訂 */
static int dlg_landscape;
static double dlg_custom_w = 21.0, dlg_custom_h = 29.7;             /* 公分，只有「自訂」會用到 */
static double dlg_margin_t, dlg_margin_b, dlg_margin_l, dlg_margin_r;   /* 公分 */

/* ---- 列印預覽 ---- */
static cairo_surface_t *preview_surf;
static int preview_page, preview_w, preview_h;

/* ---- 尋找與取代 ---- */
static char dlg_find[256] = "";
static char dlg_replace[256] = "";
static int dlg_find_case;
static int dlg_find_replace_mode;   /* 0=只有尋找 1=尋找與取代 */
static char dlg_find_status[128];

/* ------------------------------------------------------------------ */
/* 列印預覽                                                              */
/* ------------------------------------------------------------------ */

static void build_preview(void)
{
	if (preview_surf) { cairo_surface_destroy(preview_surf); preview_surf = NULL; }
	if (layout_dirty) { layout_view_scale(); rebuild_layout(); }
	if (preview_page >= lo->npage) preview_page = lo->npage - 1;
	if (preview_page < 0) preview_page = 0;
	double scale = 300.0 / doc->page_h;
	if (doc->page_w * scale > 260.0) scale = 260.0 / doc->page_w;
	preview_w = (int)(doc->page_w * scale + 0.5);
	preview_h = (int)(doc->page_h * scale + 0.5);
	preview_surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, preview_w, preview_h);
	cairo_t *cr = cairo_create(preview_surf);
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_paint(cr);
	cairo_scale(cr, scale, scale);
	layout_draw_page(cr, doc, lo, preview_page, NULL, NULL, NULL, 0, 0x1a1a1a, 0xffffff, 0xffffff, 0xcccccc);
	cairo_destroy(cr);
}

/* ------------------------------------------------------------------ */
/* 尋找與取代                                                            */
/* ------------------------------------------------------------------ */

static int text_find(const char *hay, int n, int from, const char *needle, int needle_len, int case_sens)
{
	if (!needle_len || needle_len > n) return -1;
	for (int i = from; i + needle_len <= n; i++)
		if (case_sens ? !memcmp(hay + i, needle, (size_t)needle_len) : !strncasecmp(hay + i, needle, (size_t)needle_len)) return i;
	return -1;
}

/* 在一個段落的文字裡找 dlg_find；limit_lo／limit_hi 限制搜尋範圍（-1 表示不限制那一邊）；
 * dir<0 時找範圍內「最後一個」符合的位置（往回找用）。找不到回傳 -1。 */
static int search_para(const Para *p, int dir, int limit_lo, int limit_hi)
{
	char *text = para_text(p);
	int n = (int)strlen(text);
	int lo = limit_lo < 0 ? 0 : limit_lo;
	int hi = limit_hi < 0 ? n : limit_hi;
	int needle_len = (int)strlen(dlg_find);
	int result;
	if (dir > 0) {
		result = text_find(text, hi, lo, dlg_find, needle_len, dlg_find_case);
	} else {
		result = -1;
		int scan = lo;
		while (scan + needle_len <= hi) {
			int m = text_find(text, hi, scan, dlg_find, needle_len, dlg_find_case);
			if (m < 0) break;
			result = m;
			scan = m + 1;
		}
	}
	free(text);
	return result;
}

/* 從 pos 開始往 dir 方向找下一個符合的地方，找不到就繞到文件另一端繼續找（跳過圖片，
 * 表格儲存格當作一般可編輯單位）；找到回傳 1 並把範圍存進 out_a／out_b。 */
static int doc_search(Pos pos, int dir, Pos *out_a, Pos *out_b)
{
	if (!dlg_find[0]) return 0;
	int needle_len = (int)strlen(dlg_find);
	Pos p = pos;
	int nb, nr, nc;
	for (;;) {
		const Para *para = doc_para_at_c(doc, p);
		if (para) {
			int found = search_para(para, dir, dir > 0 ? p.offset : -1, dir > 0 ? -1 : p.offset);
			if (found >= 0) { *out_a = (Pos){ p.block, p.row, p.col, found }; *out_b = (Pos){ p.block, p.row, p.col, found + needle_len }; return 1; }
		}
		int ok = dir > 0 ? doc_next_unit(doc, p.block, p.row, p.col, &nb, &nr, &nc) : doc_prev_unit(doc, p.block, p.row, p.col, &nb, &nr, &nc);
		if (!ok) break;
		p = (Pos){ nb, nr, nc, 0 };
	}
	p = dir > 0 ? doc_start(doc) : doc_end(doc);
	for (;;) {
		int same = p.block == pos.block && p.row == pos.row && p.col == pos.col;
		const Para *para = doc_para_at_c(doc, p);
		if (para) {
			int lo = dir > 0 ? -1 : (same ? pos.offset : -1);
			int hi = dir > 0 ? (same ? pos.offset : -1) : -1;
			int found = search_para(para, dir, lo, hi);
			if (found >= 0) { *out_a = (Pos){ p.block, p.row, p.col, found }; *out_b = (Pos){ p.block, p.row, p.col, found + needle_len }; return 1; }
		}
		if (same) break;
		int ok = dir > 0 ? doc_next_unit(doc, p.block, p.row, p.col, &nb, &nr, &nc) : doc_prev_unit(doc, p.block, p.row, p.col, &nb, &nr, &nc);
		if (!ok) break;
		p = (Pos){ nb, nr, nc, 0 };
	}
	return 0;
}

/* 不會繞回開頭的往前搜尋，只給「全部取代」用（避免取代的文字又被自己找到造成無窮迴圈）。 */
static int doc_search_forward_nowrap(Pos pos, Pos *out_a, Pos *out_b)
{
	if (!dlg_find[0]) return 0;
	int needle_len = (int)strlen(dlg_find);
	Pos p = pos;
	for (;;) {
		const Para *para = doc_para_at_c(doc, p);
		if (para) {
			int found = search_para(para, 1, p.offset, -1);
			if (found >= 0) { *out_a = (Pos){ p.block, p.row, p.col, found }; *out_b = (Pos){ p.block, p.row, p.col, found + needle_len }; return 1; }
		}
		int nb, nr, nc;
		if (!doc_next_unit(doc, p.block, p.row, p.col, &nb, &nr, &nc)) return 0;
		p = (Pos){ nb, nr, nc, 0 };
	}
}

static int do_find_next(int dir)
{
	Pos from = cursor_pos;
	if (has_sel) {
		Pos sa = sel_anchor, sb = cursor_pos;
		if (doc_pos_cmp(doc, sa, sb) > 0) { Pos t = sa; sa = sb; sb = t; }
		from = dir > 0 ? sb : sa;
	}
	Pos a, b;
	if (!doc_search(from, dir, &a, &b)) { snprintf(dlg_find_status, sizeof dlg_find_status, "%s", _("找不到")); return 0; }
	sel_anchor = a; cursor_pos = b; has_sel = 1;
	dlg_find_status[0] = 0;
	return 1;
}

static void do_replace_one(void)
{
	if (!has_sel) { do_find_next(1); return; }
	Pos sa = sel_anchor, sb = cursor_pos;
	if (doc_pos_cmp(doc, sa, sb) > 0) { Pos t = sa; sa = sb; sb = t; }
	const Para *p = (sa.block == sb.block && sa.row == sb.row && sa.col == sb.col) ? doc_para_at_c(doc, sa) : NULL;
	int len = (int)strlen(dlg_find);
	int match = 0;
	if (p) {
		char *text = para_text(p);
		match = (int)strlen(text) >= sb.offset && sb.offset - sa.offset == len &&
		        (dlg_find_case ? !memcmp(text + sa.offset, dlg_find, (size_t)len) : !strncasecmp(text + sa.offset, dlg_find, (size_t)len));
		free(text);
	}
	if (!match) { do_find_next(1); return; }
	push_undo(0);
	cursor_pos = doc_delete_range(doc, sa, sb);
	cursor_pos = doc_insert_text(doc, cursor_pos, dlg_replace, strlen(dlg_replace), NULL);
	has_sel = 0;
	set_modified();
	do_find_next(1);
}

static int do_replace_all(void)
{
	if (!dlg_find[0]) return 0;
	int count = 0, first = 1;
	Pos pos = doc_start(doc), a, b;
	while (doc_search_forward_nowrap(pos, &a, &b)) {
		if (first) { push_undo(0); first = 0; }
		pos = doc_delete_range(doc, a, b);
		pos = doc_insert_text(doc, pos, dlg_replace, strlen(dlg_replace), NULL);
		count++;
	}
	if (count) { cursor_pos = pos; has_sel = 0; set_modified(); }
	return count;
}

static void dlg_close(PmUI *u) { pm_ui_close(u); }

static void dlg_build(PmUI *u)
{
	dlg_ui = u;
	/* Escape 一律直接關閉對話框：放在最前面攔截，不然像「尋找」這種輸入框還留著焦點、
	 * 又按了不會關閉對話框的按鈕（找下一個…）時，entry_impl 會先把 Escape 吃掉變成「輸入框失焦」，
	 * 對話框本身永遠收不到、關不掉。 */
	if (u->key == XK_Escape) { u->key = 0; dlg_result = 0; dlg_close(u); return; }
	if (dlg_mode == 2) {
		pm_ui_title(u, _("插入表格"), NULL);
		pm_ui_group(u, NULL);
		double rows = dlg_table_rows, cols = dlg_table_cols;
		if (pm_ui_slider(u, _("列數"), &rows, 1, 20)) dlg_table_rows = (int)rows;
		if (pm_ui_slider(u, _("欄數"), &cols, 1, 10)) dlg_table_cols = (int)cols;
		pm_ui_end(u);
		pm_ui_group(u, NULL);
		if (pm_ui_button(u, "", _("插入"))) { dlg_result = 1; dlg_close(u); }
		if (pm_ui_button(u, "", _("取消"))) { dlg_result = 0; dlg_close(u); }
		pm_ui_end(u);
	} else if (dlg_mode == 3) {
		pm_ui_title(u, _("文字顏色"), NULL);
		pm_ui_group(u, NULL);
		static const unsigned swatches[] = { 0x000000, 0x5a5a5e, 0xd70015, 0xff9500, 0xffcc00, 0x34c759, 0x0a84ff, 0x5e5ce6, 0xaf52de, 0xffffff };
		int idx = 0;
		for (size_t i = 0; i < LEN(swatches); i++) if (swatches[i] == dlg_color) idx = (int)i;
		if (pm_ui_swatches(u, _("顏色"), swatches, (int)LEN(swatches), &idx)) { dlg_color = swatches[idx]; dlg_result = 1; dlg_close(u); }
		pm_ui_end(u);
		pm_ui_group(u, NULL);
		if (pm_ui_button(u, "", _("取消"))) { dlg_result = 0; dlg_close(u); }
		pm_ui_end(u);
	} else if (dlg_mode == 4) {
		pm_ui_title(u, _("字型大小"), NULL);
		pm_ui_group(u, NULL);
		if (pm_ui_slider(u, _("大小（點）"), &dlg_size, 6, 96)) {}
		pm_ui_end(u);
		pm_ui_group(u, NULL);
		if (pm_ui_button(u, "", _("確定"))) { dlg_result = 1; dlg_close(u); }
		if (pm_ui_button(u, "", _("取消"))) { dlg_result = 0; dlg_close(u); }
		pm_ui_end(u);
	} else if (dlg_mode == 7) {
		pm_ui_title(u, _("列印"), NULL);
		pm_ui_group(u, NULL);
		pm_ui_image(u, preview_surf, preview_w, preview_h);
		pm_ui_end(u);
		if (lo && lo->npage > 1) {
			pm_ui_group(u, NULL);
			if (pm_ui_button(u, "", "◀")) { preview_page--; build_preview(); }
			char pbuf[32];
			snprintf(pbuf, sizeof pbuf, "%d / %d", preview_page + 1, lo->npage);
			pm_ui_info(u, NULL, pbuf);
			if (pm_ui_button(u, "", "▶")) { preview_page++; build_preview(); }
			pm_ui_end(u);
		}
		pm_ui_group(u, NULL);
		if (dlg_nprinters > 0) {
			static const char *popts[16];
			for (int i = 0; i < dlg_nprinters; i++) popts[i] = dlg_printers[i];
			pm_ui_choice(u, _("印表機"), popts, dlg_nprinters, &dlg_printer_sel);
			pm_ui_slider(u, _("份數"), &dlg_copies, 1, 20);
		} else {
			pm_ui_note(u, _("找不到印表機（請到系統設定加入印表機）"));
		}
		pm_ui_end(u);
		pm_ui_group(u, NULL);
		if (dlg_nprinters > 0 && pm_ui_button(u, "", _("列印"))) { dlg_result = 1; dlg_close(u); }
		if (pm_ui_button(u, "", _("取消"))) { dlg_result = 0; dlg_close(u); }
		pm_ui_end(u);
	} else if (dlg_mode == 8) {
		pm_ui_title(u, _("訊息"), NULL);
		pm_ui_note(u, dlg_message);
		pm_ui_group(u, NULL);
		if (pm_ui_button(u, "", _("好"))) { dlg_result = 1; dlg_close(u); }
		pm_ui_end(u);
	} else if (dlg_mode == 9) {
		pm_ui_title(u, _("版面設定"), NULL);
		pm_ui_group(u, NULL);
		static const char *paper_opts[] = { "A4", "US Letter", "US Legal", N_("自訂") };
		pm_ui_choice(u, _("紙張大小"), paper_opts, 4, &dlg_paper_size);
		static const char *orient_opts[] = { N_("直向"), N_("橫向") };
		pm_ui_segment(u, _("方向"), orient_opts, 2, &dlg_landscape);
		if (dlg_paper_size == 3) {
			pm_ui_slider(u, _("寬度（公分）"), &dlg_custom_w, 5, 50);
			pm_ui_slider(u, _("高度（公分）"), &dlg_custom_h, 5, 50);
		}
		pm_ui_end(u);
		pm_ui_group(u, _("邊界（公分）"));
		pm_ui_slider(u, _("上"), &dlg_margin_t, 0, 5);
		pm_ui_slider(u, _("下"), &dlg_margin_b, 0, 5);
		pm_ui_slider(u, _("左"), &dlg_margin_l, 0, 5);
		pm_ui_slider(u, _("右"), &dlg_margin_r, 0, 5);
		pm_ui_end(u);
		pm_ui_group(u, NULL);
		if (pm_ui_button(u, "", _("確定"))) { dlg_result = 1; dlg_close(u); }
		if (pm_ui_button(u, "", _("取消"))) { dlg_result = 0; dlg_close(u); }
		pm_ui_end(u);
	} else {
		pm_ui_title(u, dlg_find_replace_mode ? _("取代") : _("尋找"), NULL);
		pm_ui_group(u, NULL);
		if (pm_ui_entry(u, _("尋找"), dlg_find, sizeof dlg_find, 0)) do_find_next(1);
		if (dlg_find_replace_mode) pm_ui_entry(u, _("取代為"), dlg_replace, sizeof dlg_replace, 0);
		pm_ui_switch(u, _("區分大小寫"), &dlg_find_case);
		pm_ui_end(u);
		if (dlg_find_status[0]) pm_ui_note(u, dlg_find_status);
		pm_ui_group(u, NULL);
		if (pm_ui_button(u, "", _("找上一個"))) do_find_next(-1);
		if (pm_ui_button(u, "", _("找下一個"))) do_find_next(1);
		pm_ui_end(u);
		if (dlg_find_replace_mode) {
			pm_ui_group(u, NULL);
			if (pm_ui_button(u, "", _("取代"))) do_replace_one();
			if (pm_ui_button(u, "", _("全部取代"))) {
				int n = do_replace_all();
				snprintf(dlg_find_status, sizeof dlg_find_status, _("已取代 %d 處"), n);
			}
			pm_ui_end(u);
		}
		pm_ui_group(u, NULL);
		if (pm_ui_button(u, "", _("關閉"))) { dlg_result = 0; dlg_close(u); }
		pm_ui_end(u);
	}
}

static int dlg_run(int mode, int h)
{
	dlg_mode = mode;
	dlg_result = 0;
	dlg_ui = NULL;
	pm_ui_run(_("文書處理器"), "pomelo-writer", 460, h, dlg_build, NULL, 0, app_icon);
	if (dlg_ui && dpy) { XDestroyWindow(dpy, dlg_ui->win); XFlush(dpy); }   /* 原生 Wayland 的視窗 pm_ui_run 自己收掉了 */
	return dlg_result;
}

static void say(const char *msg)
{
	snprintf(dlg_message, sizeof dlg_message, "%s", msg);
	dlg_run(8, 200);
}

/* 開啟／另存新檔對話窗一開始的資料夾：目前檔案所在的資料夾，還沒存過就是家目錄 */
static void start_dir(char *dir, size_t n)
{
	snprintf(dir, n, "%s", cur_path[0] ? cur_path : getenv("HOME") ? getenv("HOME") : "/");
	char *slash = cur_path[0] ? strrchr(dir, '/') : NULL;
	if (slash) slash == dir ? (void)(dir[1] = 0) : (void)(*slash = 0);
}

static void do_open_dialog(void)
{
	static const PmFileType types[] = {
		{ "Word / OpenDocument (.docx, .odt)", "docx;odt" }, { "Word (.docx)", "docx" }, { "OpenDocument (.odt)", "odt" },
	};
	char dir[PATH_MAX], path[PATH_MAX];
	start_dir(dir, sizeof dir);
	if (pm_file_dialog(_("開啟"), "pomelo-writer", app_icon, 0, dir, NULL, types, LEN(types), NULL, path, sizeof path))
		load_file(path);
}

static void do_page_setup_dialog(void)
{
	double w = doc->page_w, h = doc->page_h;
	dlg_landscape = w > h;
	double pw = dlg_landscape ? h : w, ph = dlg_landscape ? w : h;   /* 直向基準尺寸 */
	if (fabs(pw - 595) < 2 && fabs(ph - 842) < 2) dlg_paper_size = 0;
	else if (fabs(pw - 612) < 2 && fabs(ph - 792) < 2) dlg_paper_size = 1;
	else if (fabs(pw - 612) < 2 && fabs(ph - 1008) < 2) dlg_paper_size = 2;
	else dlg_paper_size = 3;
	dlg_custom_w = pw * 2.54 / 72.0;
	dlg_custom_h = ph * 2.54 / 72.0;
	dlg_margin_t = doc->margin_t * 2.54 / 72.0;
	dlg_margin_b = doc->margin_b * 2.54 / 72.0;
	dlg_margin_l = doc->margin_l * 2.54 / 72.0;
	dlg_margin_r = doc->margin_r * 2.54 / 72.0;
	if (!dlg_run(9, 480)) return;
	static const double sizes[3][2] = { { 595, 842 }, { 612, 792 }, { 612, 1008 } };
	double npw, nph;
	if (dlg_paper_size < 3) { npw = sizes[dlg_paper_size][0]; nph = sizes[dlg_paper_size][1]; }
	else { npw = dlg_custom_w * 72.0 / 2.54; nph = dlg_custom_h * 72.0 / 2.54; }
	push_undo(0);
	doc->page_w = dlg_landscape ? nph : npw;
	doc->page_h = dlg_landscape ? npw : nph;
	doc->margin_t = dlg_margin_t * 72.0 / 2.54;
	doc->margin_b = dlg_margin_b * 72.0 / 2.54;
	doc->margin_l = dlg_margin_l * 72.0 / 2.54;
	doc->margin_r = dlg_margin_r * 72.0 / 2.54;
	set_modified();
	layout_dirty = 1;
	zoom_locked = 0;
}

static void do_find_dialog(int replace_mode)
{
	dlg_find_replace_mode = replace_mode;
	dlg_find_status[0] = 0;
	if (has_sel) {
		char *t = selection_text();
		if (t && !strchr(t, '\n') && strlen(t) < sizeof dlg_find) snprintf(dlg_find, sizeof dlg_find, "%s", t);
		free(t);
	}
	dlg_run(10, replace_mode ? 360 : 280);
}

static void do_saveas_dialog(void)
{
	/* 存檔格式以檔名的副檔名為準（打 .docx 就存成 Word），沒打副檔名才照「類型」選的，對話窗會自動補上 */
	static const PmFileType types[] = { { "Word (.docx)", "docx" }, { "OpenDocument (.odt)", "odt" } };
	char dir[PATH_MAX], path[PATH_MAX], name[256];
	int type = cur_is_odt;
	start_dir(dir, sizeof dir);
	if (cur_path[0]) snprintf(name, sizeof name, "%s", strrchr(cur_path, '/') ? strrchr(cur_path, '/') + 1 : cur_path);
	else snprintf(name, sizeof name, "%s.%s", _("未命名"), type ? "odt" : "docx");
	if (!pm_file_dialog(_("另存新檔"), "pomelo-writer", app_icon, 1, dir, name, types, LEN(types), &type, path, sizeof path))
		return;
	save_file(path, type == 1);
}

static void do_save(void)
{
	if (!cur_path[0]) { do_saveas_dialog(); return; }
	save_file(cur_path, cur_is_odt);
}

static void do_insert_table_dialog(void)
{
	if (dlg_run(2, 300)) insert_table_here(dlg_table_rows, dlg_table_cols);
}

static void do_color_dialog(void)
{
	dlg_color = typing_style.color == COLOR_AUTO ? 0x000000 : typing_style.color;
	if (dlg_run(3, 280)) apply_char_style(fn_set_color, &dlg_color);
}

static void do_size_dialog(void)
{
	dlg_size = typing_style.size > 0 ? typing_style.size : doc->default_size;
	if (dlg_run(4, 220)) apply_char_style(fn_set_size, &dlg_size);
}

/* ------------------------------------------------------------------ */
/* 插入圖片                                                              */
/* ------------------------------------------------------------------ */

static unsigned char *read_whole_file(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	if (n < 0) { fclose(f); return NULL; }
	fseek(f, 0, SEEK_SET);
	unsigned char *buf = malloc((size_t)n);
	size_t got = fread(buf, 1, (size_t)n, f);
	fclose(f);
	if (got != (size_t)n) { free(buf); return NULL; }
	*len = (size_t)n;
	return buf;
}

static void do_insert_image(void)
{
	static const PmFileType types[] = { { "PNG / JPEG (.png, .jpg)", "png;jpg;jpeg" } };
	if (!pm_file_dialog(_("插入圖片"), "pomelo-writer", app_icon, 0, getenv("HOME"), NULL, types, LEN(types), NULL,
	                    dlg_path, sizeof dlg_path)) return;
	size_t len;
	unsigned char *data = read_whole_file(dlg_path, &len);
	if (!data) { say(_("找不到這個檔案")); return; }
	int is_png = len >= 8 && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G';
	int is_jpeg = len >= 3 && data[0] == 0xff && data[1] == 0xd8 && data[2] == 0xff;
	if (!is_png && !is_jpeg) { free(data); say(_("只能插入 PNG 或 JPEG 圖片")); return; }
	cairo_surface_t *s = pm_image_load(dlg_path);
	if (!s) { free(data); say(_("這張圖片打不開")); return; }
	int pw = cairo_image_surface_get_width(s), ph = cairo_image_surface_get_height(s);
	cairo_surface_destroy(s);
	if (pw <= 0 || ph <= 0) { free(data); say(_("這張圖片打不開")); return; }
	double w = pw * 72.0 / 96.0, h = ph * 72.0 / 96.0;
	double avail_w = doc->page_w - doc->margin_l - doc->margin_r;
	if (w > avail_w) { h *= avail_w / w; w = avail_w; }
	push_undo(0);
	int bi = doc_insert_image(doc, cursor_pos.block, data, len, is_png, pw, ph, w, h);
	cursor_pos = doc_unit_start(doc, bi, 0, 0);
	has_sel = 0;
	set_modified();
}

/* ------------------------------------------------------------------ */
/* 列印（跟 PDF 閱讀器同一招：排成一份暫存 PDF，交給 lp）                        */
/* ------------------------------------------------------------------ */

static int run_capture(char *const argv[], char *out, size_t n)
{
	int fds[2];
	if (pipe(fds)) return -1;
	pid_t pid = fork();
	if (pid == 0) {
		dup2(fds[1], 1);
		int nul = open("/dev/null", O_RDWR);
		if (nul >= 0) { dup2(nul, 0); dup2(nul, 2); }
		close(fds[0]);
		execvp(argv[0], argv);
		_exit(127);
	}
	close(fds[1]);
	size_t k = 0;
	ssize_t r;
	while (out && k + 1 < n && (r = read(fds[0], out + k, n - 1 - k)) > 0) k += (size_t)r;
	if (out) out[k] = 0;
	close(fds[0]);
	int st = 0;
	if (pid > 0) waitpid(pid, &st, 0);
	return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static void do_print_dialog(void)
{
	if (!pm_have("lp")) { say(_("沒有安裝列印系統（CUPS），無法列印")); return; }
	char out[4096] = "", def[256] = "";
	char *a1[] = { "lpstat", "-e", NULL }, *a2[] = { "lpstat", "-d", NULL };
	run_capture(a1, out, sizeof out);
	run_capture(a2, def, sizeof def);
	dlg_nprinters = 0;
	dlg_printer_sel = 0;
	for (char *l = strtok(out, "\n"); l && dlg_nprinters < 16; l = strtok(NULL, "\n")) {
		snprintf(dlg_printers[dlg_nprinters], sizeof dlg_printers[0], "%s", l);
		if (strstr(def, l)) dlg_printer_sel = dlg_nprinters;
		dlg_nprinters++;
	}
	dlg_copies = 1;
	preview_page = 0;
	build_preview();
	int ok = dlg_run(7, dlg_nprinters > 0 ? 440 : 380);
	if (preview_surf) { cairo_surface_destroy(preview_surf); preview_surf = NULL; }
	if (!ok || !dlg_nprinters) return;

	/* 排成暫存 PDF：每頁用 cairo PDF surface 畫一次，跟畫面共用 layout_draw_page */
	const char *rt = getenv("XDG_RUNTIME_DIR");
	char tmp[PATH_MAX];
	snprintf(tmp, sizeof tmp, "%s/pomelo-writer-print-%d.pdf", rt && *rt ? rt : "/tmp", (int)getpid());
	cairo_surface_t *psurf = cairo_pdf_surface_create(tmp, doc->page_w, doc->page_h);
	cairo_t *pcr = cairo_create(psurf);
	if (layout_dirty) { layout_view_scale(); rebuild_layout(); }
	for (int pg = 0; pg < lo->npage; pg++) {
		layout_draw_page(pcr, doc, lo, pg, NULL, NULL, NULL, 0, 0x000000, 0, 0, 0x999999);
		cairo_show_page(pcr);
	}
	cairo_destroy(pcr);
	cairo_surface_destroy(psurf);
	chmod(tmp, 0600);

	char copies[16];
	snprintf(copies, sizeof copies, "%d", (int)dlg_copies);
	char *argv[] = { "lp", "-d", dlg_printers[dlg_printer_sel], "-n", copies, tmp, NULL };
	char out2[512] = "";
	int rc = run_capture(argv, out2, sizeof out2);
	unlink(tmp);
	if (rc != 0) say(_("列印失敗"));
}

/* ------------------------------------------------------------------ */
/* 字型清單（常見的幾種，不是列出系統全部字型）                               */
/* ------------------------------------------------------------------ */

static const char *FONT_CHOICES[] = {
	N_("（預設）"), "Noto Sans CJK TC", "Noto Serif CJK TC", "DejaVu Sans", "DejaVu Serif", "Liberation Sans", "Liberation Serif", "monospace",
};

static void pick_font(const Arg *a) { const char *f = (const char *)a->s; apply_char_style(fn_set_font, (void *)(f[0] ? f : "")); set_modified(); }

static const char *STYLE_CHOICES[] = { N_("內文"), N_("標題 1"), N_("標題 2"), N_("標題 3") };
static void pick_style(const Arg *a) { set_para_style((PStyle)a->i); }

static void pick_zoom(const Arg *a)
{
	if (a->i <= 0) { zoom_locked = 0; view_mode = VIEW_WIDTH; }
	else { zoom_locked = 1; view_scale = a->i / 100.0; }
	layout_dirty = 1;
}

/* 縮放比例選單（工具列的百分比、狀態列的百分比都用這個）；x、y 是視窗座標，up＝往上長（狀態列） */
static void zoom_menu(int x, int y, int up)
{
	static const int zpresets[] = { 50, 75, 90, 100, 125, 150, 200 };
	int cur_pct = (int)(view_scale * 100.0 + 0.5);
	char buf[LEN(zpresets)][8];
	MenuItem m[LEN(zpresets) + 1];
	size_t j = 0;
	for (; j < LEN(zpresets); j++) {
		int on = zoom_locked && zpresets[j] == cur_pct;
		snprintf(buf[j], sizeof buf[j], "%c%d%%", on ? '\1' : '\2', zpresets[j]);
		m[j] = (MenuItem){ buf[j], pick_zoom, { .i = zpresets[j] } };
	}
	m[j] = (MenuItem){ zoom_locked || view_mode != VIEW_WIDTH ? _("符合寬度") : "\1符合寬度", pick_zoom, { .i = 0 } };
	int rx, ry; Window ch;
	PMWL_XLATE(wl, dpy, win, pm_root, x, up ? y - (10 + (int)(j + 1) * 24) : y, &rx, &ry, &ch);
	PMWL_POPUP(wl, m, j + 1, rx, ry);
}

/* ------------------------------------------------------------------ */
/* 畫面                                                                 */
/* ------------------------------------------------------------------ */

static Rect tool_btn[24];
static int ntool_btn;

/* 頁面在畫面上的左邊界（頁面座標系的 x=0 對應到畫面哪個 x），尺標跟畫布共用這個算法 */
static double ruler_doc_x0(void) { return page_left_px(cursor_page()); }   /* 尺規對齊游標所在的那一頁 */

static Rect tb_slot(cairo_t *cr, double x, double y, double w, double h, int active)
{
	Rect r = { (int)x, (int)y, (int)w, (int)h };
	tool_btn[ntool_btn++] = r;
	if (active) {
		pm_rgba(cr, T->sel, 0.18);
		pm_rrect(cr, r.x, r.y, r.w, r.h, 5, 5);
		cairo_fill(cr);
	}
	return r;
}

/* 工具列中間用來分組的細直線（不算按鈕、不佔 tool_btn） */
static void tb_divider(cairo_t *cr, double x, double y, double h)
{
	pm_rgba(cr, T->sep, 0.8);
	cairo_rectangle(cr, x, y, 1, h);
	cairo_fill(cr);
}

/* 復原／重做圖示：270 度弧線＋箭頭（重做是左右鏡射） */
static void tb_undo_icon(cairo_t *cr, Rect r, int redo)
{
	double cx = r.x + r.w / 2.0, cy = r.y + r.h / 2.0 + 1, rad = 5.5;
	double s = redo ? -1.0 : 1.0;
	double a_start = redo ? M_PI * -0.25 : M_PI * 1.25;
	double a_end = redo ? M_PI * 1.25 : M_PI * -0.25;
	pm_rgb(cr, T->bar_text);
	cairo_set_line_width(cr, 1.6);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_new_path(cr);
	if (redo) cairo_arc(cr, cx, cy, rad, a_start, a_end);
	else cairo_arc_negative(cr, cx, cy, rad, a_start, a_end);
	cairo_stroke(cr);
	double tipx = cx + rad * cos(a_start), tipy = cy + rad * sin(a_start);
	double nx = cos(a_start), ny = sin(a_start);
	double tgx = -ny * s, tgy = nx * s;
	cairo_move_to(cr, tipx + tgx * 4.6, tipy + tgy * 4.6);
	cairo_line_to(cr, tipx - nx * 3.6, tipy - ny * 3.6);
	cairo_line_to(cr, tipx + nx * 3.6, tipy + ny * 3.6);
	cairo_close_path(cr);
	cairo_fill(cr);
}

static void layout_view_scale(void)
{
	if (zoom_locked) return;
	double aw = ww - canvas_x0() - 2 * RULER_MARGIN, ah = canvas_h() - 2 * PAGE_GAP;
	double sc = aw / doc->page_w;
	if (view_mode == VIEW_PAGE) sc = MIN(sc, ah / doc->page_h);
	else if (view_mode == VIEW_MULTI) sc = MIN((aw - PAGE_GAP) / (2 * doc->page_w), ah / doc->page_h);
	view_scale = CLAMP(sc, 0.1, 3.0);
}

static void draw_toolbar(cairo_t *cr)
{
	pm_rgb(cr, T->bar);
	cairo_rectangle(cr, 0, MENU_H, ww, TOOL_H);
	cairo_fill(cr);
	pm_rgb(cr, T->sep);
	cairo_rectangle(cr, 0, MENU_H + TOOL_H - 1, ww, 1);
	cairo_fill(cr);

	ntool_btn = 0;
	double x = 8, y = MENU_H + 5, bh = TOOL_H - 10;

	/* tb_slot() 畫按鈕底（反白）＋登記點擊範圍；按鈕裡面的圖案／文字各自畫 */
#define SLOT(active_, w_) tb_slot(cr, x, y, w_, bh, active_)
#define ADVANCE(w_) (x += (w_) + 2)
#define LABEL(r, s) do { pm_rgb(cr, T->bar_text); double tw = pm_textw(cr, s); pm_text(cr, (r).x + ((r).w - tw) / 2.0, (r).y + (r).h / 2.0, s); } while (0)
#define GROUP() do { tb_divider(cr, x + 4, y + 2, bh - 4); x += 9; } while (0)

	Para *cur_para = doc_para_at(doc, cursor_pos);
	int bold = typing_style.bold, italic = typing_style.italic, underline = typing_style.underline;
	const char *cur_font = typing_style.font;
	double cur_size = typing_style.size > 0 ? typing_style.size : doc->default_size;
	if (has_sel && cur_para) {
		bold = cur_para->run[0].bold; italic = cur_para->run[0].italic; underline = cur_para->run[0].underline;
		cur_font = cur_para->run[0].font;
		cur_size = cur_para->run[0].size > 0 ? cur_para->run[0].size : doc->default_size;
	}
	Align align = cur_para ? cur_para->align : ALIGN_LEFT;
	ListType lt = cur_para ? cur_para->list_type : LIST_NONE;
	PStyle pstyle = cur_para ? cur_para->style : PSTYLE_BODY;

	Rect r;
	/* 復原／重做 */
	r = SLOT(0, 26); tb_undo_icon(cr, r, 0); ADVANCE(26);
	r = SLOT(0, 26); tb_undo_icon(cr, r, 1); ADVANCE(26);
	GROUP();

	/* 縮放：顯示目前的百分比，點了選縮放比例 */
	{
		char zb[8];
		snprintf(zb, sizeof zb, "%d%%", (int)(view_scale * 100.0 + 0.5));
		r = SLOT(0, 48); pm_font(cr, 13, 0); LABEL(r, zb); ADVANCE(48);
	}
	GROUP();

	r = SLOT(0, 72); pm_font(cr, 13, 0); LABEL(r, _(STYLE_CHOICES[pstyle])); ADVANCE(72);
	GROUP();

	{
		char fb[24];
		pm_ellipsize(cr, cur_font[0] ? cur_font : _(FONT_CHOICES[0]), 78, fb, sizeof fb);
		r = SLOT(0, 92); pm_font(cr, 13, 0); LABEL(r, fb); ADVANCE(92);
	}
	{
		char sb[8];
		snprintf(sb, sizeof sb, "%d", (int)(cur_size + 0.5));
		r = SLOT(0, 36); pm_font(cr, 13, 0); LABEL(r, sb); ADVANCE(36);
	}
	GROUP();

	r = SLOT(bold, 28); cairo_select_font_face(cr, pm_family, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD); pm_font(cr, 13, 0); LABEL(r, "B"); ADVANCE(28);
	r = SLOT(italic, 28); cairo_select_font_face(cr, pm_family, CAIRO_FONT_SLANT_ITALIC, CAIRO_FONT_WEIGHT_NORMAL); pm_font(cr, 13, 0); LABEL(r, "I"); ADVANCE(28);
	r = SLOT(underline, 28);
	pm_font(cr, 13, 0);
	LABEL(r, "U");
	if (underline) {
		pm_rgb(cr, T->bar_text);
		cairo_set_line_width(cr, 1.3);
		double uw = pm_textw(cr, "U");
		cairo_move_to(cr, r.x + (r.w - uw) / 2.0, r.y + r.h / 2.0 + 7);
		cairo_line_to(cr, r.x + (r.w + uw) / 2.0, r.y + r.h / 2.0 + 7);
		cairo_stroke(cr);
	}
	ADVANCE(28);
	r = SLOT(0, 44); pm_font(cr, 13, 0); LABEL(r, _("顏色")); ADVANCE(44);
	GROUP();

	/* 對齊：四條水平線畫出靠左／置中／靠右／齊行的示意（齊行每一條都頂滿） */
	for (int a = 0; a < 4; a++) {
		Align want = a == 0 ? ALIGN_LEFT : a == 1 ? ALIGN_CENTER : a == 2 ? ALIGN_RIGHT : ALIGN_JUSTIFY;
		r = SLOT(align == want, 28);
		pm_rgb(cr, T->bar_text);
		cairo_set_line_width(cr, 1.6);
		double lx0 = r.x + 6, lx1 = r.x + r.w - 6, cy0 = r.y + r.h / 2.0 - 5;
		for (int li = 0; li < 3; li++) {
			double ly = cy0 + li * 5;
			double l0 = lx0, l1 = lx1;
			if (li == 2 && a != 3) {   /* 第三條線縮短一點，並依對齊方向擺放；齊行不縮短 */
				double shortlen = (lx1 - lx0) * 0.55;
				if (a == 0) l1 = lx0 + shortlen;
				else if (a == 2) l0 = lx1 - shortlen;
				else { l0 = (lx0 + lx1) / 2 - shortlen / 2; l1 = (lx0 + lx1) / 2 + shortlen / 2; }
			}
			cairo_move_to(cr, l0, ly);
			cairo_line_to(cr, l1, ly);
			cairo_stroke(cr);
		}
		ADVANCE(28);
	}
	GROUP();

	r = SLOT(lt == LIST_BULLET, 28);
	pm_rgb(cr, T->bar_text);
	cairo_arc(cr, r.x + 8, r.y + r.h / 2.0, 2.2, 0, 2 * M_PI);
	cairo_fill(cr);
	cairo_set_line_width(cr, 1.6);
	cairo_move_to(cr, r.x + 14, r.y + r.h / 2.0);
	cairo_line_to(cr, r.x + r.w - 6, r.y + r.h / 2.0);
	cairo_stroke(cr);
	ADVANCE(28);

	r = SLOT(lt == LIST_DECIMAL, 28);
	pm_font(cr, 11, 0);
	LABEL(r, "1.");
	ADVANCE(28);
	GROUP();

	r = SLOT(0, 48); pm_font(cr, 13, 0); LABEL(r, _("表格")); ADVANCE(48);
	r = SLOT(0, 48); pm_font(cr, 13, 0); LABEL(r, _("圖片")); ADVANCE(48);
	GROUP();
	r = SLOT(outline_open, 48); pm_font(cr, 13, 0); LABEL(r, _("大綱")); ADVANCE(48);
#undef SLOT
#undef ADVANCE
#undef LABEL
#undef GROUP
}

/* 尺標：顯示公分刻度，兩個三角形是左右邊界（跟版面設定一致），可以直接拖曳調整 */
static void draw_ruler(cairo_t *cr)
{
	double cx0 = canvas_x0();
	pm_rgb(cr, T->bar);
	cairo_rectangle(cr, cx0, RULER_Y0, ww - cx0, RULER_H);
	cairo_fill(cr);
	pm_rgb(cr, T->sep);
	cairo_rectangle(cr, cx0, RULER_Y0 + RULER_H - 1, ww - cx0, 1);
	cairo_fill(cr);

	double x0 = ruler_doc_x0(), pw = doc->page_w * view_scale;
	double pt_per_cm = 72.0 / 2.54;

	pm_rgba(cr, T->bar_text, 0.5);
	cairo_set_line_width(cr, 1);
	pm_font(cr, 9, 0);
	for (double cm = 0; ; cm += 0.5) {
		double sx = x0 + cm * pt_per_cm * view_scale;
		if (sx > x0 + pw + 2) break;
		if (sx < cx0 - 1) continue;
		int major = fabs(cm - floor(cm + 0.5)) < 0.001;
		double th = major ? 9 : 5;
		cairo_move_to(cr, sx + 0.5, RULER_Y0 + RULER_H - th);
		cairo_line_to(cr, sx + 0.5, RULER_Y0 + RULER_H - 1);
		cairo_stroke(cr);
		if (major && cm > 0.5) {
			char buf[8];
			snprintf(buf, sizeof buf, "%d", (int)(cm + 0.5));
			double tw = pm_textw(cr, buf);
			pm_rgba(cr, T->bar_text, 0.7);
			pm_text(cr, sx - tw / 2.0, RULER_Y0 + 9, buf);
			pm_rgba(cr, T->bar_text, 0.5);
		}
	}

	/* 邊界三角形：跟版面設定對話框的邊界一致，拖曳可調整 */
	double lx = x0 + doc->margin_l * view_scale, rx = x0 + (doc->page_w - doc->margin_r) * view_scale;
	pm_rgb(cr, T->sel);
	double ty = RULER_Y0 + RULER_H;
	cairo_move_to(cr, lx - 5, ty); cairo_line_to(cr, lx + 5, ty); cairo_line_to(cr, lx, ty - 7); cairo_close_path(cr); cairo_fill(cr);
	cairo_move_to(cr, rx - 5, ty); cairo_line_to(cr, rx + 5, ty); cairo_line_to(cr, rx, ty - 7); cairo_close_path(cr); cairo_fill(cr);
}

/* 大綱面板：列出標題 1～3 的段落，點了跳過去 */
static Rect outline_row[64];
static int outline_block[64];
static int noutline_rows;

static void draw_outline(cairo_t *cr)
{
	pm_rgb(cr, T->bar);
	cairo_rectangle(cr, 0, RULER_Y0, OUTLINE_W, wh - RULER_Y0 - STATUS_H);
	cairo_fill(cr);
	pm_rgb(cr, T->sep);
	cairo_rectangle(cr, OUTLINE_W - 1, RULER_Y0, 1, wh - RULER_Y0 - STATUS_H);
	cairo_fill(cr);
	noutline_rows = 0;
	double y = TOP_H + 8;
	for (int i = 0; i < doc->nblock && noutline_rows < (int)LEN(outline_row); i++) {
		if (doc->block[i].kind != BLOCK_PARA || doc->block[i].para.style == PSTYLE_BODY) continue;
		PStyle st = doc->block[i].para.style;
		double indent = (st - PSTYLE_H1) * 14.0;
		char *t = para_text(&doc->block[i].para);
		char buf[128];
		pm_font(cr, st == PSTYLE_H1 ? 13 : 12, st == PSTYLE_H1);
		pm_ellipsize(cr, t, OUTLINE_W - 20 - indent, buf, sizeof buf);
		free(t);
		Rect row = { 0, (int)y, OUTLINE_W, 24 };
		if (cursor_pos.block == i) { pm_rgba(cr, T->sel, 0.18); cairo_rectangle(cr, row.x, row.y, row.w, row.h); cairo_fill(cr); }
		pm_rgb(cr, T->bar_text);
		pm_text(cr, 10 + indent, y + 12, buf);
		outline_row[noutline_rows] = row;
		outline_block[noutline_rows] = i;
		noutline_rows++;
		y += 24;
	}
	if (!noutline_rows) {
		pm_rgba(cr, T->bar_text, 0.5);
		pm_font(cr, 12, 0);
		pm_text(cr, 10, TOP_H + 20, _("沒有標題"));
	}
}

/* ------------------------------------------------------------------ */
/* 狀態列：左邊第幾頁、字數；右邊檢視方式（單頁／多頁／頁寬）、縮放滑桿與百分比          */
/* ------------------------------------------------------------------ */

static Rect st_view[3];
static PmZoomBar zoombar = { .lo = 0.3, .hi = 3.0 };
static int slider_drag;

/* 字數：中日韓文字一個字算一個，英文等連在一起的字母數字算一個字 */
static int count_words(void)
{
	int n = 0;
	for (int b = 0; b < doc->nblock; b++) {
		Block *bl = &doc->block[b];
		int np = bl->kind == BLOCK_PARA ? 1 : bl->kind == BLOCK_TABLE ? bl->table.nrow * bl->table.ncol : 0;
		for (int k = 0; k < np; k++) {
			Para *pa = bl->kind == BLOCK_PARA ? &bl->para : &bl->table.cell[k];
			int in_word = 0;
			for (int r = 0; r < pa->nrun; r++) {
				const unsigned char *t = (const unsigned char *)pa->run[r].text;
				for (size_t i = 0; i < pa->run[r].len; ) {
					unsigned c = t[i];
					int l = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
					if (l == 3 && i + 2 < pa->run[r].len) c = ((c & 0x0f) << 12) | ((t[i + 1] & 0x3f) << 6) | (t[i + 2] & 0x3f);
					if (c < 0x80 && isalnum((int)c)) { if (!in_word) n++; in_word = 1; }
					else {
						in_word = 0;
						if (l == 3 && c >= 0x2e80 && !(c >= 0x3000 && c <= 0x303f) && !(c >= 0xff00 && c <= 0xff0f)) n++;
					}
					i += (size_t)l;
				}
			}
		}
	}
	return n;
}

static void set_zoom(double z)
{
	zoom_locked = 1;
	view_scale = CLAMP(z, zoombar.lo, zoombar.hi);
	layout_dirty = 1;
}

static double status_button(cairo_t *cr, double x, double y, const char *label, int on, Rect *r)
{
	double w = pm_textw(cr, label) + 16;
	if (on) { pm_rgba(cr, T->sel, 0.18); pm_rrect(cr, x, y + 4, w, STATUS_H - 8, 4, 4); cairo_fill(cr); }
	pm_rgb(cr, on ? T->sel : T->bar_text);
	pm_text(cr, x + 8, y + STATUS_H / 2.0, label);
	*r = (Rect){ (int)x, (int)y, (int)w, STATUS_H };
	return x + w;
}

static void draw_status(cairo_t *cr)
{
	double y = wh - STATUS_H, cy = y + STATUS_H / 2.0;
	pm_rgb(cr, T->bar);
	cairo_rectangle(cr, 0, y, ww, STATUS_H);
	cairo_fill(cr);
	pm_rgb(cr, T->sep);
	cairo_rectangle(cr, 0, y, ww, 1);
	cairo_fill(cr);

	char buf[96];
	pm_font(cr, 12, 0);
	pm_rgb(cr, T->bar_text);
	snprintf(buf, sizeof buf, _("第 %d 頁，共 %d 頁"), cursor_page() + 1, lo->npage);
	pm_text(cr, 12, cy, buf);
	double lx = 12 + pm_textw(cr, buf) + 18;
	snprintf(buf, sizeof buf, _("%d 個字"), count_words());
	pm_rgb(cr, T->dim);
	pm_text(cr, lx, cy, buf);
	double left_end = lx + pm_textw(cr, buf) + 12;

	/* 右邊由左到右：單頁 多頁 頁寬 ｜ −  ━━●━━  ＋ 100% */
	static const char *vlabels[3] = { N_("單頁"), N_("多頁"), N_("頁寬") };
	static const int vmode[3] = { VIEW_PAGE, VIEW_MULTI, VIEW_WIDTH };
	double vw = 0;
	for (int i = 0; i < 3; i++) vw += pm_textw(cr, _(vlabels[i])) + 16 + 2;
	double x = MAX(left_end, ww - 10 - pm_zoom_width(cr) - 14 - vw);   /* 視窗太窄時往右擠 */
	for (int i = 0; i < 3; i++) x = status_button(cr, x, y, _(vlabels[i]), !zoom_locked && view_mode == vmode[i], &st_view[i]) + 2;
	x += 12;
	pm_rgb(cr, T->sep);
	cairo_rectangle(cr, x - 7, y + 7, 1, STATUS_H - 14);
	cairo_fill(cr);
	pm_zoom_draw(cr, &zoombar, x, y, STATUS_H, view_scale);
}

static int in_rect(Rect r, int x, int y) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }

static void status_click(XButtonEvent *e)
{
	if (e->button != Button1) return;
	static const int vmode[3] = { VIEW_PAGE, VIEW_MULTI, VIEW_WIDTH };
	for (int i = 0; i < 3; i++)
		if (in_rect(st_view[i], e->x, e->y)) { view_mode = vmode[i]; zoom_locked = 0; scroll_y = 0; layout_dirty = 1; return; }
	double z;
	switch (pm_zoom_click(&zoombar, e->x, e->y, view_scale, &z)) {
	case PM_ZOOM_DRAG: slider_drag = 1; /* fall through */
	case PM_ZOOM_SET: set_zoom(z); break;
	case PM_ZOOM_MENU: zoom_menu(zoombar.pct.x + zoombar.pct.w - 180, wh - STATUS_H, 1); break;   /* 選單（至少 180 寬）右緣對齊百分比 */
	}
}

static void draw(void)
{
	if (layout_dirty) { layout_view_scale(); rebuild_layout(); }
	Paint p;
	cairo_t *cr = wl ? pmwl_begin(wl, &ww, &wh) : pm_paint_begin(&p, win, ww, wh);
	double cx0 = canvas_x0();
	pm_rgb(cr, T->dim);
	cairo_rectangle(cr, cx0, TOP_H, ww - cx0, canvas_h());
	cairo_fill(cr);

	cairo_save(cr);
	cairo_rectangle(cr, cx0, TOP_H, ww - cx0, canvas_h());
	cairo_clip(cr);
	for (int pg = 0; pg < lo->npage; pg++) {
		double sx, sy;
		page_to_screen(pg, 0, 0, &sx, &sy);
		sy += TOP_H;
		double pw = doc->page_w * view_scale, ph = doc->page_h * view_scale;
		if (sy + ph < TOP_H || sy > wh - STATUS_H) continue;
		cairo_set_source_rgba(cr, 0, 0, 0, 0.18);
		cairo_rectangle(cr, sx + 3, sy + 3, pw, ph);
		cairo_fill(cr);
		cairo_set_source_rgb(cr, 1, 1, 1);
		cairo_rectangle(cr, sx, sy, pw, ph);
		cairo_fill(cr);
		cairo_save(cr);
		cairo_translate(cr, sx, sy);
		cairo_scale(cr, view_scale, view_scale);
		Pos *sa = has_sel ? &sel_anchor : NULL, *sb = has_sel ? &cursor_pos : NULL;
		/* 頁面永遠是白紙黑字，不跟著系統深色模式變色（深色模式只影響選單列、工具列這些介面外框） */
		layout_draw_page(cr, doc, lo, pg, sa, sb, &cursor_pos, cursor_visible,
		                 0x1a1a1a, T->sel, T->sel, 0xcccccc);
		cairo_restore(cr);
	}
	cairo_restore(cr);

	if (outline_open) draw_outline(cr);

	draw_toolbar(cr);
	draw_ruler(cr);
	draw_status(cr);
	pm_rgb(cr, T->bar);
	cairo_rectangle(cr, 0, 0, ww, MENU_H);
	cairo_fill(cr);
	pm_rgb(cr, T->sep);
	cairo_rectangle(cr, 0, MENU_H - 1, ww, 1);
	cairo_fill(cr);
	static const char *labels[5] = { N_("檔案"), N_("編輯"), N_("格式"), N_("插入"), N_("表格") };
	double mx = 10;
	pm_font(cr, 13, 0);
	for (int i = 0; i < 5; i++) {
		double tw = pm_textw(cr, _(labels[i]));
		menu_r[i] = (Rect){ (int)mx, 0, (int)tw + 20, MENU_H };
		if (hover_menu == i) { pm_rgba(cr, T->sel, 0.18); cairo_rectangle(cr, menu_r[i].x, 0, menu_r[i].w, MENU_H); cairo_fill(cr); }
		pm_rgb(cr, T->bar_text);
		pm_text(cr, mx + 10, MENU_H / 2.0, _(labels[i]));
		mx += tw + 20;
	}

	if (wl) {
		pmwl_end(wl, cr);
		/* 輸入法的選字列跟著游標 */
		int pg; double x, y, h, sx, sy;
		if (layout_pos_xy(doc, lo, cursor_pos, &pg, &x, &y, &h)) {
			page_to_screen(pg, x, y, &sx, &sy);
			pmwl_ime(wl, 1, (int)sx, (int)(sy + TOP_H), (int)(h * view_scale));
		}
		return;
	}
	pm_paint_end(&p, cr, 0, 0);
}

/* ------------------------------------------------------------------ */
/* 選單列                                                               */
/* ------------------------------------------------------------------ */

enum {
	A_NEW, A_OPEN, A_SAVE, A_SAVEAS, A_PAGE_SETUP, A_PRINT, A_QUIT,
	A_UNDO, A_REDO, A_CUT, A_COPY, A_PASTE, A_SELALL, A_FIND, A_REPLACE,
	A_BOLD, A_ITALIC, A_UNDERLINE, A_COLOR, A_ALIGN_L, A_ALIGN_C, A_ALIGN_R, A_ALIGN_J, A_LIST_BULLET, A_LIST_DECIMAL,
	A_STYLE_BODY, A_STYLE_H1, A_STYLE_H2, A_STYLE_H3,
	A_INSERT_IMAGE, A_PAGE_NUMBERS, A_OUTLINE_TOGGLE,
	A_TABLE_INSERT, A_ROW_ABOVE, A_ROW_BELOW, A_ROW_DEL, A_COL_LEFT, A_COL_RIGHT, A_COL_DEL,
};

static void act(int a);
static void m_act(const Arg *arg) { act(arg->i); }

static const char *L(const char *name, const char *key, int check)
{
	static char pool[32][160];
	static int k;
	char *b = pool[k++ % 32];
	snprintf(b, sizeof pool[0], "%s%s%s%s", check == 1 ? "\001" : check == 2 ? "\002" : "", name, key ? "\t" : "", key ? key : "");
	return b;
}

static int table_row_col(int *trow, int *tcol, Table **tbl)
{
	Block *b = &doc->block[cursor_pos.block];
	if (b->kind != BLOCK_TABLE) return 0;
	*trow = cursor_pos.row; *tcol = cursor_pos.col; *tbl = &b->table;
	return 1;
}

static int menu_build(int which, MenuItem *m)
{
	int k = 0;
	int in_table = doc->block[cursor_pos.block].kind == BLOCK_TABLE;
	Para *cp = doc_para_at(doc, cursor_pos);
#define ITEM(name, key, a, on) (m[k++] = (MenuItem){ L(name, key, 0), (on) ? m_act : pm_menu_off, { .i = (a) } })
#define CHECK(name, key, a, v) (m[k++] = (MenuItem){ L(name, key, (v) ? 1 : 2), m_act, { .i = (a) } })
#define SEP() (m[k++] = (MenuItem){ NULL, NULL, { 0 } })
	if (which == 0) {
		ITEM(N_("新文件"), "Ctrl+N", A_NEW, 1);
		ITEM(N_("開啟…"), "Ctrl+O", A_OPEN, 1);
		SEP();
		ITEM(N_("儲存"), "Ctrl+S", A_SAVE, 1);
		ITEM(N_("另存新檔…"), "Ctrl+Shift+S", A_SAVEAS, 1);
		SEP();
		ITEM(N_("版面設定…"), NULL, A_PAGE_SETUP, 1);
		ITEM(N_("列印…"), "Ctrl+P", A_PRINT, 1);
		SEP();
		ITEM(N_("結束"), "Ctrl+Q", A_QUIT, 1);
	} else if (which == 1) {
		ITEM(N_("復原"), "Ctrl+Z", A_UNDO, nundo > 0);
		ITEM(N_("重做"), "Ctrl+Shift+Z", A_REDO, nredo > 0);
		SEP();
		ITEM(N_("剪下"), "Ctrl+X", A_CUT, has_sel);
		ITEM(N_("拷貝"), "Ctrl+C", A_COPY, has_sel);
		ITEM(N_("貼上"), "Ctrl+V", A_PASTE, 1);
		SEP();
		ITEM(N_("全選"), "Ctrl+A", A_SELALL, 1);
		SEP();
		ITEM(N_("尋找…"), "Ctrl+F", A_FIND, 1);
		ITEM(N_("取代…"), "Ctrl+H", A_REPLACE, 1);
	} else if (which == 2) {
		CHECK(N_("內文"), NULL, A_STYLE_BODY, cp && cp->style == PSTYLE_BODY);
		CHECK(N_("標題 1"), NULL, A_STYLE_H1, cp && cp->style == PSTYLE_H1);
		CHECK(N_("標題 2"), NULL, A_STYLE_H2, cp && cp->style == PSTYLE_H2);
		CHECK(N_("標題 3"), NULL, A_STYLE_H3, cp && cp->style == PSTYLE_H3);
		SEP();
		CHECK(N_("粗體"), "Ctrl+B", A_BOLD, has_sel ? (cp && cp->run[0].bold) : typing_style.bold);
		CHECK(N_("斜體"), "Ctrl+I", A_ITALIC, has_sel ? (cp && cp->run[0].italic) : typing_style.italic);
		CHECK(N_("底線"), "Ctrl+U", A_UNDERLINE, has_sel ? (cp && cp->run[0].underline) : typing_style.underline);
		ITEM(N_("顏色…"), NULL, A_COLOR, 1);
		SEP();
		CHECK(N_("靠左對齊"), NULL, A_ALIGN_L, cp && cp->align == ALIGN_LEFT);
		CHECK(N_("置中"), NULL, A_ALIGN_C, cp && cp->align == ALIGN_CENTER);
		CHECK(N_("靠右對齊"), NULL, A_ALIGN_R, cp && cp->align == ALIGN_RIGHT);
		CHECK(N_("齊行"), NULL, A_ALIGN_J, cp && cp->align == ALIGN_JUSTIFY);
		SEP();
		CHECK(N_("項目符號清單"), NULL, A_LIST_BULLET, cp && cp->list_type == LIST_BULLET);
		CHECK(N_("編號清單"), NULL, A_LIST_DECIMAL, cp && cp->list_type == LIST_DECIMAL);
	} else if (which == 3) {
		ITEM(N_("圖片…"), NULL, A_INSERT_IMAGE, 1);
		SEP();
		CHECK(N_("頁碼"), NULL, A_PAGE_NUMBERS, doc->page_numbers);
		CHECK(N_("大綱"), NULL, A_OUTLINE_TOGGLE, outline_open);
	} else {
		ITEM(N_("插入表格…"), NULL, A_TABLE_INSERT, 1);
		SEP();
		ITEM(N_("在上面插入一列"), NULL, A_ROW_ABOVE, in_table);
		ITEM(N_("在下面插入一列"), NULL, A_ROW_BELOW, in_table);
		ITEM(N_("刪除這一列"), NULL, A_ROW_DEL, in_table);
		SEP();
		ITEM(N_("在左邊插入一欄"), NULL, A_COL_LEFT, in_table);
		ITEM(N_("在右邊插入一欄"), NULL, A_COL_RIGHT, in_table);
		ITEM(N_("刪除這一欄"), NULL, A_COL_DEL, in_table);
	}
#undef ITEM
#undef CHECK
#undef SEP
	return k;
}

static void open_bar(int which)
{
	for (int guard = 0; guard < 20; guard++) {
		MenuItem m[24];
		int n = menu_build(which, m);
		int rx, ry;
		Window ch;
		PMWL_XLATE(wl, dpy, win, pm_root, menu_r[which].x, MENU_H, &rx, &ry, &ch);
		hover_menu = which;
		pm_popup_dir = 0;
		PMWL_POPUP(wl, m, n, rx, ry);
		hover_menu = -1;
		if (!pm_popup_dir) break;
		which = (which + pm_popup_dir + 5) % 5;
		draw();
	}
}

static void act(int a)
{
	Table *tbl;
	int trow, tcol;
	switch (a) {
	case A_NEW: new_document(); break;
	case A_OPEN: do_open_dialog(); break;
	case A_SAVE: do_save(); break;
	case A_SAVEAS: do_saveas_dialog(); break;
	case A_PAGE_SETUP: do_page_setup_dialog(); break;
	case A_PRINT: do_print_dialog(); break;
	case A_QUIT: exit(0);
	case A_UNDO: do_undo(); break;
	case A_REDO: do_redo(); break;
	case A_CUT: copy_sel(1); break;
	case A_COPY: copy_sel(0); break;
	case A_PASTE: paste(); break;
	case A_SELALL: sel_anchor = doc_start(doc); cursor_pos = doc_end(doc); has_sel = 1; break;
	case A_FIND: do_find_dialog(0); break;
	case A_REPLACE: do_find_dialog(1); break;
	case A_BOLD: apply_char_style(fn_toggle_bold, NULL); break;
	case A_ITALIC: apply_char_style(fn_toggle_italic, NULL); break;
	case A_UNDERLINE: apply_char_style(fn_toggle_underline, NULL); break;
	case A_COLOR: do_color_dialog(); break;
	case A_ALIGN_L: set_para_align(ALIGN_LEFT); break;
	case A_ALIGN_C: set_para_align(ALIGN_CENTER); break;
	case A_ALIGN_R: set_para_align(ALIGN_RIGHT); break;
	case A_ALIGN_J: set_para_align(ALIGN_JUSTIFY); break;
	case A_LIST_BULLET: toggle_list(LIST_BULLET); break;
	case A_LIST_DECIMAL: toggle_list(LIST_DECIMAL); break;
	case A_STYLE_BODY: set_para_style(PSTYLE_BODY); break;
	case A_STYLE_H1: set_para_style(PSTYLE_H1); break;
	case A_STYLE_H2: set_para_style(PSTYLE_H2); break;
	case A_STYLE_H3: set_para_style(PSTYLE_H3); break;
	case A_INSERT_IMAGE: do_insert_image(); break;
	case A_PAGE_NUMBERS: push_undo(0); doc->page_numbers = !doc->page_numbers; set_modified(); break;
	case A_OUTLINE_TOGGLE: outline_open = !outline_open; layout_dirty = 1; break;
	case A_TABLE_INSERT: do_insert_table_dialog(); break;
	case A_ROW_ABOVE: if (table_row_col(&trow, &tcol, &tbl)) { push_undo(0); table_insert_row(tbl, trow); set_modified(); } break;
	case A_ROW_BELOW: if (table_row_col(&trow, &tcol, &tbl)) { push_undo(0); table_insert_row(tbl, trow + 1); set_modified(); } break;
	case A_ROW_DEL: if (table_row_col(&trow, &tcol, &tbl)) { push_undo(0); table_delete_row(tbl, trow); cursor_pos = doc_pos_clamp(doc, cursor_pos); set_modified(); } break;
	case A_COL_LEFT: if (table_row_col(&trow, &tcol, &tbl)) { push_undo(0); table_insert_col(tbl, tcol); set_modified(); } break;
	case A_COL_RIGHT: if (table_row_col(&trow, &tcol, &tbl)) { push_undo(0); table_insert_col(tbl, tcol + 1); set_modified(); } break;
	case A_COL_DEL: if (table_row_col(&trow, &tcol, &tbl)) { push_undo(0); table_delete_col(tbl, tcol); cursor_pos = doc_pos_clamp(doc, cursor_pos); set_modified(); } break;
	}
}

/* ------------------------------------------------------------------ */
/* 游標移動                                                              */
/* ------------------------------------------------------------------ */

static int line_index_at(Pos p)
{
	for (int i = 0; i < lo->nline; i++) {
		Line *l = &lo->line[i];
		if (l->block == p.block && l->row == p.row && l->col == p.col && p.offset >= l->off_start && p.offset <= l->off_end) return i;
	}
	return -1;
}

static void touch_cursor(void) { cursor_visible = 1; cursor_blink_at = now_ms() + 500; }

static void move_char(int dir, int extend)
{
	want_x_valid = 0;
	touch_cursor();
	if (!extend && has_sel) {
		Pos a = sel_anchor, b = cursor_pos;
		if (doc_pos_cmp(doc, a, b) > 0) { Pos t = a; a = b; b = t; }
		cursor_pos = dir < 0 ? a : b;
		has_sel = 0;
		return;
	}
	if (extend && !has_sel) { sel_anchor = cursor_pos; has_sel = 1; }
	Pos p = cursor_pos;
	const Para *para = doc_para_at_c(doc, p);
	int len = para ? (int)para_text_len(para) : 0;
	if (dir > 0) {
		if (p.offset < len) {
			char *t = para_text(para);
			int off = p.offset + 1;
			while (off < len && ((unsigned char)t[off] & 0xc0) == 0x80) off++;
			free(t);
			p.offset = off;
		} else {
			int nb, nr, nc;
			if (doc_next_unit(doc, p.block, p.row, p.col, &nb, &nr, &nc)) p = doc_unit_start(doc, nb, nr, nc);
		}
	} else {
		if (p.offset > 0) {
			char *t = para_text(para);
			int off = p.offset - 1;
			while (off > 0 && ((unsigned char)t[off] & 0xc0) == 0x80) off--;
			free(t);
			p.offset = off;
		} else {
			int nb, nr, nc;
			if (doc_prev_unit(doc, p.block, p.row, p.col, &nb, &nr, &nc)) p = doc_unit_end(doc, nb, nr, nc);
		}
	}
	cursor_pos = p;
	if (!extend) has_sel = 0;
}

static void move_vertical(int dir, int extend)
{
	touch_cursor();
	if (!want_x_valid) {
		int pg; double x, y, h;
		if (layout_pos_xy(doc, lo, cursor_pos, &pg, &x, &y, &h)) { want_x = x; want_x_valid = 1; }
	}
	int i = line_index_at(cursor_pos);
	if (i < 0) {
		/* 游標停在圖片上，沒有對應的文字行可以找「上一行／下一行」：改成跳到上一個／下一個可編輯單位 */
		int nb, nr, nc;
		int ok = dir < 0 ? doc_prev_unit(doc, cursor_pos.block, cursor_pos.row, cursor_pos.col, &nb, &nr, &nc)
		                 : doc_next_unit(doc, cursor_pos.block, cursor_pos.row, cursor_pos.col, &nb, &nr, &nc);
		if (!ok) return;
		if (extend && !has_sel) sel_anchor = cursor_pos;
		cursor_pos = dir < 0 ? doc_unit_end(doc, nb, nr, nc) : doc_unit_start(doc, nb, nr, nc);
		if (!extend) has_sel = 0; else has_sel = 1;
		return;
	}
	int ni = i + dir;
	if (ni < 0 || ni >= lo->nline) return;
	Line *l = &lo->line[ni];
	Pos np = layout_hit_test(doc, lo, l->page, want_x, l->y + l->h / 2);
	if (extend && !has_sel) sel_anchor = cursor_pos;
	cursor_pos = np;
	if (!extend) has_sel = 0; else has_sel = 1;
}

static void move_line_edge(int end, int extend)
{
	want_x_valid = 0;
	touch_cursor();
	if (extend && !has_sel) sel_anchor = cursor_pos;
	int i = line_index_at(cursor_pos);
	if (i < 0) return;
	Line *l = &lo->line[i];
	cursor_pos = (Pos){ l->block, l->row, l->col, end ? l->off_end : l->off_start };
	has_sel = extend;
}

static void move_doc_edge(int end, int extend)
{
	want_x_valid = 0;
	touch_cursor();
	if (extend && !has_sel) sel_anchor = cursor_pos;
	cursor_pos = end ? doc_end(doc) : doc_start(doc);
	has_sel = extend;
}

/* ------------------------------------------------------------------ */
/* 滑鼠                                                                 */
/* ------------------------------------------------------------------ */

static long last_click_ms;
static Pos last_click_pos = { -1, 0, 0, 0 };

/* 跟 layout.c 裡的同名函式各自獨立一份（都很小，不值得拆出共用標頭） */
static int w_utf8_len(unsigned char c)
{
	if (c < 0x80) return 1;
	if ((c & 0xe0) == 0xc0) return 2;
	if ((c & 0xf0) == 0xe0) return 3;
	if ((c & 0xf8) == 0xf0) return 4;
	return 1;
}
static unsigned w_utf8_decode(const char *s, int len)
{
	unsigned char c = (unsigned char)s[0];
	if (len <= 1) return c;
	if (len == 2) return (unsigned)((c & 0x1f) << 6) | ((unsigned char)s[1] & 0x3f);
	if (len == 3) return (unsigned)((c & 0x0f) << 12) | ((unsigned)((unsigned char)s[1] & 0x3f) << 6) | ((unsigned char)s[2] & 0x3f);
	return (unsigned)((c & 0x07) << 18) | ((unsigned)((unsigned char)s[1] & 0x3f) << 12) | ((unsigned)((unsigned char)s[2] & 0x3f) << 6) | ((unsigned char)s[3] & 0x3f);
}
static int w_is_cjk(unsigned cp)
{
	return (cp >= 0x2E80 && cp <= 0x9FFF) || (cp >= 0xA000 && cp <= 0xA4CF) || (cp >= 0xF900 && cp <= 0xFAFF)
	    || (cp >= 0xFF00 && cp <= 0xFFEF) || (cp >= 0x20000 && cp <= 0x2FFFF) || (cp >= 0x3000 && cp <= 0x303F);
}

/* 點兩下選字：往左右找到同一種「類別」（空白／CJK／一般文字）的邊界 */
static void select_word_at(Pos p)
{
	Para *para = doc_para_at(doc, p);
	if (!para) return;
	char *t = para_text(para);
	int len = (int)strlen(t);
	int off = CLAMP(p.offset, 0, len);
	int s = off, e = off;
	int base_off = off < len ? off : (off > 0 ? off - 1 : 0);
	unsigned base_cp = 0;
	if (len > 0) { int l2 = w_utf8_len((unsigned char)t[base_off]); base_cp = w_utf8_decode(t + base_off, l2); }
	int base_cjk = w_is_cjk(base_cp);
	int base_space = base_cp <= ' ';
	while (s > 0) {
		int bs = s - 1;
		while (bs > 0 && ((unsigned char)t[bs] & 0xc0) == 0x80) bs--;
		int l2 = w_utf8_len((unsigned char)t[bs]);
		unsigned cp = w_utf8_decode(t + bs, l2);
		if ((cp <= ' ') != base_space || (!base_space && w_is_cjk(cp) != base_cjk)) break;
		s = bs;
	}
	while (e < len) {
		int l2 = w_utf8_len((unsigned char)t[e]);
		unsigned cp = w_utf8_decode(t + e, l2);
		if ((cp <= ' ') != base_space || (!base_space && w_is_cjk(cp) != base_cjk)) break;
		e += l2;
	}
	free(t);
	sel_anchor = (Pos){ p.block, p.row, p.col, s };
	cursor_pos = (Pos){ p.block, p.row, p.col, e };
	has_sel = s != e;
}

static void on_button(XButtonEvent *e)
{
	if (e->y < MENU_H) {
		for (int i = 0; i < 5; i++) if (in_rect(menu_r[i], e->x, e->y)) open_bar(i);
		return;
	}
	if (e->y < MENU_H + TOOL_H) {
		for (int i = 0; i < ntool_btn; i++) {
			if (!in_rect(tool_btn[i], e->x, e->y)) continue;
			static const int acts[] = {
				A_UNDO, A_REDO, -4, -3, -1, -2,
				A_BOLD, A_ITALIC, A_UNDERLINE, A_COLOR,
				A_ALIGN_L, A_ALIGN_C, A_ALIGN_R, A_ALIGN_J,
				A_LIST_BULLET, A_LIST_DECIMAL,
				A_TABLE_INSERT, A_INSERT_IMAGE, A_OUTLINE_TOGGLE,
			};
			if (i < (int)LEN(acts)) {
				if (acts[i] == -1) { /* 字型：彈出選單 */
					MenuItem m[LEN(FONT_CHOICES)];
					for (size_t j = 0; j < LEN(FONT_CHOICES); j++) m[j] = (MenuItem){ _(FONT_CHOICES[j]), pick_font, { .s = j == 0 ? "" : FONT_CHOICES[j] } };
					int rx, ry; Window ch;
					PMWL_XLATE(wl, dpy, win, pm_root, tool_btn[i].x, TOP_H, &rx, &ry, &ch);
					PMWL_POPUP(wl, m, LEN(FONT_CHOICES), rx, ry);
				} else if (acts[i] == -2) do_size_dialog();
				else if (acts[i] == -3) { /* 樣式：彈出選單 */
					MenuItem m[LEN(STYLE_CHOICES)];
					for (size_t j = 0; j < LEN(STYLE_CHOICES); j++) m[j] = (MenuItem){ _(STYLE_CHOICES[j]), pick_style, { .i = (int)j } };
					int rx, ry; Window ch;
					PMWL_XLATE(wl, dpy, win, pm_root, tool_btn[i].x, TOP_H, &rx, &ry, &ch);
					PMWL_POPUP(wl, m, LEN(STYLE_CHOICES), rx, ry);
				} else if (acts[i] == -4) { /* 縮放：彈出選單 */
					zoom_menu(tool_btn[i].x, TOP_H, 0);
				} else if (acts[i] >= 0) act(acts[i]);
			}
			draw();
			return;
		}
		return;
	}
	if (e->y >= wh - STATUS_H) { status_click(e); return; }
	if (e->y < TOP_H) {
		if (e->button != Button1) return;
		double x0 = ruler_doc_x0();
		double lx = x0 + doc->margin_l * view_scale, rx = x0 + (doc->page_w - doc->margin_r) * view_scale;
		if (fabs(e->x - lx) < 6) ruler_drag = 1;
		else if (fabs(e->x - rx) < 6) ruler_drag = 2;
		return;
	}
	if (outline_open && e->x < OUTLINE_W) {
		if (e->button != Button1) return;
		for (int i = 0; i < noutline_rows; i++) {
			if (!in_rect(outline_row[i], e->x, e->y)) continue;
			cursor_pos = doc_unit_start(doc, outline_block[i], 0, 0);
			has_sel = 0;
			scroll_to_cursor();
			return;
		}
		return;
	}
	if (e->button == Button4) { scroll_y = MAX(0, scroll_y - 60); return; }
	if (e->button == Button5) { scroll_y = MIN(MAX(0, content_height_px() - canvas_h()), scroll_y + 60); return; }
	if (e->button != Button1) return;
	int page; double px, py;
	screen_to_page(e->x, e->y - TOP_H, &page, &px, &py);
	Pos np = layout_hit_test(doc, lo, page, px, py);
	int shift = e->state & ShiftMask;
	int dbl = e->time - last_click_ms < 400 && last_click_pos.block == np.block && last_click_pos.row == np.row && last_click_pos.col == np.col;
	last_click_ms = e->time;
	last_click_pos = np;
	if (dbl) { select_word_at(np); return; }
	if (shift) { if (!has_sel) sel_anchor = cursor_pos; has_sel = 1; cursor_pos = np; }
	else { cursor_pos = np; sel_anchor = np; has_sel = 0; drag_sel = 1; }
	want_x_valid = 0;
	touch_cursor();
}

static void on_motion(XMotionEvent *e)
{
	if (slider_drag) { set_zoom(pm_zoom_slider_at(&zoombar, e->x)); return; }
	if (ruler_drag) {
		double x0 = ruler_doc_x0();
		double min_margin = 8.0;   /* pt，避免拖成 0 或負的 */
		if (ruler_drag == 1) doc->margin_l = CLAMP((e->x - x0) / view_scale, 0, doc->page_w - doc->margin_r - min_margin);
		else doc->margin_r = CLAMP((x0 + doc->page_w * view_scale - e->x) / view_scale, 0, doc->page_w - doc->margin_l - min_margin);
		set_modified();
		return;
	}
	if (!drag_sel) return;
	if (e->y < TOP_H) return;
	int page; double px, py;
	screen_to_page(e->x, e->y - TOP_H, &page, &px, &py);
	Pos np = layout_hit_test(doc, lo, page, px, py);
	if (doc_pos_cmp(doc, np, sel_anchor) != 0) has_sel = 1;
	cursor_pos = np;
}

static void confirm_close(void)
{
	if (!modified) exit(0);
	/* 常見的詢問對話窗：按鈕橫排靠右，「儲存」是預設（Enter），Esc 或關掉＝取消 */
	const char *name = cur_path[0] ? strrchr(cur_path, '/') : NULL;
	name = name ? name + 1 : (cur_path[0] ? cur_path : _("未命名文件"));
	char msg[PATH_MAX + 100];
	snprintf(msg, sizeof msg, _("「%s」還沒儲存，要先儲存嗎？"), name);
	const char *btn[3] = { "不要儲存", "取消", "儲存" };
	int r = pm_ask(_("文書處理器"), "pomelo-writer", app_icon, msg, btn, 3, 2, 1);
	if (r == 2) { do_save(); if (!modified) exit(0); return; }   /* 另存新檔取消了就不結束 */
	if (r == 0) exit(0);
}

/* ------------------------------------------------------------------ */
/* 鍵盤                                                                 */
/* ------------------------------------------------------------------ */

static XIC xic;

static void handle_key(XKeyEvent *ev)
{
	char keybuf[64];
	KeySym ks;
	Status st = 0;
	int keylen = pmwl_xkey(ev, keybuf, sizeof keybuf - 1, &ks, NULL);   /* 原生 Wayland 轉出來的事件 */
	if (keylen < 0) keylen = xic ? Xutf8LookupString(xic, ev, keybuf, sizeof keybuf - 1, &ks, &st) : XLookupString(ev, keybuf, sizeof keybuf - 1, &ks, NULL);
	if (keylen < 0 || st == XBufferOverflow) keylen = 0;
	keybuf[keylen] = 0;
	int ctrl = (ev->state & ControlMask) != 0, shift = (ev->state & ShiftMask) != 0;

	if (ctrl) {
		switch (ks) {
		case XK_n: case XK_N: act(A_NEW); draw(); return;
		case XK_o: case XK_O: act(A_OPEN); draw(); return;
		case XK_s: case XK_S: act(shift ? A_SAVEAS : A_SAVE); draw(); return;
		case XK_p: case XK_P: act(A_PRINT); draw(); return;
		case XK_f: case XK_F: act(A_FIND); draw(); return;
		case XK_h: case XK_H: act(A_REPLACE); draw(); return;
		case XK_z: case XK_Z: if (shift) do_redo(); else do_undo(); draw(); return;
		case XK_y: case XK_Y: do_redo(); draw(); return;
		case XK_x: case XK_X: act(A_CUT); draw(); return;
		case XK_c: case XK_C: act(A_COPY); return;
		case XK_v: case XK_V: act(A_PASTE); return;
		case XK_a: case XK_A: act(A_SELALL); draw(); return;
		case XK_b: case XK_B: act(A_BOLD); draw(); return;
		case XK_i: case XK_I: act(A_ITALIC); draw(); return;
		case XK_u: case XK_U: act(A_UNDERLINE); draw(); return;
		case XK_q: case XK_Q: confirm_close(); return;
		case XK_plus: case XK_equal: case XK_KP_Add:
			set_zoom(view_scale * 1.1); draw(); return;
		case XK_minus: case XK_KP_Subtract:
			set_zoom(view_scale / 1.1); draw(); return;
		case XK_0: zoom_locked = 0; layout_dirty = 1; draw(); return;
		default: return;
		}
	}

	switch (ks) {
	case XK_Left: move_char(-1, shift); scroll_to_cursor(); draw(); return;
	case XK_Right: move_char(1, shift); scroll_to_cursor(); draw(); return;
	case XK_Up: move_vertical(-1, shift); scroll_to_cursor(); draw(); return;
	case XK_Down: move_vertical(1, shift); scroll_to_cursor(); draw(); return;
	case XK_Home: move_line_edge(0, shift); scroll_to_cursor(); draw(); return;
	case XK_End: move_line_edge(1, shift); scroll_to_cursor(); draw(); return;
	case XK_Prior: move_doc_edge(0, shift); scroll_to_cursor(); draw(); return;   /* 簡化：Page Up/Down 先跳文件開頭／結尾 */
	case XK_Next: move_doc_edge(1, shift); scroll_to_cursor(); draw(); return;
	case XK_BackSpace: delete_backward(); scroll_to_cursor(); draw(); return;
	case XK_Delete: delete_forward(); scroll_to_cursor(); draw(); return;
	case XK_Return: case XK_KP_Enter: insert_newline(); scroll_to_cursor(); draw(); return;
	case XK_Tab: if (!shift) { insert_tab_or_move(); scroll_to_cursor(); draw(); } return;
	case XK_ISO_Left_Tab:
		if (doc->block[cursor_pos.block].kind == BLOCK_TABLE) {
			int nb, nr, nc;
			if (doc_prev_unit(doc, cursor_pos.block, cursor_pos.row, cursor_pos.col, &nb, &nr, &nc) && doc->block[nb].kind == BLOCK_TABLE)
				cursor_pos = doc_unit_start(doc, nb, nr, nc);
			draw();
		}
		return;
	case XK_Escape: has_sel = 0; draw(); return;
	}

	if (keylen > 0 && (unsigned char)keybuf[0] >= 0x20) { type_text(keybuf, (size_t)keylen); scroll_to_cursor(); draw(); }
}

/* ------------------------------------------------------------------ */
/* 主迴圈                                                                */
/* ------------------------------------------------------------------ */

static void on_configure(int w, int h)
{
	if (w == ww && h == wh) return;
	ww = w; wh = h;
	layout_view_scale();
	layout_dirty = 1;
}

static cairo_surface_t *icon(int size);

int main(int argc, char *argv[])
{
	if (argc > 2 && !strcmp(argv[1], "--icon")) return pm_ui_write_icon(icon, argv[2]);
	if (argc > 1 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
		printf("%s: pomelo-writer [%s]\n", _("用法"), _("檔案"));
		return 0;
	}
	setlocale(LC_ALL, "");
	app_icon = icon;
	/* Wayland 工作階段（POMELO_BACKEND=x11 可強制走 X）：原生視窗，不開 X 連線 */
	int native = pmwl_connect();
	if (native) { pm_ui_native = 1; a_delete = PMWL_XCLOSE; }
	else if (!(dpy = XOpenDisplay(NULL))) { fprintf(stderr, "pomelo-writer: %s\n", _("無法開啟 X display")); return 1; }
	struct sigaction sa = { .sa_handler = SIG_IGN, .sa_flags = SA_NOCLDWAIT };
	sigaction(SIGCHLD, &sa, NULL);
	pm_conf_load();
	pm_init(pm_conf_get("appearance.font", "Noto Sans CJK TC"), pm_conf_num("appearance.font_size", 13),
	        pm_conf_int("appearance.dark", 0), pm_conf_get("appearance.icon_theme", "Yaru"));
	pm_set_theme(pm_conf_int("appearance.dark", 0), (unsigned)strtoul(pm_conf_get("appearance.accent", "#0a84ff") + 1, NULL, 16));

	doc = doc_new();
	cur_is_odt = 1;
	cursor_pos = doc_start(doc);
	typing_style.size = doc->default_size;
	typing_style.color = COLOR_AUTO;

	if (native) {
		wl = pmwl_window(_("文書處理器"), "pomelo-writer", ww, wh, 480, 360, NULL);
		if (!wl) { fprintf(stderr, "pomelo-writer: %s\n", _("無法開啟 Wayland 視窗")); return 1; }
		win = PMWL_XID(wl);
		static const char *const uri[] = { "text/uri-list" };
		pmwl_dnd_target(wl, uri, 1, PMWL_DND_COPY);   /* 拖檔案進來開啟 */
		set_title();
		layout_view_scale();
		if (argc > 1 && argv[1][0] != '-') load_file(argv[1]);
		goto loop;
	}
	a_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
	a_utf8 = XInternAtom(dpy, "UTF8_STRING", False);
	a_targets = XInternAtom(dpy, "TARGETS", False);
	a_clipboard = XInternAtom(dpy, "CLIPBOARD", False);
	a_paste = XInternAtom(dpy, "POMELO_WRITER_PASTE", False);
	a_netname = XInternAtom(dpy, "_NET_WM_NAME", False);

	XSetWindowAttributes wa = { .background_pixel = 0xffffff, .bit_gravity = NorthWestGravity, .backing_store = WhenMapped,
	                            .event_mask = ExposureMask | KeyPressMask | ButtonPressMask | ButtonReleaseMask |
	                                          PointerMotionMask | LeaveWindowMask | StructureNotifyMask | FocusChangeMask };
	win = XCreateWindow(dpy, pm_root, 0, 0, ww, wh, 0, CopyFromParent, InputOutput, CopyFromParent,
	                    CWBackPixel | CWBitGravity | CWBackingStore | CWEventMask, &wa);
	XClassHint ch = { "pomelo-writer", "Pomelo-writer" };
	XSizeHints *sz = XAllocSizeHints();
	sz->flags = PMinSize;
	sz->min_width = 480; sz->min_height = 360;
	XWMHints wm = { .flags = InputHint, .input = True };
	XSetWMProperties(dpy, win, NULL, NULL, NULL, 0, sz, &wm, &ch);
	XFree(sz);
	XSetWMProtocols(dpy, win, &a_delete, 1);
	long pid = getpid();
	XChangeProperty(dpy, win, XInternAtom(dpy, "_NET_WM_PID", False), XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&pid, 1);
	cairo_surface_t *is = icon(64);
	unsigned long *d = malloc((2 + 64 * 64) * sizeof(unsigned long));
	d[0] = 64; d[1] = 64;
	unsigned *px = (unsigned *)cairo_image_surface_get_data(is);
	for (int i = 0; i < 64 * 64; i++) d[2 + i] = px[i];
	XChangeProperty(dpy, win, XInternAtom(dpy, "_NET_WM_ICON", False), XA_CARDINAL, 32, PropModeReplace, (unsigned char *)d, 2 + 64 * 64);
	free(d);
	cairo_surface_destroy(is);
	set_title();
	static XIM xim;
	pm_xim_attach(dpy, &xim, &xic, win);
	pm_conf_watch(dpy);
	layout_view_scale();
	XMapWindow(dpy, win);
	if (argc > 1 && argv[1][0] != '-') load_file(argv[1]);   /* 視窗建好、set_title() 才有效的 win 可以用 */

loop:;
	int xfd = dpy ? ConnectionNumber(dpy) : -1;
	for (;;) {
		int dirty = 0;
		if (wl) {
			/* 原生：事件處理跟 X 版共用（轉成 XEvent）；游標閃爍每 200 ms 醒來一次 */
			PmwEvent pe;
			int got = pmwl_next(&pe, 200);
			while (got) {
				XEvent ev;
				switch (pe.type) {
				case PMW_CONF:
					pm_set_theme(pm_conf_int("appearance.dark", 0), (unsigned)strtoul(pm_conf_get("appearance.accent", "#0a84ff") + 1, NULL, 16));
					dirty = 1;
					break;
				case PMW_DND_DROP: {   /* 拖進來的檔案：開第一個 */
					char **dropped;
					int nd = pmwl_dnd_files(&dropped);
					if (nd > 0) { load_file(dropped[0]); set_title(); pm_dnd_free(dropped, nd); dirty = 1; }
					break;
				}
				default:
					if (!pmwl_to_xevent(&pe, &ev)) break;
					switch (ev.type) {
					case Expose: dirty = 1; break;
					case ConfigureNotify: on_configure(ev.xconfigure.width, ev.xconfigure.height); dirty = 1; break;
					case KeyPress: handle_key(&ev.xkey); break;
					case ButtonPress: on_button(&ev.xbutton); dirty = 1; break;
					case ButtonRelease: if (ev.xbutton.button == Button1) { drag_sel = 0; ruler_drag = 0; slider_drag = 0; if (doc_pos_cmp(doc, cursor_pos, sel_anchor) == 0) has_sel = 0; } break;
					case MotionNotify: on_motion(&ev.xmotion); if (drag_sel || ruler_drag) dirty = 1; break;
					case ClientMessage: if (ev.xclient.data.l[0] == PMWL_XCLOSE) confirm_close(); break;
					}
				}
				got = pmwl_next(&pe, 0);
			}
			long now = now_ms();
			if (now >= cursor_blink_at) { cursor_visible = !cursor_visible; cursor_blink_at = now + 500; dirty = 1; }
			if (dirty) { set_title(); draw(); }
			continue;
		}
		while (XPending(dpy)) {
			XEvent ev;
			XNextEvent(dpy, &ev);
			if (XFilterEvent(&ev, None)) continue;
			switch (ev.type) {
			case Expose: if (!ev.xexpose.count) dirty = 1; break;
			case ConfigureNotify: on_configure(ev.xconfigure.width, ev.xconfigure.height); dirty = 1; break;
			case KeyPress: handle_key(&ev.xkey); break;
			case ButtonPress: on_button(&ev.xbutton); dirty = 1; break;
			case ButtonRelease: if (ev.xbutton.button == Button1) { drag_sel = 0; ruler_drag = 0; slider_drag = 0; if (doc_pos_cmp(doc, cursor_pos, sel_anchor) == 0) has_sel = 0; } break;
			case MotionNotify: on_motion(&ev.xmotion); if (drag_sel || ruler_drag) dirty = 1; break;
			case SelectionRequest: on_selrequest(&ev.xselectionrequest); break;
			case SelectionNotify: on_selnotify(&ev.xselection); dirty = 1; break;
			case FocusIn: if (xic) XSetICFocus(xic); break;
			case FocusOut: if (xic) XUnsetICFocus(xic); break;
			case ClientMessage: if ((Atom)ev.xclient.data.l[0] == a_delete) confirm_close(); break;
			case MappingNotify: XRefreshKeyboardMapping(&ev.xmapping); break;
			case PropertyNotify:
				if (pm_conf_changed(dpy, &ev)) {
					pm_set_theme(pm_conf_int("appearance.dark", 0), (unsigned)strtoul(pm_conf_get("appearance.accent", "#0a84ff") + 1, NULL, 16));
					dirty = 1;
				}
				break;
			}
		}
		long now = now_ms();
		if (now >= cursor_blink_at) { cursor_visible = !cursor_visible; cursor_blink_at = now + 500; dirty = 1; }
		if (dirty) { set_title(); draw(); }
		struct timeval tv = { 0, 200000 };
		fd_set fds;
		FD_ZERO(&fds);
		FD_SET(xfd, &fds);
		XFlush(dpy);
		if (!XPending(dpy)) select(xfd + 1, &fds, NULL, NULL, &tv);
	}
}

/* ------------------------------------------------------------------ */
/* 圖示                                                                 */
/* ------------------------------------------------------------------ */

static cairo_surface_t *icon(int size)
{
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
	cairo_t *cr = cairo_create(s);
	cairo_scale(cr, size / 128.0, size / 128.0);
	cairo_pattern_t *g = cairo_pattern_create_linear(0, 8, 0, 120);
	cairo_pattern_add_color_stop_rgb(g, 0, 0.16, 0.45, 0.95);
	cairo_pattern_add_color_stop_rgb(g, 1, 0.05, 0.28, 0.80);
	pm_rrect(cr, 18, 8, 92, 112, 14, 14);
	cairo_set_source(cr, g);
	cairo_fill(cr);
	cairo_pattern_destroy(g);
	/* 一張白紙，右上角折角 */
	cairo_move_to(cr, 34, 22);
	cairo_line_to(cr, 78, 22);
	cairo_line_to(cr, 94, 38);
	cairo_line_to(cr, 94, 106);
	cairo_line_to(cr, 34, 106);
	cairo_close_path(cr);
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_fill(cr);
	cairo_move_to(cr, 78, 22);
	cairo_line_to(cr, 78, 38);
	cairo_line_to(cr, 94, 38);
	cairo_close_path(cr);
	cairo_set_source_rgb(cr, 0.85, 0.90, 0.97);
	cairo_fill(cr);
	/* 幾行文字 */
	cairo_set_source_rgb(cr, 0.35, 0.40, 0.50);
	for (int i = 0; i < 4; i++) {
		double y = 50 + i * 11;
		pm_rrect(cr, 42, y, i == 3 ? 28 : 44, 5, 2.5, 2.5);
		cairo_fill(cr);
	}
	cairo_destroy(cr);
	return s;
}

