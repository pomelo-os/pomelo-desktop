/* SPDX-License-Identifier: GPL-3.0-or-later */
/* eng-viet.c — 越南文（Telex、VNI，自己寫的規則，不需要函式庫）
 *
 * 一個字（音節）打的時候放在組字區，打空白、標點、Enter 這些不是字母的鍵時送出。
 *   Telex：aa â、aw ă、ee ê、oo ô、ow ơ、uw ư、w ư、dd đ；聲調 s 銳、f 玄、r 問、x 跌、j 重，z 去掉聲調
 *   VNI：  6 â ê ô、7 ơ ư、8 ă、9 đ；聲調 1 銳、2 玄、3 問、4 跌、5 重，0 去掉聲調
 * 同一個鍵按兩次就取消（ass → as、aaa → aa）。聲調放在哪個母音照傳統的規則（hòa、thủy、toán、người）。
 * Backspace 刪掉最後一個字母，聲調會跟著移到該放的地方（việt → việ）。
 */
#include "engine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <X11/keysym.h>

enum { M_NONE, M_HAT, M_BREVE, M_HORN, M_STROKE };   /* â ê ô、ă、ơ ư、đ */
typedef struct { char ch; char up, mod, from_w; } L;    /* ch：小寫的英文字母 */
typedef struct { L l[32]; int n, tone; char out[512]; } Viet;

/* 母音（含 â、ă…）× 聲調（無、銳、玄、問、跌、重） */
static const char *const vtab[12][6] = {
	{ "a", "á", "à", "ả", "ã", "ạ" }, { "ă", "ắ", "ằ", "ẳ", "ẵ", "ặ" }, { "â", "ấ", "ầ", "ẩ", "ẫ", "ậ" },
	{ "e", "é", "è", "ẻ", "ẽ", "ẹ" }, { "ê", "ế", "ề", "ể", "ễ", "ệ" }, { "i", "í", "ì", "ỉ", "ĩ", "ị" },
	{ "o", "ó", "ò", "ỏ", "õ", "ọ" }, { "ô", "ố", "ồ", "ổ", "ỗ", "ộ" }, { "ơ", "ớ", "ờ", "ở", "ỡ", "ợ" },
	{ "u", "ú", "ù", "ủ", "ũ", "ụ" }, { "ư", "ứ", "ừ", "ử", "ữ", "ự" }, { "y", "ý", "ỳ", "ỷ", "ỹ", "ỵ" },
};
static const char *const vtab_up[12][6] = {
	{ "A", "Á", "À", "Ả", "Ã", "Ạ" }, { "Ă", "Ắ", "Ằ", "Ẳ", "Ẵ", "Ặ" }, { "Â", "Ấ", "Ầ", "Ẩ", "Ẫ", "Ậ" },
	{ "E", "É", "È", "Ẻ", "Ẽ", "Ẹ" }, { "Ê", "Ế", "Ề", "Ể", "Ễ", "Ệ" }, { "I", "Í", "Ì", "Ỉ", "Ĩ", "Ị" },
	{ "O", "Ó", "Ò", "Ỏ", "Õ", "Ọ" }, { "Ô", "Ố", "Ồ", "Ổ", "Ỗ", "Ộ" }, { "Ơ", "Ớ", "Ờ", "Ở", "Ỡ", "Ợ" },
	{ "U", "Ú", "Ù", "Ủ", "Ũ", "Ụ" }, { "Ư", "Ứ", "Ừ", "Ử", "Ữ", "Ự" }, { "Y", "Ý", "Ỳ", "Ỷ", "Ỹ", "Ỵ" },
};

static int vowel(char c) { return c && strchr("aeiouy", c); }

static int vidx(const L *x)
{
	switch (x->ch) {
	case 'a': return x->mod == M_BREVE ? 1 : x->mod == M_HAT ? 2 : 0;
	case 'e': return x->mod == M_HAT ? 4 : 3;
	case 'i': return 5;
	case 'o': return x->mod == M_HAT ? 7 : x->mod == M_HORN ? 8 : 6;
	case 'u': return x->mod == M_HORN ? 10 : 9;
	case 'y': return 11;
	}
	return -1;
}

/* 母音群：qu、gi 的 u、i 算子音（quý、giá），回傳開始的位置，*end 是結束（不含） */
static int cluster(const Viet *v, int *end)
{
	int s = 0;
	while (s < v->n && !vowel(v->l[s].ch)) s++;
	if (s >= v->n) return -1;
	if (s == 1 && v->n > 2 && vowel(v->l[2].ch) && ((v->l[0].ch == 'q' && v->l[1].ch == 'u') || (v->l[0].ch == 'g' && v->l[1].ch == 'i'))) s = 2;
	int e = s;
	while (e < v->n && vowel(v->l[e].ch)) e++;
	*end = e;
	return s;
}

