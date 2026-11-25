/* SPDX-License-Identifier: GPL-3.0-or-later */
/* eng-anthy.c — 日文（Anthy，dlopen 載入）
 *
 * 打羅馬字先變成平假名（這個檔案自己的對照表，跟一般日文輸入法一樣：konnnichiha → こんにちは、
 * kk → っk、nn → ん），按空白鍵交給 Anthy 轉成漢字。轉換時：
 *   空白鍵／↓ 下一個候選字、↑ 上一個，數字鍵直接選（選完最後一段就送出），Enter 送出，
 *   ←→ 換一段，Shift+←→ 把這一段縮短／加長，Esc 或 Backspace 回到假名。
 *   F6 平假名、F7 片假名（打字時整段，轉換時這一段）。
 * Debian、Ubuntu 是 anthy（libanthy.so.1），Fedora、Rocky 是 anthy-unicode（libanthy-unicode.so.0），函式一樣。
 */
#define _GNU_SOURCE
#include "engine.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <X11/keysym.h>

/* anthy.h 裡用到的部分 */
typedef struct anthy_context *anthy_context_t;
struct anthy_conv_stat { int nr_segment; };
struct anthy_segment_stat { int nr_candidate; int seg_len; };
#define NTH_KATAKANA_CANDIDATE -2
#define NTH_HIRAGANA_CANDIDATE -3
#define ANTHY_UTF8_ENCODING 2

static int (*a_init)(void);
static anthy_context_t (*a_create)(void);
static void (*a_release)(anthy_context_t);
static void (*a_reset)(anthy_context_t);
static int (*a_set_string)(anthy_context_t, const char *);
static void (*a_resize)(anthy_context_t, int, int);
static int (*a_get_stat)(anthy_context_t, struct anthy_conv_stat *);
static int (*a_get_seg_stat)(anthy_context_t, int, struct anthy_segment_stat *);
static int (*a_get_segment)(anthy_context_t, int, int, char *, int);
static int (*a_commit_segment)(anthy_context_t, int, int);
static int (*a_set_encoding)(anthy_context_t, int);

#define MAXSEG 64
typedef struct {
	anthy_context_t ac;
	char kana[512];      /* 打好的假名（平假名；英文字母、數字照原樣） */
	char rom[8];         /* 還沒湊成假名的羅馬字 */
	int kata;            /* F7：整段用片假名 */
	int conv, nseg, cur; /* 轉換中、共幾段、目前在第幾段 */
	int sel[MAXSEG];     /* 每段選第幾個候選字（NTH_… 是平假名、片假名） */
	char out[1024];
} Anthy;

