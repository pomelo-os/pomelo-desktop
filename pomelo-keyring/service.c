/* SPDX-License-Identifier: GPL-3.0-or-later */
/* pomelo-keyring — Secret Service（org.freedesktop.secrets）的 D-Bus 實作
 *
 * 物件：
 *   /org/freedesktop/secrets                       Service
 *   /org/freedesktop/secrets/collection/login      唯一的鑰匙圈「登入」
 *   /org/freedesktop/secrets/aliases/default       同一個鑰匙圈（別名）
 *   /org/freedesktop/secrets/collection/login/<id> 項目
 *   /org/freedesktop/secrets/session/s<n>          客戶端的連線（明文或 DH 加密）
 *   /org/freedesktop/secrets/prompt/p<n>           需要使用者輸入密碼的動作
 * 另外有一個私有介面 org.pomelo.Keyring 給設定程式用（狀態、列表、刪除、鎖定、改密碼）。
 *
 * 只接受同一個使用者的連線；鎖定時搜尋仍然可以用這次登入後看過的標籤與屬性，
 * 這樣 libsecret 才會知道「有這個項目但要先解鎖」，然後透過 Prompt 請使用者輸入密碼。
 */
#define _GNU_SOURCE
#include <pomelo.h>
#include "service.h"
#include <sys/mman.h>

#define SVC "/org/freedesktop/secrets"
#define COLL SVC "/collection/login"
#define ALIAS SVC "/aliases/default"
#define SESS SVC "/session/s"
#define PRMT SVC "/prompt/p"
#define IF_SVC "org.freedesktop.Secret.Service"
#define IF_COLL "org.freedesktop.Secret.Collection"
#define IF_ITEM "org.freedesktop.Secret.Item"
#define IF_SESS "org.freedesktop.Secret.Session"
#define IF_PRMT "org.freedesktop.Secret.Prompt"
#define IF_PROPS "org.freedesktop.DBus.Properties"
#define IF_INTRO "org.freedesktop.DBus.Introspectable"
#define IF_PRIV "org.pomelo.Keyring"
#define ERR_LOCKED "org.freedesktop.Secret.Error.IsLocked"
#define ERR_NOSESS "org.freedesktop.Secret.Error.NoSession"
#define ERR_NOOBJ "org.freedesktop.Secret.Error.NoSuchObject"
#define ERR_ARGS "org.freedesktop.DBus.Error.InvalidArgs"
#define ERR_NOTSUP "org.freedesktop.DBus.Error.NotSupported"
#define ERR_DENIED "org.freedesktop.DBus.Error.AccessDenied"
#define ERR_FAILED "org.freedesktop.DBus.Error.Failed"
#define ALG_PLAIN "plain"
#define ALG_DH "dh-ietf1024-sha256-aes128-cbc-pkcs7"

static DBusConnection *bus;
static KStore *store;
static char *login_pw;   /* 登入密碼解不開時先留著：使用者用舊密碼解鎖後，改用登入密碼重新加密 */

typedef struct { unsigned id; char owner[64]; int dh; KSessionKey key; } Session;
static Session sessions[64];
static int nsessions;
static unsigned next_session = 1;

enum { P_UNLOCK, P_CREATE, P_SEARCH };
typedef struct {
	unsigned id;
	int type, shown;
	char owner[64];
	char **objs; int nobjs;                /* P_UNLOCK：要解鎖的物件 */
	char *label, *ctype; KAttr *attrs; int nattrs;   /* P_CREATE */
	unsigned char *secret; size_t slen; int replace;
	DBusMessage *deferred; int service_level;      /* P_SEARCH：鎖定時收到的搜尋，解鎖後再回覆 */
} Prompt;
static Prompt prompts[16];
static int nprompts;
static unsigned next_prompt = 1;
static void reply_search(DBusMessage *m, int service_level);

/* ------------------------------------------------------------------ */
/* 小工具                                                              */
/* ------------------------------------------------------------------ */

static void send_reply(DBusMessage *r) { dbus_connection_send(bus, r, NULL); dbus_message_unref(r); }

static DBusHandlerResult error(DBusMessage *m, const char *name, const char *text)
{
	send_reply(dbus_message_new_error(m, name, text));
	return DBUS_HANDLER_RESULT_HANDLED;
}

static int sig_is(DBusMessage *m, const char *sig) { return !strcmp(dbus_message_get_signature(m), sig); }

/* 只接受同一個使用者（結果快取，不用每次都問 dbus-daemon） */
static int same_user(const char *sender)
{
	static struct { char name[64]; int ok; } cache[32];
	static int n, next;
	for (int i = 0; i < n; i++) if (!strcmp(cache[i].name, sender)) return cache[i].ok;
	DBusError e;
	dbus_error_init(&e);
	unsigned long uid = dbus_bus_get_unix_user(bus, sender, &e);
	int ok = !dbus_error_is_set(&e) && uid == getuid();
	dbus_error_free(&e);
	int k = n < 32 ? n++ : next;
	next = (k + 1) % 32;
	snprintf(cache[k].name, sizeof cache[k].name, "%s", sender);
	cache[k].ok = ok;
	return ok;
}

/* 發出要求的程式名稱（顯示在解鎖視窗上） */
static void sender_app(const char *sender, char *out, size_t n)
{
	/* libdbus 沒有現成的函式，直接問 dbus-daemon 這個連線的 PID */
	DBusMessage *q = dbus_message_new_method_call("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
	                                              "GetConnectionUnixProcessID");
	dbus_message_append_args(q, DBUS_TYPE_STRING, &sender, DBUS_TYPE_INVALID);
	DBusMessage *a = dbus_connection_send_with_reply_and_block(bus, q, 2000, NULL);
	dbus_message_unref(q);
	dbus_uint32_t pid = 0;
	if (a) { dbus_message_get_args(a, NULL, DBUS_TYPE_UINT32, &pid, DBUS_TYPE_INVALID); dbus_message_unref(a); }
	out[0] = 0;
	if (pid) {
		char p[64];
		snprintf(p, sizeof p, "/proc/%u/comm", (unsigned)pid);
		FILE *f = fopen(p, "r");
		if (f) { if (fgets(out, (int)n, f)) out[strcspn(out, "\n")] = 0; fclose(f); }
	}
	if (!out[0]) snprintf(out, n, "%s", _("某個程式"));
}

static void item_path(const KItem *it, char *out, size_t n) { snprintf(out, n, COLL "/%u", it->id); }

static KItem *item_at_path(const char *path)
{
	size_t cl = strlen(COLL);
	if (strncmp(path, COLL "/", cl + 1)) return NULL;
	char *end;
	unsigned long id = strtoul(path + cl + 1, &end, 10);
	if (*end || !*(path + cl + 1)) return NULL;
	return ks_find(store, (unsigned)id);
}

static int is_collection(const char *path) { return !strcmp(path, COLL) || !strcmp(path, ALIAS); }

static Session *find_session(const char *path)
{
	if (!path || strncmp(path, SESS, strlen(SESS))) return NULL;
	unsigned id = (unsigned)strtoul(path + strlen(SESS), NULL, 10);
	for (int i = 0; i < nsessions; i++) if (sessions[i].id == id) return &sessions[i];
	return NULL;
}

static void append_path(DBusMessageIter *it, const char *p) { dbus_message_iter_append_basic(it, DBUS_TYPE_OBJECT_PATH, &p); }

static void append_bytes(DBusMessageIter *it, const unsigned char *p, size_t n)
{
	DBusMessageIter a;
	const unsigned char *pp = p ? p : (const unsigned char *)"";
	dbus_message_iter_open_container(it, DBUS_TYPE_ARRAY, "y", &a);
	dbus_message_iter_append_fixed_array(&a, DBUS_TYPE_BYTE, &pp, (int)n);
	dbus_message_iter_close_container(it, &a);
}

