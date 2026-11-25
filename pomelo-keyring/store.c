/* SPDX-License-Identifier: GPL-3.0-or-later */
/* pomelo-keyring — 鑰匙圈檔案與加密
 *
 * 檔案格式（版本 1，~/.local/share/pomelo/keyring/login.keyring）：
 *   0  "PMKR"
 *   4  版本（1）＋ 3 位元組保留
 *   8  PBKDF2 次數（u32 小端）
 *  12  鹽（16）
 *  28  GCM IV（12）
 *  40  GCM 標籤（16）
 *  56  密文長度（u32 小端）
 *  60  密文
 * 金鑰：PBKDF2-HMAC-SHA256(密碼, 鹽, 次數) → 32 位元組；密文：AES-256-GCM，前 40 位元組當作附加認證資料。
 * 密文解開後是所有項目（標籤、屬性、密碼）；也就是連「存了哪些網站的密碼」都看不到。
 *
 * 密碼錯和檔案被竄改都會讓 GCM 驗證失敗，兩者分不出來，一律回報「密碼不正確」。
 */
#define _GNU_SOURCE
#include "store.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/rand.h>

#define HDR 60
#define AAD_LEN 40
#define DEFAULT_ITERS 300000u
#define MAX_FILE (64u << 20)
#define MAX_ITEMS 20000
#define MAX_STR (1u << 20)

void kc_wipe(void *p, size_t n) { if (p && n) explicit_bzero(p, n); }
int kc_random(unsigned char *b, size_t n) { return RAND_bytes(b, (int)n) == 1 ? 0 : -1; }

static int derive(const char *pw, const unsigned char salt[16], uint32_t iters, unsigned char out[32])
{
	return PKCS5_PBKDF2_HMAC(pw, (int)strlen(pw), salt, 16, (int)iters, EVP_sha256(), 32, out) == 1 ? 0 : -1;
}

/* ---------------- 序列化 ---------------- */

typedef struct { unsigned char *d; size_t n, cap; } W;
static void w_put(W *w, const void *p, size_t n)
{
	if (w->n + n > w->cap) {
		size_t c = w->cap ? w->cap * 2 : 4096;
		while (c < w->n + n) c *= 2;
		unsigned char *d = malloc(c);
		if (w->d) { memcpy(d, w->d, w->n); kc_wipe(w->d, w->cap); free(w->d); }
		w->d = d;
		w->cap = c;
	}
	memcpy(w->d + w->n, p, n);
	w->n += n;
}
static void w_u32(W *w, uint32_t v) { unsigned char b[4] = { v, v >> 8, v >> 16, v >> 24 }; w_put(w, b, 4); }
static void w_i64(W *w, int64_t v) { w_u32(w, (uint32_t)v); w_u32(w, (uint32_t)((uint64_t)v >> 32)); }
static void w_bytes(W *w, const void *p, size_t n) { w_u32(w, (uint32_t)n); w_put(w, p, n); }
static void w_str(W *w, const char *s) { w_bytes(w, s ? s : "", s ? strlen(s) : 0); }

typedef struct { const unsigned char *d; size_t n, pos; int err; } R;
static uint32_t r_u32(R *r)
{
	if (r->err || r->pos + 4 > r->n) { r->err = 1; return 0; }
	uint32_t v = r->d[r->pos] | r->d[r->pos + 1] << 8 | r->d[r->pos + 2] << 16 | (uint32_t)r->d[r->pos + 3] << 24;
	r->pos += 4;
	return v;
}
static int64_t r_i64(R *r) { uint32_t lo = r_u32(r), hi = r_u32(r); return (int64_t)(((uint64_t)hi << 32) | lo); }
static const unsigned char *r_bytes(R *r, size_t *n)
{
	uint32_t len = r_u32(r);
	if (r->err || len > MAX_STR || r->pos + len > r->n) { r->err = 1; *n = 0; return NULL; }
	const unsigned char *p = r->d + r->pos;
	r->pos += len;
	*n = len;
	return p;
}
static char *r_str(R *r)
{
	size_t n;
	const unsigned char *p = r_bytes(r, &n);
	if (!p) return NULL;
	if (memchr(p, 0, n)) { r->err = 1; return NULL; }
	return strndup((const char *)p, n);
}

static void free_item(KItem *it)
{
	free(it->label);
	free(it->ctype);
	ks_free_attrs(it->attrs, it->nattrs);
	if (it->secret) { kc_wipe(it->secret, it->slen); free(it->secret); }
	memset(it, 0, sizeof *it);
}