/* ---- 羅馬字 → 平假名 ---- */
static const char *const romaji[][2] = {
	{ "a", "あ" }, { "i", "い" }, { "u", "う" }, { "e", "え" }, { "o", "お" },
	{ "ka", "か" }, { "ki", "き" }, { "ku", "く" }, { "ke", "け" }, { "ko", "こ" },
	{ "sa", "さ" }, { "si", "し" }, { "shi", "し" }, { "su", "す" }, { "se", "せ" }, { "so", "そ" },
	{ "ta", "た" }, { "ti", "ち" }, { "chi", "ち" }, { "tu", "つ" }, { "tsu", "つ" }, { "te", "て" }, { "to", "と" },
	{ "na", "な" }, { "ni", "に" }, { "nu", "ぬ" }, { "ne", "ね" }, { "no", "の" },
	{ "ha", "は" }, { "hi", "ひ" }, { "hu", "ふ" }, { "fu", "ふ" }, { "he", "へ" }, { "ho", "ほ" },
	{ "ma", "ま" }, { "mi", "み" }, { "mu", "む" }, { "me", "め" }, { "mo", "も" },
	{ "ya", "や" }, { "yi", "い" }, { "yu", "ゆ" }, { "ye", "いぇ" }, { "yo", "よ" },
	{ "ra", "ら" }, { "ri", "り" }, { "ru", "る" }, { "re", "れ" }, { "ro", "ろ" },
	{ "wa", "わ" }, { "wi", "うぃ" }, { "wu", "う" }, { "we", "うぇ" }, { "wo", "を" },
	{ "nn", "ん" }, { "n'", "ん" }, { "xn", "ん" },
	{ "ga", "が" }, { "gi", "ぎ" }, { "gu", "ぐ" }, { "ge", "げ" }, { "go", "ご" },
	{ "za", "ざ" }, { "zi", "じ" }, { "ji", "じ" }, { "zu", "ず" }, { "ze", "ぜ" }, { "zo", "ぞ" },
	{ "da", "だ" }, { "di", "ぢ" }, { "du", "づ" }, { "de", "で" }, { "do", "ど" },
	{ "ba", "ば" }, { "bi", "び" }, { "bu", "ぶ" }, { "be", "べ" }, { "bo", "ぼ" },
	{ "pa", "ぱ" }, { "pi", "ぴ" }, { "pu", "ぷ" }, { "pe", "ぺ" }, { "po", "ぽ" },
	{ "ca", "か" }, { "ci", "し" }, { "cu", "く" }, { "ce", "せ" }, { "co", "こ" },
	{ "la", "ぁ" }, { "li", "ぃ" }, { "lu", "ぅ" }, { "le", "ぇ" }, { "lo", "ぉ" },
	{ "xa", "ぁ" }, { "xi", "ぃ" }, { "xu", "ぅ" }, { "xe", "ぇ" }, { "xo", "ぉ" },
	{ "xya", "ゃ" }, { "xyu", "ゅ" }, { "xyo", "ょ" }, { "lya", "ゃ" }, { "lyu", "ゅ" }, { "lyo", "ょ" },
	{ "xtu", "っ" }, { "xtsu", "っ" }, { "ltu", "っ" }, { "ltsu", "っ" }, { "xwa", "ゎ" }, { "lwa", "ゎ" },
	{ "xka", "ゕ" }, { "xke", "ゖ" },
	{ "kya", "きゃ" }, { "kyi", "きぃ" }, { "kyu", "きゅ" }, { "kye", "きぇ" }, { "kyo", "きょ" },
	{ "gya", "ぎゃ" }, { "gyi", "ぎぃ" }, { "gyu", "ぎゅ" }, { "gye", "ぎぇ" }, { "gyo", "ぎょ" },
	{ "sya", "しゃ" }, { "syi", "しぃ" }, { "syu", "しゅ" }, { "sye", "しぇ" }, { "syo", "しょ" },
	{ "sha", "しゃ" }, { "shu", "しゅ" }, { "she", "しぇ" }, { "sho", "しょ" },
	{ "zya", "じゃ" }, { "zyi", "じぃ" }, { "zyu", "じゅ" }, { "zye", "じぇ" }, { "zyo", "じょ" },
	{ "ja", "じゃ" }, { "ju", "じゅ" }, { "je", "じぇ" }, { "jo", "じょ" },
	{ "jya", "じゃ" }, { "jyi", "じぃ" }, { "jyu", "じゅ" }, { "jye", "じぇ" }, { "jyo", "じょ" },
	{ "tya", "ちゃ" }, { "tyi", "ちぃ" }, { "tyu", "ちゅ" }, { "tye", "ちぇ" }, { "tyo", "ちょ" },
	{ "cha", "ちゃ" }, { "chu", "ちゅ" }, { "che", "ちぇ" }, { "cho", "ちょ" },
	{ "cya", "ちゃ" }, { "cyi", "ちぃ" }, { "cyu", "ちゅ" }, { "cye", "ちぇ" }, { "cyo", "ちょ" },
	{ "dya", "ぢゃ" }, { "dyi", "ぢぃ" }, { "dyu", "ぢゅ" }, { "dye", "ぢぇ" }, { "dyo", "ぢょ" },
	{ "tsa", "つぁ" }, { "tsi", "つぃ" }, { "tse", "つぇ" }, { "tso", "つぉ" },
	{ "tha", "てゃ" }, { "thi", "てぃ" }, { "thu", "てゅ" }, { "the", "てぇ" }, { "tho", "てょ" },
	{ "dha", "でゃ" }, { "dhi", "でぃ" }, { "dhu", "でゅ" }, { "dhe", "でぇ" }, { "dho", "でょ" },
	{ "twu", "とぅ" }, { "dwu", "どぅ" },
	{ "nya", "にゃ" }, { "nyi", "にぃ" }, { "nyu", "にゅ" }, { "nye", "にぇ" }, { "nyo", "にょ" },
	{ "hya", "ひゃ" }, { "hyi", "ひぃ" }, { "hyu", "ひゅ" }, { "hye", "ひぇ" }, { "hyo", "ひょ" },
	{ "bya", "びゃ" }, { "byi", "びぃ" }, { "byu", "びゅ" }, { "bye", "びぇ" }, { "byo", "びょ" },
	{ "pya", "ぴゃ" }, { "pyi", "ぴぃ" }, { "pyu", "ぴゅ" }, { "pye", "ぴぇ" }, { "pyo", "ぴょ" },
	{ "mya", "みゃ" }, { "myi", "みぃ" }, { "myu", "みゅ" }, { "mye", "みぇ" }, { "myo", "みょ" },
	{ "rya", "りゃ" }, { "ryi", "りぃ" }, { "ryu", "りゅ" }, { "rye", "りぇ" }, { "ryo", "りょ" },
	{ "fa", "ふぁ" }, { "fi", "ふぃ" }, { "fe", "ふぇ" }, { "fo", "ふぉ" },
	{ "fya", "ふゃ" }, { "fyu", "ふゅ" }, { "fyo", "ふょ" },
	{ "va", "ゔぁ" }, { "vi", "ゔぃ" }, { "vu", "ゔ" }, { "ve", "ゔぇ" }, { "vo", "ゔぉ" },
	{ "wha", "うぁ" }, { "whi", "うぃ" }, { "whe", "うぇ" }, { "who", "うぉ" },
	{ "qa", "くぁ" }, { "qi", "くぃ" }, { "qe", "くぇ" }, { "qo", "くぉ" }, { "kwa", "くぁ" }, { "gwa", "ぐぁ" },
};

