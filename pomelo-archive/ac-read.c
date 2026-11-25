/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ac-read.c — 開啟、列出、測試、安全解壓 */
#define _GNU_SOURCE
#include "ac-int.h"
#include <dirent.h>
#include <ftw.h>
#include <iconv.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>

#define LA_BLOCK 65536

/* ---------- 小工具 ---------- */

static long long now_ms(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static const char *src_of(const AcList *l) { return l->plain ? l->plain : l->file; }

static int valid_utf8(const char *s)
{
	const unsigned char *p = (const unsigned char *)s;
	while (*p) {
		if (*p < 0x80) { p++; continue; }
		int n = *p >= 0xf0 ? 3 : *p >= 0xe0 ? 2 : *p >= 0xc2 ? 1 : -1;
		if (n < 0 || *p > 0xf4) return 0;
		for (int i = 1; i <= n; i++) if ((p[i] & 0xc0) != 0x80) return 0;
		p += n + 1;
	}
	return 1;
}

static int mkdir_p(const char *path, mode_t mode)
{
	char tmp[PATH_MAX];
	snprintf(tmp, sizeof tmp, "%s", path);
	for (char *p = tmp + 1; *p; p++)
		if (*p == '/') { *p = 0; if (mkdir(tmp, mode) && errno != EEXIST) return -1; *p = '/'; }
	if (mkdir(tmp, mode) && errno != EEXIST) return -1;
	return 0;
}

static int rm_cb(const char *p, const struct stat *st, int flag, struct FTW *f)
{
	(void)st; (void)flag; (void)f;
	return remove(p);
}

static void rmtree(const char *path) { nftw(path, rm_cb, 32, FTW_DEPTH | FTW_PHYS); }

/* ---------- 偵測 ---------- */

static int sniff(const char *file, unsigned char *buf, size_t n)
{
	int fd = open(file, O_RDONLY | O_CLOEXEC);
	if (fd < 0) return -1;
	ssize_t k = read(fd, buf, n);
	close(fd);
	return (int)k;
}

static int is_gpg_name_or_magic(const char *file, const unsigned char *m, int n)
{
	size_t l = strlen(file);
	int ext = (l > 4 && (!strcasecmp(file + l - 4, ".gpg") || !strcasecmp(file + l - 4, ".pgp")));
	if (n < 2) return 0;
	/* 對稱加密的 OpenPGP：新格式標籤 0xC3（Symmetric-Key Encrypted Session Key）、舊格式 0x8C／0x8D（SKESK／SEIPD） */
	int magic = m[0] == 0xc3 || m[0] == 0x8c || (m[0] == 0xc1 && ext);
	return ext && magic;
}

static int ends_ci(const char *s, const char *suf)
{
	size_t a = strlen(s), b = strlen(suf);
	return a >= b && !strcasecmp(s + a - b, suf);
}

static int is_7z_magic(const unsigned char *m, int n) { return n >= 6 && !memcmp(m, "7z\xbc\xaf\x27\x1c", 6); }
static int is_rar_magic(const unsigned char *m, int n) { return n >= 7 && !memcmp(m, "Rar!\x1a\x07", 6); }

/* ---------- libarchive：開啟與列出 ---------- */

static struct archive *la_new_read(const char *charset, const char *pw, int raw)
{
	struct archive *a = archive_read_new();
	archive_read_support_filter_all(a);
	archive_read_support_format_all(a);
	if (raw) archive_read_support_format_raw(a);
	if (charset && *charset && strcmp(charset, "UTF-8")) {
		char opt[64];
		snprintf(opt, sizeof opt, "hdrcharset=%s", charset);
		archive_read_set_options(a, opt);
	}
	if (pw && *pw) archive_read_add_passphrase(a, pw);
	return a;
}

static const char *fmt_label(const char *n, char *out, size_t on)
{
	if (!strncasecmp(n, "ZIP", 3)) snprintf(out, on, "ZIP");
	else if (!strncmp(n, "7-Zip", 5)) snprintf(out, on, "7z");
	else if (strstr(n, "tar") || strstr(n, "TAR") || strstr(n, "pax") || strstr(n, "ustar")) snprintf(out, on, "TAR");
	else if (!strncmp(n, "ISO9660", 7)) snprintf(out, on, "ISO 9660");
	else if (!strncasecmp(n, "cpio", 4) || strcasestr(n, "cpio")) snprintf(out, on, "cpio");
	else if (!strncasecmp(n, "ar ", 3) || !strcmp(n, "ar")) snprintf(out, on, "ar");
	else if (!strncasecmp(n, "CAB", 3)) snprintf(out, on, "CAB");
	else if (!strncasecmp(n, "LHA", 3)) snprintf(out, on, "LHA");
	else if (!strncasecmp(n, "xar", 3)) snprintf(out, on, "XAR");
	else if (!strncasecmp(n, "raw", 3)) snprintf(out, on, "單一檔案");
	else if (!strncasecmp(n, "RPM", 3)) snprintf(out, on, "RPM");
	else snprintf(out, on, "%s", n);
	return out;
}

static void fill_codec(struct archive *a, char *codec, size_t cn)
{
	codec[0] = 0;
	int n = archive_filter_count(a);
	for (int i = 0; i < n; i++) {
		const char *fn = archive_filter_name(a, i);
		if (!fn || !strcmp(fn, "none")) continue;
		if (!strncmp(fn, "gzip", 4)) snprintf(codec, cn, "gzip");
		else if (!strncmp(fn, "bzip2", 5)) snprintf(codec, cn, "bzip2");
		else if (!strncmp(fn, "xz", 2)) snprintf(codec, cn, "xz");
		else if (!strncmp(fn, "zstd", 4)) snprintf(codec, cn, "zstd");
		else if (!strncmp(fn, "lzip", 4)) snprintf(codec, cn, "lzip");
		else if (!strncmp(fn, "lz4", 3)) snprintf(codec, cn, "lz4");
		else if (!strncmp(fn, "lzma", 4)) snprintf(codec, cn, "lzma");
		else if (!strncmp(fn, "compress", 8)) snprintf(codec, cn, "compress");
		else snprintf(codec, cn, "%.15s", fn);
		return;
	}
}

static int map_type(unsigned ft)
{
	if (ft == AE_IFDIR) return AE_DIR;
	if (ft == AE_IFLNK) return AE_LINK;
	if (ft == AE_IFREG || ft == 0) return AE_FILE;
	return AE_OTHER;
}

/* 一個項目的路徑，轉成 UTF-8；轉不過回傳 NULL */
static const char *entry_name(struct archive_entry *e)
{
	const char *u = archive_entry_pathname_utf8(e);
	if (u && valid_utf8(u)) return u;
	return NULL;
}

/* 用指定編碼列出：成功回傳 0，並把名稱全部轉得過的結果放進 l；失敗回傳 -1（名稱轉不過或讀取錯誤） */
static int la_list_with(AcList *l, const char *src, const char *charset, const char *pw, int raw, char *err, size_t en, int *names_bad)
{
	struct archive *a = la_new_read(charset, pw, raw);
	*names_bad = 0;
	if (archive_read_open_filename(a, src, LA_BLOCK) != ARCHIVE_OK) {
		AC_SETERR(err, en, "%s", archive_error_string(a) ? archive_error_string(a) : "open");
		archive_read_free(a);
		return -2;
	}
	struct archive_entry *e;
	int r;
	size_t keep = l->n;
	long long enc_unknown = 0;
	while ((r = archive_read_next_header(a, &e)) == ARCHIVE_OK || r == ARCHIVE_WARN) {
		const char *nm = entry_name(e);
		char clean[PATH_MAX];
		if (!nm) { (*names_bad)++; archive_read_data_skip(a); continue; }
		if (raw && archive_format(a) == ARCHIVE_FORMAT_RAW) {
			char base[256];
			ac_base_noext(l->file, base, sizeof base);
			snprintf(clean, sizeof clean, "%s", base);
		} else if (!ac_path_clean(nm, clean, sizeof clean)) {
			/* 不安全的路徑：照樣列出（讓使用者看得到），解壓時會被擋掉 */
			snprintf(clean, sizeof clean, "%s", nm);
		}
		AcEntry *x = ac_list_add(l);
		x->path = strdup(clean);
		x->type = map_type(archive_entry_filetype(e));
		x->size = archive_entry_size_is_set(e) ? archive_entry_size(e) : -1;
		x->mtime = archive_entry_mtime_is_set(e) ? archive_entry_mtime(e) : 0;
		x->mode = archive_entry_perm(e);
		x->enc = archive_entry_is_data_encrypted(e) ? 1 : 0;
		if (archive_entry_is_metadata_encrypted(e)) l->enc_names = 1;
		const char *sl = archive_entry_symlink_utf8(e);
		if (sl && x->type == AE_LINK) x->link = strdup(sl);
		if (x->enc) l->enc_data = 1;
		if (x->type == AE_DIR) x->size = 0;
		if (x->size > 0 && x->type == AE_FILE) l->total += x->size;
		archive_read_data_skip(a);
		(void)enc_unknown;
	}
	int ret = 0;
	if (r != ARCHIVE_EOF) {
		AC_SETERR(err, en, "%s", archive_error_string(a) ? archive_error_string(a) : "read");
		ret = -2;
	}
	if (l->n == keep && !*names_bad && ret == 0 && !raw) ret = 0;
	if (!l->fmt[0]) {
		char fn[64];
		fmt_label(archive_format_name(a) ? archive_format_name(a) : "?", fn, sizeof fn);
		snprintf(l->fmt, sizeof l->fmt, "%s", fn);
		fill_codec(a, l->codec, sizeof l->codec);
	}
	if (archive_read_has_encrypted_entries(a) > 0) l->enc_data = 1;
	archive_read_free(a);
	return ret;
}

static void list_reset(AcList *l)
{
	for (size_t i = 0; i < l->n; i++) { free(l->e[i].path); free(l->e[i].link); }
	l->n = 0;
	l->total = 0;
	l->enc_data = l->enc_names = 0;
	l->fmt[0] = 0;
	l->codec[0] = 0;
}

/* 常用字（繁簡都有）：轉對了的檔名會有很多這些字，轉錯的（罕用字）幾乎沒有 */
static const char common_chars[] =
	"的一是不了人我在有他這中大來上國個到說們為子和你地出道也時年得就那要下以生會自著去之過家學對可她裡後小麼心多天而能好都然沒日於起還發成事只作當想看文無開手十用主行方又如前所本見經頭面公同三已老從動兩長知民樣現分將外但身些與高意進把法此實回二理美點月明其種聲全工己話兒者向情部正名際電因次它間第題最女常機係被關"
	"个们这说对时后来会国无为过开关点机动发学问门页见马东车长书气电业务网应该于现头实体从两样种声间题广场报数边认让设许请论总几结读调计证传达选讲资转满队负类须观导话"
	"資料測試照片報告檔案新增備份圖片文檔下載視頻音樂軟體系統設定使用者桌面影像聲音網路程式說明版本更新安裝移除搜尋功能內容項目目錄名稱大小時間日期"
	"备份图片档文下载视频音乐软件系统设置用户桌面影像声音网络程序说明版本更新安装移除搜索功能内容项目名称大小时间日期测试文件报告资料夹";

static int is_common(unsigned cp)
{
	const unsigned char *p = (const unsigned char *)common_chars;
	while (*p) {
		unsigned c; int n;
		if (*p >= 0xf0) { c = *p & 7; n = 3; } else if (*p >= 0xe0) { c = *p & 15; n = 2; } else { c = *p & 31; n = 1; }
		for (int k = 1; k <= n; k++) c = (c << 6) | (p[k] & 63);
		if (c == cp) return 1;
		p += n + 1;
	}
	return 0;
}

/* 編碼評分：轉出來的字越像常見的字越高；假名偏日文、韓文字母偏韓文 */
static int charset_score(AcList *l)
{
	int score = 0;
	for (size_t i = 0; i < l->n; i++) {
		const unsigned char *p = (const unsigned char *)l->e[i].path;
		while (*p) {
			unsigned cp; int n;
			if (*p < 0x80) { p++; continue; }
			if (*p >= 0xf0) { cp = *p & 7; n = 3; } else if (*p >= 0xe0) { cp = *p & 15; n = 2; } else { cp = *p & 31; n = 1; }
			for (int k = 1; k <= n && p[k]; k++) cp = (cp << 6) | (p[k] & 63);
			p += n + 1;
			if (cp >= 0x4e00 && cp <= 0x9fff) score += is_common(cp) ? 4 : 1;
			else if ((cp >= 0x3040 && cp <= 0x30ff) || (cp >= 0xac00 && cp <= 0xd7a3)) score += 3;
			else if ((cp >= 0x3000 && cp <= 0x303f) || (cp >= 0xff00 && cp <= 0xffef)) score += 1;   /* 全形標點 */
			else score -= 4;                                                                          /* 罕用字、符號、拉丁擴充：多半是轉錯了 */
		}
	}
	return score;
}

/* 名稱轉不過的時候，依序試台灣最常遇到的編碼，挑轉得過而且最像字的 */
static int la_detect_charset(AcList *l, const char *src, const char *pw, int raw)
{
	static const char *const cands[] = { "BIG5", "GBK", "SHIFT_JIS", "EUC-KR", "EUC-JP", "CP1252", NULL };
	const char *best = NULL;
	int best_score = -1000000;
	for (int i = 0; cands[i]; i++) {
		list_reset(l);
		int bad = 0;
		char err[200];
		int r = la_list_with(l, src, cands[i], pw, raw, err, sizeof err, &bad);
		if (r || bad || !l->n) continue;
		int s = charset_score(l);
		if (i >= 4) s -= 5;
		if (s > best_score) { best_score = s; best = cands[i]; }
	}
	list_reset(l);
	if (!best) return 0;
	int bad = 0;
	char err[200];
	la_list_with(l, src, best, pw, raw, err, sizeof err, &bad);
	snprintf(l->charset, sizeof l->charset, "%s", best);
	l->charset_guessed = 1;
	return 1;
}

static int la_open(AcList *l, const char *pw, const char *charset, char *err, size_t en, int *unrec)
{
	const char *src = src_of(l);
	int bad = 0;
	char e1[300] = "";
	*unrec = 0;
	int r = la_list_with(l, src, charset, pw, 0, e1, sizeof e1, &bad);
	if (r == -2 && (strstr(e1, "Unrecognized") || strstr(e1, "unrecognized") || strstr(e1, "Damaged tar archive") == NULL)) {
		/* 單一檔案壓縮（foo.txt.gz）：用 raw 格式再試一次，但只接受「確實有壓縮外層」的 */
		list_reset(l);
		struct archive *p = la_new_read(NULL, NULL, 1);
		int ok = archive_read_open_filename(p, src, LA_BLOCK) == ARCHIVE_OK && archive_read_next_header2(p, archive_entry_new()) >= 0;
		int comp = 0;
		if (ok) { for (int i = 0; i < archive_filter_count(p); i++) { const char *fn = archive_filter_name(p, i); if (fn && strcmp(fn, "none")) comp = 1; } }
		archive_read_free(p);
		if (comp) {
			list_reset(l);
			bad = 0;
			char e2[300] = "";
			r = la_list_with(l, src, charset, pw, 1, e2, sizeof e2, &bad);
			if (r == 0) { snprintf(l->fmt, sizeof l->fmt, "單一檔案"); r = 0; goto charset_done; }
		}
		*unrec = strstr(e1, "nrecognized") != NULL;
		list_reset(l);
		AC_SETERR(err, en, "%s", e1);
		return AC_ERR;
	}
	if (r == -2 && l->n == 0) {
		AC_SETERR(err, en, "%s", e1);
		return AC_ERR;
	}
charset_done:
	if (bad && (!charset || !*charset)) {
		if (!la_detect_charset(l, src, pw, 0)) {
			/* 全部轉不過：用 UTF-8 容錯，把壞的位元組換成 ? 列出來 */
			list_reset(l);
			struct archive *a = la_new_read("CP1252", pw, 0);
			archive_read_free(a);
			la_list_with(l, src, "CP1252", pw, 0, e1, sizeof e1, &bad);
			snprintf(l->charset, sizeof l->charset, "CP1252");
			l->charset_guessed = 1;
		}
	} else if (charset && *charset) snprintf(l->charset, sizeof l->charset, "%s", charset);
	return AC_OK;
}

/* ---------- 外部程式：7-Zip 列表 ---------- */

static int has_text(const char *s, const char *a) { return s && strstr(s, a) != NULL; }

static int pw_failed(const char *out)
{
	return has_text(out, "Wrong password") || has_text(out, "Cannot open encrypted archive") || has_text(out, "Incorrect password") ||
	       has_text(out, "wrong password") || has_text(out, "checksum error in encrypted") || has_text(out, "password is incorrect") ||
	       has_text(out, "Corrupt header or wrong password");
}

static int need_pw(const char *out)
{
	return has_text(out, "Enter password") || has_text(out, "Enter password:") || has_text(out, "password is required");
}

/* 逐行切（保留空行：外部程式用空行隔開每個項目，strtok 會把空行吃掉） */
static char *next_line(char **pp)
{
	char *p = *pp;
	if (!p) return NULL;
	char *nl = strchr(p, '\n');
	if (nl) { *nl = 0; *pp = nl + 1; } else *pp = NULL;
	return p;
}

static int sz_parse(const char *s, long long *out)
{
	if (!s || !*s) return 0;
	*out = strtoll(s, NULL, 10);
	return 1;
}

static time_t sz_time(const char *s)
{
	struct tm tm;
	memset(&tm, 0, sizeof tm);
	if (!s || sscanf(s, "%d-%d-%d %d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec) < 3) return 0;
	tm.tm_year -= 1900; tm.tm_mon -= 1; tm.tm_isdst = -1;
	return mktime(&tm);
}

static void guess_fmt_name(const char *t, char *out, size_t n)
{
	if (!strcasecmp(t, "SquashFS")) snprintf(out, n, "SquashFS");
	else if (!strcasecmp(t, "FAT")) snprintf(out, n, "FAT 映像");
	else if (!strcasecmp(t, "Ext")) snprintf(out, n, "ext 映像");
	else if (!strcasecmp(t, "NTFS")) snprintf(out, n, "NTFS 映像");
	else if (!strcasecmp(t, "7z")) snprintf(out, n, "7z");
	else if (!strcasecmp(t, "zip")) snprintf(out, n, "ZIP");
	else if (!strcasecmp(t, "Iso")) snprintf(out, n, "ISO 9660");
	else snprintf(out, n, "%s", t);
}

/* 解析 `7z l -slt` 的輸出 */
static int sz_parse_list(AcList *l, char *out)
{
	char *p = strstr(out, "\n----------");
	char type[40] = "";
	char *hdr = out;
	if (p) {
		*p = 0;
		char *t = strstr(hdr, "\nType = ");
		if (t) { t += 8; snprintf(type, sizeof type, "%.*s", (int)strcspn(t, "\r\n"), t); }
		p++;
		while (*p == '-') p++;
		p += strspn(p, "\r\n");
	} else return -1;
	if (type[0]) guess_fmt_name(type, l->fmt, sizeof l->fmt);
	AcEntry cur;
	int have = 0, is_dir = 0, is_link = 0;
	long long size = -1, csize = -1;
	char path[PATH_MAX] = "", mode[20] = "", mod[40] = "", enc = 0;
	char *line, *cursor = p;
	for (;; ) {
		line = next_line(&cursor);
		int end = !line;
		if (line) line[strcspn(line, "\r")] = 0;
		if (end || !*line) {
			if (have && path[0]) {
				AcEntry *x = ac_list_add(l);
				char clean[PATH_MAX];
				if (!ac_path_clean(path, clean, sizeof clean)) snprintf(clean, sizeof clean, "%s", path);
				x->path = strdup(clean);
				x->type = is_dir ? AE_DIR : is_link ? AE_LINK : AE_FILE;
				x->size = is_dir ? 0 : size;
				x->csize = csize;
				x->mtime = sz_time(mod);
				x->enc = enc;
				if (enc) l->enc_data = 1;
				if (x->type == AE_FILE && size > 0) l->total += size;
			}
			(void)cur;
			have = is_dir = is_link = enc = 0;
			size = csize = -1;
			path[0] = mode[0] = mod[0] = 0;
			if (end) break;
			continue;
		}
		char *eq = strstr(line, " = ");
		if (!eq) continue;
		*eq = 0;
		const char *k = line, *v = eq + 3;
		if (!strcmp(k, "Path")) { snprintf(path, sizeof path, "%s", v); have = 1; }
		else if (!strcmp(k, "Folder")) is_dir = v[0] == '+';
		else if (!strcmp(k, "Size")) sz_parse(v, &size);
		else if (!strcmp(k, "Packed Size")) sz_parse(v, &csize);
		else if (!strcmp(k, "Modified")) snprintf(mod, sizeof mod, "%s", v);
		else if (!strcmp(k, "Mode")) { snprintf(mode, sizeof mode, "%s", v); if (v[0] == 'l') is_link = 1; if (v[0] == 'd') is_dir = 1; }
		else if (!strcmp(k, "Attributes")) { if (v[0] == 'D') is_dir = 1; const char *sp = strchr(v, ' '); if (sp && sp[1] == 'l') is_link = 1; if (sp && sp[1] == 'd') is_dir = 1; }
		else if (!strcmp(k, "Symbolic Link")) { if (v[0]) is_link = 1; }   /* ext 映像每個項目都有這個欄位，沒有連結時是空的 */
		else if (!strcmp(k, "Encrypted")) enc = v[0] == '+';
	}
	return 0;
}

static int sz_open(AcList *l, const char *pw, char *err, size_t en)
{
	const char *z = ac_7z();
	if (!z) { AC_SETERR(err, en, "需要 7-Zip 才能讀這種格式（請安裝 7zip 或 p7zip）"); return AC_NO_BACKEND; }
	char *argv[] = { (char *)z, "l", "-slt", "-sccUTF-8", "-spd", (char *)src_of(l), NULL };
	size_t pl = 0;
	char *in = pw && *pw ? ac_pw_lines(pw, &pl) : NULL;
	char *out = NULL;
	int rc = ac_run(argv, in, pl, NULL, NULL, NULL, &out);
	ac_pw_free(in, pl);
	if (!out) { AC_SETERR(err, en, "沒辦法執行 7-Zip"); return AC_ERR; }
	int ret = AC_OK;
	if (pw_failed(out)) ret = pw && *pw ? AC_BAD_PASSWORD : AC_NEED_PASSWORD;
	else if (need_pw(out) && !(pw && *pw)) ret = AC_NEED_PASSWORD;
	else if (rc != 0 && !strstr(out, "\n----------")) {
		const char *m = strstr(out, "ERROR:");
		AC_SETERR(err, en, "%.*s", m ? (int)strcspn(m, "\r\n") : 200, m ? m : "7-Zip 沒辦法讀這個檔案");
		ret = AC_ERR;
	} else if (sz_parse_list(l, out)) { AC_SETERR(err, en, "無法解析 7-Zip 的輸出"); ret = AC_ERR; }
	if (ret == AC_OK && !l->n) {
		const char *m = strstr(out, "ERROR");
		if (m) { AC_SETERR(err, en, "%.*s", (int)strcspn(m, "\r\n"), m); ret = AC_ERR; }
	}
	if (ret == AC_OK) {
		if (pw && *pw && l->n && !strstr(out, "Encrypted = +")) { /* 標頭加密的不會列 Encrypted 欄位 */ }
		if (strstr(out, "Headers Size") == NULL && pw && *pw && !l->enc_data) l->enc_names = l->enc_names;
	}
	free(out);
	return ret;
}

/* ---------- gpg：把 tar.gz.gpg 這類解成暫存檔 ---------- */

static int gpg_decrypt(AcList *l, const char *pw, char *err, size_t en)
{
	if (!pw || !*pw) return AC_NEED_PASSWORD;
	if (!ac_have_gpg()) { AC_SETERR(err, en, "需要 gpg 才能打開 .gpg 加密檔（請安裝 gnupg 或 gpg）"); return AC_NO_BACKEND; }
	char tmpl[PATH_MAX], td[PATH_MAX];
	if (ac_tmpdir(td, sizeof td)) { AC_SETERR(err, en, "沒辦法建立暫存資料夾"); return AC_ERR; }
	snprintf(tmpl, sizeof tmpl, "%s/pa-gpg-XXXXXX", td);
	int fd = mkstemp(tmpl);
	if (fd < 0) { AC_SETERR(err, en, "沒辦法建立暫存檔"); return AC_ERR; }
	close(fd);
	chmod(tmpl, 0600);
	char *argv[] = { (char *)ac_gpg(), "--batch", "--yes", "--quiet", "--no-tty", "--pinentry-mode", "loopback", "--passphrase-fd", "0", "-o", tmpl, "-d", l->file, NULL };
	size_t pl = 0;
	char *in = ac_pw_lines(pw, &pl);
	char *out = NULL;
	int rc = ac_run(argv, in, pl, NULL, NULL, NULL, &out);
	ac_pw_free(in, pl);
	struct stat st;
	if (rc != 0 || stat(tmpl, &st) || st.st_size == 0) {
		unlink(tmpl);
		int bad = out && (strstr(out, "bad session key") || strstr(out, "decryption failed") || strstr(out, "Bad session") || strstr(out, "no valid OpenPGP"));
		if (out && !bad && !strstr(out, "gpg:")) bad = 1;
		free(out);
		if (bad) return AC_BAD_PASSWORD;
		AC_SETERR(err, en, "gpg 解密失敗");
		return AC_ERR;
	}
	free(out);
	l->plain = strdup(tmpl);
	return AC_OK;
}

/* ---------- ac_open ---------- */

int ac_open(AcList *l, const char *file, const char *pw, const char *charset, char *err, size_t en)
{
	struct stat st;
	memset(l, 0, sizeof *l);
	if (stat(file, &st) || !S_ISREG(st.st_mode)) { AC_SETERR(err, en, "找不到這個檔案：%s", file); return AC_ERR; }
	l->file = strdup(file);
	l->file_size = st.st_size;
	unsigned char magic[16];
	int mn = sniff(file, magic, sizeof magic);
	if (mn < 0) { AC_SETERR(err, en, "沒辦法讀取：%s", strerror(errno)); ac_close(l); return AC_ERR; }
	int rc;
	if (is_rar_magic(magic, mn) || ends_ci(file, ".rar") || ends_ci(file, ".r00")) {   /* RAR 一律不支援：授權（unRAR 限制條款）。不呼叫任何 rar／unrar，也不交給 libarchive／7-Zip */
		 AC_SETERR(err, en, "不支援的格式（RAR）"); ac_close(l); return AC_ERR; }
	if (is_gpg_name_or_magic(file, magic, mn)) {
		rc = gpg_decrypt(l, pw, err, en);
		if (rc) { ac_close(l); return rc; }
		l->backend = AB_GPG;
		l->enc_names = 1;
		l->enc_data = 1;
		int unrec;
		rc = la_open(l, NULL, charset, err, en, &unrec);
		if (rc) { ac_close(l); return rc; }
		snprintf(l->note, sizeof l->note, "OpenPGP 加密（AES-256 對稱加密）");
		return AC_OK;
	}
	if (is_7z_magic(magic, mn) && ac_7z()) {
		l->backend = AB_7Z;
		rc = sz_open(l, pw, err, en);
		if (rc == AC_OK) {
			snprintf(l->fmt, sizeof l->fmt, "7z");
			if (pw && *pw) { l->enc_data = 1; }
			snprintf(l->note, sizeof l->note, "用 7-Zip 讀取");
			return AC_OK;
		}
		if (rc == AC_NEED_PASSWORD || rc == AC_BAD_PASSWORD) { ac_close(l); return rc; }
		/* 7-Zip 讀不了就退回 libarchive */
		list_reset(l);
	}
	int unrec = 0;
	l->backend = AB_LA;
	rc = la_open(l, pw, charset, err, en, &unrec);
	if (rc == AC_OK && l->n == 0 && !strcmp(l->fmt, "TAR") && ac_7z()) {
		/* 空的 tar：多半是磁碟映像（開頭一堆零被當成 tar 的結尾），交給 7-Zip 試試 */
		AcList l2;
		memset(&l2, 0, sizeof l2);
		l2.file = strdup(l->file);
		l2.file_size = l->file_size;
		l2.backend = AB_7Z;
		char e2[200];
		if (sz_open(&l2, pw, e2, sizeof e2) == AC_OK && l2.n) {
			char *f = l->file; l->file = NULL;
			ac_close(l);
			*l = l2;
			l->file = f;
			snprintf(l->note, sizeof l->note, "用 7-Zip 讀取");
			return AC_OK;
		}
		ac_close(&l2);
	}
	if (rc == AC_OK && l->n == 0 && !strcmp(l->fmt, "TAR")) { AC_SETERR(err, en, "認不得這個格式，或是空的封存檔（磁碟映像與 SquashFS 需要 7-Zip：請安裝 7zip 或 p7zip）"); ac_close(l); return AC_ERR; }
	if (rc == AC_OK) {
		if (!strncmp(l->fmt, "7z", 2) && !ac_7z() && l->enc_data) snprintf(l->note, sizeof l->note, "加密的 7z 需要 7-Zip 才能解壓");
		return AC_OK;
	}
	/* libarchive 不認得 */
	list_reset(l);
	if (ac_7z()) {
		l->backend = AB_7Z;
		rc = sz_open(l, pw, err, en);
		if (rc == AC_OK) { snprintf(l->note, sizeof l->note, "用 7-Zip 讀取"); return AC_OK; }
		ac_close(l);
		return rc;
	}
	if (unrec) AC_SETERR(err, en, "認不得這個格式（SquashFS、磁碟映像等需要 7-Zip：請安裝 7zip 或 p7zip）");
	ac_close(l);
	return unrec ? AC_NO_BACKEND : AC_ERR;
}

/* ---------- 密碼檢查與測試 ---------- */

static int la_read_all(struct archive *a, struct archive_entry *e, long long *done, long long limit, AcProgress p, void *pu, const char *cur, long long total, int *cancel)
{
	const void *buf;
	size_t len;
	la_int64_t off;
	long long last = now_ms();
	int r;
	(void)e;
	while ((r = archive_read_data_block(a, &buf, &len, &off)) == ARCHIVE_OK) {
		*done += (long long)len;
		if (limit && *done > limit) return -3;
		if (p && now_ms() - last > 80) {
			last = now_ms();
			if (p(pu, *done, total, cur)) { *cancel = 1; return -4; }
		}
	}
	return r == ARCHIVE_EOF ? 0 : r;
}

int ac_test(AcList *l, const char *pw, AcProgress p, void *pu, char *err, size_t en)
{
	if (l->backend == AB_7Z) {
		const char *tool = ac_7z();
				if (!tool) { AC_SETERR(err, en, "需要 7-Zip（請安裝 7zip 或 p7zip）"); return AC_NO_BACKEND; }
		char *argv7[] = { (char *)tool, "t", "-spd", "-sccUTF-8", (char *)src_of(l), NULL };
		size_t pl = 0;
		char *in = pw && *pw ? ac_pw_lines(pw, &pl) : NULL;
		char *out = NULL;
		int rc = ac_run(argv7, in, pl, NULL, NULL, NULL, &out);
		ac_pw_free(in, pl);
		int ret = AC_OK;
		if (out && pw_failed(out)) ret = AC_BAD_PASSWORD;
		else if (out && need_pw(out) && !(pw && *pw)) ret = AC_NEED_PASSWORD;
		else if (rc != 0) {
			const char *m = out ? strstr(out, "ERROR") : NULL;
			if (!m && out) m = strstr(out, "rror");
			AC_SETERR(err, en, "%.*s", m ? (int)strcspn(m, "\r\n") : 40, m ? m : "測試失敗");
			ret = AC_ERR;
		}
		free(out);
		return ret;
	}
	struct archive *a = la_new_read(l->charset, pw, strcmp(l->fmt, "單一檔案") == 0);
	if (archive_read_open_filename(a, src_of(l), LA_BLOCK) != ARCHIVE_OK) {
		AC_SETERR(err, en, "%s", archive_error_string(a));
		archive_read_free(a);
		return AC_ERR;
	}
	struct archive_entry *e;
	long long done = 0;
	int r, ret = AC_OK, cancel = 0;
	while ((r = archive_read_next_header(a, &e)) == ARCHIVE_OK || r == ARCHIVE_WARN) {
		if (archive_entry_filetype(e) != AE_IFREG) { archive_read_data_skip(a); continue; }   /* 資料夾、連結沒有資料可讀 */
		int rr = la_read_all(a, e, &done, 0, p, pu, archive_entry_pathname(e), l->total, &cancel);
		if (cancel) { ret = AC_CANCEL; break; }
		if (rr) {
			const char *m = archive_error_string(a);
			if (m && (strstr(m, "assphrase") || strstr(m, "assword"))) ret = (pw && *pw) ? AC_BAD_PASSWORD : AC_NEED_PASSWORD;
			else { ret = AC_ERR; AC_SETERR(err, en, "%s：%s", archive_entry_pathname(e), m ? m : "讀取失敗"); }
			break;
		}
	}
	if (ret == AC_OK && r != ARCHIVE_EOF) { ret = AC_ERR; AC_SETERR(err, en, "%s", archive_error_string(a)); }
	archive_read_free(a);
	return ret;
}

/* ---------- 安全：清單層級檢查 ---------- */

static int under(const char *path, const char *prefix)
{
	size_t n = strlen(prefix);
	return !strncmp(path, prefix, n) && (path[n] == 0 || path[n] == '/');
}

static int selected(const AcExtract *o, const char *path)
{
	if (!o->nonly) return 1;
	for (int i = 0; i < o->nonly; i++) if (under(path, o->only[i])) return 1;
	return 0;
}

/* 連結的目標在解壓目錄裡面嗎（用文字運算，不碰檔案系統）：relpath 是連結本身的位置。
 * 只收「開頭幾個 ..（不超過連結所在的深度）＋一般路徑」：.. 夾在中間（a/../b）的不收——那種路徑如果 a 本身是符號連結，
 * 實際走到哪裡文字運算算不出來，可以用來繞出去。 */
static int link_target_inside(const char *relpath, const char *target)
{
	if (!target || !*target || target[0] == '/') return 0;
	char dir[PATH_MAX];
	snprintf(dir, sizeof dir, "%s", relpath);
	char *sl = strrchr(dir, '/');
	if (sl) *sl = 0; else dir[0] = 0;
	int depth = 0;
	for (const char *p = dir; *p;) {
		while (*p == '/') p++;
		if (!*p) break;
		depth++;
		while (*p && *p != '/') p++;
	}
	int normal = 0;
	for (const char *p = target; *p;) {
		while (*p == '/') p++;
		if (!*p) break;
		const char *e = p;
		while (*e && *e != '/') e++;
		size_t l = (size_t)(e - p);
		if (l == 2 && p[0] == '.' && p[1] == '.') { if (normal || --depth < 0) return 0; }
		else if (!(l == 1 && p[0] == '.')) normal = 1;
		p = e;
	}
	return 1;
}

/* 外部程式解出來的樹：把不安全的東西清掉（符號連結指到外面、特殊檔、s 位元） */
struct walk { const char *root; int unsafe, special; };
static struct walk *walk_ctx;

static int walk_cb(const char *path, const struct stat *st, int flag, struct FTW *f)
{
	(void)f;
	struct walk *w = walk_ctx;
	if (flag == FTW_SL || flag == FTW_SLN || S_ISLNK(st->st_mode)) {
		char tgt[PATH_MAX];
		ssize_t k = readlink(path, tgt, sizeof tgt - 1);
		if (k < 0) return 0;
		tgt[k] = 0;
		const char *rel = path + strlen(w->root);
		while (*rel == '/') rel++;
		if (!link_target_inside(rel, tgt)) { unlink(path); w->unsafe++; }
	} else if (S_ISDIR(st->st_mode)) {
		if ((st->st_mode & 0700) != 0700) chmod(path, (st->st_mode | 0700) & 0777);
		else if (st->st_mode & 07000) chmod(path, st->st_mode & 0777);
	} else if (S_ISREG(st->st_mode)) {
		if (st->st_mode & 07000) chmod(path, st->st_mode & 0777);
	} else { unlink(path); w->special++; }
	return 0;
}

int ac_check_dest_safe_tree(const char *root, int *removed)
{
	struct walk w = { root, 0, 0 };
	walk_ctx = &w;
	nftw(root, walk_cb, 32, FTW_PHYS);
	if (removed) *removed = w.unsafe + w.special;
	return w.unsafe + w.special;
}

/* 清單層級：有沒有項目要穿過同一個封存檔裡的符號連結（zip-slip 的連結版）；有就整包擋下 */
static int list_through_link(const AcList *l, const AcExtract *o, char *why, size_t wn)
{
	for (size_t i = 0; i < l->n; i++) {
		if (l->e[i].type != AE_LINK) continue;
		for (size_t j = 0; j < l->n; j++) {
			if (j == i) continue;
			const char *pj = l->e[j].path, *pi = l->e[i].path;
			size_t n = strlen(pi);
			if (!strncmp(pj, pi, n) && pj[n] == '/' && (selected(o, pj) || selected(o, pi))) {
				snprintf(why, wn, "「%s」要寫進符號連結「%s」裡面，這種封存檔很可能是惡意的，已經擋下", pj, pi);
				return 1;
			}
		}
	}
	return 0;
}

/* ---------- 解壓：共用的前後處理 ---------- */

static long long free_space(const char *dir)
{
	char p[PATH_MAX];
	struct statvfs sv;
	snprintf(p, sizeof p, "%s", dir);
	for (;;) {
		if (!statvfs(p, &sv)) return (long long)sv.f_bavail * (long long)sv.f_frsize;
		char *sl = strrchr(p, '/');
		if (!sl || sl == p) { snprintf(p, sizeof p, "/"); if (statvfs(p, &sv)) return -1; return (long long)sv.f_bavail * (long long)sv.f_frsize; }
		*sl = 0;
	}
}

static void top_name(const char *path, char *out, size_t n)
{
	size_t k = strcspn(path, "/");
	snprintf(out, n, "%.*s", (int)k, path);
}

/* 合併搬運：把 src 搬到 dst，已存在的依 policy 處理 */
static int merge_move(const char *src, const char *dst, int policy, int *skipped, char *err, size_t en)
{
	struct stat ss, ds;
	if (lstat(dst, &ds)) {
		if (rename(src, dst)) { AC_SETERR(err, en, "沒辦法搬到 %s：%s", dst, strerror(errno)); return -1; }
		return 0;
	}
	if (policy == AC_OW_NO) { (*skipped)++; return 0; }
	if (policy == AC_OW_RENAME) {
		char dir[PATH_MAX], nm[PATH_MAX], uniq[PATH_MAX], full[PATH_MAX];
		snprintf(dir, sizeof dir, "%s", dst);
		char *sl = strrchr(dir, '/');
		snprintf(nm, sizeof nm, "%s", sl ? sl + 1 : dir);
		if (sl) *sl = 0;
		ac_unique_name(dir, nm, uniq, sizeof uniq);
		snprintf(full, sizeof full, "%s/%s", dir, uniq);
		if (rename(src, full)) { AC_SETERR(err, en, "沒辦法搬到 %s：%s", full, strerror(errno)); return -1; }
		return 0;
	}
	/* AC_OW_YES */
	if (lstat(src, &ss)) return -1;
	if (S_ISDIR(ss.st_mode) && S_ISDIR(ds.st_mode)) {
		DIR *d = opendir(src);
		if (!d) return -1;
		struct dirent *de;
		int rc = 0;
		while ((de = readdir(d))) {
			if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
			char s2[PATH_MAX], d2[PATH_MAX];
			snprintf(s2, sizeof s2, "%s/%s", src, de->d_name);
			snprintf(d2, sizeof d2, "%s/%s", dst, de->d_name);
			if (merge_move(s2, d2, policy, skipped, err, en)) { rc = -1; break; }
		}
		closedir(d);
		return rc;
	}
	if (S_ISDIR(ds.st_mode)) rmtree(dst); else unlink(dst);
	if (rename(src, dst)) { AC_SETERR(err, en, "沒辦法取代 %s：%s", dst, strerror(errno)); return -1; }
	return 0;
}

/* 一個外部程式的進度行：7z 印 " 45% 12 - 路徑"，只解析百分比 */
struct extprog { AcProgress p; void *pu; long long total; AcExtract *o; char last[16]; };

static int ext_prog_cb(void *u, const char *d, size_t n)
{
	struct extprog *e = u;
	if (!e->p) return 0;
	for (size_t i = 0; i < n; i++) {
		if (d[i] == '%' && i >= 1 && isdigit((unsigned char)d[i - 1])) {
			int k = (int)i - 1;
			while (k > 0 && isdigit((unsigned char)d[k - 1])) k--;
			long pct = strtol(d + k, NULL, 10);
			if (e->p(e->pu, (long long)(pct * (e->total > 0 ? e->total : 100) / 100), e->total > 0 ? e->total : 100, "")) return 1;
		}
	}
	return 0;
}

/* ---------- libarchive 解壓到暫存資料夾 ---------- */

static int la_extract_to(AcList *l, AcExtract *o, const char *stage, long long cap, int *cancel_out)
{
	int is_raw = !strcmp(l->fmt, "單一檔案");
	struct archive *a = la_new_read(l->charset, o->password, is_raw);
	if (archive_read_open_filename(a, src_of(l), LA_BLOCK) != ARCHIVE_OK) {
		AC_SETERR(o->err, sizeof o->err, "%s", archive_error_string(a));
		archive_read_free(a);
		return AC_ERR;
	}
	struct archive *w = archive_write_disk_new();
	int flags = ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_SECURE_SYMLINKS | ARCHIVE_EXTRACT_SECURE_NODOTDOT | ARCHIVE_EXTRACT_SPARSE;   /* 路徑是我們自己組的絕對路徑（暫存資料夾已經 realpath，裡面沒有符號連結），所以不加 NOABSOLUTEPATHS */
	archive_write_disk_set_options(w, flags);
	struct archive_entry *e;
	int r, ret = AC_OK;
	long long done = 0, last = now_ms();
	while ((r = archive_read_next_header(a, &e)) == ARCHIVE_OK || r == ARCHIVE_WARN) {
		const char *nm = entry_name(e);
		char clean[PATH_MAX], full[PATH_MAX];
		if (!nm) nm = archive_entry_pathname(e);
		if (is_raw) { char b[256]; ac_base_noext(l->file, b, sizeof b); snprintf(clean, sizeof clean, "%s", b); }
		else if (!nm || !ac_path_clean(nm, clean, sizeof clean)) { o->skipped_unsafe++; archive_read_data_skip(a); continue; }
		if (!selected(o, clean)) { archive_read_data_skip(a); continue; }
		unsigned ft = archive_entry_filetype(e);
		if (ft != AE_IFREG && ft != AE_IFDIR && ft != AE_IFLNK && ft != 0) { o->skipped_special++; archive_read_data_skip(a); continue; }
		const char *hl = archive_entry_hardlink_utf8(e);
		if (hl) {
			char hc[PATH_MAX];
			if (hl[0] == '/' || !ac_path_clean(hl, hc, sizeof hc)) { o->skipped_unsafe++; archive_read_data_skip(a); continue; }
			snprintf(full, sizeof full, "%s/%s", stage, hc);
			archive_entry_set_hardlink(e, full);
		}
		if (ft == AE_IFLNK) {
			const char *tg = archive_entry_symlink_utf8(e);
			char tbuf[PATH_MAX];
			if (tg && archive_entry_is_data_encrypted(e) && !valid_utf8(tg)) tg = NULL;
			if (!tg) {   /* 加密的 zip：連結的目標存在資料裡，要先讀出來 */
				ssize_t k = archive_read_data(a, tbuf, sizeof tbuf - 1);
				if (k > 0 && k < (ssize_t)sizeof tbuf - 1) { tbuf[k] = 0; if (!strchr(tbuf, '\n') && valid_utf8(tbuf)) { archive_entry_set_symlink(e, tbuf); tg = tbuf; } }
			}
			if (!tg && archive_entry_is_data_encrypted(e)) { o->skipped_special++; archive_read_data_skip(a); continue; }   /* libarchive 讀不出加密 zip 裡連結的目標 */
			if (!tg || !link_target_inside(clean, tg)) { o->skipped_unsafe++; archive_read_data_skip(a); continue; }
		}
		snprintf(full, sizeof full, "%s/%s", stage, clean);
		archive_entry_set_pathname(e, full);
		mode_t perm = archive_entry_perm(e) & 0777;
		if (ft == AE_IFDIR) perm |= 0700;   /* 資料夾自己要能寫進去；檔案維持封存檔裡的權限（去掉 s、t 位元） */
		archive_entry_set_perm(e, perm);
		archive_entry_set_uid(e, getuid());
		archive_entry_set_gid(e, getgid());
		archive_entry_set_uname(e, NULL);
		archive_entry_set_gname(e, NULL);
		if (archive_write_header(w, e) < ARCHIVE_WARN) {
			const char *m = archive_error_string(w);
			if ((m && strstr(m, "symlink")) || hl) { o->skipped_unsafe++; archive_read_data_skip(a); continue; }   /* 連到外面的符號連結、目標不存在的硬連結：略過 */
			AC_SETERR(o->err, sizeof o->err, "%s：%s", clean, m ? m : "寫入失敗");
			ret = AC_ERR;
			break;
		}
		if (ft == AE_IFREG || ft == 0) {
			const void *buf;
			size_t len;
			la_int64_t off;
			while ((r = archive_read_data_block(a, &buf, &len, &off)) == ARCHIVE_OK) {
				done += (long long)len;
				if (cap && done > cap) { ret = AC_BOMB; AC_SETERR(o->err, sizeof o->err, "解開的資料比標示的大小多太多，這個檔案可能是壓縮炸彈，已停止"); break; }
				if (archive_write_data_block(w, buf, len, off) < ARCHIVE_WARN) {
					const char *m = archive_error_string(w);
					int nospc = m && (strstr(m, "o space") || strstr(m, "ENOSPC"));
					ret = nospc ? AC_NOSPACE : AC_ERR;
					AC_SETERR(o->err, sizeof o->err, "%s：%s", clean, m ? m : "寫入失敗");
					break;
				}
				if (o->progress && now_ms() - last > 80) {
					last = now_ms();
					if (o->progress(o->pu, done, l->total, clean)) { ret = AC_CANCEL; *cancel_out = 1; break; }
				}
			}
			if (ret == AC_OK && r != ARCHIVE_EOF) {
				const char *m = archive_error_string(a);
				if (m && (strstr(m, "assphrase") || strstr(m, "assword"))) ret = (o->password && *o->password) ? AC_BAD_PASSWORD : AC_NEED_PASSWORD;
				else { ret = AC_ERR; AC_SETERR(o->err, sizeof o->err, "%s：%s", clean, m ? m : "讀取失敗"); }
			}
			if (ret != AC_OK) break;
			o->nfiles++;
		}
		archive_write_finish_entry(w);
	}
	if (ret == AC_OK && r != ARCHIVE_EOF) {
		const char *m = archive_error_string(a);
		if (m && (strstr(m, "assphrase") || strstr(m, "assword"))) ret = (o->password && *o->password) ? AC_BAD_PASSWORD : AC_NEED_PASSWORD;
		else { ret = AC_ERR; AC_SETERR(o->err, sizeof o->err, "%s", m ? m : "讀取失敗"); }
	}
	o->nbytes = done;
	archive_write_free(w);
	archive_read_free(a);
	return ret;
}

/* ---------- 外部程式解壓到暫存資料夾 ---------- */

static int ext_extract_to(AcList *l, AcExtract *o, const char *stage)
{
	const char *tool = ac_7z();
	if (!tool) {
		AC_SETERR(o->err, sizeof o->err, "%s", "需要 7-Zip 才能讀這種格式（請安裝 7zip 或 p7zip）");
		return AC_NO_BACKEND;
	}
	char outopt[PATH_MAX + 8];
	char *argv[300];
	int k = 0;
	argv[k++] = (char *)tool;
	snprintf(outopt, sizeof outopt, "-o%s", stage);
	argv[k++] = "x"; argv[k++] = "-y"; argv[k++] = "-bsp1"; argv[k++] = "-spd"; argv[k++] = "-sccUTF-8"; argv[k++] = outopt;
	argv[k++] = (char *)src_of(l);
	for (int i = 0; i < o->nonly && k < 290; i++) argv[k++] = (char *)o->only[i];
	argv[k] = NULL;
	size_t pl = 0;
	char *in = o->password && *o->password ? ac_pw_lines(o->password, &pl) : NULL;
	char *out = NULL;
	struct extprog ep = { o->progress, o->pu, l->total, o, "" };
	int rc = ac_run(argv, in, pl, NULL, ext_prog_cb, &ep, &out);
	ac_pw_free(in, pl);
	int ret = AC_OK;
	if (rc == -2) ret = AC_CANCEL;
	else if (out && pw_failed(out)) ret = o->password && *o->password ? AC_BAD_PASSWORD : AC_NEED_PASSWORD;
	else if (out && need_pw(out) && !(o->password && *o->password)) ret = AC_NEED_PASSWORD;
	else if (rc != 0) {
		/* 舊版 7-Zip（22.04 的 21.07）讀 ext 映像裡「指向資料夾的符號連結」會報 Cannot create symbolic link：
		 * 如果所有的錯誤都只是這一種，其他檔案都解好了，就當作略過這幾個連結，不要整個失敗 */
		int nerr = 0, nlink = 0;
		if (out) for (const char *e = out; (e = strstr(e, "ERROR:")); e += 6) { nerr++; if (strncmp(e + 6, " Cannot create symbolic link", 28) == 0) nlink++; }
		if (nerr > 0 && nerr == nlink) o->skipped_special += nlink;
		else {
			const char *m = out ? strstr(out, "ERROR") : NULL;
			if (!m && out) m = strstr(out, "rror");
			AC_SETERR(o->err, sizeof o->err, "%.*s", m ? (int)strcspn(m, "\r\n") : 40, m ? m : "解壓失敗");
			ret = (out && (strstr(out, "No space") || strstr(out, "no space"))) ? AC_NOSPACE : AC_ERR;
		}
	}
	free(out);
	if (ret == AC_OK) {
		int removed = 0;
		ac_check_dest_safe_tree(stage, &removed);
		o->skipped_unsafe += removed;
	}
	return ret;
}

/* ---------- ac_extract ---------- */

static void join_conflict(AcExtract *o, const char *name)
{
	o->nconflicts++;
	size_t cur = strlen(o->conflicts);
	if (cur + strlen(name) + 3 < sizeof o->conflicts && o->nconflicts <= 8) snprintf(o->conflicts + cur, sizeof o->conflicts - cur, "%s%s", cur ? "、" : "", name);
}

int ac_extract(AcList *l, AcExtract *o)
{
	o->err[0] = o->conflicts[0] = o->outdir[0] = o->first[0] = 0;
	o->nconflicts = o->skipped_unsafe = o->skipped_special = o->nfiles = 0;
	o->nbytes = 0;
	const char *dest = o->dest && *o->dest ? o->dest : ".";
	char destabs[PATH_MAX];
	if (!realpath(dest, destabs)) {
		if (mkdir_p(dest, 0755) || !realpath(dest, destabs)) { AC_SETERR(o->err, sizeof o->err, "沒辦法建立資料夾：%s", dest); return AC_ERR; }
	}
	char why[400];
	if (list_through_link(l, o, why, sizeof why)) { AC_SETERR(o->err, sizeof o->err, "%s", why); return AC_UNSAFE; }
	/* 要解出哪些項目、頂層有哪幾個 */
	long long total = 0;
	size_t nsel = 0;
	char tops[512][256];
	int ntop = 0;
	for (size_t i = 0; i < l->n; i++) {
		const AcEntry *e = &l->e[i];
		char clean[PATH_MAX];
		if (!ac_path_clean(e->path, clean, sizeof clean)) continue;
		if (!selected(o, clean)) continue;
		nsel++;
		if (e->type == AE_FILE && e->size > 0) total += e->size;
		char t[256];
		top_name(clean, t, sizeof t);
		int dup = 0;
		for (int j = 0; j < ntop; j++) if (!strcmp(tops[j], t)) { dup = 1; break; }
		if (!dup && ntop < 512) snprintf(tops[ntop++], 256, "%s", t);
	}
	if (!nsel && l->n) { AC_SETERR(o->err, sizeof o->err, "沒有選到任何項目"); return AC_ERR; }
	if (!l->n) { AC_SETERR(o->err, sizeof o->err, "這個封存檔是空的"); return AC_ERR; }
	/* 炸彈與空間 */
	long long avail = free_space(destabs);
	if (total > 0 && avail >= 0 && total + (64LL << 20) > avail) {
		char a[32], b[32];
		ac_human(a, sizeof a, (double)total);
		ac_human(b, sizeof b, (double)avail);
		AC_SETERR(o->err, sizeof o->err, "磁碟空間不夠：解開要 %s，這裡只剩 %s", a, b);
		return AC_NOSPACE;
	}
	if (!o->force) {
		double ratio = l->file_size > 0 ? (double)total / (double)l->file_size : 0;
		if ((total > (1LL << 30) && ratio > 100) || l->n > 2000000) {
			char a[32], b[32];
			ac_human(a, sizeof a, (double)total);
			ac_human(b, sizeof b, (double)l->file_size);
			AC_SETERR(o->err, sizeof o->err, "這個封存檔只有 %s，解開卻有 %s（%.0f 倍，%zu 個項目）。正常的壓縮檔很少這麼極端，可能是壓縮炸彈", b, a, ratio, l->n);
			return AC_BOMB;
		}
	}
	/* 目的地：要不要建資料夾 */
	char base[PATH_MAX] = "", target[PATH_MAX];
	int wrap = o->into_folder && ntop != 1;
	if (wrap) ac_base_noext(l->file, base, sizeof base);
	if (wrap && !base[0]) snprintf(base, sizeof base, "解壓縮");
	/* 同名檢查 */
	if (wrap) {
		snprintf(target, sizeof target, "%s/%s", destabs, base);
		struct stat st;
		if (!lstat(target, &st)) join_conflict(o, base);
	} else {
		for (int i = 0; i < ntop; i++) {
			snprintf(target, sizeof target, "%s/%s", destabs, tops[i]);
			struct stat st;
			if (!lstat(target, &st)) join_conflict(o, tops[i]);
		}
	}
	if (o->nconflicts && o->overwrite == AC_OW_ASK) return AC_CONFLICT;
	/* 暫存資料夾（跟目的地同一個檔案系統，搬動才是 rename） */
	char stage[PATH_MAX];
	snprintf(stage, sizeof stage, "%s/.pomelo-extract-XXXXXX", destabs);
	if (!mkdtemp(stage)) { AC_SETERR(o->err, sizeof o->err, "目的資料夾不能寫入：%s", strerror(errno)); return AC_ERR; }
	long long cap = o->max_bytes;
	if (!cap) {
		if (total > 0) cap = total + total / 8 + (16LL << 20);
		else if (avail > 0) cap = avail * 9 / 10;
	}
	int cancel = 0;
	int ret;
	if (l->backend == AB_LA || l->backend == AB_GPG) ret = la_extract_to(l, o, stage, cap, &cancel);
	else ret = ext_extract_to(l, o, stage);
	if (ret != AC_OK) { rmtree(stage); return ret; }
	/* 搬到目的地 */
	char dstdir[PATH_MAX];
	if (wrap) snprintf(dstdir, sizeof dstdir, "%s/%s", destabs, base); else snprintf(dstdir, sizeof dstdir, "%s", destabs);
	int skipped = 0;
	if (wrap) {
		if (mkdir(dstdir, 0755) && errno != EEXIST) { AC_SETERR(o->err, sizeof o->err, "沒辦法建立資料夾：%s", dstdir); rmtree(stage); return AC_ERR; }
		if (o->nconflicts && o->overwrite == AC_OW_RENAME) {
			char uniq[256];
			ac_unique_name(destabs, base, uniq, sizeof uniq);
			snprintf(dstdir, sizeof dstdir, "%s/%s", destabs, uniq);
			if (mkdir(dstdir, 0755) && errno != EEXIST) { AC_SETERR(o->err, sizeof o->err, "沒辦法建立資料夾：%s", dstdir); rmtree(stage); return AC_ERR; }
		}
	}
	DIR *d = opendir(stage);
	struct dirent *de;
	int mrc = 0;
	while (d && (de = readdir(d))) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
		char s[PATH_MAX], t[PATH_MAX];
		snprintf(s, sizeof s, "%s/%s", stage, de->d_name);
		snprintf(t, sizeof t, "%s/%s", dstdir, de->d_name);
		int pol = wrap && o->overwrite == AC_OW_RENAME ? AC_OW_YES : o->overwrite;   /* 資料夾已經改過名，裡面直接放 */
		if (merge_move(s, t, pol, &skipped, o->err, sizeof o->err)) { mrc = -1; break; }
		if (!o->first[0]) snprintf(o->first, sizeof o->first, "%s", t);
	}
	if (d) closedir(d);
	rmtree(stage);
	if (mrc) return AC_ERR;
	snprintf(o->outdir, sizeof o->outdir, "%s", dstdir);
	if (!wrap && ntop == 1 && o->first[0] == 0) snprintf(o->first, sizeof o->first, "%s/%s", dstdir, tops[0]);
	if (l->backend != AB_LA && l->backend != AB_GPG) o->nfiles = (int)nsel;
	return AC_OK;
}

int ac_check_password(AcList *l, const char *pw)
{
	/* 找最小的加密檔案，完整讀一遍（連 CRC／驗證碼都對才算密碼正確） */
	if (l->backend == AB_7Z) return ac_test(l, pw, NULL, NULL, NULL, 0);
	struct archive *a = la_new_read(l->charset, pw, 0);
	if (archive_read_open_filename(a, src_of(l), LA_BLOCK) != ARCHIVE_OK) { archive_read_free(a); return AC_ERR; }
	struct archive_entry *e;
	int r, ret = AC_OK, cancel = 0;
	long long done = 0;
	while ((r = archive_read_next_header(a, &e)) == ARCHIVE_OK || r == ARCHIVE_WARN) {
		if (!archive_entry_is_data_encrypted(e) || archive_entry_size(e) > (4 << 20)) { archive_read_data_skip(a); continue; }
		int rr = la_read_all(a, e, &done, 0, NULL, NULL, "", 0, &cancel);
		if (rr) { ret = (pw && *pw) ? AC_BAD_PASSWORD : AC_NEED_PASSWORD; }
		break;
	}
	archive_read_free(a);
	return ret;
}