/* 把密碼依該連線的方式（明文或 DH）包成 (oayays) */
static int append_secret(DBusMessageIter *it, Session *s, const KItem *item)
{
	char spath[96];
	snprintf(spath, sizeof spath, SESS "%u", s->id);
	unsigned char iv[16], *enc = NULL;
	size_t encn = 0;
	if (s->dh && kc_encrypt(&s->key, item->secret, item->slen, iv, &enc, &encn)) return -1;
	DBusMessageIter st;
	dbus_message_iter_open_container(it, DBUS_TYPE_STRUCT, NULL, &st);
	append_path(&st, spath);
	append_bytes(&st, s->dh ? iv : NULL, s->dh ? 16 : 0);
	append_bytes(&st, s->dh ? enc : item->secret, s->dh ? encn : item->slen);
	const char *ct = item->ctype ? item->ctype : "text/plain";
	dbus_message_iter_append_basic(&st, DBUS_TYPE_STRING, &ct);
	dbus_message_iter_close_container(it, &st);
	if (enc) { kc_wipe(enc, encn); free(enc); }
	return 0;
}

static int read_byte_array(DBusMessageIter *it, const unsigned char **p, int *n)
{
	if (dbus_message_iter_get_arg_type(it) != DBUS_TYPE_ARRAY || dbus_message_iter_get_element_type(it) != DBUS_TYPE_BYTE) return -1;
	DBusMessageIter a;
	dbus_message_iter_recurse(it, &a);
	if (dbus_message_iter_get_arg_type(&a) == DBUS_TYPE_BYTE) dbus_message_iter_get_fixed_array(&a, p, n);
	else { *p = NULL; *n = 0; }
	return 0;
}

/* 讀 (oayays)，解開後的密碼放在 *out（要 kc_wipe + free）；回傳錯誤名稱或 NULL */
static const char *read_secret(DBusMessageIter *it, unsigned char **out, size_t *outn, char **ctype)
{
	*out = NULL;
	if (dbus_message_iter_get_arg_type(it) != DBUS_TYPE_STRUCT) return ERR_ARGS;
	DBusMessageIter st;
	dbus_message_iter_recurse(it, &st);
	if (dbus_message_iter_get_arg_type(&st) != DBUS_TYPE_OBJECT_PATH) return ERR_ARGS;
	const char *spath, *ct;
	dbus_message_iter_get_basic(&st, &spath);
	Session *s = find_session(spath);
	if (!s) return ERR_NOSESS;
	dbus_message_iter_next(&st);
	const unsigned char *params, *val;
	int pn, vn;
	if (read_byte_array(&st, &params, &pn)) return ERR_ARGS;
	dbus_message_iter_next(&st);
	if (read_byte_array(&st, &val, &vn)) return ERR_ARGS;
	dbus_message_iter_next(&st);
	if (dbus_message_iter_get_arg_type(&st) != DBUS_TYPE_STRING) return ERR_ARGS;
	dbus_message_iter_get_basic(&st, &ct);
	*ctype = strdup(ct);
	if (s->dh) {
		if (pn != 16 || kc_decrypt(&s->key, params, val, (size_t)vn, out, outn)) return ERR_ARGS;
	} else {
		*out = malloc((size_t)vn + 1);
		memcpy(*out, val, (size_t)vn);
		(*out)[vn] = 0;
		*outn = (size_t)vn;
	}
	return NULL;
}

/* a{ss} → KAttr */
static int read_attrs(DBusMessageIter *it, KAttr **out, int *n)
{
	*out = NULL; *n = 0;
	if (dbus_message_iter_get_arg_type(it) != DBUS_TYPE_ARRAY) return -1;
	DBusMessageIter a;
	dbus_message_iter_recurse(it, &a);
	int cap = 8;
	KAttr *v = calloc((size_t)cap, sizeof *v);
	while (dbus_message_iter_get_arg_type(&a) == DBUS_TYPE_DICT_ENTRY) {
		DBusMessageIter e;
		dbus_message_iter_recurse(&a, &e);
		const char *k, *val;
		if (dbus_message_iter_get_arg_type(&e) != DBUS_TYPE_STRING) break;
		dbus_message_iter_get_basic(&e, &k);
		dbus_message_iter_next(&e);
		if (dbus_message_iter_get_arg_type(&e) != DBUS_TYPE_STRING) break;
		dbus_message_iter_get_basic(&e, &val);
		if (*n == cap) { cap *= 2; v = realloc(v, (size_t)cap * sizeof *v); }
		v[*n].key = strdup(k);
		v[*n].val = strdup(val);
		(*n)++;
		if (*n > 1000) break;
		dbus_message_iter_next(&a);
	}
	*out = v;
	return 0;
}

static void append_attrs(DBusMessageIter *it, const KAttr *a, int n)
{
	DBusMessageIter arr, e;
	dbus_message_iter_open_container(it, DBUS_TYPE_ARRAY, "{ss}", &arr);
	for (int i = 0; i < n; i++) {
		dbus_message_iter_open_container(&arr, DBUS_TYPE_DICT_ENTRY, NULL, &e);
		dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &a[i].key);
		dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &a[i].val);
		dbus_message_iter_close_container(&arr, &e);
	}
	dbus_message_iter_close_container(it, &arr);
}

static void append_variant_bool(DBusMessageIter *it, int b)
{
	DBusMessageIter v;
	dbus_bool_t bb = b;
	dbus_message_iter_open_container(it, DBUS_TYPE_VARIANT, "b", &v);
	dbus_message_iter_append_basic(&v, DBUS_TYPE_BOOLEAN, &bb);
	dbus_message_iter_close_container(it, &v);
}
static void append_variant_str(DBusMessageIter *it, const char *s)
{
	DBusMessageIter v;
	dbus_message_iter_open_container(it, DBUS_TYPE_VARIANT, "s", &v);
	dbus_message_iter_append_basic(&v, DBUS_TYPE_STRING, &s);
	dbus_message_iter_close_container(it, &v);
}
static void append_variant_u64(DBusMessageIter *it, uint64_t u)
{
	DBusMessageIter v;
	dbus_message_iter_open_container(it, DBUS_TYPE_VARIANT, "t", &v);
	dbus_message_iter_append_basic(&v, DBUS_TYPE_UINT64, &u);
	dbus_message_iter_close_container(it, &v);
}
static void append_variant_path(DBusMessageIter *it, const char *p)
{
	DBusMessageIter v;
	dbus_message_iter_open_container(it, DBUS_TYPE_VARIANT, "o", &v);
	append_path(&v, p);
	dbus_message_iter_close_container(it, &v);
}
static void append_variant_paths(DBusMessageIter *it, char **paths, int n)
{
	DBusMessageIter v, a;
	dbus_message_iter_open_container(it, DBUS_TYPE_VARIANT, "ao", &v);
	dbus_message_iter_open_container(&v, DBUS_TYPE_ARRAY, "o", &a);
	for (int i = 0; i < n; i++) append_path(&a, paths[i]);
	dbus_message_iter_close_container(&v, &a);
	dbus_message_iter_close_container(it, &v);
}
static void append_variant_attrs(DBusMessageIter *it, const KAttr *a, int n)
{
	DBusMessageIter v;
	dbus_message_iter_open_container(it, DBUS_TYPE_VARIANT, "a{ss}", &v);
	append_attrs(&v, a, n);
	dbus_message_iter_close_container(it, &v);
}
static void append_item_paths(DBusMessageIter *it)
{
	DBusMessageIter a;
	dbus_message_iter_open_container(it, DBUS_TYPE_ARRAY, "o", &a);
	for (int i = 0; i < store->nitems; i++) {
		char p[128];
		item_path(&store->items[i], p, sizeof p);
		append_path(&a, p);
	}
	dbus_message_iter_close_container(it, &a);
}

