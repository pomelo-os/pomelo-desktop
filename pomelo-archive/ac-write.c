/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ac-write.c — 建立壓縮檔、加入檔案、能力偵測 */
#define _GNU_SOURCE
#include "ac-int.h"
#include <dirent.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <signal.h>

#define WBLOCK 65536

static long long now_ms(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* ---------- 能力偵測：真的寫一個小檔進記憶體試試看（比查版本號可靠） ---------- */

typedef struct {
	const char *fmt;       /* zip 7z tar cpio iso raw */
	const char *filter;    /* gzip bzip2 xz zstd lzip lz4 或 NULL */
	const char *opts;      /* 格式選項 */
} Probe;

static int setup_write(struct archive *a, const Probe *p)
{
	int r = ARCHIVE_OK;
	if (!strcmp(p->fmt, "zip")) r = archive_write_set_format_zip(a);
	else if (!strcmp(p->fmt, "7z")) r = archive_write_set_format_7zip(a);
	else if (!strcmp(p->fmt, "tar")) r = archive_write_set_format_pax_restricted(a);
	else if (!strcmp(p->fmt, "cpio")) r = archive_write_set_format_cpio_newc(a);
	else if (!strcmp(p->fmt, "iso")) r = archive_write_set_format_iso9660(a);
	else if (!strcmp(p->fmt, "raw")) r = archive_write_set_format_raw(a);
	else return ARCHIVE_FATAL;
	if (r < ARCHIVE_WARN) return r;
	if (p->filter) {
		if (!strcmp(p->filter, "gzip")) r = archive_write_add_filter_gzip(a);
		else if (!strcmp(p->filter, "bzip2")) r = archive_write_add_filter_bzip2(a);
		else if (!strcmp(p->filter, "xz")) r = archive_write_add_filter_xz(a);
		else if (!strcmp(p->filter, "zstd")) r = archive_write_add_filter_zstd(a);
		else if (!strcmp(p->filter, "lzip")) r = archive_write_add_filter_lzip(a);
		else if (!strcmp(p->filter, "lz4")) r = archive_write_add_filter_lz4(a);
		else return ARCHIVE_FATAL;
		if (r < ARCHIVE_WARN) return r;
	}
	if (p->opts && *p->opts && archive_write_set_options(a, p->opts) < ARCHIVE_WARN) return ARCHIVE_FATAL;
	return ARCHIVE_OK;
}

/* 寫一個 11 位元組的檔案進記憶體，再讀回來比對；pw 非 NULL 就當加密測試 */
static int probe(const Probe *p, const char *pw)
{
	static const char data[] = "hello world";
	const size_t cap = 1 << 20;   /* ISO 映像最小就要 100 多 KB（libarchive 3.5 比 3.7 大），64 KB 會誤判成不能寫 */
	char *buf = malloc(cap);
	size_t used = 0;
	struct archive *a = archive_write_new();
	int ok = 0;
	if (setup_write(a, p) < ARCHIVE_WARN) goto out;
	if (pw && archive_write_set_passphrase(a, pw) < ARCHIVE_WARN) goto out;
	if (archive_write_open_memory(a, buf, cap, &used) != ARCHIVE_OK) goto out;
	struct archive_entry *e = archive_entry_new();
	archive_entry_set_pathname(e, "probe.txt");
	archive_entry_set_size(e, 11);
	archive_entry_set_filetype(e, AE_IFREG);
	archive_entry_set_perm(e, 0644);
	archive_entry_set_mtime(e, 1700000000, 0);
	int r = archive_write_header(a, e);
	archive_entry_free(e);
	if (r < ARCHIVE_WARN) goto out;
	archive_write_data(a, data, 11);
	if (archive_write_close(a) < ARCHIVE_WARN) goto out;
	if (!strcmp(p->fmt, "iso")) { ok = used > 2048; goto out; }
	struct archive *rd = archive_read_new();
	archive_read_support_filter_all(rd);
	archive_read_support_format_all(rd);
	if (!strcmp(p->fmt, "raw")) archive_read_support_format_raw(rd);
	if (pw) archive_read_add_passphrase(rd, pw);
	if (archive_read_open_memory(rd, buf, used) == ARCHIVE_OK) {
		struct archive_entry *re;
		if (archive_read_next_header(rd, &re) == ARCHIVE_OK) {
			char got[32];
			ssize_t k = archive_read_data(rd, got, sizeof got);
			ok = k == 11 && !memcmp(got, data, 11);
		}
	}
	archive_read_free(rd);
out:
	archive_write_free(a);
	free(buf);
	return ok;
}

static int have_mksquashfs(void)
{
	static int r = -1;
	if (r < 0) {
		const char *p = getenv("PATH");
		char *dup = strdup(p ? p : "/usr/bin:/bin"), *save = NULL;
		r = 0;
		for (char *d = strtok_r(dup, ":", &save); d && !r; d = strtok_r(NULL, ":", &save)) {
			char full[PATH_MAX];
			snprintf(full, sizeof full, "%s/mksquashfs", d);
			r = !access(full, X_OK);
		}
		free(dup);
	}
	return r;
}

static AcFormat fmts[24];
static int nfmts;

const AcFormat *ac_write_formats(int *n)
{
	if (nfmts) { *n = nfmts; return fmts; }
	int gpg = ac_have_gpg(), z7 = ac_7z() != NULL;
	int zip = probe(&(Probe){ "zip", NULL, NULL }, NULL);
	int zaes = probe(&(Probe){ "zip", NULL, "zip:encryption=aes256" }, "pw");
	int zcrypt = probe(&(Probe){ "zip", NULL, "zip:encryption=zipcrypt" }, "pw");
	int la7z = probe(&(Probe){ "7z", NULL, NULL }, NULL);
	int tar = probe(&(Probe){ "tar", NULL, NULL }, NULL);
	int gz = probe(&(Probe){ "tar", "gzip", NULL }, NULL);
	int bz = probe(&(Probe){ "tar", "bzip2", NULL }, NULL);
	int xz = probe(&(Probe){ "tar", "xz", NULL }, NULL);
	int zst = probe(&(Probe){ "tar", "zstd", NULL }, NULL);
	int lz = probe(&(Probe){ "tar", "lzip", NULL }, NULL);
	int lz4 = probe(&(Probe){ "tar", "lz4", NULL }, NULL);
	int cpio = probe(&(Probe){ "cpio", NULL, NULL }, NULL);
	int iso = probe(&(Probe){ "iso", NULL, NULL }, NULL);
	int rgz = probe(&(Probe){ "raw", "gzip", NULL }, NULL);
	int rbz = probe(&(Probe){ "raw", "bzip2", NULL }, NULL);
	int rxz = probe(&(Probe){ "raw", "xz", NULL }, NULL);
	int rzs = probe(&(Probe){ "raw", "zstd", NULL }, NULL);
	int rlz = probe(&(Probe){ "raw", "lzip", NULL }, NULL);
	const char *nogpg = "要加密需要 gpg（請安裝 gnupg 或 gpg）";
	const char *no7z = "需要 7-Zip 才能加密（請安裝 7zip 或 p7zip）";
	struct { const char *id, *label, *ext; int w, enc, names; const char *why, *warn; } t[] = {
		{ "zip", "ZIP", ".zip", zip, zaes, 0, zip ? (zaes ? NULL : "這個 libarchive 版本不能做 AES 加密的 ZIP") : "libarchive 不能寫 ZIP", NULL },
		{ "zip-zipcrypto", "ZIP（舊式加密）", ".zip", zip && zcrypt, zip && zcrypt, 0, (zip && zcrypt) ? NULL : "libarchive 不能寫舊式加密的 ZIP", "舊式 ZipCrypto 加密強度低，容易被破解；只有在對方的軟體不支援 AES 時才用" },
		{ "7z", "7z", ".7z", la7z || z7, z7, z7, z7 ? NULL : (la7z ? no7z : "需要 7-Zip（請安裝 7zip 或 p7zip）"), NULL },
		{ "tar", "TAR", ".tar", tar, gpg, 0, tar ? (gpg ? NULL : nogpg) : "libarchive 不能寫 TAR", NULL },
		{ "tar.gz", "TAR.GZ（gzip）", ".tar.gz", gz, gpg, 0, gz ? (gpg ? NULL : nogpg) : "沒有 gzip 支援", NULL },
		{ "tar.bz2", "TAR.BZ2（bzip2）", ".tar.bz2", bz, gpg, 0, bz ? (gpg ? NULL : nogpg) : "沒有 bzip2 支援", NULL },
		{ "tar.xz", "TAR.XZ（xz）", ".tar.xz", xz, gpg, 0, xz ? (gpg ? NULL : nogpg) : "沒有 xz 支援", NULL },
		{ "tar.zst", "TAR.ZST（zstd）", ".tar.zst", zst, gpg, 0, zst ? (gpg ? NULL : nogpg) : "沒有 zstd 支援（請安裝 zstd）", NULL },
		{ "tar.lz", "TAR.LZ（lzip）", ".tar.lz", lz, gpg, 0, lz ? (gpg ? NULL : nogpg) : "沒有 lzip 支援", NULL },
		{ "tar.lz4", "TAR.LZ4（lz4）", ".tar.lz4", lz4, gpg, 0, lz4 ? (gpg ? NULL : nogpg) : "沒有 lz4 支援", NULL },
		{ "cpio", "CPIO（newc）", ".cpio", cpio, gpg, 0, cpio ? (gpg ? NULL : nogpg) : "libarchive 不能寫 cpio", NULL },
		{ "iso", "ISO 9660 光碟映像", ".iso", iso, 0, 0, iso ? "光碟映像不能加密" : "libarchive 不能寫 ISO", NULL },
		{ "squashfs", "SquashFS（sq）", ".squashfs", have_mksquashfs(), 0, 0, have_mksquashfs() ? "SquashFS 不能加密" : "需要 mksquashfs（請安裝 squashfs-tools）", NULL },
		{ "gz", "單一檔案 .gz", ".gz", rgz, gpg, 0, rgz ? (gpg ? NULL : nogpg) : "沒有 gzip 支援", NULL },
		{ "bz2", "單一檔案 .bz2", ".bz2", rbz, gpg, 0, rbz ? (gpg ? NULL : nogpg) : "沒有 bzip2 支援", NULL },
		{ "xz", "單一檔案 .xz", ".xz", rxz, gpg, 0, rxz ? (gpg ? NULL : nogpg) : "沒有 xz 支援", NULL },
		{ "zst", "單一檔案 .zst", ".zst", rzs, gpg, 0, rzs ? (gpg ? NULL : nogpg) : "沒有 zstd 支援", NULL },
		{ "lz", "單一檔案 .lz", ".lz", rlz, gpg, 0, rlz ? (gpg ? NULL : nogpg) : "沒有 lzip 支援", NULL },
	};
	for (unsigned i = 0; i < sizeof t / sizeof *t; i++) {
		fmts[nfmts++] = (AcFormat){ t[i].id, t[i].label, t[i].ext, t[i].w, t[i].enc && t[i].w, t[i].names && t[i].w, t[i].why, t[i].warn };
	}
	*n = nfmts;
	return fmts;
}

void ac_print_formats(void *fp)
{
	FILE *f = fp ? fp : stdout;
	int n;
	const AcFormat *w = ac_write_formats(&n);
	fprintf(f, "libarchive：%s\n", archive_version_details());
	fprintf(f, "7-Zip：%s\n", ac_7z() ? ac_7z() : "（沒有）可以安裝 7zip 或 p7zip：讀 SquashFS、磁碟映像、加密的 7z，也能建立加密的 7z");
	fprintf(f, "gpg：%s\n", ac_have_gpg() ? "有" : "（沒有）tar／cpio 等格式的加密需要它（gnupg 或 gpg）");
	fprintf(f, "\n讀取：zip jar 7z tar tgz tbz txz tzst tlz cpio ar deb rpm iso cab lha xar 單檔 gz bz2 xz zst lz lzma lz4 Z%s\n",
	        ac_7z() ? " squashfs 磁碟映像(FAT/ext/NTFS) dmg vhd wim" : "");
	fprintf(f, "%-14s %-6s %-6s %-6s %s\n", "格式", "建立", "加密", "加密檔名", "說明");
	for (int i = 0; i < n; i++)
		fprintf(f, "%-14s %-6s %-6s %-6s %s\n", w[i].id, w[i].can_write ? "可" : "不可", w[i].can_encrypt ? "可" : "-", w[i].enc_names ? "可" : "-",
		        w[i].why_not ? w[i].why_not : (w[i].warn ? w[i].warn : ""));
}

/* ---------- 走訪輸入 ---------- */

typedef struct {
	struct archive *a;
	AcCreate *c;
	long long done, total;
	long long last_cb;
	dev_t out_dev; ino_t out_ino; int have_out;
	int cancel;
	char *buf;
} W;

static void count_tree(const char *path, long long *total)
{
	struct stat st;
	if (lstat(path, &st)) return;
	if (S_ISREG(st.st_mode)) { *total += st.st_size; return; }
	if (!S_ISDIR(st.st_mode)) return;
	DIR *d = opendir(path);
	if (!d) return;
	struct dirent *de;
	while ((de = readdir(d))) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
		char sub[PATH_MAX];
		snprintf(sub, sizeof sub, "%s/%s", path, de->d_name);
		count_tree(sub, total);
	}
	closedir(d);
}

