/* SPDX-License-Identifier: GPL-3.0-or-later */
/* json.c — 很小的 JSON 剖析器（被 pomelo-vm.c #include）
 *
 * 只給 QMP 回應與 QEMU 的韌體說明檔（/usr/share/qemu/firmware 裡的 .json）用：整份剖析成一棵樹，用 js_get 取值。
 * 字串裡的 \uXXXX 會轉成 UTF-8（含代理對）。數字一律存 double。
 */

enum { JS_NULL, JS_BOOL, JS_NUM, JS_STR, JS_ARR, JS_OBJ };

typedef struct Js Js;
struct Js {
	int type;
	double num;          /* JS_NUM、JS_BOOL */
	char *str;           /* JS_STR */
	char *key;           /* 在物件裡時的鍵 */
	Js *kids;            /* JS_ARR、JS_OBJ 的第一個元素 */
	Js *next;
	int n;
};

static void js_free(Js *j)
{
	while (j) {
		Js *nx = j->next;
		js_free(j->kids);
		free(j->str);
		free(j->key);
		free(j);
		j = nx;
	}
}

static void js_ws(const char **p) { while (**p == ' ' || **p == '\t' || **p == '\n' || **p == '\r') (*p)++; }

static void js_utf8(char **o, unsigned cp)
{
	char *d = *o;
	if (cp < 0x80) *d++ = (char)cp;
	else if (cp < 0x800) { *d++ = (char)(0xc0 | (cp >> 6)); *d++ = (char)(0x80 | (cp & 0x3f)); }
	else if (cp < 0x10000) { *d++ = (char)(0xe0 | (cp >> 12)); *d++ = (char)(0x80 | ((cp >> 6) & 0x3f)); *d++ = (char)(0x80 | (cp & 0x3f)); }
	else { *d++ = (char)(0xf0 | (cp >> 18)); *d++ = (char)(0x80 | ((cp >> 12) & 0x3f)); *d++ = (char)(0x80 | ((cp >> 6) & 0x3f)); *d++ = (char)(0x80 | (cp & 0x3f)); }
	*o = d;
}

static char *js_string(const char **p)
{
	const char *s = *p + 1, *e = s;
	while (*e && *e != '"') { if (*e == '\\' && e[1]) e++; e++; }
	if (*e != '"') return NULL;
	char *out = malloc((size_t)(e - s) + 4), *o = out;
	while (s < e) {
		if (*s != '\\') { *o++ = *s++; continue; }
		s++;
		switch (*s) {
		case 'n': *o++ = '\n'; s++; break;
		case 't': *o++ = '\t'; s++; break;
		case 'r': *o++ = '\r'; s++; break;
		case 'b': *o++ = '\b'; s++; break;
		case 'f': *o++ = '\f'; s++; break;
		case 'u': {
			unsigned cp = 0;
			if (sscanf(s + 1, "%4x", &cp) != 1) { s++; break; }
			s += 5;
			if (cp >= 0xd800 && cp < 0xdc00 && s[0] == '\\' && s[1] == 'u') {
				unsigned lo = 0;
				if (sscanf(s + 2, "%4x", &lo) == 1 && lo >= 0xdc00 && lo < 0xe000) { cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00); s += 6; }
			}
			js_utf8(&o, cp);
			break;
		}
		default: *o++ = *s++; break;
		}
	}
	*o = 0;
	*p = e + 1;
	return out;
}

static Js *js_value(const char **p, int depth)
{
	js_ws(p);
	if (depth > 64) return NULL;
	Js *j = calloc(1, sizeof *j);
	const char *s = *p;
	if (*s == '{' || *s == '[') {
		int obj = *s == '{';
		j->type = obj ? JS_OBJ : JS_ARR;
		(*p)++;
		Js **tail = &j->kids;
		js_ws(p);
		if (**p == (obj ? '}' : ']')) { (*p)++; return j; }
		for (;;) {
			char *key = NULL;
			if (obj) {
				js_ws(p);
				if (**p != '"' || !(key = js_string(p))) goto bad;
				js_ws(p);
				if (**p != ':') { free(key); goto bad; }
				(*p)++;
			}
			Js *k = js_value(p, depth + 1);
			if (!k) { free(key); goto bad; }
			k->key = key;
			*tail = k;
			tail = &k->next;
			j->n++;
			js_ws(p);
			if (**p == ',') { (*p)++; continue; }
			if (**p == (obj ? '}' : ']')) { (*p)++; return j; }
			goto bad;
		}
	}
	if (*s == '"') {
		j->type = JS_STR;
		if (!(j->str = js_string(p))) goto bad;
		return j;
	}
	if (!strncmp(s, "true", 4)) { j->type = JS_BOOL; j->num = 1; *p += 4; return j; }
	if (!strncmp(s, "false", 5)) { j->type = JS_BOOL; *p += 5; return j; }
	if (!strncmp(s, "null", 4)) { j->type = JS_NULL; *p += 4; return j; }
	if (*s == '-' || (*s >= '0' && *s <= '9')) {
		char *e;
		j->type = JS_NUM;
		j->num = strtod(s, &e);
		*p = e;
		return j;
	}
bad:
	js_free(j);
	return NULL;
}

static Js *js_parse(const char *s)
{
	if (!s) return NULL;
	return js_value(&s, 0);
}

static Js *js_get(const Js *j, const char *key)
{
	if (!j || j->type != JS_OBJ) return NULL;
	for (Js *k = j->kids; k; k = k->next)
		if (k->key && !strcmp(k->key, key)) return k;
	return NULL;
}

static const char *js_str(const Js *j, const char *def) { return j && j->type == JS_STR ? j->str : def; }
static double js_num(const Js *j, double def) { return j && (j->type == JS_NUM || j->type == JS_BOOL) ? j->num : def; }

/* 陣列裡有沒有這個字串（例如 firmware 的 features） */
static int js_has(const Js *arr, const char *s)
{
	if (!arr || arr->type != JS_ARR) return 0;
	for (Js *k = arr->kids; k; k = k->next)
		if (k->type == JS_STR && !strcmp(k->str, s)) return 1;
	return 0;
}

/* 把字串寫成 JSON 字串（含引號），用在送給 QMP 的指令 */
static void js_quote(char *out, size_t n, const char *s)
{
	size_t k = 0;
	if (n < 3) { if (n) out[0] = 0; return; }
	out[k++] = '"';
	for (; *s && k + 8 < n; s++) {
		unsigned char c = (unsigned char)*s;
		if (c == '"' || c == '\\') { out[k++] = '\\'; out[k++] = (char)c; }
		else if (c == '\n') { out[k++] = '\\'; out[k++] = 'n'; }
		else if (c < 0x20) k += (size_t)snprintf(out + k, n - k, "\\u%04x", c);
		else out[k++] = (char)c;
	}
	out[k++] = '"';
	out[k] = 0;
}