static void dict_open(DBusMessageIter *arr, DBusMessageIter *e, const char *key)
{
	dbus_message_iter_open_container(arr, DBUS_TYPE_DICT_ENTRY, NULL, e);
	dbus_message_iter_append_basic(e, DBUS_TYPE_STRING, &key);
}

/* ---------------- 訊號 ---------------- */

static void emit_item_signal(const char *name, const KItem *it)
{
	char p[128];
	item_path(it, p, sizeof p);
	DBusMessage *s = dbus_message_new_signal(COLL, IF_COLL, name);
	const char *pp = p;
	dbus_message_append_args(s, DBUS_TYPE_OBJECT_PATH, &pp, DBUS_TYPE_INVALID);
	send_reply(s);
}

static void emit_locked_changed(void)
{
	DBusMessage *s = dbus_message_new_signal(COLL, IF_PROPS, "PropertiesChanged");
	DBusMessageIter it, arr, e, inv;
	const char *iface = IF_COLL;
	dbus_message_iter_init_append(s, &it);
	dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &iface);
	dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &arr);
	dict_open(&arr, &e, "Locked");
	append_variant_bool(&e, !store->unlocked);
	dbus_message_iter_close_container(&arr, &e);
	dbus_message_iter_close_container(&it, &arr);
	dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "s", &inv);
	dbus_message_iter_close_container(&it, &inv);
	send_reply(s);
	DBusMessage *s2 = dbus_message_new_signal(SVC, IF_SVC, "CollectionChanged");
	const char *cp = COLL;
	dbus_message_append_args(s2, DBUS_TYPE_OBJECT_PATH, &cp, DBUS_TYPE_INVALID);
	send_reply(s2);
}

/* ------------------------------------------------------------------ */
/* Prompt（需要使用者輸入密碼的動作）                                    */
/* ------------------------------------------------------------------ */

static void prompt_free(Prompt *p)
{
	for (int i = 0; i < p->nobjs; i++) free(p->objs[i]);
	free(p->objs);
	free(p->label); free(p->ctype);
	ks_free_attrs(p->attrs, p->nattrs);
	if (p->secret) { kc_wipe(p->secret, p->slen); free(p->secret); }
	if (p->deferred) dbus_message_unref(p->deferred);
	memset(p, 0, sizeof *p);
}

static Prompt *prompt_new(const char *owner, int type, char *path, size_t n)
{
	if (nprompts == (int)LEN(prompts)) { prompt_free(&prompts[0]); memmove(prompts, prompts + 1, sizeof prompts - sizeof prompts[0]); nprompts--; }
	Prompt *p = &prompts[nprompts++];
	memset(p, 0, sizeof *p);
	p->id = next_prompt++;
	p->type = type;
	snprintf(p->owner, sizeof p->owner, "%s", owner ? owner : "");
	snprintf(path, n, PRMT "%u", p->id);
	return p;
}

static Prompt *prompt_find(unsigned id)
{
	for (int i = 0; i < nprompts; i++) if (prompts[i].id == id) return &prompts[i];
	return NULL;
}

static void prompt_remove(Prompt *p)
{
	int i = (int)(p - prompts);
	prompt_free(p);
	memmove(&prompts[i], &prompts[i + 1], (size_t)(nprompts - i - 1) * sizeof *prompts);
	nprompts--;
}

/* 執行 prompt 代表的動作（鑰匙圈已解鎖），並發出 Completed */
static void prompt_complete(Prompt *p, int dismissed)
{
	if (p->type == P_SEARCH) {
		if (dismissed) {
			/* 沒解鎖：回兩個空陣列（Service）或一個空陣列（Collection） */
			DBusMessage *r = dbus_message_new_method_return(p->deferred);
			DBusMessageIter it, a;
			dbus_message_iter_init_append(r, &it);
			for (int k = 0; k < (p->service_level ? 2 : 1); k++) {
				dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "o", &a);
				dbus_message_iter_close_container(&it, &a);
			}
			send_reply(r);
		} else reply_search(p->deferred, p->service_level);
		prompt_remove(p);
		return;
	}
	char path[96];
	snprintf(path, sizeof path, PRMT "%u", p->id);
	DBusMessage *s = dbus_message_new_signal(path, IF_PRMT, "Completed");
	DBusMessageIter it;
	dbus_bool_t d = dismissed;
	dbus_message_iter_init_append(s, &it);
	dbus_message_iter_append_basic(&it, DBUS_TYPE_BOOLEAN, &d);
	/* 取消時結果也要是正確的型別（libsecret 會檢查）：解鎖給空的 ao、建立給 "/" */
	if (dismissed && p->type == P_UNLOCK) {
		append_variant_paths(&it, NULL, 0);
	} else if (dismissed) {
		append_variant_path(&it, "/");
	} else if (p->type == P_UNLOCK) {
		/* 只回報真的存在的物件 */
		char **ok = calloc((size_t)p->nobjs + 1, sizeof *ok);
		int n = 0;
		for (int i = 0; i < p->nobjs; i++)
			if (is_collection(p->objs[i]) || item_at_path(p->objs[i])) ok[n++] = p->objs[i];
		append_variant_paths(&it, ok, n);
		free(ok);
	} else {
		KItem *it2 = NULL;
		if (p->replace)
			for (int i = 0; i < store->nitems && !it2; i++)
				if (store->items[i].nattrs == p->nattrs && ks_match(&store->items[i], p->attrs, p->nattrs)) it2 = &store->items[i];
		int existed = it2 != NULL;
		if (it2) { ks_set_label(it2, p->label); ks_set_secret(it2, p->secret, p->slen, p->ctype); }
		else it2 = ks_add(store, p->label, p->attrs, p->nattrs, p->secret, p->slen, p->ctype);
		char ip[128] = "/";
		if (it2 && !ks_save(store)) { item_path(it2, ip, sizeof ip); emit_item_signal(existed ? "ItemChanged" : "ItemCreated", it2); }
		append_variant_path(&it, ip);
	}
	send_reply(s);
	prompt_remove(p);
}

/* 讓下一個排隊的 prompt 顯示視窗 */
static void prompt_show_next(void)
{
	for (int i = 0; i < nprompts; i++) {
		Prompt *p = &prompts[i];
		if (p->shown != 1) continue;   /* 1 = 客戶端已經呼叫 Prompt()，等著顯示 */
		if (store->unlocked) { prompt_complete(p, 0); return prompt_show_next(); }
		char app[64];
		sender_app(p->owner, app, sizeof app);
		int r = ui_prompt_open(p->id, app, !store->exists);
		if (r == 0) { p->shown = 2; return; }
		if (r == -1) { prompt_complete(p, 1); return prompt_show_next(); }
		return;   /* -2：有別的視窗在顯示，等它關掉 */
	}
}

void service_prompt_done(unsigned id, const char *pw)
{
	Prompt *p = prompt_find(id);
	if (!p) { ui_prompt_close(id); return prompt_show_next(); }
	if (!pw) {
		ui_prompt_close(id);
		prompt_complete(p, 1);
		return prompt_show_next();
	}
	int rc;
	if (!store->exists) rc = ks_create(store, pw);
	else rc = ks_unlock(store, pw);
	if (rc == -1) { ui_prompt_error(id, _("密碼不正確，請再試一次。")); return; }
	if (rc) { ui_prompt_error(id, _("鑰匙圈檔案損壞，無法開啟。")); return; }
	/* 用登入密碼解不開，但使用者知道舊密碼：改成用登入密碼加密，下次登入就會自動解鎖 */
	if (login_pw && strcmp(login_pw, pw)) {
		ks_change_password(store, login_pw);
		kc_wipe(login_pw, strlen(login_pw));
		free(login_pw);
		login_pw = NULL;
	}
	ui_prompt_close(id);
	emit_locked_changed();
	prompt_complete(p, 0);
	/* 其他等著解鎖的 prompt 也都完成了 */
	for (int i = 0; i < nprompts;) {
		if (prompts[i].shown) prompt_complete(&prompts[i], 0);
		else i++;
	}
	prompt_show_next();
}

