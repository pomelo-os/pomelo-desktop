/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ac.h — pomelo-archive 的核心（不含介面）：讀、列、測試、安全解壓、建立壓縮檔
 *
 * 後端：libarchive（行程內）＋外部程式 7-Zip（7z／7zz／7za）、gpg（tar 系列的加密）。
 * 密碼一律不放命令列：7z 從標準輸入讀、gpg 用 --passphrase-fd，用完立刻清掉。
 * 這一層不 #include <pomelo.h>（那邊有 dpy、T、sh 之類的巨集），GUI 與命令列都在上面一層。 */
#ifndef AC_H
#define AC_H

#include <limits.h>
#include <stddef.h>
#include <time.h>

/* 回傳碼 */
enum {
	AC_OK = 0,
	AC_ERR,            /* 一般錯誤（訊息在 err） */
	AC_NEED_PASSWORD,  /* 要密碼 */
	AC_BAD_PASSWORD,   /* 密碼不對 */
	AC_NO_BACKEND,     /* 缺外部程式（訊息說明缺什麼、怎麼裝） */
	AC_CANCEL,         /* 使用者取消 */
	AC_UNSAFE,         /* 內容不安全，整個擋下（只有清單層級就能確定的情況） */
	AC_NOSPACE,        /* 磁碟空間不夠 */
	AC_BOMB,           /* 解壓後大小或壓縮比不正常，要 force 才繼續 */
	AC_CONFLICT        /* 目的地已有同名檔案，要先決定怎麼處理（overwrite = AC_OW_ASK） */
};

enum { AE_FILE, AE_DIR, AE_LINK, AE_OTHER };
enum { AB_LA, AB_7Z, AB_GPG };
enum { AC_OW_ASK, AC_OW_YES, AC_OW_NO, AC_OW_RENAME };

typedef struct {
	char *path;             /* 封存檔裡的路徑（UTF-8，沒有開頭的 ./ 和結尾的 /） */
	long long size, csize;  /* 解開後大小；壓縮後大小（-1 = 不知道） */
	time_t mtime;           /* 0 = 不知道 */
	int type;               /* AE_* */
	int enc;                /* 這個項目有加密 */
	unsigned mode;          /* 權限位元（0 = 不知道） */
	char *link;             /* 符號連結的目標；沒有就 NULL */
} AcEntry;

typedef struct {
	char *file;              /* 開啟的檔案 */
	char fmt[32];            /* 格式名稱（顯示用）：ZIP、7z、TAR、ISO 9660、SquashFS、FAT 映像… */
	char codec[16];          /* 外層壓縮：gzip bzip2 xz zstd lzip lz4 lzma compress，沒有是空字串 */
	int backend;             /* AB_* */
	int enc_data;            /* 有項目加密 */
	int enc_names;           /* 連檔名都加密（7z -mhe、gpg 整包） */
	char charset[16];        /* 檔名編碼（空 = UTF-8 或封存檔本來就有標示） */
	int charset_guessed;     /* 1 = 是自動偵測出來的（可以讓使用者換） */
	AcEntry *e;
	size_t n, cap;
	long long total;         /* 所有項目解開後的總大小 */
	long long file_size;     /* 封存檔本身的大小 */
	char note[256];          /* 給使用者看的補充說明（例如「用 7-Zip 讀」） */
	char *plain;             /* gpg 解密後的內層檔（暫存、0600）；close 時刪掉 */
	char passwd_hint;        /* 內部：打開時用過密碼 */
} AcList;

/* 進度：done/total 是位元組（total 可能是 0 = 不知道），cur 是目前處理的檔名；回傳非 0 = 取消 */
typedef int (*AcProgress)(void *user, long long done, long long total, const char *cur);

typedef struct {
	const char *dest;               /* 目的資料夾（不存在會建立） */
	const char *password;
	const char *const *only;        /* 只解這些路徑（資料夾含底下全部）；NULL = 全部 */
	int nonly;
	int overwrite;                  /* AC_OW_* */
	int into_folder;                /* 1：頂層不只一項時，建一個以封存檔命名的資料夾（--extract-here） */
	int force;                      /* 1：不管炸彈警告 */
	long long max_bytes;            /* 解壓的位元組上限（0 = 依清單算） */
	AcProgress progress; void *pu;
	/* 結果 */
	char outdir[PATH_MAX];          /* 實際解到哪個資料夾 */
	char first[PATH_MAX];           /* 解出來的第一個頂層項目（單一項目時，給「打開」用） */
	char err[512];
	char conflicts[1024];           /* AC_CONFLICT 時：同名的項目（逗號分隔，最多列幾個） */
	int nconflicts;
	int skipped_unsafe;             /* 因為不安全而略過的項目數 */
	int skipped_special;            /* 裝置檔、管線等略過的數量 */
	int nfiles; long long nbytes;
} AcExtract;