static void serialize(KStore *s, W *w)
{
	w_u32(w, 0x4b524d50);
	w_u32(w, s->next_id);
	w_u32(w, (uint32_t)s->nitems);
	for (int i = 0; i < s->nitems; i++) {
		KItem *it = &s->items[i];
		w_u32(w, it->id);
		w_i64(w, it->created);
		w_i64(w, it->modified);
		w_str(w, it->label);
		w_str(w, it->ctype);
		w_u32(w, (uint32_t)it->nattrs);
		for (int k = 0; k < it->nattrs; k++) { w_str(w, it->attrs[k].key); w_str(w, it->attrs[k].val); }
		w_bytes(w, it->secret ? it->secret : (const unsigned char *)"", it->secret ? it->slen : 0);
	}
}

static int parse(KStore *s, const unsigned char *d, size_t n)
{
	R r = { d, n, 0, 0 };
	if (r_u32(&r) != 0x4b524d50) return -1;
	unsigned next = r_u32(&r);
	uint32_t cnt = r_u32(&r);
	if (r.err || cnt > MAX_ITEMS) return -1;
	KItem *items = calloc(cnt ? cnt : 1, sizeof *items);
	for (uint32_t i = 0; i < cnt; i++) {
		KItem *it = &items[i];
		it->id = r_u32(&r);
		it->created = r_i64(&r);
		it->modified = r_i64(&r);
		it->label = r_str(&r);
		it->ctype = r_str(&r);
		uint32_t na = r_u32(&r);
		if (r.err || na > 1000) goto fail;
		it->attrs = calloc(na ? na : 1, sizeof *it->attrs);
		for (uint32_t k = 0; k < na; k++) {
			it->attrs[k].key = r_str(&r);
			it->attrs[k].val = r_str(&r);
			it->nattrs = (int)k + 1;
			if (r.err) goto fail;
		}
		size_t sl;
		const unsigned char *sec = r_bytes(&r, &sl);
		if (r.err) goto fail;
		it->secret = malloc(sl + 1);
		memcpy(it->secret, sec, sl);
		it->secret[sl] = 0;
		it->slen = sl;
		if (it->id >= next) next = it->id + 1;
	}
	if (r.pos != n) goto fail;
	for (int i = 0; i < s->nitems; i++) free_item(&s->items[i]);
	free(s->items);
	s->items = items;
	s->nitems = (int)cnt;
	s->cap = (int)(cnt ? cnt : 1);
	s->next_id = next ? next : 1;
	return 0;
fail:
	for (uint32_t i = 0; i < cnt; i++) free_item(&items[i]);
	free(items);
	return -1;
}

/* ---------------- AES-GCM ---------------- */

static int gcm(int enc, const unsigned char key[32], const unsigned char iv[12], const unsigned char *aad, size_t aadn,
               const unsigned char *in, size_t n, unsigned char *out, unsigned char tag[16])
{
	EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
	int len = 0, ok = 0;
	if (!c) return -1;
	if (EVP_CipherInit_ex(c, EVP_aes_256_gcm(), NULL, NULL, NULL, enc) != 1) goto out;
	if (EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_IVLEN, 12, NULL) != 1) goto out;
	if (EVP_CipherInit_ex(c, NULL, NULL, key, iv, enc) != 1) goto out;
	if (aadn && EVP_CipherUpdate(c, NULL, &len, aad, (int)aadn) != 1) goto out;
	if (n && EVP_CipherUpdate(c, out, &len, in, (int)n) != 1) goto out;
	if (!enc && EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_TAG, 16, tag) != 1) goto out;
	if (EVP_CipherFinal_ex(c, out + len, &len) != 1) goto out;
	if (enc && EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_GET_TAG, 16, tag) != 1) goto out;
	ok = 1;
out:
	EVP_CIPHER_CTX_free(c);
	return ok ? 0 : -1;
}

/* ---------------- 檔案 ---------------- */

static void mkdirs(const char *path)
{
	char p[PATH_MAX];
	snprintf(p, sizeof p, "%s", path);
	char *slash = strrchr(p, '/');
	if (!slash) return;
	*slash = 0;
	for (char *c = p + 1; *c; c++) if (*c == '/') { *c = 0; mkdir(p, 0700); *c = '/'; }
	mkdir(p, 0700);
}

