/* SPDX-License-Identifier: GPL-3.0-or-later */
/* xml.c — 見 xml.h */
#define _GNU_SOURCE
#include "xml.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ------------------------------------------------------------------ */
/* 剖析                                                                  */
/* ------------------------------------------------------------------ */

typedef struct { const char *s, *end; } Cur;

static void skip_space(Cur *c) { while (c->s < c->end && (unsigned char)*c->s <= ' ') c->s++; }

/* 把一段（可能含實體）文字解出來，長度為 len；回傳 malloc 的字串 */
static char *unescape(const char *s, size_t len)
{
	char *out = malloc(len + 1);
	size_t o = 0;
	for (size_t i = 0; i < len;) {
		if (s[i] == '&') {
			const char *amp = s + i;
			size_t rem = len - i;
			if (rem >= 5 && !strncmp(amp, "&amp;", 5)) { out[o++] = '&'; i += 5; continue; }
			if (rem >= 4 && !strncmp(amp, "&lt;", 4)) { out[o++] = '<'; i += 4; continue; }
			if (rem >= 4 && !strncmp(amp, "&gt;", 4)) { out[o++] = '>'; i += 4; continue; }
			if (rem >= 6 && !strncmp(amp, "&quot;", 6)) { out[o++] = '"'; i += 6; continue; }
			if (rem >= 6 && !strncmp(amp, "&apos;", 6)) { out[o++] = '\''; i += 6; continue; }
			if (rem >= 2 && amp[1] == '#') {
				size_t j = i + 2;
				int hex = j < len && (s[j] == 'x' || s[j] == 'X');
				if (hex) j++;
				long cp = strtol(s + j, NULL, hex ? 16 : 10);
				const char *semi = memchr(s + j, ';', len - j);
				if (semi) {
					/* 編碼成 UTF-8 */
					if (cp < 0x80) out[o++] = (char)cp;
					else if (cp < 0x800) { out[o++] = (char)(0xc0 | (cp >> 6)); out[o++] = (char)(0x80 | (cp & 0x3f)); }
					else if (cp < 0x10000) {
						out[o++] = (char)(0xe0 | (cp >> 12)); out[o++] = (char)(0x80 | ((cp >> 6) & 0x3f)); out[o++] = (char)(0x80 | (cp & 0x3f));
					} else {
						out[o++] = (char)(0xf0 | (cp >> 18)); out[o++] = (char)(0x80 | ((cp >> 12) & 0x3f));
						out[o++] = (char)(0x80 | ((cp >> 6) & 0x3f)); out[o++] = (char)(0x80 | (cp & 0x3f));
					}
					i = (size_t)(semi - s) + 1;
					continue;
				}
			}
			out[o++] = s[i++];   /* 不是合法實體：照抄 & */
		} else
			out[o++] = s[i++];
	}
	out[o] = 0;
	return out;
}

static XmlNode *new_node(void) { return calloc(1, sizeof(XmlNode)); }

static XmlAttr *parse_attrs(Cur *c)
{
	XmlAttr *head = NULL, **tail = &head;
	for (;;) {
		skip_space(c);
		if (c->s >= c->end || *c->s == '>' || *c->s == '/' || *c->s == '?') break;
		const char *ns = c->s;
		while (c->s < c->end && *c->s != '=' && (unsigned char)*c->s > ' ' && *c->s != '/' && *c->s != '>') c->s++;
		size_t nlen = (size_t)(c->s - ns);
		if (!nlen) break;
		skip_space(c);
		char *val = strdup("");
		if (c->s < c->end && *c->s == '=') {
			c->s++;
			skip_space(c);
			if (c->s < c->end && (*c->s == '"' || *c->s == '\'')) {
				char q = *c->s++;
				const char *vs = c->s;
				while (c->s < c->end && *c->s != q) c->s++;
				free(val);
				val = unescape(vs, (size_t)(c->s - vs));
				if (c->s < c->end) c->s++;
			}
		}
		XmlAttr *a = calloc(1, sizeof *a);
		a->name = malloc(nlen + 1);
		memcpy(a->name, ns, nlen);
		a->name[nlen] = 0;
		a->value = val;
		*tail = a;
		tail = &a->next;
	}
	return head;
}

