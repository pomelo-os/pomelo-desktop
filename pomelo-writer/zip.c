/* SPDX-License-Identifier: GPL-3.0-or-later */
/* zip.c — 見 zip.h */
#define _GNU_SOURCE
#include "zip.h"
#include <zlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define U16(p)  (unsigned)((p)[0] | ((p)[1] << 8))
#define U32(p)  (unsigned)((unsigned)(p)[0] | ((unsigned)(p)[1] << 8) | ((unsigned)(p)[2] << 16) | ((unsigned)(p)[3] << 24))

static void put_u16(unsigned char *p, unsigned v) { p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; }
static void put_u32(unsigned char *p, unsigned v) { p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; p[2] = (v >> 16) & 0xff; p[3] = (v >> 24) & 0xff; }

/* ------------------------------------------------------------------ */
/* 讀取                                                                  */
/* ------------------------------------------------------------------ */

/* 從檔案尾端往回找 End Of Central Directory（註解長度不定，最多 65535 bytes） */
static long find_eocd(const unsigned char *d, size_t len)
{
	if (len < 22) return -1;
	size_t max_back = len < 22 + 65535 ? len : 22 + 65535;
	for (size_t back = 22; back <= max_back; back++) {
		size_t i = len - back;
		if (d[i] == 0x50 && d[i + 1] == 0x4b && d[i + 2] == 0x05 && d[i + 3] == 0x06) return (long)i;
	}
	return -1;
}

Zip *zip_open_mem(const unsigned char *data, size_t len)
{
	long eocd_off = find_eocd(data, len);
	if (eocd_off < 0) return NULL;
	const unsigned char *eocd = data + eocd_off;
	unsigned n = U16(eocd + 10);              /* 中央目錄項目數 */
	unsigned cd_off = U32(eocd + 16);
	if (cd_off > len) return NULL;

	Zip *z = calloc(1, sizeof *z);
	z->data = malloc(len);
	memcpy(z->data, data, len);
	z->len = len;
	z->entries = calloc(n ? n : 1, sizeof(ZipEntry));

	const unsigned char *p = z->data + cd_off;
	const unsigned char *end = z->data + len;
	int k = 0;
	for (unsigned i = 0; i < n && p + 46 <= end; i++) {
		if (!(p[0] == 0x50 && p[1] == 0x4b && p[2] == 0x01 && p[3] == 0x02)) break;   /* 損毀的中央目錄 */
		unsigned method = U16(p + 10);
		unsigned crc = U32(p + 16);
		unsigned csize = U32(p + 20);
		unsigned usize = U32(p + 24);
		unsigned namelen = U16(p + 28);
		unsigned extralen = U16(p + 30);
		unsigned commentlen = U16(p + 32);
		unsigned lho = U32(p + 42);
		if (p + 46 + namelen > end) break;
		ZipEntry *e = &z->entries[k];
		e->name = malloc(namelen + 1);
		memcpy(e->name, p + 46, namelen);
		e->name[namelen] = 0;
		e->method = method;
		e->crc = crc;
		e->csize = csize;
		e->usize = usize;
		e->local_offset = lho;
		k++;
		p += 46 + namelen + extralen + commentlen;
	}
	z->nentries = k;
	return z;
}

Zip *zip_open(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (len < 0) { fclose(f); return NULL; }
	unsigned char *buf = malloc((size_t)len);
	size_t got = fread(buf, 1, (size_t)len, f);
	fclose(f);
	if (got != (size_t)len) { free(buf); return NULL; }
	Zip *z = zip_open_mem(buf, (size_t)len);
	free(buf);
	return z;
}

void zip_close(Zip *z)
{
	if (!z) return;
	for (int i = 0; i < z->nentries; i++) free(z->entries[i].name);
	free(z->entries);
	free(z->data);
	free(z);
}

const ZipEntry *zip_find(const Zip *z, const char *name)
{
	for (int i = 0; i < z->nentries; i++)
		if (!strcmp(z->entries[i].name, name)) return &z->entries[i];
	return NULL;
}

unsigned char *zip_read(const Zip *z, const char *name, size_t *outlen)
{
	const ZipEntry *e = zip_find(z, name);
	if (!e) return NULL;
	if (e->local_offset + 30 > z->len) return NULL;
	const unsigned char *lh = z->data + e->local_offset;
	if (!(lh[0] == 0x50 && lh[1] == 0x4b && lh[2] == 0x03 && lh[3] == 0x04)) return NULL;
	unsigned namelen = U16(lh + 26), extralen = U16(lh + 28);
	size_t data_off = e->local_offset + 30 + namelen + extralen;
	if (data_off + e->csize > z->len) return NULL;
	const unsigned char *src = z->data + data_off;

	unsigned char *out = malloc(e->usize ? e->usize : 1);
	if (e->method == 0) {
		memcpy(out, src, e->usize);
	} else if (e->method == 8) {
		z_stream zs = { 0 };
		if (inflateInit2(&zs, -15) != Z_OK) { free(out); return NULL; }
		zs.next_in = (Bytef *)src;
		zs.avail_in = (uInt)e->csize;
		zs.next_out = out;
		zs.avail_out = (uInt)e->usize;
		int r = inflate(&zs, Z_FINISH);
		inflateEnd(&zs);
		if (r != Z_STREAM_END && !(r == Z_OK && zs.avail_out == 0)) { free(out); return NULL; }
	} else {
		free(out);
		return NULL;   /* 不支援的壓縮方式（.docx／.odt 不會用到） */
	}
	if (outlen) *outlen = e->usize;
	return out;
}