int ks_init(KStore *s, const char *path)
{
	memset(s, 0, sizeof *s);
	snprintf(s->path, sizeof s->path, "%s", path);
	s->next_id = 1;
	s->iters = DEFAULT_ITERS;
	mlock(s->key, sizeof s->key);
	struct stat st;
	s->exists = !stat(path, &st) && st.st_size >= HDR;
	return 0;
}

int ks_save(KStore *s)
{
	if (!s->unlocked) return -1;
	W w = { 0 };
	serialize(s, &w);
	unsigned char hdr[HDR];
	memcpy(hdr, "PMKR", 4);
	hdr[4] = 1; hdr[5] = hdr[6] = hdr[7] = 0;
	hdr[8] = s->iters; hdr[9] = s->iters >> 8; hdr[10] = s->iters >> 16; hdr[11] = s->iters >> 24;
	memcpy(hdr + 12, s->salt, 16);
	if (kc_random(hdr + 28, 12)) { kc_wipe(w.d, w.cap); free(w.d); return -1; }
	unsigned char *ct = malloc(w.n + 16), tag[16];
	if (gcm(1, s->key, hdr + 28, hdr, AAD_LEN, w.d, w.n, ct, tag)) { kc_wipe(w.d, w.cap); free(w.d); free(ct); return -1; }
	memcpy(hdr + 40, tag, 16);
	uint32_t n = (uint32_t)w.n;
	hdr[56] = n; hdr[57] = n >> 8; hdr[58] = n >> 16; hdr[59] = n >> 24;
	kc_wipe(w.d, w.cap);
	free(w.d);
	mkdirs(s->path);
	char tmp[PATH_MAX + 8];
	snprintf(tmp, sizeof tmp, "%s.tmp", s->path);
	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	int ok = fd >= 0 && write(fd, hdr, HDR) == HDR && (n == 0 || write(fd, ct, n) == (ssize_t)n) && fsync(fd) == 0;
	if (fd >= 0) close(fd);
	free(ct);
	if (!ok || rename(tmp, s->path)) { unlink(tmp); return -1; }
	s->exists = 1;
	return 0;
}

int ks_create(KStore *s, const char *pw)
{
	if (kc_random(s->salt, 16)) return -1;
	s->iters = DEFAULT_ITERS;
	if (derive(pw, s->salt, s->iters, s->key)) return -1;
	for (int i = 0; i < s->nitems; i++) free_item(&s->items[i]);
	s->nitems = 0;
	s->next_id = 1;
	s->unlocked = 1;
	s->meta = 1;
	if (ks_save(s)) { ks_forget(s); return -1; }
	return 0;
}

int ks_unlock(KStore *s, const char *pw)
{
	int fd = open(s->path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) return -2;
	struct stat st;
	if (fstat(fd, &st) || st.st_size < HDR || st.st_size > MAX_FILE) { close(fd); return -2; }
	size_t total = (size_t)st.st_size;
	unsigned char *f = malloc(total);
	size_t got = 0;
	ssize_t r;
	while (got < total && (r = read(fd, f + got, total - got)) > 0) got += (size_t)r;
	close(fd);
	if (got != total || memcmp(f, "PMKR", 4) || f[4] != 1) { free(f); return -2; }
	uint32_t iters = f[8] | f[9] << 8 | f[10] << 16 | (uint32_t)f[11] << 24;
	uint32_t n = f[56] | f[57] << 8 | f[58] << 16 | (uint32_t)f[59] << 24;
	if (iters < 1000 || iters > 50000000 || n != total - HDR) { free(f); return -2; }
	unsigned char key[32];
	mlock(key, sizeof key);
	if (derive(pw, f + 12, iters, key)) { free(f); return -2; }
	unsigned char *pt = malloc(n + 1);
	int rc = gcm(0, key, f + 28, f, AAD_LEN, f + HDR, n, pt, f + 40);
	if (rc) { kc_wipe(key, 32); kc_wipe(pt, n); free(pt); free(f); return -1; }
	rc = parse(s, pt, n);
	kc_wipe(pt, n);
	free(pt);
	if (rc) { kc_wipe(key, 32); free(f); return -2; }
	memcpy(s->key, key, 32);
	memcpy(s->salt, f + 12, 16);
	s->iters = iters;
	kc_wipe(key, 32);
	free(f);
	s->unlocked = 1;
	s->meta = 1;
	s->exists = 1;
	return 0;
}

void ks_lock(KStore *s)
{
	for (int i = 0; i < s->nitems; i++) {
		KItem *it = &s->items[i];
		if (it->secret) { kc_wipe(it->secret, it->slen); free(it->secret); it->secret = NULL; it->slen = 0; }
	}
	kc_wipe(s->key, sizeof s->key);
	s->unlocked = 0;
}