/* 聲調放在哪一個 */
static int tone_pos(const Viet *v)
{
	int e, s = cluster(v, &e);
	if (s < 0) return -1;
	int pick = -1;
	for (int i = s; i < e; i++) if (v->l[i].mod) pick = i;   /* 有 â ê ô ă ơ ư 的放那裡（ươ 放 ơ） */
	if (pick >= 0) return pick;
	int len = e - s;
	if (len == 1) return s;
	if (len >= 3) return s + 1;                 /* khoái、khuỷu */
	return e < v->n ? s + 1 : s;               /* 後面有子音：toán；沒有：hòa、thủy、của */
}

static void render(const Viet *v, char *out, size_t n)
{
	int tp = v->tone ? tone_pos(v) : -1;
	size_t o = 0;
	out[0] = 0;
	for (int i = 0; i < v->n; i++) {
		const L *x = &v->l[i];
		char one[2] = { x->up ? (char)(x->ch - 32) : x->ch, 0 };
		const char *s = one;
		int k = vidx(x);
		if (k >= 0) s = (x->up ? vtab_up : vtab)[k][i == tp ? v->tone : 0];
		else if (x->ch == 'd' && x->mod == M_STROKE) s = x->up ? "Đ" : "đ";
		size_t len = strlen(s);
		if (o + len + 1 > n) break;
		memcpy(out + o, s, len + 1);
		o += len;
	}
}

static int load(void) { return 1; }
static void *create(void) { return calloc(1, sizeof(Viet)); }
static void destroy(void *c) { free(c); }
static void conf(void *c) { (void)c; }

static int has_vowel(const Viet *v)
{
	for (int i = 0; i < v->n; i++) if (vowel(v->l[i].ch)) return 1;
	return 0;
}

static void add(Viet *v, char ch, int up, int mod, int from_w)
{
	if (v->n >= (int)(sizeof v->l / sizeof v->l[0])) return;
	v->l[v->n++] = (L){ ch, (char)up, (char)mod, (char)from_w };
}

/* 送出這個字 */
static void commit_word(Viet *v)
{
	if (!v->n) return;
	char w[256];
	render(v, w, sizeof w);
	size_t o = strlen(v->out);
	snprintf(v->out + o, sizeof v->out - o, "%s", w);
	v->n = v->tone = 0;
}

/* 聲調鍵：同一個再按一次就取消，並把鍵當字母打出來 */
static int set_tone(Viet *v, int t, char ch, int up)
{
	if (!has_vowel(v)) return 0;
	if (v->tone == t) { v->tone = 0; add(v, ch, up, 0, 0); }
	else v->tone = t;
	return 1;
}

/* 最後一個母音 base（還沒加符號、或已經是 mod）：回傳位置 */
static int last_vowel(const Viet *v, const char *bases, int mod)
{
	for (int i = v->n - 1; i >= 0; i--) {
		if (!vowel(v->l[i].ch)) { if (i < v->n - 1 && vowel(v->l[i + 1].ch)) break; continue; }   /* 只看最後一組母音 */
		if (strchr(bases, v->l[i].ch) && v->l[i].mod == mod) return i;
	}
	return -1;
}

/* â ê ô（Telex：aa ee oo；VNI：6） */
static int hat(Viet *v, const char *bases, char lit, int up)
{
	int i = last_vowel(v, bases, M_NONE);
	if (i >= 0) { v->l[i].mod = M_HAT; return 1; }
	if ((i = last_vowel(v, bases, M_HAT)) >= 0) { v->l[i].mod = M_NONE; add(v, lit, up, 0, 0); return 1; }
	return 0;
}