/* 剖析從目前位置開始的一串兄弟節點，直到遇到 </ 或資料結束 */
static XmlNode *parse_nodes(Cur *c)
{
	XmlNode *head = NULL, **tail = &head;
	for (;;) {
		if (c->s >= c->end) break;
		if (*c->s == '<') {
			if (c->s + 1 < c->end && c->s[1] == '/') break;   /* 結束標籤，交給呼叫者處理 */
			if (c->s + 3 < c->end && !strncmp(c->s, "<!--", 4)) {
				const char *e = memmem(c->s, (size_t)(c->end - c->s), "-->", 3);
				c->s = e ? e + 3 : c->end;
				continue;
			}
			if (c->s + 8 < c->end && !strncmp(c->s, "<![CDATA[", 9)) {
				const char *e = memmem(c->s + 9, (size_t)(c->end - c->s - 9), "]]>", 3);
				const char *cs = c->s + 9, *ce = e ? e : c->end;
				XmlNode *t = new_node();
				t->text = malloc((size_t)(ce - cs) + 1);
				memcpy(t->text, cs, (size_t)(ce - cs));
				t->text[ce - cs] = 0;
				*tail = t; tail = &t->next;
				c->s = e ? e + 3 : c->end;
				continue;
			}
			if (c->s + 1 < c->end && (c->s[1] == '?' || c->s[1] == '!')) {   /* <?xml ...?>、<!DOCTYPE ...> */
				const char *e = memchr(c->s, '>', (size_t)(c->end - c->s));
				c->s = e ? e + 1 : c->end;
				continue;
			}
			/* 元素開始 */
			c->s++;
			const char *ts = c->s;
			while (c->s < c->end && (unsigned char)*c->s > ' ' && *c->s != '>' && *c->s != '/') c->s++;
			size_t tlen = (size_t)(c->s - ts);
			if (!tlen) { c->s++; continue; }   /* 壞資料：往前跳一個字元，避免死迴圈 */
			XmlNode *n = new_node();
			n->tag = malloc(tlen + 1);
			memcpy(n->tag, ts, tlen);
			n->tag[tlen] = 0;
			n->attrs = parse_attrs(c);
			skip_space(c);
			int selfclose = c->s < c->end && *c->s == '/';
			if (selfclose) c->s++;
			if (c->s < c->end && *c->s == '>') c->s++;
			if (!selfclose) {
				n->children = parse_nodes(c);
				/* 吃掉 </tag> */
				if (c->s + 1 < c->end && c->s[0] == '<' && c->s[1] == '/') {
					const char *e = memchr(c->s, '>', (size_t)(c->end - c->s));
					c->s = e ? e + 1 : c->end;
				}
			}
			*tail = n; tail = &n->next;
		} else {
			const char *ts = c->s;
			while (c->s < c->end && *c->s != '<') c->s++;
			size_t tlen = (size_t)(c->s - ts);
			/* 純空白的文字節點不留（避免排版用的換行、縮排污染文字內容） */
			int blank = 1;
			for (size_t i = 0; i < tlen; i++) if ((unsigned char)ts[i] > ' ') { blank = 0; break; }
			if (!blank) {
				XmlNode *t = new_node();
				t->text = unescape(ts, tlen);
				*tail = t; tail = &t->next;
			}
		}
	}
	return head;
}

XmlNode *xml_parse(const char *data, size_t len)
{
	Cur c = { data, data + len };
	XmlNode *top = parse_nodes(&c);
	/* 正常的 XML 最外層只有一個元素；找到它，把前後其餘的節點（通常沒有）切開各自釋放 */
	XmlNode *prev = NULL, *root = NULL;
	for (XmlNode *n = top; n; prev = n, n = n->next)
		if (n->tag) { root = n; break; }
	if (!root) { xml_free(top); return NULL; }
	if (prev) prev->next = NULL; else top = NULL;   /* 切斷 root 跟它前面節點的連結 */
	XmlNode *after = root->next;
	root->next = NULL;
	if (top) xml_free(top);     /* root 前面的雜散節點 */
	xml_free(after);             /* root 後面的雜散節點 */
	return root;
}

void xml_free(XmlNode *n)
{
	while (n) {
		XmlNode *next = n->next;
		xml_free(n->children);
		for (XmlAttr *a = n->attrs; a;) { XmlAttr *an = a->next; free(a->name); free(a->value); free(a); a = an; }
		free(n->tag);
		free(n->text);
		free(n);
		n = next;
	}
}

XmlNode *xml_child(const XmlNode *n, const char *tag)
{
	if (!n) return NULL;
	for (XmlNode *c = n->children; c; c = c->next)
		if (c->tag && !strcmp(c->tag, tag)) return c;
	return NULL;
}

XmlNode *xml_next(const XmlNode *n, const char *tag)
{
	if (!n) return NULL;
	for (XmlNode *c = n->next; c; c = c->next)
		if (c->tag && !strcmp(c->tag, tag)) return c;
	return NULL;
}