/* ------------------------------------------------------------------ */
/* Service                                                             */
/* ------------------------------------------------------------------ */

static DBusHandlerResult h_open_session(DBusMessage *m)
{
	if (!sig_is(m, "sv")) return error(m, ERR_ARGS, "expected sv");
	DBusMessageIter it, v;
	const char *alg;
	dbus_message_iter_init(m, &it);
	dbus_message_iter_get_basic(&it, &alg);
	dbus_message_iter_next(&it);
	dbus_message_iter_recurse(&it, &v);
	if (nsessions == (int)LEN(sessions)) {   /* 太多了：丟掉最舊的 */
		kc_wipe(&sessions[0].key, sizeof sessions[0].key);
		memmove(sessions, sessions + 1, sizeof sessions - sizeof sessions[0]);
		nsessions--;
	}
	Session *s = &sessions[nsessions];
	memset(s, 0, sizeof *s);
	s->id = next_session++;
	snprintf(s->owner, sizeof s->owner, "%s", dbus_message_get_sender(m));
	DBusMessage *r = dbus_message_new_method_return(m);
	DBusMessageIter rit, rv;
	dbus_message_iter_init_append(r, &rit);
	if (!strcmp(alg, ALG_PLAIN)) {
		append_variant_str(&rit, "");
	} else if (!strcmp(alg, ALG_DH)) {
		const unsigned char *peer;
		int pn;
		unsigned char mine[128];
		size_t mn = sizeof mine;
		if (read_byte_array(&v, &peer, &pn) || pn < 1 || pn > 128 || kc_dh(peer, (size_t)pn, mine, &mn, &s->key)) {
			dbus_message_unref(r);
			return error(m, ERR_ARGS, "bad DH public key");
		}
		s->dh = 1;
		dbus_message_iter_open_container(&rit, DBUS_TYPE_VARIANT, "ay", &rv);
		append_bytes(&rv, mine, mn);
		dbus_message_iter_close_container(&rit, &rv);
	} else {
		dbus_message_unref(r);
		return error(m, ERR_NOTSUP, "unsupported algorithm");
	}
	nsessions++;
	char path[96];
	snprintf(path, sizeof path, SESS "%u", s->id);
	append_path(&rit, path);
	send_reply(r);
	return DBUS_HANDLER_RESULT_HANDLED;
}

static void prompt_show_next(void);

static void reply_search(DBusMessage *m, int service_level)
{
	DBusMessageIter it;
	dbus_message_iter_init(m, &it);
	KAttr *a;
	int n;
	read_attrs(&it, &a, &n);
	DBusMessage *r = dbus_message_new_method_return(m);
	DBusMessageIter rit, arr, arr2;
	dbus_message_iter_init_append(r, &rit);
	dbus_message_iter_open_container(&rit, DBUS_TYPE_ARRAY, "o", &arr);
	if (service_level && !store->unlocked) dbus_message_iter_close_container(&rit, &arr), dbus_message_iter_open_container(&rit, DBUS_TYPE_ARRAY, "o", &arr2);
	DBusMessageIter *target = service_level && !store->unlocked ? &arr2 : &arr;
	if (store->meta || store->unlocked)
		for (int i = 0; i < store->nitems; i++)
			if (ks_match(&store->items[i], a, n)) {
				char p[128];
				item_path(&store->items[i], p, sizeof p);
				append_path(target, p);
			}
	dbus_message_iter_close_container(&rit, target);
	if (service_level && store->unlocked) {   /* 第二個陣列（鎖定的）是空的 */
		dbus_message_iter_open_container(&rit, DBUS_TYPE_ARRAY, "o", &arr2);
		dbus_message_iter_close_container(&rit, &arr2);
	}
	ks_free_attrs(a, n);
	send_reply(r);
}

static DBusHandlerResult h_search(DBusMessage *m, int service_level)
{
	if (!sig_is(m, "a{ss}")) return error(m, ERR_ARGS, "expected a{ss}");
	if (!store->unlocked && !store->meta && store->exists) {
		/* 鎖定而且還不知道裡面有什麼：先請使用者解鎖，再回覆（取消就回空的）。
		 * 不然 libsecret 會以為沒有這個項目，連解鎖都不會試。 */
		char ppath[96];
		Prompt *p = prompt_new(dbus_message_get_sender(m), P_SEARCH, ppath, sizeof ppath);
		p->deferred = dbus_message_ref(m);
		p->service_level = service_level;
		p->shown = 1;
		prompt_show_next();
		return DBUS_HANDLER_RESULT_HANDLED;
	}
	reply_search(m, service_level);
	return DBUS_HANDLER_RESULT_HANDLED;
}

static int read_paths(DBusMessageIter *it, char ***out)
{
	int n = 0, cap = 8;
	char **v = calloc((size_t)cap, sizeof *v);
	DBusMessageIter a;
	if (dbus_message_iter_get_arg_type(it) == DBUS_TYPE_ARRAY) {
		dbus_message_iter_recurse(it, &a);
		while (dbus_message_iter_get_arg_type(&a) == DBUS_TYPE_OBJECT_PATH && n < 1000) {
			const char *p;
			dbus_message_iter_get_basic(&a, &p);
			if (n == cap) { cap *= 2; v = realloc(v, (size_t)cap * sizeof *v); }
			v[n++] = strdup(p);
			dbus_message_iter_next(&a);
		}
	}
	*out = v;
	return n;
}