static void scan_tree(const char *path, long long *bytes, long *files, int (*cancel)(void *), void *cu, int *stop)
{
	struct stat st;
	if (*stop || lstat(path, &st)) return;
	if (cancel && (++*files % 256 == 0) && cancel(cu)) { *stop = 1; return; }
	if (S_ISREG(st.st_mode)) { *bytes += st.st_size; return; }
	if (!S_ISDIR(st.st_mode)) return;
	DIR *d = opendir(path);
	if (!d) return;
	struct dirent *de;
	while (!*stop && (de = readdir(d))) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
		char sub[PATH_MAX];
		snprintf(sub, sizeof sub, "%s/%s", path, de->d_name);
		scan_tree(sub, bytes, files, cancel, cu, stop);
	}
	closedir(d);
}

void ac_scan(const char *const *inputs, int n, long long *bytes, long *files, int (*cancel)(void *), void *cu)
{
	int stop = 0;
	*bytes = 0;
	*files = 0;
	for (int i = 0; i < n && !stop; i++) scan_tree(inputs[i], bytes, files, cancel, cu, &stop);
}

static int cmp_names(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static int w_add(W *w, const char *path, const char *arcpath);

static int w_add_dir_children(W *w, const char *path, const char *arcpath)
{
	DIR *d = opendir(path);
	if (!d) {
		w->c->nskipped++;
		if (!w->c->skipped_first[0]) snprintf(w->c->skipped_first, sizeof w->c->skipped_first, "%s", path);
		return 0;
	}
	char **names = NULL;
	size_t n = 0, cap = 0;
	struct dirent *de;
	while ((de = readdir(d))) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
		if (n == cap) { cap = cap ? cap * 2 : 64; names = realloc(names, cap * sizeof *names); }
		names[n++] = strdup(de->d_name);
	}
	closedir(d);
	qsort(names, n, sizeof *names, cmp_names);
	int rc = 0;
	for (size_t i = 0; i < n && !rc; i++) {
		char sub[PATH_MAX], asub[PATH_MAX];
		snprintf(sub, sizeof sub, "%s/%s", path, names[i]);
		snprintf(asub, sizeof asub, "%s/%s", arcpath, names[i]);
		rc = w_add(w, sub, asub);
	}
	for (size_t i = 0; i < n; i++) free(names[i]);
	free(names);
	return rc;
}