const char *xml_attr(const XmlNode *n, const char *name)
{
	if (!n) return NULL;
	for (XmlAttr *a = n->attrs; a; a = a->next)
		if (!strcmp(a->name, name)) return a->value;
	return NULL;
}

static void collect_text(const XmlNode *n, char **buf, size_t *len, size_t *cap)
{
	for (const XmlNode *c = n->children; c; c = c->next) {
		const char *t = c->tag ? NULL : c->text;
		if (t) {
			size_t tl = strlen(t);
			if (*len + tl + 1 > *cap) { *cap = (*len + tl + 1) * 2; *buf = realloc(*buf, *cap); }
			memcpy(*buf + *len, t, tl);
			*len += tl;
		} else if (c->tag)
			collect_text(c, buf, len, cap);
	}
}

char *xml_text(const XmlNode *n)
{
	size_t len = 0, cap = 64;
	char *buf = malloc(cap);
	buf[0] = 0;
	if (n) collect_text(n, &buf, &len, &cap);
	buf[len] = 0;
	return buf;
}

/* ------------------------------------------------------------------ */
/* 寫入                                                                  */
/* ------------------------------------------------------------------ */

struct XmlWriter {
	char *buf;
	size_t len, cap;
	char **stack;      /* 還沒關閉的標籤名稱 */
	int depth, stackcap;
	int open_tag_pending;   /* 剛 xml_open 還沒遇到 '>'（還能加屬性） */
};

static void w_ensure(XmlWriter *w, size_t more)
{
	if (w->len + more + 1 > w->cap) {
		w->cap = (w->len + more + 1) * 2;
		w->buf = realloc(w->buf, w->cap);
	}
}
static void w_raw(XmlWriter *w, const char *s, size_t n) { w_ensure(w, n); memcpy(w->buf + w->len, s, n); w->len += n; }
static void w_str(XmlWriter *w, const char *s) { w_raw(w, s, strlen(s)); }

static void w_escaped(XmlWriter *w, const char *s, int is_attr)
{
	for (; *s; s++) {
		switch (*s) {
		case '&': w_str(w, "&amp;"); break;
		case '<': w_str(w, "&lt;"); break;
		case '>': w_str(w, "&gt;"); break;
		case '"': if (is_attr) { w_str(w, "&quot;"); break; } w_raw(w, s, 1); break;
		default: w_raw(w, s, 1);
		}
	}
}

static void close_open_tag(XmlWriter *w)
{
	if (w->open_tag_pending) { w_str(w, ">"); w->open_tag_pending = 0; }
}

XmlWriter *xml_writer_new(void)
{
	XmlWriter *w = calloc(1, sizeof *w);
	w->cap = 4096;
	w->buf = malloc(w->cap);
	w_str(w, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>");
	return w;
}

void xml_open(XmlWriter *w, const char *tag)
{
	close_open_tag(w);
	if (w->depth >= w->stackcap) { w->stackcap = w->stackcap ? w->stackcap * 2 : 16; w->stack = realloc(w->stack, (size_t)w->stackcap * sizeof *w->stack); }
	w->stack[w->depth++] = strdup(tag);
	w_str(w, "<");
	w_str(w, tag);
	w->open_tag_pending = 1;
}

void xml_attr_set(XmlWriter *w, const char *name, const char *value)
{
	w_str(w, " ");
	w_str(w, name);
	w_str(w, "=\"");
	w_escaped(w, value, 1);
	w_str(w, "\"");
}

void xml_text_out(XmlWriter *w, const char *text)
{
	close_open_tag(w);
	w_escaped(w, text, 0);
}

void xml_close(XmlWriter *w)
{
	if (w->depth <= 0) return;
	char *tag = w->stack[--w->depth];
	if (w->open_tag_pending) { w_str(w, "/>"); w->open_tag_pending = 0; }
	else { w_str(w, "</"); w_str(w, tag); w_str(w, ">"); }
	free(tag);
}

char *xml_writer_finish(XmlWriter *w, size_t *outlen)
{
	while (w->depth > 0) xml_close(w);
	close_open_tag(w);
	char *out = malloc(w->len + 1);
	memcpy(out, w->buf, w->len);
	out[w->len] = 0;
	if (outlen) *outlen = w->len;
	return out;
}

void xml_writer_free(XmlWriter *w)
{
	if (!w) return;
	for (int i = 0; i < w->depth; i++) free(w->stack[i]);
	free(w->stack);
	free(w->buf);
	free(w);
}