typedef struct {
	const char *out;                /* 輸出檔 */
	const char *format;             /* zip zip-zipcrypto 7z tar tar.gz tar.bz2 tar.xz tar.zst tar.lz tar.lz4 cpio iso gz bz2 xz zst lz */
	int level;                      /* 1～9，0 = 預設 */
	const char *password;           /* 非 NULL 且非空 = 加密 */
	int encrypt_names;              /* 7z：連檔名一起加密 */
	const char *const *inputs; int ninputs;
	int follow_links;               /* 0 = 符號連結存成連結 */
	AcProgress progress; void *pu;
	char err[512];
	int nfiles; long long nbytes;
	int nskipped;                   /* 讀不了而略過的檔案數（權限不夠、裝置檔…） */
	char skipped_first[PATH_MAX];   /* 第一個略過的檔案 */
	char outpath[PATH_MAX];         /* 實際輸出的檔案路徑（加密 tar 會多一個 .gpg） */
} AcCreate;

/* 能力 */
typedef struct {
	const char *id;                 /* 格式代號（zip、7z、tar.gz…） */
	const char *label;              /* 顯示名稱 */
	const char *ext;                /* 副檔名（含點） */
	int can_write;                  /* 這台電腦能建立 */
	int can_encrypt;                /* 建立時能加密 */
	int enc_names;                  /* 能連檔名一起加密 */
	const char *why_not;            /* 不能建立／不能加密的原因（給使用者看；NULL = 可以） */
	const char *warn;               /* 提醒（例如 ZipCrypto 較不安全） */
} AcFormat;
const AcFormat *ac_write_formats(int *n);
void ac_print_formats(void *file);                 /* --formats：FILE * */
const char *ac_7z(void);                           /* 7-Zip 的執行檔名（沒有 NULL） */
int ac_have_gpg(void);
int ac_ext_for(const char *format, char *out, size_t n);   /* 格式代號 → 副檔名 */

/* 開啟與列出：回傳 AC_*。password 可以是 NULL。成功時 l 要用 ac_close 釋放 */
int ac_open(AcList *l, const char *file, const char *password, const char *charset, char *err, size_t en);
void ac_close(AcList *l);
int ac_check_password(AcList *l, const char *password);   /* AC_OK／AC_BAD_PASSWORD／AC_NEED_PASSWORD */
int ac_test(AcList *l, const char *password, AcProgress p, void *pu, char *err, size_t en);
int ac_extract(AcList *l, AcExtract *o);
int ac_create(AcCreate *c);
/* 加入到既有的封存檔（zip、7z、tar 系列能改；其他回傳 AC_ERR 並說明） */
int ac_add(const char *archive, const char *password, const char *const *inputs, int n, AcProgress p, void *pu, char *err, size_t en);
int ac_can_modify(const AcList *l, char *why, size_t n);

/* 掃描要壓縮的東西：位元組數與檔案數（cancel 回傳非 0 就停） */
void ac_scan(const char *const *inputs, int n, long long *bytes, long *files, int (*cancel)(void *), void *cu);

/* 工具 */
int ac_is_archive_name(const char *path);          /* 依副檔名／檔名判斷像不像封存檔 */
char *ac_base_noext(const char *file, char *out, size_t n);   /* 檔名去掉副檔名（.tar.gz 整段去掉） */
void ac_unique_name(const char *dir, const char *name, char *out, size_t n);   /* name、name (2)… 找一個不存在的 */
int ac_path_clean(const char *in, char *out, size_t n);       /* 整理封存檔裡的路徑：去 ./、/，擋 ..；回傳 1 = 安全、0 = 不安全 */
int ac_tmpdir(char *out, size_t n);                /* 暫存資料夾（使用者快取裡，0700）；回傳 0 = 成功 */
void ac_wipe(void *p, size_t n);
void ac_init_locale(void);                         /* 先呼叫一次：把 LC_CTYPE 設成 UTF-8（libarchive 轉檔名用） */
void ac_human(char *out, size_t n, double bytes);
const char *ac_charsets(int i);                    /* 手動選單用的編碼清單；NULL = 結束 */

#endif