static int w_add(W *w, const char *path, const char *arcpath)
{
	struct stat st;
	if (lstat(path, &st)) {
		w->c->nskipped++;
		if (!w->c->skipped_first[0]) snprintf(w->c->skipped_first, sizeof w->c->skipped_first, "%s", path);
		return 0;
	}
	if (w->have_out && st.st_dev == w->out_dev && st.st_ino == w->out_ino) return 0;   /* 輸出檔自己 */
	struct stat tst = st;
	if (w->c->follow_links && S_ISLNK(st.st_mode)) { if (!stat(path, &tst)) st = tst; }
	struct archive_entry *e = archive_entry_new();
	archive_entry_set_pathname(e, arcpath);
	archive_entry_copy_stat(e, &st);
	archive_entry_set_uid(e, st.st_uid);
	archive_entry_set_gid(e, st.st_gid);
	archive_entry_set_uname(e, NULL);
	archive_entry_set_gname(e, NULL);
	int rc = 0;
	if (S_ISLNK(st.st_mode)) {
		char tgt[PATH_MAX];
		ssize_t k = readlink(path, tgt, sizeof tgt - 1);
		if (k < 0) { archive_entry_free(e); w->c->nskipped++; return 0; }
		tgt[k] = 0;
		archive_entry_set_symlink(e, tgt);
		archive_entry_set_size(e, 0);
		if (archive_write_header(w->a, e) < ARCHIVE_WARN) { snprintf(w->c->err, sizeof w->c->err, "%s", archive_error_string(w->a)); rc = -1; }
	} else if (S_ISDIR(st.st_mode)) {
		archive_entry_set_size(e, 0);
		if (archive_write_header(w->a, e) < ARCHIVE_WARN) { snprintf(w->c->err, sizeof w->c->err, "%s", archive_error_string(w->a)); rc = -1; }
		if (!rc) rc = w_add_dir_children(w, path, arcpath);
	} else if (S_ISREG(st.st_mode)) {
		int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
		if (fd < 0 && w->c->follow_links) fd = open(path, O_RDONLY | O_CLOEXEC);
		if (fd < 0) {
			archive_entry_free(e);
			w->c->nskipped++;
			if (!w->c->skipped_first[0]) snprintf(w->c->skipped_first, sizeof w->c->skipped_first, "%s", path);
			return 0;
		}
		if (archive_write_header(w->a, e) < ARCHIVE_WARN) { snprintf(w->c->err, sizeof w->c->err, "%s", archive_error_string(w->a)); rc = -1; }
		long long left = st.st_size;
		while (!rc && left > 0) {
			ssize_t k = read(fd, w->buf, WBLOCK);
			if (k <= 0) break;
			if (k > left) k = (ssize_t)left;
			if (archive_write_data(w->a, w->buf, (size_t)k) < 0) { snprintf(w->c->err, sizeof w->c->err, "%s", archive_error_string(w->a)); rc = -1; break; }
			left -= k;
			w->done += k;
			if (w->c->progress && now_ms() - w->last_cb > 80) {
				w->last_cb = now_ms();
				if (w->c->progress(w->c->pu, w->done, w->total, arcpath)) { w->cancel = 1; rc = -2; }
			}
		}
		/* 檔案在壓縮的途中變短了：補零，不然標頭寫的大小跟實際資料對不上，封存檔會壞掉 */
		while (!rc && left > 0) {
			size_t z = left > WBLOCK ? WBLOCK : (size_t)left;
			memset(w->buf, 0, z);
			archive_write_data(w->a, w->buf, z);
			left -= (long long)z;
		}
		close(fd);
		if (!rc) w->c->nfiles++;
	} else {
		w->c->nskipped++;
		if (!w->c->skipped_first[0]) snprintf(w->c->skipped_first, sizeof w->c->skipped_first, "%s", path);
	}
	archive_entry_free(e);
	return rc;
}

