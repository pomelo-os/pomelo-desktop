/* SPDX-License-Identifier: GPL-3.0-or-later */
/* layout.h — 把 Document 排成一頁一頁的版面：換行、分頁、點擊定位、游標座標。
 *
 * 已知限制：不做孤行／寡行控制（段落可能剛好從頁尾斷開）；表格整張要嘛放得下、
 * 要嘛整張換到下一頁（不會把表格的列拆到兩頁）；齊行目前當作靠左對齊，不會真的撐開字距；
 * Tab 一律跳到下一個 36pt（0.5 吋）定位點。
 */
#ifndef POMELO_WRITER_LAYOUT_H
#define POMELO_WRITER_LAYOUT_H
#include "doc.h"
#include <cairo/cairo.h>

typedef struct {
	int block, row, col;        /* 這個視覺行屬於哪個可編輯單位（段落或表格儲存格） */
	int off_start, off_end;      /* 在該段落文字裡的 [start,end) byte 範圍 */
	int page;
	double x, y, w, h, ascent;    /* 頁面座標系，單位 pt；h 含行距，ascent 是基線到行頂端的距離 */
	int is_table_row_first;       /* 這一行是不是它所屬表格某一列的第一行（畫格線用的提示，可忽略） */
} Line;

typedef struct {
	int block;
	int page;
	double x, y, w, h;           /* 整張表格的外框，頁面座標系 */
	double *colx;                 /* ncol+1 個直線 x 位置（相對 x） */
	double *rowy;                 /* nrow+1 個橫線 y 位置（相對 y） */
	int nrow, ncol;
} TableGeom;

typedef struct {
	int block;
	int page;
	double x, y, w, h;           /* 頁面座標系 */
	void *surf;                   /* cairo_surface_t*，layout_build 時解碼好，layout_free 釋放 */
} ImageGeom;

typedef struct {
	Line *line; int nline, linecap;
	TableGeom *tbl; int ntbl, tblcap;
	ImageGeom *img; int nimg, imgcap;
	int npage;
} Layout;

/* 用 cr 量字（不會畫出來，只是借用來源的字型設定）排出整份文件的版面。
 * 呼叫者之後排版寬度、字型若有變動（例如換了預設字型），要重新呼叫一次。 */
Layout *layout_build(cairo_t *cr, const Document *d);
void layout_free(Layout *lo);

/* 第 page 頁某個 (x,y)（頁面座標）最接近哪個文件位置 */
Pos layout_hit_test(const Document *d, const Layout *lo, int page, double x, double y);
/* 文件位置對應的游標座標：page/x/y/h（y 是行頂端）；回傳 0 表示這個位置目前排不出來（文件剛變動還沒重排） */
int layout_pos_xy(const Document *d, const Layout *lo, Pos p, int *page, double *x, double *y, double *h);

/* 畫出第 page 頁的內容（文字、表格格線、清單符號／編號）。
 * sel_a／sel_b 有選取範圍時畫反白；cursor 非 NULL 時畫游標（cursor_on=0 時不畫，閃爍用）。 */
void layout_draw_page(cairo_t *cr, const Document *d, const Layout *lo, int page,
                      const Pos *sel_a, const Pos *sel_b, const Pos *cursor, int cursor_on,
                      unsigned text_color, unsigned sel_color, unsigned cursor_color, unsigned grid_color);

#endif
