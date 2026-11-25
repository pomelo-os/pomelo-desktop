/* SPDX-License-Identifier: GPL-3.0-or-later */
/* eng-hangul.c — 韓文（libhangul，dlopen 載入）
 *
 * 照按鍵把字母組成韓文字（預設兩排式，「2」）。英文字母鍵交給 libhangul；它不要的鍵（數字、標點、
 * 空白、Enter、方向鍵…）先把組到一半的字送出，再把鍵原樣交給程式。Ctrl、Alt 組合鍵也一樣。
 */
#define _GNU_SOURCE
#include "engine.h"
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <X11/keysym.h>

typedef struct _HangulInputContext HIC;
static HIC *(*h_new)(const char *keyboard);
static void (*h_delete)(HIC *);
static bool (*h_process)(HIC *, int ascii);
static void (*h_reset)(HIC *);
static bool (*h_backspace)(HIC *);
static bool (*h_is_empty)(HIC *);
static const uint32_t *(*h_preedit)(HIC *);
static const uint32_t *(*h_commit)(HIC *);
static const uint32_t *(*h_flush)(HIC *);
static void (*h_select_keyboard)(HIC *, const char *);

typedef struct { HIC *ic; char out[512]; } Hangul;

static int load(void)
{
	static int state;   /* 0 還沒試、1 可以、2 找不到 */
	if (state) return state == 1;
	void *h = dlopen("libhangul.so.1", RTLD_NOW | RTLD_LOCAL);
	state = 2;
	if (!h) return 0;
	*(void **)&h_new = dlsym(h, "hangul_ic_new");
	*(void **)&h_delete = dlsym(h, "hangul_ic_delete");
	*(void **)&h_process = dlsym(h, "hangul_ic_process");
	*(void **)&h_reset = dlsym(h, "hangul_ic_reset");
	*(void **)&h_backspace = dlsym(h, "hangul_ic_backspace");
	*(void **)&h_is_empty = dlsym(h, "hangul_ic_is_empty");
	*(void **)&h_preedit = dlsym(h, "hangul_ic_get_preedit_string");
	*(void **)&h_commit = dlsym(h, "hangul_ic_get_commit_string");
	*(void **)&h_flush = dlsym(h, "hangul_ic_flush");
	*(void **)&h_select_keyboard = dlsym(h, "hangul_ic_select_keyboard");
	if (h_new && h_delete && h_process && h_reset && h_backspace && h_is_empty && h_preedit && h_commit && h_flush) state = 1;
	return state == 1;
}

static void *create(void)
{
	Hangul *g = calloc(1, sizeof *g);
	if (!g) return NULL;
	g->ic = h_new(conf_hangul_kb[0] ? conf_hangul_kb : "2");
	if (!g->ic) { free(g); return NULL; }
	return g;
}

static void destroy(void *c)
{
	Hangul *g = c;
	if (!g) return;
	h_delete(g->ic);
	free(g);
}

static void conf(void *c)
{
	Hangul *g = c;
	if (h_select_keyboard) h_select_keyboard(g->ic, conf_hangul_kb[0] ? conf_hangul_kb : "2");
}

static void take(Hangul *g, const uint32_t *s)
{
	if (s && *s) utf8_append(g->out, sizeof g->out, s);
}

static void do_flush(Hangul *g)
{
	if (!h_is_empty(g->ic)) take(g, h_flush(g->ic));
}

static int key(void *c, unsigned ks, unsigned state)
{
	Hangul *g = c;
	if (state & (4 | 8 | 64)) { do_flush(g); return 0; }   /* Ctrl、Alt、Super 組合鍵：先送出組到一半的字 */
	if (ks == XK_BackSpace) return h_backspace(g->ic);
	if (ks >= 0x21 && ks < 0x7f) {
		/* Caps Lock 開著時 X 給的是大寫：兩排式的大寫是雙子音，要照 Shift 有沒有按來決定 */
		int ch = (int)ks;
		if (ch >= 'A' && ch <= 'Z' && !(state & 1)) ch += 32;
		else if (ch >= 'a' && ch <= 'z' && (state & 1)) ch -= 32;
		int ate = h_process(g->ic, ch);
		take(g, h_commit(g->ic));
		if (!ate) do_flush(g);   /* 數字、標點：送出組到一半的字，鍵交給程式 */
		return ate;
	}
	do_flush(g);   /* 空白、Enter、方向鍵…：字送出，鍵交給程式 */
	return 0;
}

static void view(void *c, View *v)
{
	Hangul *g = c;
	const uint32_t *p = h_preedit(g->ic);
	if (p && *p) utf8_append(v->comp, sizeof v->comp, p);
}

static int commit(void *c, char *out, size_t n)
{
	Hangul *g = c;
	if (!g->out[0]) return 0;
	snprintf(out, n, "%s", g->out);
	g->out[0] = 0;
	return 1;
}

static void flush(void *c) { do_flush(c); }
static void reset(void *c) { Hangul *g = c; h_reset(g->ic); g->out[0] = 0; }
static void choose(void *c, int i) { (void)c; (void)i; }

const Engine eng_hangul = { "hangul", N_("韓文"), "한", load, create, destroy, conf, key, view, commit, flush, reset, choose };