/* ---------- gpg 管線 ---------- */

typedef struct { pid_t pid; int data_fd; } Gpg;

static int gpg_start(Gpg *g, const char *out, const char *pw)
{
	int dp[2], pp[2];
	signal(SIGPIPE, SIG_IGN);
	if (pipe2(dp, O_CLOEXEC) || pipe2(pp, O_CLOEXEC)) return -1;
	g->pid = fork();
	if (g->pid < 0) return -1;
	if (!g->pid) {
		if (dup2(dp[0], 0) < 0) _exit(126);
		if (dup2(pp[0], 3) < 0) _exit(126);
		int nul = open("/dev/null", O_WRONLY);
		if (nul >= 0) { dup2(nul, 1); dup2(nul, 2); }
		execlp(ac_gpg(), "gpg", "--batch", "--yes", "--quiet", "--no-tty", "--pinentry-mode", "loopback", "--passphrase-fd", "3",
		       "--symmetric", "--cipher-algo", "AES256", "--s2k-mode", "3", "--s2k-digest-algo", "SHA512", "--s2k-count", "65011712",
		       "--compress-algo", "none", "-o", out, (char *)NULL);
		_exit(127);
	}
	close(dp[0]); close(pp[0]);
	size_t l = strlen(pw);
	char *line = malloc(l + 2);
	memcpy(line, pw, l);
	line[l] = '\n';
	if (write(pp[1], line, l + 1) < 0) { /* gpg 已經結束 */ }
	ac_wipe(line, l + 1);
	free(line);
	close(pp[1]);
	g->data_fd = dp[1];
	return 0;
}

static int gpg_finish(Gpg *g)
{
	close(g->data_fd);
	int st = 0;
	waitpid(g->pid, &st, 0);
	return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
}

/* ---------- 建立 ---------- */

static int level_for(const char *filter, int lvl)
{
	if (!lvl) return 0;
	if (filter && !strcmp(filter, "zstd")) { static const int z[] = { 0, 1, 2, 3, 5, 7, 9, 12, 15, 19 }; return z[lvl > 9 ? 9 : lvl]; }
	return lvl > 9 ? 9 : lvl;
}

typedef struct { const char *id; const char *fmt, *filter, *opts; int single; } Spec;

static const Spec specs[] = {
	{ "zip", "zip", NULL, NULL, 0 }, { "zip-zipcrypto", "zip", NULL, NULL, 0 }, { "7z", "7z", NULL, "7zip:compression=lzma2", 0 },
	{ "tar", "tar", NULL, NULL, 0 }, { "tar.gz", "tar", "gzip", NULL, 0 }, { "tar.bz2", "tar", "bzip2", NULL, 0 }, { "tar.xz", "tar", "xz", NULL, 0 },
	{ "tar.zst", "tar", "zstd", NULL, 0 }, { "tar.lz", "tar", "lzip", NULL, 0 }, { "tar.lz4", "tar", "lz4", NULL, 0 },
	{ "cpio", "cpio", NULL, NULL, 0 }, { "iso", "iso", NULL, "iso9660:joliet,rockridge", 0 },
	{ "gz", "raw", "gzip", NULL, 1 }, { "bz2", "raw", "bzip2", NULL, 1 }, { "xz", "raw", "xz", NULL, 1 }, { "zst", "raw", "zstd", NULL, 1 }, { "lz", "raw", "lzip", NULL, 1 },
};

static const Spec *find_spec(const char *id)
{
	for (unsigned i = 0; i < sizeof specs / sizeof *specs; i++) if (!strcmp(specs[i].id, id)) return &specs[i];
	return NULL;
}

