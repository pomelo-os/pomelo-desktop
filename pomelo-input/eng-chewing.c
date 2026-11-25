/* SPDX-License-Identifier: GPL-3.0-or-later */
/* eng-chewing.c — 注音（新酷音引擎 libchewing）
 *
 * 選字：候選字列開著時 ←→ 移動反白（到頭自動翻頁），Enter 選反白的那個，數字鍵直接選，
 * 空白鍵、PageUp／PageDown、↑↓ 照新酷音原本的方式（翻頁、換詞長）。 */
#include "engine.h"
#include <chewing.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <X11/keysym.h>

#define SHIFT 1
#define CTRL 4

/* commit_Check 一直是 1 直到下一個按鍵：taken 記住這次的字已經送過了 */
typedef struct { ChewingContext *ctx; int taken, hl, page, total; } Chewing;   /* hl：這一頁反白的是第幾個 */

static int load(void) { return 1; }

static void apply(void *c)
{
	ChewingContext *ctx = ((Chewing *)c)->ctx;
	int kb = chewing_KBStr2Num(conf_kb);
	chewing_set_KBType(ctx, kb < 0 ? 0 : kb);
	int sel[9] = { '1', '2', '3', '4', '5', '6', '7', '8', '9' };
	chewing_set_selKey(ctx, sel, 9);
	chewing_set_candPerPage(ctx, conf_cand_n);
	chewing_set_maxChiSymbolLen(ctx, 20);
	chewing_set_spaceAsSelection(ctx, conf_space_select);
	chewing_set_escCleanAllBuf(ctx, 1);
	chewing_set_autoShiftCur(ctx, 1);
	chewing_set_phraseChoiceRearward(ctx, 1);
	chewing_set_easySymbolInput(ctx, 0);
	chewing_set_ChiEngMode(ctx, CHINESE_MODE);
}

static void *create(void)
{
	Chewing *c = calloc(1, sizeof *c);
	if (!c) return NULL;
	c->ctx = chewing_new();
	if (!c->ctx) { free(c); return NULL; }
	apply(c);
	return c;
}

static void destroy(void *c)
{
	if (!c) return;
	chewing_delete(((Chewing *)c)->ctx);
	free(c);
}

/* 候選字列：這一頁有幾個 */
static int page_count(ChewingContext *ctx)
{
	int per = chewing_cand_ChoicePerPage(ctx), total = chewing_cand_TotalChoice(ctx), first = chewing_cand_CurrentPage(ctx) * per;
	return total - first < per ? total - first : per;
}

static int key(void *c, unsigned ks, unsigned state)
{
	Chewing *ch = c;
	ChewingContext *ctx = ch->ctx;
	/* 新酷音真的處理這個鍵才清掉 taken；不處理的鍵（Ctrl、Ctrl+S…）不能清，不然上一次送出的字會再送一次 */
	int was_taken = ch->taken;
	ch->taken = 0;
	int shift = !!(state & SHIFT);
	/* 候選字列開著：←→ 移反白、Enter 選反白的 */
	if (chewing_cand_TotalChoice(ctx) > 0 && !(state & CTRL) && !shift) {
		int n = page_count(ctx);
		if (ks == XK_Left || ks == XK_KP_Left) {
			if (ch->hl > 0) ch->hl--;
			else { chewing_handle_Left(ctx); ch->hl = page_count(ctx) - 1; ch->page = chewing_cand_CurrentPage(ctx); }
			return 1;
		}
		if (ks == XK_Right || ks == XK_KP_Right) {
			if (ch->hl + 1 < n) ch->hl++;
			else { chewing_handle_Right(ctx); ch->hl = 0; ch->page = chewing_cand_CurrentPage(ctx); }
			return 1;
		}
		if (ks == XK_Return || ks == XK_KP_Enter) {
			chewing_cand_choose_by_index(ctx, chewing_cand_CurrentPage(ctx) * chewing_cand_ChoicePerPage(ctx) + (ch->hl < n ? ch->hl : 0));
			ch->hl = 0;
			return 1;
		}
	}
	if (state & CTRL) {
		if (ks >= XK_0 && ks <= XK_9 && chewing_buffer_Check(ctx)) { chewing_handle_CtrlNum(ctx, (int)ks); return 1; }
		ch->taken = was_taken;
		return 0;   /* 其他 Ctrl 組合（複製貼上…）交給程式 */
	}
	switch (ks) {
	case XK_space: if (shift) chewing_handle_ShiftSpace(ctx); else chewing_handle_Space(ctx); break;
	case XK_Return: case XK_KP_Enter: chewing_handle_Enter(ctx); break;
	case XK_BackSpace: chewing_handle_Backspace(ctx); break;
	case XK_Escape: chewing_handle_Esc(ctx); break;
	case XK_Delete: chewing_handle_Del(ctx); break;
	case XK_Tab: chewing_handle_Tab(ctx); break;
	case XK_Left: if (shift) chewing_handle_ShiftLeft(ctx); else chewing_handle_Left(ctx); break;
	case XK_Right: if (shift) chewing_handle_ShiftRight(ctx); else chewing_handle_Right(ctx); break;
	case XK_Up: chewing_handle_Up(ctx); break;
	case XK_Down: chewing_handle_Down(ctx); break;
	case XK_Home: chewing_handle_Home(ctx); break;
	case XK_End: chewing_handle_End(ctx); break;
	case XK_Page_Up: chewing_handle_PageUp(ctx); break;
	case XK_Page_Down: chewing_handle_PageDown(ctx); break;
	default:
		if (ks >= 0x20 && ks < 0x7f) chewing_handle_Default(ctx, (int)ks);
		else { ch->taken = was_taken; return 0; }   /* 功能鍵、Ctrl 本身等：不處理 */
	}
	return !chewing_keystroke_CheckIgnore(ctx);   /* 例如組字區是空的時候按 Enter：交給程式 */
}