/* ơ ư（uo → ươ）、ă（Telex 的 w 兩個都管；VNI：7 ơ ư、8 ă） */
static int horn(Viet *v, int allow_horn, int allow_breve, char lit, int up)
{
	int e, s = cluster(v, &e);
	if (allow_horn && s >= 0) {
		for (int i = s; i + 1 < e; i++)
			if (v->l[i].ch == 'u' && v->l[i + 1].ch == 'o' && !v->l[i].mod && !v->l[i + 1].mod) {
				v->l[i].mod = v->l[i + 1].mod = M_HORN;
				return 1;
			}
	}
	int i = allow_horn ? last_vowel(v, "ou", M_NONE) : -1;
	if (i < 0 && allow_breve) i = last_vowel(v, "a", M_NONE);
	if (i >= 0 && !(i > 0 && v->l[i].ch == 'u' && s == i + 1)) {   /* qu 的 u 不算 */
		v->l[i].mod = v->l[i].ch == 'a' ? M_BREVE : M_HORN;
		return 1;
	}
	/* 已經加過：取消，把鍵打出來 */
	int j = allow_horn ? last_vowel(v, "ou", M_HORN) : -1;
	if (j < 0 && allow_breve) j = last_vowel(v, "a", M_BREVE);
	if (j >= 0) {
		if (v->l[j].from_w) { v->l[j] = (L){ lit, (char)up, 0, 0 }; return 1; }   /* w → ư → w */
		for (int k = 0; k < v->n; k++) if (v->l[k].mod == M_HORN || v->l[k].mod == M_BREVE) v->l[k].mod = M_NONE;
		add(v, lit, up, 0, 0);
		return 1;
	}
	return 0;
}

/* đ（Telex：dd；VNI：9）：只有字首的 d */
static int stroke(Viet *v, char lit, int up)
{
	if (!v->n || v->l[0].ch != 'd') return 0;
	if (v->l[0].mod == M_STROKE) { v->l[0].mod = M_NONE; add(v, lit, up, 0, 0); }
	else v->l[0].mod = M_STROKE;
	return 1;
}

static int telex(Viet *v, char c, int up)
{
	switch (c) {
	case 's': return set_tone(v, 1, c, up);
	case 'f': return set_tone(v, 2, c, up);
	case 'r': return set_tone(v, 3, c, up);
	case 'x': return set_tone(v, 4, c, up);
	case 'j': return set_tone(v, 5, c, up);
	case 'z': if (v->tone) { v->tone = 0; return 1; } return 0;
	case 'a': return hat(v, "a", c, up);
	case 'e': return hat(v, "e", c, up);
	case 'o': return hat(v, "o", c, up);
	case 'd': return v->n && stroke(v, c, up);
	case 'w':
		if (horn(v, 1, 1, c, up)) return 1;
		add(v, 'u', up, M_HORN, 1);   /* 單獨的 w：ư */
		return 1;
	}
	return 0;
}

static int vni(Viet *v, char c)
{
	switch (c) {
	case '1': case '2': case '3': case '4': case '5': return set_tone(v, c - '0', c, 0);
	case '0': if (v->tone) { v->tone = 0; return 1; } return 0;
	case '6': return hat(v, "aeo", c, 0);
	case '7': return horn(v, 1, 0, c, 0);
	case '8': return horn(v, 0, 1, c, 0);
	case '9': return stroke(v, c, 0);
	}
	return 0;
}

static int key(void *c, unsigned ks, unsigned state)
{
	Viet *v = c;
	if (state & (4 | 8 | 64)) { commit_word(v); return 0; }   /* Ctrl、Alt、Super 組合鍵 */
	if (ks == XK_BackSpace) {
		if (!v->n) return 0;
		v->n--;
		if (!has_vowel(v)) v->tone = 0;
		return 1;
	}
	int is_vni = !strcmp(conf_viet, "vni");
	if ((ks >= 'a' && ks <= 'z') || (ks >= 'A' && ks <= 'Z')) {
		/* 大小寫照 X 給的字（Shift、Caps Lock 都算）；聲調、符號鍵大小寫都可以 */
		int up = ks <= 'Z';
		char ch = (char)(ks | 0x20);
		if (v->n >= (int)(sizeof v->l / sizeof v->l[0]) - 1) commit_word(v);
		if (!is_vni && v->n && telex(v, ch, up)) return 1;
		if (!is_vni && !v->n && ch == 'w') { add(v, 'u', up, M_HORN, 1); return 1; }
		add(v, ch, up, 0, 0);
		return 1;
	}
	if (is_vni && v->n && ks >= '0' && ks <= '9' && vni(v, (char)ks)) return 1;
	commit_word(v);   /* 空白、標點、數字、Enter、方向鍵…：送出這個字，鍵交給程式 */
	return 0;
}

static void view(void *c, View *v)
{
	render(c, v->comp, sizeof v->comp);
}

static int commit(void *c, char *out, size_t n)
{
	Viet *v = c;
	if (!v->out[0]) return 0;
	snprintf(out, n, "%s", v->out);
	v->out[0] = 0;
	return 1;
}

static void flush(void *c) { commit_word(c); }
static void reset(void *c) { Viet *v = c; v->n = v->tone = 0; v->out[0] = 0; }
static void choose(void *c, int i) { (void)c; (void)i; }

const Engine eng_viet = { "viet", N_("越南文"), "V", load, create, destroy, conf, key, view, commit, flush, reset, choose };