/* ------------------------------------------------------------------ */
/* 寫入                                                                  */
/* ------------------------------------------------------------------ */

typedef struct {
	char *name;
	unsigned char *cdata;   /* 壓縮後的內容（store 時等於原內容） */
	size_t clen, ulen;
	unsigned method, crc;
	size_t local_offset;
} WEntry;

struct ZipWriter {
	WEntry *entries;
	int n, cap;
};

ZipWriter *zip_writer_new(void)
{
	ZipWriter *w = calloc(1, sizeof *w);
	return w;
}

void zip_writer_add(ZipWriter *w, const char *name, const unsigned char *data, size_t len, int store)
{
	if (w->n >= w->cap) {
		w->cap = w->cap ? w->cap * 2 : 8;
		w->entries = realloc(w->entries, (size_t)w->cap * sizeof *w->entries);
	}
	WEntry *e = &w->entries[w->n++];
	e->name = strdup(name);
	e->ulen = len;
	e->crc = (unsigned)crc32(0, data, (uInt)len);
	if (store || len == 0) {
		e->method = 0;
		e->clen = len;
		e->cdata = malloc(len ? len : 1);
		memcpy(e->cdata, data, len);
		return;
	}
	e->method = 8;
	uLongf bound = compressBound((uLong)len) + 32;
	unsigned char *tmp = malloc(bound);
	z_stream zs = { 0 };
	deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);
	zs.next_in = (Bytef *)data;
	zs.avail_in = (uInt)len;
	zs.next_out = tmp;
	zs.avail_out = (uInt)bound;
	deflate(&zs, Z_FINISH);
	e->clen = zs.total_out;
	deflateEnd(&zs);
	e->cdata = malloc(e->clen ? e->clen : 1);
	memcpy(e->cdata, tmp, e->clen);
	free(tmp);
	/* 壓縮完反而變大（短文字常見）：乾脆存檔，省得有些讀取器不開心 */
	if (e->clen >= e->ulen) {
		free(e->cdata);
		e->method = 0;
		e->clen = len;
		e->cdata = malloc(len ? len : 1);
		memcpy(e->cdata, data, len);
	}
}

unsigned char *zip_writer_finish(ZipWriter *w, size_t *outlen)
{
	size_t cap = 1024;
	for (int i = 0; i < w->n; i++) cap += 30 + strlen(w->entries[i].name) + w->entries[i].clen + 46 + strlen(w->entries[i].name);
	unsigned char *out = malloc(cap);
	size_t pos = 0;

	for (int i = 0; i < w->n; i++) {
		WEntry *e = &w->entries[i];
		e->local_offset = pos;
		size_t nlen = strlen(e->name);
		unsigned char lh[30] = { 0x50, 0x4b, 0x03, 0x04 };
		put_u16(lh + 4, 20);              /* version needed */
		put_u16(lh + 6, 0);                /* flags */
		put_u16(lh + 8, (unsigned)e->method);
		put_u16(lh + 10, 0); put_u16(lh + 12, 0);   /* mod time/date（不重要） */
		put_u32(lh + 14, e->crc);
		put_u32(lh + 18, (unsigned)e->clen);
		put_u32(lh + 22, (unsigned)e->ulen);
		put_u16(lh + 26, (unsigned)nlen);
		put_u16(lh + 28, 0);
		memcpy(out + pos, lh, 30); pos += 30;
		memcpy(out + pos, e->name, nlen); pos += nlen;
		memcpy(out + pos, e->cdata, e->clen); pos += e->clen;
	}

	size_t cd_start = pos;
	for (int i = 0; i < w->n; i++) {
		WEntry *e = &w->entries[i];
		size_t nlen = strlen(e->name);
		unsigned char ch[46] = { 0x50, 0x4b, 0x01, 0x02 };
		put_u16(ch + 4, 20); put_u16(ch + 6, 20); put_u16(ch + 8, 0);
		put_u16(ch + 10, (unsigned)e->method);
		put_u16(ch + 12, 0); put_u16(ch + 14, 0);
		put_u32(ch + 16, e->crc);
		put_u32(ch + 20, (unsigned)e->clen);
		put_u32(ch + 24, (unsigned)e->ulen);
		put_u16(ch + 28, (unsigned)nlen);
		put_u16(ch + 30, 0); put_u16(ch + 32, 0);
		put_u16(ch + 34, 0); put_u16(ch + 36, 0);
		put_u32(ch + 38, 0);
		put_u32(ch + 42, (unsigned)e->local_offset);
		memcpy(out + pos, ch, 46); pos += 46;
		memcpy(out + pos, e->name, nlen); pos += nlen;
	}
	size_t cd_len = pos - cd_start;

	unsigned char eocd[22] = { 0x50, 0x4b, 0x05, 0x06 };
	put_u16(eocd + 4, 0); put_u16(eocd + 6, 0);
	put_u16(eocd + 8, (unsigned)w->n);
	put_u16(eocd + 10, (unsigned)w->n);
	put_u32(eocd + 12, (unsigned)cd_len);
	put_u32(eocd + 16, (unsigned)cd_start);
	put_u16(eocd + 20, 0);
	memcpy(out + pos, eocd, 22); pos += 22;

	*outlen = pos;
	return out;
}

void zip_writer_free(ZipWriter *w)
{
	if (!w) return;
	for (int i = 0; i < w->n; i++) { free(w->entries[i].name); free(w->entries[i].cdata); }
	free(w->entries);
	free(w);
}
