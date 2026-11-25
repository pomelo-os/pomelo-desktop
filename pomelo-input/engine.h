/* SPDX-License-Identifier: GPL-3.0-or-later */
/* engine.h — pomelo-input 的輸入法引擎
 *
 * 注音（libchewing，直接連結）；韓文（libhangul）、拼音／倉頡／速成／粵拼（librime）、日文（Anthy）用 dlopen 載入，
 * 系統沒裝那個函式庫就不能選；越南文（Telex／VNI）是自己寫的規則，不需要函式庫。
 */
#ifndef POMELO_INPUT_ENGINE_H
#define POMELO_INPUT_ENGINE_H
#include <stddef.h>

#ifndef N_
#define N_(s) s   /* 標記要翻譯的字（顯示時由選單列、設定頁翻譯） */
#endif
#define CAND_MAX 10

/* 選字列要畫的東西 */
typedef struct {
	char pre[512];             /* 組字區：游標前已轉好的字 */
	char comp[160];            /* 正在拼的（強調色、底線）：注音、韓文字母、羅馬字… */
	char post[512];            /* 游標後的字 */
	char cand[CAND_MAX][96];
	char label[CAND_MAX][8];   /* 候選字前面的標籤；空的就用 1、2、3… */
	char note[CAND_MAX][48];   /* 候選字後面的小字（讀音、補碼提示），灰色 */
	int ncand, page, npage, sel;   /* npage = 0：不知道總共幾頁（看 more） */
	int more;                  /* 後面還有下一頁 */
	char aux[160];             /* 最下面的說明（例如「正在準備拼音…」） */
} View;

typedef struct Engine {
	const char *id;            /* 設定裡的名字（input.engines） */
	const char *name;          /* 顯示的名稱（要翻譯） */
	const char *badge;         /* 選單列上的一個字：注、한、拼… */
	int (*load)(void);         /* 1 = 可以用（函式庫找得到） */
	void *(*create)(void);
	void (*destroy)(void *);
	void (*conf)(void *);      /* 設定改了 */
	/* 按下一個鍵（ks 是 X 的 keysym，state 是 X 的修飾鍵）：1 = 吃掉，0 = 原樣交給程式。
	 * 不管吃不吃，之後都會先拿 commit() 的字送出去，再把沒吃掉的鍵轉給程式 */
	int (*key)(void *, unsigned ks, unsigned state);
	void (*view)(void *, View *);
	int (*commit)(void *, char *out, size_t n);   /* 有要送出的字：放進 out，回傳 1（取一次就清掉） */
	void (*flush)(void *);     /* 組字區直接送出（切換輸入法、切到英文時；之後用 commit() 取） */
	void (*reset)(void *);     /* 清掉組字區 */
	void (*choose)(void *, int i);   /* 點選這一頁第 i 個候選字 */
} Engine;

extern const Engine eng_chewing, eng_hangul, eng_rime_pinyin, eng_rime_cangjie, eng_rime_quick, eng_rime_jyutping, eng_anthy, eng_viet;

/* 共用的設定（pomelo-input.c 讀 settings.conf） */
extern char conf_kb[32], conf_hangul_kb[16], conf_viet[8];
extern int conf_space_select, conf_cand_n;

/* 翻譯（pomelo-input.c；字串要用 N_() 標記，翻譯檔才收得到） */
const char *tr(const char *s);

/* UTF-32 → UTF-8（附加在 out 後面） */
void utf8_append(char *out, size_t n, const unsigned *s);

#endif