static void view(void *c, View *v)
{
	Chewing *ch = c;
	ChewingContext *ctx = ch->ctx;
	const char *buf = chewing_buffer_String_static(ctx);
	int cur = chewing_cursor_Current(ctx);
	/* 游標是「第幾個字」，把 UTF-8 切成前後兩段 */
	int i = 0, k = 0;
	while (buf[i] && k < cur) { i += (unsigned char)buf[i] < 0x80 ? 1 : (unsigned char)buf[i] < 0xe0 ? 2 : (unsigned char)buf[i] < 0xf0 ? 3 : 4; k++; }
	snprintf(v->pre, sizeof v->pre, "%.*s", i, buf);
	snprintf(v->post, sizeof v->post, "%s", buf + i);
	snprintf(v->comp, sizeof v->comp, "%s", chewing_bopomofo_String_static(ctx));
	if (chewing_cand_TotalChoice(ctx) > 0) {
		int per = chewing_cand_ChoicePerPage(ctx), total = chewing_cand_TotalChoice(ctx);
		v->page = chewing_cand_CurrentPage(ctx);
		v->npage = chewing_cand_TotalPage(ctx);
		/* 換頁或換了一組候選字（例如 ↓ 換詞長）：反白回到第一個 */
		if (v->page != ch->page || total != ch->total) { ch->hl = 0; ch->page = v->page; ch->total = total; }
		for (int j = 0; j < per && j < CAND_MAX; j++) {
			int idx = v->page * per + j;
			if (idx >= total) break;
			const char *s = chewing_cand_string_by_index_static(ctx, idx);
			snprintf(v->cand[v->ncand++], sizeof v->cand[0], "%s", s ? s : "");
		}
		v->sel = ch->hl < v->ncand ? ch->hl : 0;
	} else ch->total = 0;
	if (chewing_aux_Check(ctx)) snprintf(v->aux, sizeof v->aux, "%s", chewing_aux_String_static(ctx));
}

static int commit(void *c, char *out, size_t n)
{
	Chewing *ch = c;
	if (ch->taken || !chewing_commit_Check(ch->ctx)) return 0;
	snprintf(out, n, "%s", chewing_commit_String_static(ch->ctx));
	ch->taken = 1;   /* 取過了：下一次不要再送一次 */
	return out[0] != 0;
}

static void flush(void *c)
{
	Chewing *ch = c;
	ch->taken = 0;
	if (chewing_buffer_Check(ch->ctx)) chewing_commit_preedit_buf(ch->ctx);
	else ch->taken = 1;   /* 沒有東西要送：不要把上一次的又送一次 */
}

static void reset(void *c)
{
	chewing_Reset(((Chewing *)c)->ctx);
	apply(c);
}

static void choose(void *c, int i)
{
	((Chewing *)c)->taken = 0;
	ChewingContext *ctx = ((Chewing *)c)->ctx;
	int per = chewing_cand_ChoicePerPage(ctx);
	chewing_cand_choose_by_index(ctx, chewing_cand_CurrentPage(ctx) * per + i);
}

const Engine eng_chewing = { "chewing", N_("注音"), "注", load, create, destroy, apply, key, view, commit, flush, reset, choose };
