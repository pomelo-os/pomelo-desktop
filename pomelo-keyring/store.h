/* SPDX-License-Identifier: GPL-3.0-or-later */
/* pomelo-keyring — 鑰匙圈檔案與加密（見 store.c） */
#ifndef STORE_H
#define STORE_H
#include <limits.h>
#include <stddef.h>
#include <stdint.h>

typedef struct { char *key, *val; } KAttr;

typedef struct {
	unsigned id;
	char *label, *ctype;
	KAttr *attrs;
	int nattrs;
	unsigned char *secret;   /* 鎖定時是 NULL */
	size_t slen;
	int64_t created, modified;
} KItem;

typedef struct {
	char path[PATH_MAX];
	int exists;      /* 檔案存在 */
	int unlocked;    /* 金鑰在記憶體裡，可以讀寫密碼 */
	int meta;        /* 鎖定了，但這次登入後解鎖過，還記得項目的標籤與屬性（搜尋用） */
	KItem *items;
	int nitems, cap;
	unsigned next_id;
	unsigned char key[32];
	unsigned char salt[16];
	uint32_t iters;
} KStore;

int ks_init(KStore *s, const char *path);
int ks_create(KStore *s, const char *pw);          /* 建立新的空鑰匙圈並存檔 */
int ks_unlock(KStore *s, const char *pw);          /* 0 成功；-1 密碼錯（或檔案被改過）；-2 檔案格式錯 */
void ks_lock(KStore *s);                           /* 清掉密碼與金鑰，保留標籤與屬性 */
void ks_forget(KStore *s);                         /* 連標籤與屬性都清掉 */
int ks_save(KStore *s);                            /* 加密後整個寫回（先寫暫存檔再改名） */
int ks_change_password(KStore *s, const char *newpw);

KItem *ks_add(KStore *s, const char *label, const KAttr *attrs, int n, const unsigned char *sec, size_t slen, const char *ctype);
int ks_set_secret(KItem *it, const unsigned char *sec, size_t slen, const char *ctype);
int ks_set_label(KItem *it, const char *label);
int ks_set_attrs(KItem *it, const KAttr *attrs, int n);
KItem *ks_find(KStore *s, unsigned id);
int ks_delete(KStore *s, unsigned id);
int ks_match(const KItem *it, const KAttr *attrs, int n);   /* 所有屬性都相同 */
void ks_free_attrs(KAttr *a, int n);
KAttr *ks_copy_attrs(const KAttr *a, int n);

/* 傳輸加密：Secret Service 的 dh-ietf1024-sha256-aes128-cbc-pkcs7 */
typedef struct { unsigned char key[16]; } KSessionKey;
/* 用對方的公鑰算出共用金鑰，並產生我們的公鑰（my_pub 至少 128 位元組；*mlen 傳入容量、傳出長度） */
int kc_dh(const unsigned char *peer_pub, size_t plen, unsigned char *my_pub, size_t *mlen, KSessionKey *out);
int kc_encrypt(const KSessionKey *k, const unsigned char *in, size_t n, unsigned char iv[16], unsigned char **out, size_t *outn);
int kc_decrypt(const KSessionKey *k, const unsigned char iv[16], const unsigned char *in, size_t n, unsigned char **out, size_t *outn);
int kc_random(unsigned char *buf, size_t n);
void kc_wipe(void *p, size_t n);

#endif