void ks_forget(KStore *s)
{
	ks_lock(s);
	for (int i = 0; i < s->nitems; i++) free_item(&s->items[i]);
	free(s->items);
	s->items = NULL;
	s->nitems = s->cap = 0;
	s->meta = 0;
}

int ks_change_password(KStore *s, const char *newpw)
{
	if (!s->unlocked) return -1;
	unsigned char oldkey[32], oldsalt[16];
	memcpy(oldkey, s->key, 32);
	memcpy(oldsalt, s->salt, 16);
	if (kc_random(s->salt, 16) || derive(newpw, s->salt, s->iters, s->key) || ks_save(s)) {
		memcpy(s->key, oldkey, 32);
		memcpy(s->salt, oldsalt, 16);
		kc_wipe(oldkey, 32);
		return -1;
	}
	kc_wipe(oldkey, 32);
	return 0;
}

/* ---------------- 項目 ---------------- */

void ks_free_attrs(KAttr *a, int n)
{
	for (int i = 0; i < n; i++) { free(a[i].key); free(a[i].val); }
	free(a);
}

KAttr *ks_copy_attrs(const KAttr *a, int n)
{
	KAttr *c = calloc(n ? n : 1, sizeof *c);
	for (int i = 0; i < n; i++) { c[i].key = strdup(a[i].key); c[i].val = strdup(a[i].val); }
	return c;
}

static int64_t now(void) { return (int64_t)time(NULL); }

KItem *ks_add(KStore *s, const char *label, const KAttr *attrs, int n, const unsigned char *sec, size_t slen, const char *ctype)
{
	if (!s->unlocked || s->nitems >= MAX_ITEMS) return NULL;
	if (s->nitems == s->cap) {
		s->cap = s->cap ? s->cap * 2 : 16;
		s->items = realloc(s->items, (size_t)s->cap * sizeof *s->items);
	}
	KItem *it = &s->items[s->nitems++];
	memset(it, 0, sizeof *it);
	it->id = s->next_id++;
	it->label = strdup(label ? label : "");
	it->attrs = ks_copy_attrs(attrs, n);
	it->nattrs = n;
	it->created = it->modified = now();
	ks_set_secret(it, sec, slen, ctype);
	return it;
}

int ks_set_secret(KItem *it, const unsigned char *sec, size_t slen, const char *ctype)
{
	if (it->secret) { kc_wipe(it->secret, it->slen); free(it->secret); }
	it->secret = malloc(slen + 1);
	memcpy(it->secret, sec, slen);
	it->secret[slen] = 0;
	it->slen = slen;
	free(it->ctype);
	it->ctype = strdup(ctype && *ctype ? ctype : "text/plain");
	it->modified = now();
	return 0;
}

int ks_set_label(KItem *it, const char *label)
{
	free(it->label);
	it->label = strdup(label ? label : "");
	it->modified = now();
	return 0;
}

int ks_set_attrs(KItem *it, const KAttr *attrs, int n)
{
	ks_free_attrs(it->attrs, it->nattrs);
	it->attrs = ks_copy_attrs(attrs, n);
	it->nattrs = n;
	it->modified = now();
	return 0;
}

KItem *ks_find(KStore *s, unsigned id)
{
	for (int i = 0; i < s->nitems; i++) if (s->items[i].id == id) return &s->items[i];
	return NULL;
}

int ks_delete(KStore *s, unsigned id)
{
	for (int i = 0; i < s->nitems; i++)
		if (s->items[i].id == id) {
			free_item(&s->items[i]);
			memmove(&s->items[i], &s->items[i + 1], (size_t)(s->nitems - i - 1) * sizeof *s->items);
			s->nitems--;
			return 0;
		}
	return -1;
}

int ks_match(const KItem *it, const KAttr *attrs, int n)
{
	for (int k = 0; k < n; k++) {
		int found = 0;
		for (int i = 0; i < it->nattrs && !found; i++)
			found = !strcmp(it->attrs[i].key, attrs[k].key) && !strcmp(it->attrs[i].val, attrs[k].val);
		if (!found) return 0;
	}
	return 1;
}

/* ---------------- 傳輸加密（DH 群組 2，RFC 2409）---------------- */