/* 標點：打字時換成全形 */
static const char *punct(int c)
{
	switch (c) {
	case ',': return "、";
	case '.': return "。";
	case '-': return "ー";
	case '[': return "「";
	case ']': return "」";
	case '/': return "・";
	case '~': return "～";
	case '!': return "！";
	case '?': return "？";
	}
	return NULL;
}

static int load(void)
{
	static int state;   /* 0 還沒試、1 可以、2 找不到 */
	if (state) return state == 1;
	state = 2;
	static const char *libs[] = { "libanthy.so.1", "libanthy-unicode.so.0", "libanthy-unicode.so.1" };
	void *h = NULL;
	for (unsigned i = 0; i < sizeof libs / sizeof libs[0] && !h; i++) h = dlopen(libs[i], RTLD_NOW | RTLD_LOCAL);
	if (!h) return 0;
	*(void **)&a_init = dlsym(h, "anthy_init");
	*(void **)&a_create = dlsym(h, "anthy_create_context");
	*(void **)&a_release = dlsym(h, "anthy_release_context");
	*(void **)&a_reset = dlsym(h, "anthy_reset_context");
	*(void **)&a_set_string = dlsym(h, "anthy_set_string");
	*(void **)&a_resize = dlsym(h, "anthy_resize_segment");
	*(void **)&a_get_stat = dlsym(h, "anthy_get_stat");
	*(void **)&a_get_seg_stat = dlsym(h, "anthy_get_segment_stat");
	*(void **)&a_get_segment = dlsym(h, "anthy_get_segment");
	*(void **)&a_commit_segment = dlsym(h, "anthy_commit_segment");
	*(void **)&a_set_encoding = dlsym(h, "anthy_context_set_encoding");
	if (!a_init || !a_create || !a_release || !a_reset || !a_set_string || !a_resize || !a_get_stat
	    || !a_get_seg_stat || !a_get_segment || !a_commit_segment) return 0;
	if (a_init() < 0) return 0;   /* 例如找不到字典 */
	state = 1;
	return 1;
}

static void *create(void)
{
	Anthy *a = calloc(1, sizeof *a);
	if (!a) return NULL;
	a->ac = a_create();
	if (!a->ac) { free(a); return NULL; }
	if (a_set_encoding) a_set_encoding(a->ac, ANTHY_UTF8_ENCODING);
	return a;
}

static void destroy(void *c)
{
	Anthy *a = c;
	if (!a) return;
	a_release(a->ac);
	free(a);
}