static const char *bn(const char *p)
{
	size_t n = strlen(p);
	while (n > 1 && p[n - 1] == '/') n--;
	static char buf[PATH_MAX];
	snprintf(buf, sizeof buf, "%.*s", (int)n, p);
	char *s = strrchr(buf, '/');
	return s && s[1] ? s + 1 : buf;
}

/* 7-Zip 外部程式建立（加密 7z，或這個 libarchive 沒有 7z 寫入） */
struct sz_prog { AcProgress p; void *pu; };

static int sz_prog_cb(void *u, const char *d, size_t n)
{
	struct sz_prog *sp = u;
	if (!sp->p) return 0;
	for (size_t i = 1; i < n; i++)
		if (d[i] == '%' && isdigit((unsigned char)d[i - 1])) {
			int k = (int)i - 1;
			while (k > 0 && isdigit((unsigned char)d[k - 1])) k--;
			if (sp->p(sp->pu, strtol(d + k, NULL, 10), 100, "")) return 1;
		}
	return 0;
}

static int create_7z_ext(AcCreate *c, const char *tmp)
{
	const char *z = ac_7z();
	if (!z) { snprintf(c->err, sizeof c->err, "需要 7-Zip 才能建立加密的 7z（請安裝 7zip 或 p7zip）"); return AC_NO_BACKEND; }
	/* 全部在同一個資料夾就在那裡執行，不同資料夾就做一個放符號連結的暫存資料夾 */
	char parent[PATH_MAX] = "", stage[PATH_MAX] = "";
	int same = 1;
	for (int i = 0; i < c->ninputs; i++) {
		char tmpp[PATH_MAX], dir[PATH_MAX];
		snprintf(tmpp, sizeof tmpp, "%s", c->inputs[i]);
		size_t n = strlen(tmpp);
		while (n > 1 && tmpp[n - 1] == '/') tmpp[--n] = 0;
		char *s = strrchr(tmpp, '/');
		if (s) { *s = 0; snprintf(dir, sizeof dir, "%s", s == tmpp ? "/" : tmpp); } else snprintf(dir, sizeof dir, ".");
		if (!i) snprintf(parent, sizeof parent, "%s", dir); else if (strcmp(parent, dir)) same = 0;
	}
	char *argv[300];
	int k = 0;
	argv[k++] = (char *)z; argv[k++] = "a"; argv[k++] = "-t7z"; argv[k++] = "-bsp1"; argv[k++] = "-spd"; argv[k++] = "-sccUTF-8"; argv[k++] = "-snl";
	char mx[16], tmpopt[PATH_MAX];
	snprintf(mx, sizeof mx, "-mx=%d", c->level ? (c->level > 9 ? 9 : c->level) : 5);
	argv[k++] = mx;
	argv[k++] = "-p";
	if (c->encrypt_names) argv[k++] = "-mhe=on";
	if (!same) {
		snprintf(stage, sizeof stage, "%s.stage-XXXXXX", tmp);
		if (!mkdtemp(stage)) { snprintf(c->err, sizeof c->err, "沒辦法建立暫存資料夾"); return AC_ERR; }
	}
	snprintf(tmpopt, sizeof tmpopt, "%s", tmp);
	argv[k++] = "--";
	argv[k++] = tmpopt;
	char *names[256];
	int nn = 0;
	for (int i = 0; i < c->ninputs && nn < 250; i++) {
		char *b = strdup(bn(c->inputs[i]));
		if (!same) {
			char lnk[PATH_MAX], abs[PATH_MAX];
			if (!realpath(c->inputs[i], abs)) { free(b); continue; }
			snprintf(lnk, sizeof lnk, "%s/%s", stage, b);
			if (symlink(abs, lnk)) { /* 同名：略過 */ }
		}
		names[nn++] = b;
		argv[k++] = b;
	}
	argv[k] = NULL;
	size_t pl = 0;
	char *in = ac_pw_lines(c->password, &pl);
	struct sz_prog sp = { c->progress, c->pu };
	char *out = NULL;
	/* -snl 在 7zz 才有；p7zip 16.02 沒有這個選項，遇到就拿掉再試 */
	int rc = ac_run(argv, in, pl, same ? parent : stage, sz_prog_cb, &sp, &out);
	if (rc != 0 && out && strstr(out, "Unsupported switch") && strstr(out, "snl")) {
		free(out); out = NULL;
		for (int i = 0; i < k; i++) if (argv[i] && !strcmp(argv[i], "-snl")) { memmove(&argv[i], &argv[i + 1], (size_t)(k - i) * sizeof *argv); k--; break; }
		rc = ac_run(argv, in, pl, same ? parent : stage, sz_prog_cb, &sp, &out);
	}
	ac_pw_free(in, pl);
	for (int i = 0; i < nn; i++) free(names[i]);
	if (stage[0]) {
		char cmd[PATH_MAX + 16];
		DIR *d = opendir(stage);
		struct dirent *de;
		while (d && (de = readdir(d))) { if (de->d_name[0] != '.') { char l[PATH_MAX]; snprintf(l, sizeof l, "%s/%s", stage, de->d_name); unlink(l); } }
		if (d) closedir(d);
		rmdir(stage);
		(void)cmd;
	}
	int ret = AC_OK;
	if (rc == -2) ret = AC_CANCEL;
	else if (rc != 0) {
		const char *m = out ? strstr(out, "ERROR") : NULL;
		snprintf(c->err, sizeof c->err, "%.*s", m ? (int)strcspn(m, "\r\n") : 40, m ? m : "7-Zip 建立失敗");
		ret = AC_ERR;
	}
	free(out);
	return ret;
}

