/* SPDX-License-Identifier: GPL-3.0-or-later */
/* zip.h — 讀寫 .docx／.odt 用的最小 ZIP 容器（只處理存檔與 deflate 兩種壓縮方式，
 * 不支援加密、分割壓縮檔、ZIP64；.docx／.odt 都是這種最簡單的 ZIP，夠用）。
 * 壓縮本身交給系統的 zlib（raw deflate，windowBits=-15）。
 */
#ifndef POMELO_WRITER_ZIP_H
#define POMELO_WRITER_ZIP_H
#include <stddef.h>

typedef struct {
	char *name;                 /* zip 裡的路徑，例如 "word/document.xml" */
	size_t local_offset;        /* local file header 在檔案裡的位移 */
	size_t csize, usize;        /* 壓縮後／解壓後大小 */
	unsigned method;             /* 0 = 存檔、8 = deflate */
	unsigned crc;
} ZipEntry;

typedef struct {
	unsigned char *data;        /* 整個 zip 檔案的內容（讀進記憶體） */
	size_t len;
	ZipEntry *entries;
	int nentries;
} Zip;

/* 打開一個 zip 檔案（讀進記憶體、解析中央目錄）；失敗回傳 NULL */
Zip *zip_open(const char *path);
Zip *zip_open_mem(const unsigned char *data, size_t len);   /* 會複製一份 */
void zip_close(Zip *z);

/* 找某個路徑的項目（區分大小寫，完全比對） */
const ZipEntry *zip_find(const Zip *z, const char *name);
/* 解壓一個項目到記憶體（呼叫者要 free）；找不到或解壓失敗回傳 NULL */
unsigned char *zip_read(const Zip *z, const char *name, size_t *outlen);

/* ---- 寫入 ---- */
typedef struct ZipWriter ZipWriter;
ZipWriter *zip_writer_new(void);
/* store = 1 時不壓縮直接存檔（ODT 的 "mimetype" 依規格一定要存檔，當作第一個項目） */
void zip_writer_add(ZipWriter *w, const char *name, const unsigned char *data, size_t len, int store);
/* 組出完整的 zip 檔案內容（呼叫者要 free）；寫完後 ZipWriter 不能再用 */
unsigned char *zip_writer_finish(ZipWriter *w, size_t *outlen);
void zip_writer_free(ZipWriter *w);

#endif