static void conf(void *c) { (void)c; }

static void kana_add(Anthy *a, const char *s)
{
	size_t o = strlen(a->kana);
	if (o + strlen(s) < sizeof a->kana) memcpy(a->kana + o, s, strlen(s) + 1);
}

/* 羅馬字湊不成假名的部分：n → ん，其他照原樣 */
static void rom_flush(Anthy *a)
{
	if (!strcmp(a->rom, "n")) kana_add(a, "ん");
	else kana_add(a, a->rom);
	a->rom[0] = 0;
}

static int is_vowel(int c) { return c && strchr("aiueo", c); }

/* 加一個小寫字母（或 '）到羅馬字，能湊成假名的就換掉 */
static void rom_add(Anthy *a, int ch)
{
	size_t n = strlen(a->rom);
	if (n + 1 >= sizeof a->rom) rom_flush(a), n = 0;
	a->rom[n] = (char)ch;
	a->rom[n + 1] = 0;
	for (;;) {
		size_t len = strlen(a->rom);
		if (!len) return;
		int prefix = 0;
		for (unsigned i = 0; i < sizeof romaji / sizeof romaji[0]; i++) {
			if (!strcmp(romaji[i][0], a->rom)) { kana_add(a, romaji[i][1]); a->rom[0] = 0; return; }
			if (!strncmp(romaji[i][0], a->rom, len)) prefix = 1;
		}
		if (prefix) return;   /* 還要再打 */
		char c0 = a->rom[0], c1 = a->rom[1];
		if (c0 == 'n' && c1 && !is_vowel(c1) && c1 != 'y') kana_add(a, "ん");   /* kanji：n 後面接子音 */
		else if (c1 == c0 && !is_vowel(c0)) kana_add(a, "っ");                 /* kitte：重複的子音 */
		else { char t[2] = { c0, 0 }; kana_add(a, t); }                          /* 湊不成：照原樣 */
		memmove(a->rom, a->rom + 1, len);
	}
}

/* 去掉最後一個 UTF-8 字 */
static void pop_char(char *s)
{
	size_t n = strlen(s);
	while (n > 0 && ((unsigned char)s[n - 1] & 0xc0) == 0x80) n--;
	if (n > 0) n--;
	s[n] = 0;
}

/* 平假名 → 片假名 */
static void to_katakana(const char *in, char *out, size_t n)
{
	size_t o = 0;
	for (const unsigned char *p = (const unsigned char *)in; *p && o + 4 < n;) {
		/* ぁ（U+3041）～ゖ（U+3096）：E3 81 81 ～ E3 82 96 */
		if (p[0] == 0xe3 && (p[1] == 0x81 || p[1] == 0x82) && p[2]) {
			unsigned cp = 0x3000 | (unsigned)(p[1] & 0x3f) << 6 | (p[2] & 0x3f);
			if (cp >= 0x3041 && cp <= 0x3096) cp += 0x60;
			out[o++] = (char)0xe3;
			out[o++] = (char)(0x80 | (cp >> 6 & 0x3f));
			out[o++] = (char)(0x80 | (cp & 0x3f));
			p += 3;
		} else out[o++] = (char)*p++;
	}
	out[o] = 0;
}

static void emit(Anthy *a, const char *s)
{
	size_t o = strlen(a->out);
	snprintf(a->out + o, sizeof a->out - o, "%s", s);
}

static void clear(Anthy *a)
{
	a->kana[0] = a->rom[0] = 0;
	a->kata = a->conv = a->nseg = a->cur = 0;
}

static int seg_cands(Anthy *a, int i)
{
	struct anthy_segment_stat ss;
	return a_get_seg_stat(a->ac, i, &ss) ? 0 : ss.nr_candidate;
}

static void seg_text(Anthy *a, int i, char *buf, int n)
{
	if (a_get_segment(a->ac, i, a->sel[i], buf, n) < 0) buf[0] = 0;
}

/* 重新看有幾段（開始轉換、改段落長度之後）；from 以後的段落選回第一個候選字 */
static void segs(Anthy *a, int from)
{
	struct anthy_conv_stat cs;
	a->nseg = a_get_stat(a->ac, &cs) ? 0 : cs.nr_segment;
	if (a->nseg > MAXSEG) a->nseg = MAXSEG;
	for (int i = from; i < a->nseg; i++) a->sel[i] = 0;
	if (a->cur >= a->nseg) a->cur = a->nseg ? a->nseg - 1 : 0;
}

