/* SPDX-License-Identifier: GPL-3.0-or-later */
/* doc.h — pomelo-writer 的文件模型：段落、文字格式、簡單表格、圖片、頁碼。
 *
 * 已知限制（第一版）：表格儲存格裡只能有一個段落（按 Enter 會跳到下一格，不會分段）；
 * 不支援頁首、巢狀表格、追蹤修訂；圖片只能自己佔一整行（不能跟文字同一段混排），
 * 只認得 PNG／JPEG；頁碼只能開關、不能自訂格式或位置。
 */
#ifndef POMELO_WRITER_DOC_H
#define POMELO_WRITER_DOC_H
#include <stddef.h>

typedef enum { ALIGN_LEFT, ALIGN_CENTER, ALIGN_RIGHT, ALIGN_JUSTIFY } Align;
typedef enum { LIST_NONE, LIST_BULLET, LIST_DECIMAL } ListType;
/* 段落樣式：內文或標題 1～3（決定預設字級與粗細；真正的字元樣式〔run〕有設定時優先） */
typedef enum { PSTYLE_BODY, PSTYLE_H1, PSTYLE_H2, PSTYLE_H3 } PStyle;

#define COLOR_AUTO 0xff000001u   /* 「自動」顏色（通常是黑色，但跟著佈景主題） */

typedef struct {
	char *text;             /* UTF-8，不含 NUL 以外的特殊意義；用 len 取長度 */
	size_t len;
	int bold, italic, underline, strike;
	char font[64];           /* 空字串＝跟著文件預設字型 */
	double size;             /* pt；0＝跟著文件預設大小 */
	unsigned color;          /* 0xRRGGBB，或 COLOR_AUTO */
} Run;

typedef struct {
	Run *run; int nrun, runcap;
	Align align;
	ListType list_type;
	int list_level;          /* 0 起算的清單縮排層級 */
	int list_id;              /* 同一個清單（給編號清單算第幾項用）的分組編號；0＝不屬於任何清單群組 */
	PStyle style;              /* 內文／標題 1～3 */
} Para;

typedef struct {
	int nrow, ncol;
	Para *cell;               /* nrow*ncol，列優先；每格剛好一個段落 */
	double *colw;             /* ncol 個欄寬（pt）；NULL＝平均分配 */
} Table;

/* 圖片：自己佔一整個區塊（不能跟文字同一段）；data 是原始檔案位元組（PNG 或 JPEG），
 * 存檔時原封不動寫回去，不會重新編碼。pw/ph 是解碼出來的像素尺寸，w/h 是文件裡顯示的大小（pt）。 */
typedef struct {
	unsigned char *data;
	size_t datalen;
	int is_png;               /* 0 = JPEG，1 = PNG */
	int pw, ph;
	double w, h;
} Image;

typedef enum { BLOCK_PARA, BLOCK_TABLE, BLOCK_IMAGE } BlockKind;

typedef struct {
	BlockKind kind;
	Para para;                /* kind==BLOCK_PARA 時使用 */
	Table table;              /* kind==BLOCK_TABLE 時使用 */
	Image image;               /* kind==BLOCK_IMAGE 時使用 */
} Block;

typedef struct {
	Block *block; int nblock, blockcap;
	double page_w, page_h;                         /* pt（1pt = 1/72 吋） */
	double margin_l, margin_r, margin_t, margin_b;  /* pt */
	char default_font[64];
	double default_size;
	int next_list_id;         /* 配發新清單群組編號用 */
	int page_numbers;          /* 頁尾中間顯示頁碼 */
} Document;

/* 文件裡的一個位置：block 是第幾個區塊；BLOCK_TABLE 時 row/col 指哪一格，否則忽略；
 * offset 是那個段落（連接所有 run 的文字）裡的字元（byte）位移。 */
typedef struct {
	int block, row, col;
	int offset;
} Pos;

/* ---- 建立／釋放 ---- */
Document *doc_new(void);                 /* A4、2.5cm 邊界、預設字型大小 12pt 的空白文件（一個空段落） */
void doc_free(Document *d);
Document *doc_clone(const Document *d);  /* 深層複製，undo/redo 用 */
size_t doc_size_estimate(const Document *d);   /* 粗估占用位元組數，undo 的容量預算用 */

/* ---- 段落存取 ---- */
Para *doc_para_at(Document *d, Pos p);           /* 解析出 p 指到的那個 Para；p 不合法回傳 NULL */
const Para *doc_para_at_c(const Document *d, Pos p);
size_t para_text_len(const Para *p);              /* 段落文字總長度（bytes） */
char *para_text(const Para *p);                   /* 整段文字，呼叫者要 free */
/* 第 offset 個 byte 落在哪個 run、run 內位移多少；offset==總長度時回傳最後一個 run 的尾端 */
int para_run_at(const Para *p, int offset, int *run_offset);