static const char *DH_PRIME_HEX =
	"FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD129024E088A67CC74"
	"020BBEA63B139B22514A08798E3404DDEF9519B3CD3A431B302B0A6DF25F1437"
	"4FE1356D6D51C245E485B576625E7EC6F44C42E9A637ED6B0BFF5CB6F406B7ED"
	"EE386BFB5A899FA5AE9F24117C4B1FE649286651ECE65381FFFFFFFFFFFFFFFF";

/* HKDF-SHA256，沒有鹽、沒有 info，取 16 位元組（跟 libsecret、secretstorage 一樣） */
static int hkdf16(const unsigned char *ikm, size_t n, unsigned char out[16])
{
	EVP_PKEY_CTX *c = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, NULL);
	size_t len = 16;
	int ok = c && EVP_PKEY_derive_init(c) == 1 && EVP_PKEY_CTX_set_hkdf_md(c, EVP_sha256()) == 1 &&
	         EVP_PKEY_CTX_set1_hkdf_key(c, ikm, (int)n) == 1 && EVP_PKEY_derive(c, out, &len) == 1 && len == 16;
	EVP_PKEY_CTX_free(c);
	return ok ? 0 : -1;
}

int kc_dh(const unsigned char *peer_pub, size_t plen, unsigned char *my_pub, size_t *mlen, KSessionKey *out)
{
	int rc = -1;
	BN_CTX *ctx = BN_CTX_new();
	BIGNUM *p = NULL, *g = BN_new(), *priv = BN_secure_new(), *pub = BN_new(), *peer = NULL, *shared = BN_secure_new();
	unsigned char ikm[128];
	if (!ctx || !g || !priv || !pub || !shared) goto out;
	if (!BN_hex2bn(&p, DH_PRIME_HEX) || !BN_set_word(g, 2)) goto out;
	peer = BN_bin2bn(peer_pub, (int)plen, NULL);
	if (!peer) goto out;
	/* 對方的公鑰必須在 2～p-2 之間，不然共用金鑰會很弱 */
	BIGNUM *pm2 = BN_dup(p);
	BN_sub_word(pm2, 2);
	int bad = BN_cmp(peer, BN_value_one()) <= 0 || BN_cmp(peer, pm2) > 0;
	BN_free(pm2);
	if (bad) goto out;
	if (!BN_rand(priv, 1024, BN_RAND_TOP_ANY, BN_RAND_BOTTOM_ANY)) goto out;
	if (!BN_mod_exp(pub, g, priv, p, ctx) || !BN_mod_exp(shared, peer, priv, p, ctx)) goto out;
	int n = BN_num_bytes(pub);
	if ((size_t)n > *mlen) goto out;
	BN_bn2bin(pub, my_pub);
	*mlen = (size_t)n;
	int sn = BN_num_bytes(shared);
	if (sn > (int)sizeof ikm) goto out;
	BN_bn2bin(shared, ikm);   /* 大端、不含前導零 */
	if (hkdf16(ikm, (size_t)sn, out->key)) goto out;
	rc = 0;
out:
	kc_wipe(ikm, sizeof ikm);
	BN_clear_free(priv);
	BN_clear_free(shared);
	BN_free(p); BN_free(g); BN_free(pub); BN_free(peer);
	BN_CTX_free(ctx);
	return rc;
}

static int cbc(int enc, const KSessionKey *k, const unsigned char iv[16], const unsigned char *in, size_t n,
               unsigned char **out, size_t *outn)
{
	EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
	if (!c) return -1;
	*out = malloc(n + 32);
	int len = 0, total = 0, ok = 0;
	if (EVP_CipherInit_ex(c, EVP_aes_128_cbc(), NULL, k->key, iv, enc) != 1) goto done;
	if (n && EVP_CipherUpdate(c, *out, &len, in, (int)n) != 1) goto done;
	total = len;
	if (EVP_CipherFinal_ex(c, *out + total, &len) != 1) goto done;
	total += len;
	ok = 1;
done:
	EVP_CIPHER_CTX_free(c);
	if (!ok) { kc_wipe(*out, n + 32); free(*out); *out = NULL; return -1; }
	*outn = (size_t)total;
	return 0;
}

int kc_encrypt(const KSessionKey *k, const unsigned char *in, size_t n, unsigned char iv[16], unsigned char **out, size_t *outn)
{
	if (kc_random(iv, 16)) return -1;
	return cbc(1, k, iv, in, n, out, outn);
}

int kc_decrypt(const KSessionKey *k, const unsigned char iv[16], const unsigned char *in, size_t n, unsigned char **out, size_t *outn)
{
	if (n == 0 || n % 16) return -1;
	return cbc(0, k, iv, in, n, out, outn);
}