static int start_conv(Anthy *a)
{
	rom_flush(a);
	if (!a->kana[0]) return 0;
	if (a_set_string(a->ac, a->kana)) return 0;
	a->cur = 0;
	segs(a, 0);
	if (!a->nseg) return 0;
	a->conv = 1;
	return 1;
}

/* 送出：轉換中送出每一段（也讓 Anthy 學起來），否則送出假名 */
static void commit_all(Anthy *a)
{
	if (a->conv) {
		char buf[512];
		for (int i = 0; i < a->nseg; i++) {
			seg_text(a, i, buf, sizeof buf);
			emit(a, buf);
		}
		for (int i = 0; i < a->nseg; i++) if (a->sel[i] >= 0) a_commit_segment(a->ac, i, a->sel[i]);
		a_reset(a->ac);
	} else {
		rom_flush(a);
		if (a->kata) {
			char k[sizeof a->kana];
			to_katakana(a->kana, k, sizeof k);
			emit(a, k);
		} else emit(a, a->kana);
	}
	clear(a);
}

/* 選這一段的第 idx 個；選完最後一段就整句送出，否則跳到下一段 */
static void pick(Anthy *a, int idx)
{
	int n = seg_cands(a, a->cur);
	if (idx < 0 || idx >= n) return;
	a->sel[a->cur] = idx;
	if (a->cur + 1 < a->nseg) a->cur++;
	else commit_all(a);
}

static int key_conv(Anthy *a, unsigned ks, unsigned state)
{
	int n = seg_cands(a, a->cur), per = conf_cand_n, *s = &a->sel[a->cur];
	switch (ks) {
	case XK_space: case XK_Down: case XK_KP_Down: case XK_Henkan_Mode:
		*s = *s < 0 || *s + 1 >= n ? 0 : *s + 1;
		return 1;
	case XK_Up: case XK_KP_Up:
		*s = *s <= 0 ? n - 1 : *s - 1;
		return 1;
	case XK_Page_Down: case XK_KP_Page_Down:
		*s = *s < 0 ? 0 : (*s / per + 1) * per < n ? (*s / per + 1) * per : 0;
		return 1;
	case XK_Page_Up: case XK_KP_Page_Up:
		*s = *s < per ? 0 : (*s / per - 1) * per;
		return 1;
	case XK_Left: case XK_KP_Left:
		if (state & 1) { a_resize(a->ac, a->cur, -1); segs(a, a->cur); }
		else if (a->cur > 0) a->cur--;
		return 1;
	case XK_Right: case XK_KP_Right:
		if (state & 1) { a_resize(a->ac, a->cur, 1); segs(a, a->cur); }
		else if (a->cur + 1 < a->nseg) a->cur++;
		return 1;
	case XK_Return: case XK_KP_Enter:
		commit_all(a);
		return 1;
	case XK_Escape: case XK_BackSpace:   /* 回到假名 */
		a->conv = 0;
		a_reset(a->ac);
		return 1;
	case XK_F6: *s = NTH_HIRAGANA_CANDIDATE; return 1;
	case XK_F7: *s = NTH_KATAKANA_CANDIDATE; return 1;
	}
	if (ks >= '1' && ks <= '9') {   /* 選這一頁的第幾個 */
		int first = (*s < 0 ? 0 : *s / per) * per, i = (int)(ks - '1');
		if (i < per) pick(a, first + i);
		return 1;
	}
	return -1;   /* 其他鍵：先送出再當一般的鍵處理 */
}