/* ---- 編輯：在段落內 ---- */
/* 在段落的 offset 處插入文字（會依照「樣板」在插入點切分、合併 run）；
 * style 可為 NULL，這時沿用 offset 左邊那個字的樣式（offset==0 時用右邊，都沒有用段落預設） */
void para_insert(Para *p, int offset, const char *text, size_t len, const Run *style);
void para_delete(Para *p, int start, int end);    /* 刪除 [start,end) 的文字 */
/* 把 [start,end) 範圍內的字元樣式都交給 fn 修改一次（fn 可以直接改 Run 的欄位）；
 * 範圍邊界會先切開 run，讓範圍剛好對齊 run 邊界 */
void para_apply_style(Para *p, int start, int end, void (*fn)(Run *r, void *ctx), void *ctx);
void para_init(Para *p);              /* 初始化成「一個空 run」的段落 */
void para_clear(Para *p);             /* 釋放段落內容（run 陣列），之後可以再 para_init */
void para_copy_into(Para *dst, const Para *src);   /* 深層複製（dst 事先不用初始化，會被覆蓋） */

/* ---- 編輯：段落分割與合併（文件層級，處理 Enter／開頭按 Backspace） ---- */
/* 在 p 的 offset 處切開：p 保留前半，回傳後半（新的 Para，呼叫者要自己放進文件或 free） */
Para para_split(Para *p, int offset);
/* 把 src 的內容接到 dst 後面（文字與 run 都接上）；src 接完後內容清空（視為已經轉移） */
void para_append(Para *dst, Para *src);

/* ---- 編輯：文件層級（區塊） ---- */
/* 在 Pos 所在段落插入換行：原段落在 offset 處切開，後半變成新段落插在後面（表格儲存格裡會跳到下一格，見下） */
Pos doc_insert_para_break(Document *d, Pos at);
/* 刪掉第 block 個區塊（段落或整個表格）；表格要刪列／欄用 table_* 函式 */
void doc_delete_block(Document *d, int block);
Pos doc_insert_text(Document *d, Pos at, const char *text, size_t len, const Run *style);   /* 回傳插入後的新位置 */
Pos doc_delete_range(Document *d, Pos a, Pos b);   /* a、b 不分先後；回傳合併後留下的位置 */
int doc_pos_cmp(const Document *d, Pos a, Pos b);  /* a 在 b 前面回傳 -1，之後回傳 1，相同回傳 0 */
Pos doc_pos_clamp(const Document *d, Pos p);        /* 修正越界的 offset／row/col（刪除後常需要） */

/* ---- 在「可編輯單位」之間移動（段落或表格儲存格）：見 doc.h 開頭的位址說明 ---- */
Pos doc_unit_start(const Document *d, int block, int row, int col);   /* 該單位開頭（offset=0） */
Pos doc_unit_end(const Document *d, int block, int row, int col);     /* 該單位結尾（offset=文字長度） */
int doc_next_unit(const Document *d, int block, int row, int col, int *nb, int *nr, int *nc);   /* 沒有下一個回傳 0 */
int doc_prev_unit(const Document *d, int block, int row, int col, int *nb, int *nr, int *nc);
Pos doc_start(const Document *d);
Pos doc_end(const Document *d);

/* ---- 表格 ---- */
int doc_insert_table(Document *d, int block, int nrow, int ncol);   /* 在第 block 個區塊「之後」插入新表格；回傳新表格的 block 編號 */
void table_insert_row(Table *t, int at);     /* at==t->nrow 時加在最後 */
void table_delete_row(Table *t, int at);
void table_insert_col(Table *t, int at);
void table_delete_col(Table *t, int at);

/* ---- 圖片 ---- */
/* 在第 block 個區塊「之後」插入一張圖片；data 的擁有權轉移給文件（之後 doc_free／doc_delete_block 會釋放），
 * 呼叫端不要再用或再 free 它。回傳新圖片的 block 編號。 */
int doc_insert_image(Document *d, int block, unsigned char *data, size_t datalen, int is_png, int pw, int ph, double w, double h);

/* ---- 清單 ---- */
int doc_new_list_id(Document *d);

/* ---- 樣式 ---- */
double pstyle_size(PStyle s, double default_size);   /* 這個樣式預設的字級 */
int pstyle_bold(PStyle s);                            /* 這個樣式預設要不要粗體 */

#endif