static int create_squashfs(AcCreate *c, const char *tmp)
{
	char *argv[300];
	int k = 0;
	argv[k++] = "mksquashfs";
	for (int i = 0; i < c->ninputs && k < 290; i++) argv[k++] = (char *)c->inputs[i];
	argv[k++] = (char *)tmp; argv[k++] = "-noappend"; argv[k++] = "-no-progress"; argv[k++] = "-keep-as-directory"; argv[k++] = "-comp"; argv[k++] = "xz";
	argv[k] = NULL;
	char *out = NULL;
	int rc = ac_run(argv, NULL, 0, NULL, NULL, NULL, &out);
	int ret = AC_OK;
	if (rc == 127 || rc == -1) { snprintf(c->err, sizeof c->err, "需要 mksquashfs（請安裝 squashfs-tools）"); ret = AC_NO_BACKEND; }
	else if (rc != 0) { snprintf(c->err, sizeof c->err, "mksquashfs 失敗：%.200s", out ? out : ""); ret = AC_ERR; }
	free(out);
	return ret;
}

int ac_create(AcCreate *c)
{
	c->err[0] = 0;
	c->nfiles = c->nskipped = 0;
	c->nbytes = 0;
	c->skipped_first[0] = c->outpath[0] = 0;
	if (!c->out || !*c->out || !c->format || c->ninputs < 1) { snprintf(c->err, sizeof c->err, "沒有指定輸出或內容"); return AC_ERR; }
	int enc = c->password && *c->password;
	const Spec *sp = find_spec(c->format);
	int sq = !strcmp(c->format, "squashfs");
	if (!sp && !sq) { snprintf(c->err, sizeof c->err, "不認得的格式：%s", c->format); return AC_ERR; }
	for (int i = 0; i < c->ninputs; i++) {
		struct stat st;
		if (lstat(c->inputs[i], &st)) { snprintf(c->err, sizeof c->err, "找不到：%s", c->inputs[i]); return AC_ERR; }
	}
	if (sp && sp->single) {
		struct stat st;
		if (c->ninputs != 1 || lstat(c->inputs[0], &st) || !S_ISREG(st.st_mode)) {
			snprintf(c->err, sizeof c->err, "單一檔案格式（.%s）只能壓縮一個檔案；資料夾或多個檔案請選 tar.gz、zip 之類的格式", c->format);
			return AC_ERR;
		}
	}
	int use_gpg = enc && strcmp(c->format, "zip") && strcmp(c->format, "zip-zipcrypto") && strcmp(c->format, "7z") && !sq;
	if (use_gpg && !sq && !ac_have_gpg()) { snprintf(c->err, sizeof c->err, "要加密這種格式需要 gpg（請安裝 gnupg 或 gpg）"); return AC_NO_BACKEND; }
	if (enc && sq) { snprintf(c->err, sizeof c->err, "SquashFS 不能加密"); return AC_ERR; }
	if (enc && !strcmp(c->format, "iso")) { snprintf(c->err, sizeof c->err, "光碟映像不能加密"); return AC_ERR; }
	/* 輸出路徑：tar 系列加密要多 .gpg */
	char outpath[PATH_MAX];
	snprintf(outpath, sizeof outpath, "%s", c->out);
	if (use_gpg) {
		size_t l = strlen(outpath);
		if (l < 4 || strcasecmp(outpath + l - 4, ".gpg")) strncat(outpath, ".gpg", sizeof outpath - l - 1);
	}
	snprintf(c->outpath, sizeof c->outpath, "%s", outpath);
	char tmp[PATH_MAX];
	snprintf(tmp, sizeof tmp, "%s.pomelo-tmp-XXXXXX", outpath);
	int tfd = mkstemp(tmp);
	if (tfd < 0) { snprintf(c->err, sizeof c->err, "輸出的資料夾不能寫入：%s", strerror(errno)); return AC_ERR; }
	struct stat ost;
	fstat(tfd, &ost);
	close(tfd);
	int ret = AC_OK;
	if (sq) ret = create_squashfs(c, tmp);
	else if (!strcmp(c->format, "7z") && (enc || !probe(&(Probe){ "7z", NULL, NULL }, NULL)) && ac_7z()) { unlink(tmp); ret = create_7z_ext(c, tmp); }
	else {
		W w = { .c = c, .have_out = 1, .out_dev = ost.st_dev, .out_ino = ost.st_ino };
		w.buf = malloc(WBLOCK);
		for (int i = 0; i < c->ninputs; i++) count_tree(c->inputs[i], &w.total);
		struct archive *a = archive_write_new();
		w.a = a;
		Probe pr = { sp->fmt, sp->filter, NULL };
		if (setup_write(a, &pr) < ARCHIVE_WARN) { snprintf(c->err, sizeof c->err, "這個系統的 libarchive 不能建立 %s：%s", c->format, archive_error_string(a) ? archive_error_string(a) : ""); ret = AC_ERR; goto done; }
		if (sp->opts) archive_write_set_options(a, sp->opts);
		if (c->level) {
			char o[40];
			snprintf(o, sizeof o, "compression-level=%d", level_for(sp->filter, c->level));
			archive_write_set_options(a, o);
		}
		if (!strcmp(sp->fmt, "tar")) archive_write_set_options(a, "hdrcharset=UTF-8");
		if (!strcmp(c->format, "zip") && enc) {
			if (archive_write_set_options(a, "zip:encryption=aes256") < ARCHIVE_WARN) { snprintf(c->err, sizeof c->err, "這個系統的 libarchive 不能做 AES 加密的 ZIP"); ret = AC_ERR; goto done; }
			archive_write_set_passphrase(a, c->password);
		} else if (!strcmp(c->format, "zip-zipcrypto") && enc) {
			if (archive_write_set_options(a, "zip:encryption=zipcrypt") < ARCHIVE_WARN) { snprintf(c->err, sizeof c->err, "這個系統的 libarchive 不能做舊式加密的 ZIP"); ret = AC_ERR; goto done; }
			archive_write_set_passphrase(a, c->password);
		}
		archive_write_set_bytes_in_last_block(a, 1);   /* 輸出到管線時 libarchive 預設把最後一個區塊補零到 10240：zstd 等格式後面多出來的零會讓別的軟體讀不了 */
		Gpg g = { 0, -1 };
		int open_rc;
		if (use_gpg) {
			if (gpg_start(&g, tmp, c->password)) { snprintf(c->err, sizeof c->err, "沒辦法執行 gpg"); ret = AC_ERR; goto done; }
			open_rc = archive_write_open_fd(a, g.data_fd);
		} else open_rc = archive_write_open_filename(a, tmp);
		if (open_rc != ARCHIVE_OK) { snprintf(c->err, sizeof c->err, "沒辦法寫入：%s", archive_error_string(a)); ret = AC_ERR; if (use_gpg) gpg_finish(&g); goto done; }
		for (int i = 0; i < c->ninputs && ret == AC_OK; i++) {
			const char *name = bn(c->inputs[i]);
			if (sp->single) {
				struct stat st;
				stat(c->inputs[i], &st);
				struct archive_entry *e = archive_entry_new();
				archive_entry_set_pathname(e, name);
				archive_entry_set_filetype(e, AE_IFREG);
				archive_entry_set_size(e, st.st_size);
				archive_write_header(a, e);
				archive_entry_free(e);
				int fd = open(c->inputs[i], O_RDONLY | O_CLOEXEC);
				long long left = st.st_size;
				while (fd >= 0 && left > 0) {
					ssize_t k = read(fd, w.buf, WBLOCK);
					if (k <= 0) break;
					if (archive_write_data(a, w.buf, (size_t)k) < 0) { snprintf(c->err, sizeof c->err, "%s", archive_error_string(a)); ret = AC_ERR; break; }
					left -= k; w.done += k;
					if (c->progress && w.done % (1 << 20) < WBLOCK && c->progress(c->pu, w.done, w.total, name)) { ret = AC_CANCEL; break; }
				}
				if (fd >= 0) close(fd);
				c->nfiles = 1;
				continue;
			}
			int rc = w_add(&w, c->inputs[i], name);
			if (rc == -2) ret = AC_CANCEL; else if (rc) ret = AC_ERR;
		}
		int cr = archive_write_close(a);
		if (ret == AC_OK && cr < ARCHIVE_WARN) { snprintf(c->err, sizeof c->err, "%s", archive_error_string(a)); ret = AC_ERR; }
		if (use_gpg && gpg_finish(&g) && ret == AC_OK) { snprintf(c->err, sizeof c->err, "gpg 加密失敗"); ret = AC_ERR; }
		c->nbytes = w.done;
done:
		archive_write_free(a);
		free(w.buf);
	}
	if (ret != AC_OK) { unlink(tmp); return ret; }
	{ mode_t um = umask(0); umask(um); chmod(tmp, (enc ? 0600 : 0666) & ~um); }
	if (rename(tmp, outpath)) { snprintf(c->err, sizeof c->err, "沒辦法存成 %s：%s", outpath, strerror(errno)); unlink(tmp); return AC_ERR; }
	return AC_OK;
}