static int key(void *c, unsigned ks, unsigned state)
{
	Anthy *a = c;
	int composing = a->kana[0] || a->rom[0];
	if (state & (4 | 8 | 64)) {   /* Ctrl、Alt、Super 組合鍵：先送出，鍵交給程式 */
		if (composing || a->conv) commit_all(a);
		return 0;
	}
	if (a->conv) {
		int r = key_conv(a, ks, state);
		if (r >= 0) return r;
		commit_all(a);
		composing = 0;
	}
	/* 字母：Shift 決定大小寫（不管 Caps Lock）；大寫照原樣放進去 */
	if ((ks >= 'a' && ks <= 'z') || (ks >= 'A' && ks <= 'Z')) {
		int ch = (int)(ks | 0x20);
		if (state & 1) { rom_flush(a); char t[2] = { (char)(ch - 32), 0 }; kana_add(a, t); }
		else rom_add(a, ch);
		return 1;
	}
	if (ks == '\'' && !strcmp(a->rom, "n")) { rom_add(a, '\''); return 1; }
	const char *p = ks < 0x80 ? punct((int)ks) : NULL;
	if (p) { rom_flush(a); kana_add(a, p); return 1; }
	if (composing && ks >= 0x21 && ks < 0x7f) {   /* 數字、其他符號：正在打字就放進去，不然交給程式 */
		rom_flush(a);
		char t[2] = { (char)ks, 0 };
		kana_add(a, t);
		return 1;
	}
	if (!composing) return 0;
	switch (ks) {
	case XK_space: case XK_Henkan_Mode: start_conv(a); return 1;
	case XK_Return: case XK_KP_Enter: commit_all(a); return 1;
	case XK_BackSpace:
		if (a->rom[0]) a->rom[strlen(a->rom) - 1] = 0;
		else pop_char(a->kana);
		if (!a->kana[0] && !a->rom[0]) a->kata = 0;
		return 1;
	case XK_Escape: clear(a); return 1;
	case XK_F6: a->kata = 0; return 1;
	case XK_F7: a->kata = 1; return 1;
	case XK_Left: case XK_Right: case XK_Up: case XK_Down: case XK_Home: case XK_End: case XK_Tab:
		return 1;   /* 打到一半：不要移動程式裡的游標 */
	}
	return 0;
}

static void view(void *c, View *v)
{
	Anthy *a = c;
	if (!a->conv) {
		if (a->kata) to_katakana(a->kana, v->comp, sizeof v->comp);
		else snprintf(v->comp, sizeof v->comp, "%s", a->kana);
		size_t o = strlen(v->comp);
		snprintf(v->comp + o, sizeof v->comp - o, "%s", a->rom);
		return;
	}
	char buf[512];
	for (int i = 0; i < a->nseg; i++) {
		seg_text(a, i, buf, sizeof buf);
		char *dst = i < a->cur ? v->pre : i == a->cur ? v->comp : v->post;
		size_t cap = i < a->cur ? sizeof v->pre : i == a->cur ? sizeof v->comp : sizeof v->post, o = strlen(dst);
		snprintf(dst + o, cap - o, "%s", buf);
	}
	int n = seg_cands(a, a->cur), per = conf_cand_n, s = a->sel[a->cur];
	int page = s < 0 ? 0 : s / per;
	for (int i = page * per; i < n && v->ncand < per && v->ncand < CAND_MAX; i++) {
		if (a_get_segment(a->ac, a->cur, i, v->cand[v->ncand], sizeof v->cand[0]) < 0) v->cand[v->ncand][0] = 0;
		v->ncand++;
	}
	v->sel = s < 0 ? -1 : s - page * per;
	v->page = page;
	v->npage = (n + per - 1) / per;
	if (a->nseg > 1) snprintf(v->aux, sizeof v->aux, "%s", tr(N_("←→ 換一段　Shift+←→ 調整這一段的長度")));
}

static int commit(void *c, char *out, size_t n)
{
	Anthy *a = c;
	if (!a->out[0]) return 0;
	snprintf(out, n, "%s", a->out);
	a->out[0] = 0;
	return 1;
}

static void flush(void *c)
{
	Anthy *a = c;
	if (a->conv || a->kana[0] || a->rom[0]) commit_all(a);
}

static void reset(void *c)
{
	Anthy *a = c;
	if (a->conv) a_reset(a->ac);
	clear(a);
	a->out[0] = 0;
}

static void choose(void *c, int i)
{
	Anthy *a = c;
	if (!a->conv) return;
	int s = a->sel[a->cur];
	pick(a, (s < 0 ? 0 : s / conf_cand_n) * conf_cand_n + i);
}

const Engine eng_anthy = { "anthy", N_("日文"), "あ", load, create, destroy, conf, key, view, commit, flush, reset, choose };