static DBusHandlerResult h_unlock(DBusMessage *m)
{
	if (!sig_is(m, "ao")) return error(m, ERR_ARGS, "expected ao");
	DBusMessageIter it;
	dbus_message_iter_init(m, &it);
	char **objs;
	int n = read_paths(&it, &objs);
	DBusMessage *r = dbus_message_new_method_return(m);
	DBusMessageIter rit, arr;
	dbus_message_iter_init_append(r, &rit);
	dbus_message_iter_open_container(&rit, DBUS_TYPE_ARRAY, "o", &arr);
	char ppath[96] = "/";
	if (store->unlocked) {
		for (int i = 0; i < n; i++) if (is_collection(objs[i]) || item_at_path(objs[i])) append_path(&arr, objs[i]);
		for (int i = 0; i < n; i++) free(objs[i]);
		free(objs);
	} else {
		Prompt *p = prompt_new(dbus_message_get_sender(m), P_UNLOCK, ppath, sizeof ppath);
		p->objs = objs;
		p->nobjs = n;
	}
	dbus_message_iter_close_container(&rit, &arr);
	append_path(&rit, ppath);
	send_reply(r);
	return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult h_lock(DBusMessage *m)
{
	if (!sig_is(m, "ao")) return error(m, ERR_ARGS, "expected ao");
	DBusMessageIter it;
	dbus_message_iter_init(m, &it);
	char **objs;
	int n = read_paths(&it, &objs);
	int was = store->unlocked;
	if (n) ks_lock(store);
	DBusMessage *r = dbus_message_new_method_return(m);
	DBusMessageIter rit, arr;
	dbus_message_iter_init_append(r, &rit);
	dbus_message_iter_open_container(&rit, DBUS_TYPE_ARRAY, "o", &arr);
	for (int i = 0; i < n; i++) { if (is_collection(objs[i]) || item_at_path(objs[i])) append_path(&arr, objs[i]); free(objs[i]); }
	free(objs);
	dbus_message_iter_close_container(&rit, &arr);
	append_path(&rit, "/");
	send_reply(r);
	if (was && !store->unlocked) emit_locked_changed();
	return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult h_get_secrets(DBusMessage *m)
{
	if (!sig_is(m, "aoo")) return error(m, ERR_ARGS, "expected aoo");
	DBusMessageIter it;
	dbus_message_iter_init(m, &it);
	char **objs;
	int n = read_paths(&it, &objs);
	dbus_message_iter_next(&it);
	const char *spath;
	dbus_message_iter_get_basic(&it, &spath);
	Session *s = find_session(spath);
	if (!s) { for (int i = 0; i < n; i++) free(objs[i]); free(objs); return error(m, ERR_NOSESS, "no such session"); }
	DBusMessage *r = dbus_message_new_method_return(m);
	DBusMessageIter rit, arr, e;
	dbus_message_iter_init_append(r, &rit);
	dbus_message_iter_open_container(&rit, DBUS_TYPE_ARRAY, "{o(oayays)}", &arr);
	for (int i = 0; i < n; i++) {
		KItem *item = store->unlocked ? item_at_path(objs[i]) : NULL;
		if (item) {
			dbus_message_iter_open_container(&arr, DBUS_TYPE_DICT_ENTRY, NULL, &e);
			append_path(&e, objs[i]);
			append_secret(&e, s, item);
			dbus_message_iter_close_container(&arr, &e);
		}
		free(objs[i]);
	}
	free(objs);
	dbus_message_iter_close_container(&rit, &arr);
	send_reply(r);
	return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult h_service(DBusMessage *m)
{
	const char *name = dbus_message_get_member(m);
	if (!strcmp(name, "OpenSession")) return h_open_session(m);
	if (!strcmp(name, "SearchItems")) return h_search(m, 1);
	if (!strcmp(name, "Unlock")) return h_unlock(m);
	if (!strcmp(name, "Lock")) return h_lock(m);
	if (!strcmp(name, "GetSecrets")) return h_get_secrets(m);
	if (!strcmp(name, "ReadAlias")) {
		const char *alias = "";
		dbus_message_get_args(m, NULL, DBUS_TYPE_STRING, &alias, DBUS_TYPE_INVALID);
		const char *p = !strcmp(alias, "default") || !strcmp(alias, "login") ? COLL : "/";
		DBusMessage *r = dbus_message_new_method_return(m);
		dbus_message_append_args(r, DBUS_TYPE_OBJECT_PATH, &p, DBUS_TYPE_INVALID);
		send_reply(r);
		return DBUS_HANDLER_RESULT_HANDLED;
	}
	if (!strcmp(name, "SetAlias")) { send_reply(dbus_message_new_method_return(m)); return DBUS_HANDLER_RESULT_HANDLED; }
	if (!strcmp(name, "CreateCollection")) {
		/* 只有一個鑰匙圈：要「預設」的就給它，其他的不支援 */
		if (!sig_is(m, "a{sv}s")) return error(m, ERR_ARGS, "expected a{sv}s");
		DBusMessageIter it;
		dbus_message_iter_init(m, &it);
		dbus_message_iter_next(&it);
		const char *alias;
		dbus_message_iter_get_basic(&it, &alias);
		if (strcmp(alias, "default") && strcmp(alias, "login") && *alias) return error(m, ERR_NOTSUP, "only one keyring");
		DBusMessage *r = dbus_message_new_method_return(m);
		const char *c = COLL, *np = "/";
		dbus_message_append_args(r, DBUS_TYPE_OBJECT_PATH, &c, DBUS_TYPE_OBJECT_PATH, &np, DBUS_TYPE_INVALID);
		send_reply(r);
		return DBUS_HANDLER_RESULT_HANDLED;
	}
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

/* ------------------------------------------------------------------ */
/* Collection                                                          */
/* ------------------------------------------------------------------ */

static DBusHandlerResult h_create_item(DBusMessage *m)
{
	if (!sig_is(m, "a{sv}(oayays)b")) return error(m, ERR_ARGS, "expected a{sv}(oayays)b");
	DBusMessageIter it, props;
	dbus_message_iter_init(m, &it);
	char *label = NULL;
	KAttr *attrs = NULL;
	int nattrs = 0;
	dbus_message_iter_recurse(&it, &props);
	while (dbus_message_iter_get_arg_type(&props) == DBUS_TYPE_DICT_ENTRY) {
		DBusMessageIter e, v;
		const char *k;
		dbus_message_iter_recurse(&props, &e);
		dbus_message_iter_get_basic(&e, &k);
		dbus_message_iter_next(&e);
		dbus_message_iter_recurse(&e, &v);
		if (!strcmp(k, IF_ITEM ".Label") && dbus_message_iter_get_arg_type(&v) == DBUS_TYPE_STRING) {
			const char *s;
			dbus_message_iter_get_basic(&v, &s);
			free(label);
			label = strdup(s);
		} else if (!strcmp(k, IF_ITEM ".Attributes")) {
			ks_free_attrs(attrs, nattrs);
			read_attrs(&v, &attrs, &nattrs);
		}
		dbus_message_iter_next(&props);
	}
	dbus_message_iter_next(&it);
	unsigned char *sec;
	size_t slen;
	char *ctype = NULL;
	const char *err = read_secret(&it, &sec, &slen, &ctype);
	if (err) { free(label); free(ctype); ks_free_attrs(attrs, nattrs); return error(m, err, "bad secret"); }
	dbus_message_iter_next(&it);
	dbus_bool_t replace = 0;
	dbus_message_iter_get_basic(&it, &replace);

	DBusMessage *r = dbus_message_new_method_return(m);
	DBusMessageIter rit;
	dbus_message_iter_init_append(r, &rit);
	if (!store->unlocked) {
		/* 鎖定中：建立一個 prompt，解鎖後再寫入 */
		char ppath[96];
		Prompt *p = prompt_new(dbus_message_get_sender(m), P_CREATE, ppath, sizeof ppath);
		p->label = label; p->attrs = attrs; p->nattrs = nattrs;
		p->secret = sec; p->slen = slen; p->ctype = ctype; p->replace = replace;
		append_path(&rit, "/");
		append_path(&rit, ppath);
		send_reply(r);
		return DBUS_HANDLER_RESULT_HANDLED;
	}
	KItem *item = NULL;
	if (replace)
		for (int i = 0; i < store->nitems && !item; i++)
			if (store->items[i].nattrs == nattrs && ks_match(&store->items[i], attrs, nattrs)) item = &store->items[i];
	int changed = item != NULL;
	if (item) { ks_set_label(item, label); ks_set_secret(item, sec, slen, ctype); }
	else item = ks_add(store, label, attrs, nattrs, sec, slen, ctype);
	kc_wipe(sec, slen);
	free(sec); free(label); free(ctype);
	ks_free_attrs(attrs, nattrs);
	if (!item || ks_save(store)) { dbus_message_unref(r); return error(m, ERR_FAILED, "cannot save keyring"); }
	char ip[128];
	item_path(item, ip, sizeof ip);
	append_path(&rit, ip);
	append_path(&rit, "/");
	send_reply(r);
	emit_item_signal(changed ? "ItemChanged" : "ItemCreated", item);
	return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult h_collection(DBusMessage *m)
{
	const char *name = dbus_message_get_member(m);
	if (!strcmp(name, "SearchItems")) return h_search(m, 0);
	if (!strcmp(name, "CreateItem")) return h_create_item(m);
	if (!strcmp(name, "Delete")) return error(m, ERR_NOTSUP, "the login keyring cannot be deleted");
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

/* ------------------------------------------------------------------ */
/* Item                                                                */
/* ------------------------------------------------------------------ */

static DBusHandlerResult h_item(DBusMessage *m, KItem *item)
{
	const char *name = dbus_message_get_member(m);
	if (!strcmp(name, "Delete")) {
		if (!store->unlocked) return error(m, ERR_LOCKED, "keyring is locked");
		KItem copy = *item;
		copy.secret = NULL;
		unsigned id = item->id;
		ks_delete(store, id);
		if (ks_save(store)) return error(m, ERR_FAILED, "cannot save keyring");
		DBusMessage *r = dbus_message_new_method_return(m);
		const char *np = "/";
		dbus_message_append_args(r, DBUS_TYPE_OBJECT_PATH, &np, DBUS_TYPE_INVALID);
		send_reply(r);
		emit_item_signal("ItemDeleted", &copy);
		return DBUS_HANDLER_RESULT_HANDLED;
	}
	if (!strcmp(name, "GetSecret")) {
		if (!sig_is(m, "o")) return error(m, ERR_ARGS, "expected o");
		if (!store->unlocked) return error(m, ERR_LOCKED, "keyring is locked");
		const char *spath;
		dbus_message_get_args(m, NULL, DBUS_TYPE_OBJECT_PATH, &spath, DBUS_TYPE_INVALID);
		Session *s = find_session(spath);
		if (!s) return error(m, ERR_NOSESS, "no such session");
		DBusMessage *r = dbus_message_new_method_return(m);
		DBusMessageIter rit;
		dbus_message_iter_init_append(r, &rit);
		if (append_secret(&rit, s, item)) { dbus_message_unref(r); return error(m, ERR_FAILED, "encrypt failed"); }
		send_reply(r);
		return DBUS_HANDLER_RESULT_HANDLED;
	}
	if (!strcmp(name, "SetSecret")) {
		if (!sig_is(m, "(oayays)")) return error(m, ERR_ARGS, "expected (oayays)");
		if (!store->unlocked) return error(m, ERR_LOCKED, "keyring is locked");
		DBusMessageIter it;
		dbus_message_iter_init(m, &it);
		unsigned char *sec;
		size_t slen;
		char *ctype = NULL;
		const char *err = read_secret(&it, &sec, &slen, &ctype);
		if (err) { free(ctype); return error(m, err, "bad secret"); }
		ks_set_secret(item, sec, slen, ctype);
		kc_wipe(sec, slen);
		free(sec); free(ctype);
		if (ks_save(store)) return error(m, ERR_FAILED, "cannot save keyring");
		send_reply(dbus_message_new_method_return(m));
		emit_item_signal("ItemChanged", item);
		return DBUS_HANDLER_RESULT_HANDLED;
	}
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

/* ------------------------------------------------------------------ */
/* Properties                                                          */
/* ------------------------------------------------------------------ */

enum { O_SVC, O_COLL, O_ITEM };

static int append_prop(DBusMessageIter *it, int kind, KItem *item, const char *name)
{
	if (kind == O_SVC) {
		if (!strcmp(name, "Collections")) {
			DBusMessageIter v, a;
			dbus_message_iter_open_container(it, DBUS_TYPE_VARIANT, "ao", &v);
			dbus_message_iter_open_container(&v, DBUS_TYPE_ARRAY, "o", &a);
			append_path(&a, COLL);
			dbus_message_iter_close_container(&v, &a);
			dbus_message_iter_close_container(it, &v);
			return 1;
		}
		return 0;
	}
	if (kind == O_COLL) {
		if (!strcmp(name, "Items")) {
			DBusMessageIter v;
			dbus_message_iter_open_container(it, DBUS_TYPE_VARIANT, "ao", &v);
			append_item_paths(&v);
			dbus_message_iter_close_container(it, &v);
		} else if (!strcmp(name, "Label")) append_variant_str(it, _("登入"));
		else if (!strcmp(name, "Locked")) append_variant_bool(it, !store->unlocked);
		else if (!strcmp(name, "Created") || !strcmp(name, "Modified")) {
			uint64_t t = 0;
			for (int i = 0; i < store->nitems; i++) {
				int64_t x = !strcmp(name, "Created") ? store->items[i].created : store->items[i].modified;
				if (!strcmp(name, "Created") ? (t == 0 || (uint64_t)x < t) : (uint64_t)x > t) t = (uint64_t)x;
			}
			append_variant_u64(it, t);
		} else return 0;
		return 1;
	}
	if (!strcmp(name, "Locked")) append_variant_bool(it, !store->unlocked);
	else if (!strcmp(name, "Attributes")) append_variant_attrs(it, item->attrs, item->nattrs);
	else if (!strcmp(name, "Label")) append_variant_str(it, item->label ? item->label : "");
	else if (!strcmp(name, "Created")) append_variant_u64(it, (uint64_t)item->created);
	else if (!strcmp(name, "Modified")) append_variant_u64(it, (uint64_t)item->modified);
	else return 0;
	return 1;
}

static DBusHandlerResult h_props(DBusMessage *m, int kind, KItem *item)
{
	const char *name = dbus_message_get_member(m), *iface = "", *prop = "";
	static const char *svc_props[] = { "Collections" };
	static const char *coll_props[] = { "Items", "Label", "Locked", "Created", "Modified" };
	static const char *item_props[] = { "Locked", "Attributes", "Label", "Created", "Modified" };
	const char **list = kind == O_SVC ? svc_props : kind == O_COLL ? coll_props : item_props;
	int nlist = kind == O_SVC ? 1 : 5;
	if (!strcmp(name, "Get")) {
		if (!sig_is(m, "ss")) return error(m, ERR_ARGS, "expected ss");
		dbus_message_get_args(m, NULL, DBUS_TYPE_STRING, &iface, DBUS_TYPE_STRING, &prop, DBUS_TYPE_INVALID);
		DBusMessage *r = dbus_message_new_method_return(m);
		DBusMessageIter it;
		dbus_message_iter_init_append(r, &it);
		if (!append_prop(&it, kind, item, prop)) { dbus_message_unref(r); return error(m, ERR_ARGS, "no such property"); }
		send_reply(r);
		return DBUS_HANDLER_RESULT_HANDLED;
	}
	if (!strcmp(name, "GetAll")) {
		DBusMessage *r = dbus_message_new_method_return(m);
		DBusMessageIter it, arr, e;
		dbus_message_iter_init_append(r, &it);
		dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &arr);
		for (int i = 0; i < nlist; i++) {
			dict_open(&arr, &e, list[i]);
			append_prop(&e, kind, item, list[i]);
			dbus_message_iter_close_container(&arr, &e);
		}
		dbus_message_iter_close_container(&it, &arr);
		send_reply(r);
		return DBUS_HANDLER_RESULT_HANDLED;
	}
	if (!strcmp(name, "Set")) {
		if (!sig_is(m, "ssv")) return error(m, ERR_ARGS, "expected ssv");
		DBusMessageIter it, v;
		dbus_message_iter_init(m, &it);
		dbus_message_iter_get_basic(&it, &iface);
		dbus_message_iter_next(&it);
		dbus_message_iter_get_basic(&it, &prop);
		dbus_message_iter_next(&it);
		dbus_message_iter_recurse(&it, &v);
		if (kind == O_ITEM) {
			if (!store->unlocked) return error(m, ERR_LOCKED, "keyring is locked");
			if (!strcmp(prop, "Label") && dbus_message_iter_get_arg_type(&v) == DBUS_TYPE_STRING) {
				const char *s;
				dbus_message_iter_get_basic(&v, &s);
				ks_set_label(item, s);
			} else if (!strcmp(prop, "Attributes")) {
				KAttr *a;
				int n;
				if (read_attrs(&v, &a, &n)) return error(m, ERR_ARGS, "expected a{ss}");
				ks_set_attrs(item, a, n);
				ks_free_attrs(a, n);
			} else return error(m, ERR_ARGS, "read-only property");
			if (ks_save(store)) return error(m, ERR_FAILED, "cannot save keyring");
			emit_item_signal("ItemChanged", item);
		}
		send_reply(dbus_message_new_method_return(m));
		return DBUS_HANDLER_RESULT_HANDLED;
	}
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

/* ------------------------------------------------------------------ */
/* 私有介面（設定程式用）                                               */
/* ------------------------------------------------------------------ */

static DBusHandlerResult h_private(DBusMessage *m)
{
	const char *name = dbus_message_get_member(m);
	if (!strcmp(name, "Status")) {
		DBusMessage *r = dbus_message_new_method_return(m);
		dbus_bool_t ex = store->exists, locked = !store->unlocked;
		dbus_uint32_t n = (dbus_uint32_t)store->nitems;
		dbus_message_append_args(r, DBUS_TYPE_BOOLEAN, &ex, DBUS_TYPE_BOOLEAN, &locked, DBUS_TYPE_UINT32, &n, DBUS_TYPE_INVALID);
		send_reply(r);
		return DBUS_HANDLER_RESULT_HANDLED;
	}
	if (!strcmp(name, "ListItems")) {
		DBusMessage *r = dbus_message_new_method_return(m);
		DBusMessageIter it, arr, st;
		dbus_message_iter_init_append(r, &it);
		dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "(uss)", &arr);
		if (store->meta || store->unlocked)
			for (int i = 0; i < store->nitems; i++) {
				KItem *x = &store->items[i];
				char sum[300] = "";
				for (int k = 0; k < x->nattrs; k++) {
					size_t l = strlen(sum);
					if (l > 200) break;
					snprintf(sum + l, sizeof sum - l, "%s%s=%s", l ? "，" : "", x->attrs[k].key, x->attrs[k].val);
				}
				dbus_message_iter_open_container(&arr, DBUS_TYPE_STRUCT, NULL, &st);
				dbus_uint32_t id = x->id;
				const char *lab = x->label ? x->label : "", *sp = sum;
				dbus_message_iter_append_basic(&st, DBUS_TYPE_UINT32, &id);
				dbus_message_iter_append_basic(&st, DBUS_TYPE_STRING, &lab);
				dbus_message_iter_append_basic(&st, DBUS_TYPE_STRING, &sp);
				dbus_message_iter_close_container(&arr, &st);
			}
		dbus_message_iter_close_container(&it, &arr);
		send_reply(r);
		return DBUS_HANDLER_RESULT_HANDLED;
	}
	if (!strcmp(name, "DeleteItem")) {
		dbus_uint32_t id = 0;
		dbus_message_get_args(m, NULL, DBUS_TYPE_UINT32, &id, DBUS_TYPE_INVALID);
		KItem *x = ks_find(store, id);
		dbus_bool_t ok = 0;
		if (x && store->unlocked) {
			KItem copy = *x;
			copy.secret = NULL;
			ks_delete(store, id);
			ok = !ks_save(store);
			if (ok) emit_item_signal("ItemDeleted", &copy);
		}
		DBusMessage *r = dbus_message_new_method_return(m);
		dbus_message_append_args(r, DBUS_TYPE_BOOLEAN, &ok, DBUS_TYPE_INVALID);
		send_reply(r);
		return DBUS_HANDLER_RESULT_HANDLED;
	}
	if (!strcmp(name, "Lock")) {
		int was = store->unlocked;
		ks_lock(store);
		send_reply(dbus_message_new_method_return(m));
		if (was) emit_locked_changed();
		return DBUS_HANDLER_RESULT_HANDLED;
	}
	if (!strcmp(name, "Unlock")) {
		const char *pw = "";
		if (!sig_is(m, "s")) return error(m, ERR_ARGS, "expected s");
		dbus_message_get_args(m, NULL, DBUS_TYPE_STRING, &pw, DBUS_TYPE_INVALID);
		dbus_bool_t ok = store->unlocked;
		if (!ok) {
			int rc = store->exists ? ks_unlock(store, pw) : ks_create(store, pw);
			ok = rc == 0;
			if (ok) emit_locked_changed();
		}
		DBusMessage *r = dbus_message_new_method_return(m);
		dbus_message_append_args(r, DBUS_TYPE_BOOLEAN, &ok, DBUS_TYPE_INVALID);
		send_reply(r);
		return DBUS_HANDLER_RESULT_HANDLED;
	}
	if (!strcmp(name, "ChangePassword")) {
		const char *old = "", *nw = "";
		if (!sig_is(m, "ss")) return error(m, ERR_ARGS, "expected ss");
		dbus_message_get_args(m, NULL, DBUS_TYPE_STRING, &old, DBUS_TYPE_STRING, &nw, DBUS_TYPE_INVALID);
		dbus_bool_t ok = 0;
		/* 要先用舊密碼驗證（就算現在已經解鎖） */
		KStore tmp;
		ks_init(&tmp, store->path);
		if (*nw && !ks_unlock(&tmp, old)) {
			ks_forget(&tmp);
			if (!store->unlocked) ks_unlock(store, old);
			ok = store->unlocked && !ks_change_password(store, nw);
		}
		ks_forget(&tmp);
		DBusMessage *r = dbus_message_new_method_return(m);
		dbus_message_append_args(r, DBUS_TYPE_BOOLEAN, &ok, DBUS_TYPE_INVALID);
		send_reply(r);
		return DBUS_HANDLER_RESULT_HANDLED;
	}
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

/* ------------------------------------------------------------------ */
/* Introspect                                                          */
/* ------------------------------------------------------------------ */

static const char *xml_head =
	"<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\" "
	"\"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n<node>"
	"<interface name=\"org.freedesktop.DBus.Introspectable\"><method name=\"Introspect\"><arg type=\"s\" direction=\"out\"/></method></interface>"
	"<interface name=\"org.freedesktop.DBus.Properties\">"
	"<method name=\"Get\"><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"v\" direction=\"out\"/></method>"
	"<method name=\"GetAll\"><arg type=\"s\" direction=\"in\"/><arg type=\"a{sv}\" direction=\"out\"/></method>"
	"<method name=\"Set\"><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"v\" direction=\"in\"/></method>"
	"<signal name=\"PropertiesChanged\"><arg type=\"s\"/><arg type=\"a{sv}\"/><arg type=\"as\"/></signal></interface>";
static const char *xml_svc =
	"<interface name=\"org.freedesktop.Secret.Service\">"
	"<method name=\"OpenSession\"><arg type=\"s\" direction=\"in\"/><arg type=\"v\" direction=\"in\"/><arg type=\"v\" direction=\"out\"/><arg type=\"o\" direction=\"out\"/></method>"
	"<method name=\"CreateCollection\"><arg type=\"a{sv}\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"o\" direction=\"out\"/><arg type=\"o\" direction=\"out\"/></method>"
	"<method name=\"SearchItems\"><arg type=\"a{ss}\" direction=\"in\"/><arg type=\"ao\" direction=\"out\"/><arg type=\"ao\" direction=\"out\"/></method>"
	"<method name=\"Unlock\"><arg type=\"ao\" direction=\"in\"/><arg type=\"ao\" direction=\"out\"/><arg type=\"o\" direction=\"out\"/></method>"
	"<method name=\"Lock\"><arg type=\"ao\" direction=\"in\"/><arg type=\"ao\" direction=\"out\"/><arg type=\"o\" direction=\"out\"/></method>"
	"<method name=\"GetSecrets\"><arg type=\"ao\" direction=\"in\"/><arg type=\"o\" direction=\"in\"/><arg type=\"a{o(oayays)}\" direction=\"out\"/></method>"
	"<method name=\"ReadAlias\"><arg type=\"s\" direction=\"in\"/><arg type=\"o\" direction=\"out\"/></method>"
	"<method name=\"SetAlias\"><arg type=\"s\" direction=\"in\"/><arg type=\"o\" direction=\"in\"/></method>"
	"<property name=\"Collections\" type=\"ao\" access=\"read\"/>"
	"<signal name=\"CollectionCreated\"><arg type=\"o\"/></signal><signal name=\"CollectionDeleted\"><arg type=\"o\"/></signal><signal name=\"CollectionChanged\"><arg type=\"o\"/></signal>"
	"</interface><interface name=\"org.pomelo.Keyring\">"
	"<method name=\"Status\"><arg type=\"b\" direction=\"out\"/><arg type=\"b\" direction=\"out\"/><arg type=\"u\" direction=\"out\"/></method>"
	"<method name=\"ListItems\"><arg type=\"a(uss)\" direction=\"out\"/></method>"
	"<method name=\"DeleteItem\"><arg type=\"u\" direction=\"in\"/><arg type=\"b\" direction=\"out\"/></method>"
	"<method name=\"Lock\"/><method name=\"Unlock\"><arg type=\"s\" direction=\"in\"/><arg type=\"b\" direction=\"out\"/></method>"
	"<method name=\"ChangePassword\"><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"b\" direction=\"out\"/></method>"
	"</interface>";
static const char *xml_coll =
	"<interface name=\"org.freedesktop.Secret.Collection\">"
	"<method name=\"Delete\"><arg type=\"o\" direction=\"out\"/></method>"
	"<method name=\"SearchItems\"><arg type=\"a{ss}\" direction=\"in\"/><arg type=\"ao\" direction=\"out\"/></method>"
	"<method name=\"CreateItem\"><arg type=\"a{sv}\" direction=\"in\"/><arg type=\"(oayays)\" direction=\"in\"/><arg type=\"b\" direction=\"in\"/><arg type=\"o\" direction=\"out\"/><arg type=\"o\" direction=\"out\"/></method>"
	"<property name=\"Items\" type=\"ao\" access=\"read\"/><property name=\"Label\" type=\"s\" access=\"readwrite\"/>"
	"<property name=\"Locked\" type=\"b\" access=\"read\"/><property name=\"Created\" type=\"t\" access=\"read\"/><property name=\"Modified\" type=\"t\" access=\"read\"/>"
	"<signal name=\"ItemCreated\"><arg type=\"o\"/></signal><signal name=\"ItemDeleted\"><arg type=\"o\"/></signal><signal name=\"ItemChanged\"><arg type=\"o\"/></signal>"
	"</interface>";
static const char *xml_item =
	"<interface name=\"org.freedesktop.Secret.Item\">"
	"<method name=\"Delete\"><arg type=\"o\" direction=\"out\"/></method>"
	"<method name=\"GetSecret\"><arg type=\"o\" direction=\"in\"/><arg type=\"(oayays)\" direction=\"out\"/></method>"
	"<method name=\"SetSecret\"><arg type=\"(oayays)\" direction=\"in\"/></method>"
	"<property name=\"Locked\" type=\"b\" access=\"read\"/><property name=\"Attributes\" type=\"a{ss}\" access=\"readwrite\"/>"
	"<property name=\"Label\" type=\"s\" access=\"readwrite\"/><property name=\"Created\" type=\"t\" access=\"read\"/><property name=\"Modified\" type=\"t\" access=\"read\"/>"
	"</interface>";
static const char *xml_sess = "<interface name=\"org.freedesktop.Secret.Session\"><method name=\"Close\"/></interface>";
static const char *xml_prmt =
	"<interface name=\"org.freedesktop.Secret.Prompt\"><method name=\"Prompt\"><arg type=\"s\" direction=\"in\"/></method>"
	"<method name=\"Dismiss\"/><signal name=\"Completed\"><arg type=\"b\"/><arg type=\"v\"/></signal></interface>";

static DBusHandlerResult h_introspect(DBusMessage *m, const char *body, int with_items)
{
	char *xml = malloc(strlen(xml_head) + strlen(body) + 64 + (size_t)store->nitems * 32);
	strcpy(xml, xml_head);
	strcat(xml, body);
	if (with_items)
		for (int i = 0; i < store->nitems; i++) sprintf(xml + strlen(xml), "<node name=\"%u\"/>", store->items[i].id);
	strcat(xml, "</node>");
	DBusMessage *r = dbus_message_new_method_return(m);
	dbus_message_append_args(r, DBUS_TYPE_STRING, &xml, DBUS_TYPE_INVALID);
	send_reply(r);
	free(xml);
	return DBUS_HANDLER_RESULT_HANDLED;
}

/* ------------------------------------------------------------------ */
/* 分派                                                                */
/* ------------------------------------------------------------------ */

static DBusHandlerResult dispatch(DBusConnection *c, DBusMessage *m, void *data)
{
	(void)c; (void)data;
	if (dbus_message_get_type(m) != DBUS_MESSAGE_TYPE_METHOD_CALL) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	const char *path = dbus_message_get_path(m), *iface = dbus_message_get_interface(m), *name = dbus_message_get_member(m);
	const char *sender = dbus_message_get_sender(m);
	if (!path || !sender) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	if (!same_user(sender)) return error(m, ERR_DENIED, "not the same user");
	if (!iface) iface = "";
	int is_intro = !strcmp(iface, IF_INTRO) && !strcmp(name, "Introspect");
	int is_props = !strcmp(iface, IF_PROPS);

	if (!strcmp(path, SVC)) {
		if (is_intro) return h_introspect(m, xml_svc, 0);
		if (is_props) return h_props(m, O_SVC, NULL);
		if (!strcmp(iface, IF_PRIV)) return h_private(m);
		return h_service(m);
	}
	if (is_collection(path)) {
		if (is_intro) return h_introspect(m, xml_coll, 1);
		if (is_props) return h_props(m, O_COLL, NULL);
		return h_collection(m);
	}
	KItem *item = item_at_path(path);
	if (item) {
		if (is_intro) return h_introspect(m, xml_item, 0);
		if (is_props) return h_props(m, O_ITEM, item);
		return h_item(m, item);
	}
	if (!strncmp(path, COLL "/", strlen(COLL) + 1)) return error(m, ERR_NOOBJ, "no such item");
	Session *s = find_session(path);
	if (s) {
		if (is_intro) return h_introspect(m, xml_sess, 0);
		if (!strcmp(name, "Close")) {
			kc_wipe(&s->key, sizeof s->key);
			int i = (int)(s - sessions);
			memmove(&sessions[i], &sessions[i + 1], (size_t)(nsessions - i - 1) * sizeof *sessions);
			nsessions--;
			send_reply(dbus_message_new_method_return(m));
			return DBUS_HANDLER_RESULT_HANDLED;
		}
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}
	if (!strncmp(path, PRMT, strlen(PRMT))) {
		unsigned id = (unsigned)strtoul(path + strlen(PRMT), NULL, 10);
		Prompt *p = prompt_find(id);
		if (!p) return error(m, ERR_NOOBJ, "no such prompt");
		if (is_intro) return h_introspect(m, xml_prmt, 0);
		if (!strcmp(name, "Prompt")) {
			send_reply(dbus_message_new_method_return(m));
			if (!p->shown) { p->shown = 1; prompt_show_next(); }
			return DBUS_HANDLER_RESULT_HANDLED;
		}
		if (!strcmp(name, "Dismiss")) {
			send_reply(dbus_message_new_method_return(m));
			if (p->shown == 2) ui_prompt_close(p->id);
			prompt_complete(p, 1);
			prompt_show_next();
			return DBUS_HANDLER_RESULT_HANDLED;
		}
	}
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static DBusObjectPathVTable vtable = { .message_function = dispatch };

void service_init(DBusConnection *c, KStore *s, char *pw)
{
	bus = c;
	store = s;
	if (pw) mlock(pw, strlen(pw) + 1);
	if (pw && *pw) {
		int rc = s->exists ? ks_unlock(s, pw) : ks_create(s, pw);
		if (rc == 0) { kc_wipe(pw, strlen(pw)); free(pw); }
		else login_pw = pw;   /* 登入密碼跟鑰匙圈密碼不同（可能改過密碼）：留著等解鎖後同步 */
	} else if (pw) { free(pw); }
	dbus_connection_register_fallback(c, SVC, &vtable, NULL);
}

int service_is_locked(void) { return !store->unlocked; }

void service_lock(void)
{
	int was = store->unlocked;
	ks_lock(store);
	if (was) emit_locked_changed();
}