/* ---------- 加入到既有的封存檔 ---------- */

int ac_can_modify(const AcList *l, char *why, size_t n)
{
	if (l->backend == AB_GPG) { AC_SETERR(why, n, "加密的 tar 不能直接加入檔案：請先解壓縮，再重新壓縮"); return 0; }
	if (!strcmp(l->fmt, "ZIP") || !strcmp(l->fmt, "TAR")) return 1;
	if (!strcmp(l->fmt, "7z")) { if (ac_7z() || 1) return 1; }
	AC_SETERR(why, n, "%s 格式不能直接加入檔案，請解壓縮後重新壓縮", l->fmt);
	return 0;
}

static const char *codec_filter(const char *c)
{
	if (!c || !*c) return NULL;
	if (!strcmp(c, "gzip") || !strcmp(c, "bzip2") || !strcmp(c, "xz") || !strcmp(c, "zstd") || !strcmp(c, "lzip") || !strcmp(c, "lz4")) return c;
	return NULL;
}

static int under_path(const char *path, const char *prefix)
{
	size_t n = strlen(prefix);
	return !strncmp(path, prefix, n) && (path[n] == 0 || path[n] == '/');
}

int ac_add(const char *archive, const char *pw, const char *const *inputs, int n, AcProgress p, void *pu, char *err, size_t en)
{
	AcList l;
	int rc = ac_open(&l, archive, pw, NULL, err, en);
	if (rc) return rc;
	char why[200];
	if (!ac_can_modify(&l, why, sizeof why)) { AC_SETERR(err, en, "%s", why); ac_close(&l); return AC_ERR; }
	if (l.enc_data && !(pw && *pw)) { ac_close(&l); return AC_NEED_PASSWORD; }
	char tmp[PATH_MAX];
	snprintf(tmp, sizeof tmp, "%s.pomelo-tmp-XXXXXX", archive);
	int tfd = mkstemp(tmp);
	if (tfd < 0) { AC_SETERR(err, en, "沒辦法寫入：%s", strerror(errno)); ac_close(&l); return AC_ERR; }
	close(tfd);
	/* 7z（有 7-Zip）：直接用 7z a 加進去 */
	if (!strcmp(l.fmt, "7z") && l.backend == AB_7Z && ac_7z()) {
		unlink(tmp);
		char *argv[300];
		int k = 0;
		argv[k++] = (char *)ac_7z(); argv[k++] = "a"; argv[k++] = "-bsp1"; argv[k++] = "-spd"; argv[k++] = "-sccUTF-8";
		if (pw && *pw) argv[k++] = "-p";
		if (l.enc_names) argv[k++] = "-mhe=on";
		argv[k++] = "--";
		argv[k++] = (char *)archive;
		for (int i = 0; i < n && k < 290; i++) argv[k++] = (char *)inputs[i];
		argv[k] = NULL;
		size_t pl = 0;
		char *in = pw && *pw ? ac_pw_lines(pw, &pl) : NULL;
		struct sz_prog sp = { p, pu };
		char *out = NULL;
		rc = ac_run(argv, in, pl, NULL, sz_prog_cb, &sp, &out);
		ac_pw_free(in, pl);
		int ret = rc == 0 ? AC_OK : rc == -2 ? AC_CANCEL : AC_ERR;
		if (ret == AC_ERR) { const char *m = out ? strstr(out, "ERROR") : NULL; AC_SETERR(err, en, "%.*s", m ? (int)strcspn(m, "\r\n") : 30, m ? m : "7-Zip 加入失敗"); }
		free(out);
		ac_close(&l);
		return ret;
	}
	/* 其他：把舊的全部複製到新檔，再加新的，最後換掉原檔 */
	const char *fmt_id = !strcmp(l.fmt, "ZIP") ? (l.enc_data ? "zip" : "zip") : !strcmp(l.fmt, "7z") ? "7z" : "tar";
	const char *filter = !strcmp(fmt_id, "tar") ? codec_filter(l.codec) : NULL;
	struct archive *a = archive_write_new();
	Probe pr = { !strcmp(fmt_id, "zip") ? "zip" : !strcmp(fmt_id, "7z") ? "7z" : "tar", filter, !strcmp(fmt_id, "7z") ? "7zip:compression=lzma2" : NULL };
	if (setup_write(a, &pr) < ARCHIVE_WARN) { AC_SETERR(err, en, "這個系統沒辦法寫這種格式"); archive_write_free(a); unlink(tmp); ac_close(&l); return AC_ERR; }
	if (!strcmp(fmt_id, "zip") && l.enc_data) { archive_write_set_options(a, "zip:encryption=aes256"); archive_write_set_passphrase(a, pw); }
	if (archive_write_open_filename(a, tmp) != ARCHIVE_OK) { AC_SETERR(err, en, "%s", archive_error_string(a)); archive_write_free(a); unlink(tmp); ac_close(&l); return AC_ERR; }
	/* 新項目的頂層名稱：舊的同名項目要被取代 */
	const char *newtop[256];
	int ntop = 0;
	for (int i = 0; i < n && ntop < 256; i++) newtop[ntop++] = bn(inputs[i]);
	char *tops[256];
	for (int i = 0; i < ntop; i++) tops[i] = strdup(newtop[i]);
	struct archive *r = archive_read_new();
	archive_read_support_filter_all(r);
	archive_read_support_format_all(r);
	if (l.charset[0]) { char o[64]; snprintf(o, sizeof o, "hdrcharset=%s", l.charset); archive_read_set_options(r, o); }
	if (pw && *pw) archive_read_add_passphrase(r, pw);
	int ret = AC_OK;
	if (archive_read_open_filename(r, archive, WBLOCK) != ARCHIVE_OK) { AC_SETERR(err, en, "%s", archive_error_string(r)); ret = AC_ERR; }
	struct archive_entry *e;
	char *buf = malloc(WBLOCK);
	while (ret == AC_OK && archive_read_next_header(r, &e) == ARCHIVE_OK) {
		const char *nm = archive_entry_pathname_utf8(e);
		int drop = 0;
		for (int i = 0; nm && i < ntop; i++) if (under_path(nm, tops[i])) drop = 1;
		if (drop) { archive_read_data_skip(r); continue; }
		if (nm) archive_entry_set_pathname_utf8(e, nm);
		if (archive_write_header(a, e) < ARCHIVE_WARN) { AC_SETERR(err, en, "%s", archive_error_string(a)); ret = AC_ERR; break; }
		if (archive_entry_filetype(e) == AE_IFREG) {
			ssize_t k;
			while ((k = archive_read_data(r, buf, WBLOCK)) > 0) if (archive_write_data(a, buf, (size_t)k) < 0) { AC_SETERR(err, en, "%s", archive_error_string(a)); ret = AC_ERR; break; }
			if (k < 0 && ret == AC_OK) { AC_SETERR(err, en, "讀取原有內容失敗：%s", archive_error_string(r)); ret = (pw && *pw) ? AC_BAD_PASSWORD : AC_NEED_PASSWORD; }
		}
	}
	archive_read_free(r);
	free(buf);
	if (ret == AC_OK) {
		/* 新的內容用同一個 writer 接著寫：借用 w_add */
		AcCreate c = { 0 };
		c.progress = p; c.pu = pu;
		W w = { .c = &c, .a = a };
		w.buf = malloc(WBLOCK);
		struct stat ost;
		if (!stat(tmp, &ost)) { w.have_out = 1; w.out_dev = ost.st_dev; w.out_ino = ost.st_ino; }
		for (int i = 0; i < n; i++) count_tree(inputs[i], &w.total);
		for (int i = 0; i < n && ret == AC_OK; i++) {
			int rr = w_add(&w, inputs[i], bn(inputs[i]));
			if (rr == -2) ret = AC_CANCEL; else if (rr) { AC_SETERR(err, en, "%s", c.err); ret = AC_ERR; }
		}
		free(w.buf);
	}
	int cr = archive_write_close(a);
	if (ret == AC_OK && cr < ARCHIVE_WARN) { AC_SETERR(err, en, "%s", archive_error_string(a)); ret = AC_ERR; }
	archive_write_free(a);
	for (int i = 0; i < ntop; i++) free(tops[i]);
	if (ret == AC_OK) { struct stat st; if (!stat(archive, &st)) chmod(tmp, st.st_mode & 0777); if (rename(tmp, archive)) { AC_SETERR(err, en, "沒辦法取代原檔：%s", strerror(errno)); ret = AC_ERR; } }
	if (ret != AC_OK) unlink(tmp);
	ac_close(&l);
	return ret;
}
