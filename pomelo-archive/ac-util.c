/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ac-util.c — 共用小工具：路徑整理、外部程式執行、能力偵測 */
#define _GNU_SOURCE
#include "ac-int.h"
#include <dirent.h>
#include <ftw.h>
#include <langinfo.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <locale.h>
/* libarchive 轉檔名的編碼看目前的 locale：一定要是 UTF-8（使用者的 locale 是 C／POSIX 的話，中文檔名會變成問號） */
void ac_init_locale(void)
{
	static int done;
	if (done) return;
	done = 1;
	setlocale(LC_ALL, "");
	const char *cs = nl_langinfo(CODESET);
	if (cs && (!strcasecmp(cs, "UTF-8") || !strcasecmp(cs, "UTF8"))) return;
	if (!setlocale(LC_CTYPE, "C.UTF-8") && !setlocale(LC_CTYPE, "en_US.UTF-8")) setlocale(LC_CTYPE, "C.utf8");
}

/* 暫存資料夾（gpg 解密後的內層封存檔、瀏覽視窗解出來要打開的檔案）：放在使用者的快取資料夾，不放 /run/user（那是記憶體，大檔會把記憶體吃光）。
 * 資料夾權限 0700；一天以前留下來的（程式當掉沒收乾淨）順便清掉。 */
static int rm_entry(const char *p, const struct stat *st, int flag, struct FTW *w) { (void)st; (void)flag; (void)w; return remove(p); }

int ac_tmpdir(char *out, size_t n)
{
	const char *c = getenv("XDG_CACHE_HOME"), *h = getenv("HOME");
	if (c && *c) snprintf(out, n, "%s/pomelo/archive-tmp", c);
	else snprintf(out, n, "%s/.cache/pomelo/archive-tmp", h && *h ? h : "/tmp");
	char tmp[PATH_MAX];
	snprintf(tmp, sizeof tmp, "%s", out);
	for (char *p = tmp + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(tmp, 0700); *p = '/'; }
	if (mkdir(tmp, 0700) && errno != EEXIST) return -1;
	chmod(tmp, 0700);
	DIR *d = opendir(out);
	if (d) {
		struct dirent *de;
		time_t now = time(NULL);
		while ((de = readdir(d))) {
			if (strncmp(de->d_name, "pa-", 3)) continue;
			char f[PATH_MAX];
			struct stat st;
			snprintf(f, sizeof f, "%s/%s", out, de->d_name);
			if (lstat(f, &st) || now - st.st_mtime <= 86400) continue;
			if (S_ISDIR(st.st_mode)) nftw(f, rm_entry, 16, FTW_DEPTH | FTW_PHYS);   /* pa-open-XXXXXX：解出來的一兩個檔案（可能有子資料夾） */
			else unlink(f);
		}
		closedir(d);
	}
	return 0;
}

void ac_wipe(void *p, size_t n)
{
	volatile unsigned char *v = p;
	while (n--) *v++ = 0;
}

void ac_human(char *out, size_t n, double b)
{
	const char *u[] = { "B", "KB", "MB", "GB", "TB" };
	int i = 0;
	while (b >= 1000 && i < 4) { b /= 1000; i++; }
	if (i == 0) snprintf(out, n, "%.0f %s", b, u[i]);
	else snprintf(out, n, b < 10 ? "%.1f %s" : "%.0f %s", b, u[i]);
}

/* ---- 路徑 ---- */

int ac_path_clean(const char *in, char *out, size_t n)
{
	size_t o = 0;
	const char *p = in;
	int safe = 1;
	if (!in || !*in || strchr(in, '\0') - in >= (long)PATH_MAX) { if (n) out[0] = 0; return 0; }
	for (;;) {
		while (*p == '/') p++;
		if (!*p) break;
		const char *e = p;
		while (*e && *e != '/') e++;
		size_t l = (size_t)(e - p);
		if (l == 1 && p[0] == '.') { p = e; continue; }
		if (l == 2 && p[0] == '.' && p[1] == '.') { safe = 0; p = e; continue; }
		if (o + l + 2 >= n) { safe = 0; break; }
		if (o) out[o++] = '/';
		memcpy(out + o, p, l);
		o += l;
		p = e;
	}
	out[o] = 0;
	if (!o) safe = 0;
	return safe;
}

/* ---- 副檔名 ---- */

static const char *const multi_ext[] = { ".tar.gz", ".tar.bz2", ".tar.xz", ".tar.zst", ".tar.lz", ".tar.lz4", ".tar.lzma", ".tar.Z", ".tar.gpg", ".tar.gz.gpg", ".tar.bz2.gpg", ".tar.xz.gpg", ".tar.zst.gpg", ".tar.lz.gpg", ".tar.lz4.gpg", NULL };
static const char *const arch_ext[] = {
	".zip", ".jar", ".war", ".apk", ".epub", ".7z", ".tar", ".tgz", ".tbz", ".tbz2", ".txz", ".tzst", ".tlz", ".taz",
	".gz", ".bz2", ".xz", ".zst", ".lz", ".lzma", ".lz4", ".z", ".cpio", ".ar", ".a", ".deb", ".rpm", ".iso", ".img", ".squashfs", ".sq", ".sfs",
	".cab", ".lzh", ".lha", ".xar", ".gpg", ".pgp", ".vhd", ".vmdk", ".dmg", ".wim", ".ntfs", ".fat", ".ext2", ".ext3", ".ext4", ".udf", NULL };

static int ends_ci(const char *s, const char *suf)
{
	size_t a = strlen(s), b = strlen(suf);
	return a >= b && !strcasecmp(s + a - b, suf);
}

int ac_is_archive_name(const char *path)
{
	for (int i = 0; arch_ext[i]; i++) if (ends_ci(path, arch_ext[i])) return 1;
	return 0;
}

char *ac_base_noext(const char *file, char *out, size_t n)
{
	const char *b = strrchr(file, '/');
	b = b ? b + 1 : file;
	snprintf(out, n, "%s", b);
	for (int i = 0; multi_ext[i]; i++)
		if (ends_ci(out, multi_ext[i])) { out[strlen(out) - strlen(multi_ext[i])] = 0; return out; }
	char *d = strrchr(out, '.');
	if (d && d != out) *d = 0;
	return out;
}

void ac_unique_name(const char *dir, const char *name, char *out, size_t n)
{
	char path[PATH_MAX];
	struct stat st;
	snprintf(out, n, "%s", name);
	snprintf(path, sizeof path, "%s/%s", dir, out);
	if (lstat(path, &st)) return;
	const char *dot = strrchr(name, '.');
	size_t base = dot && dot != name ? (size_t)(dot - name) : strlen(name);
	for (int i = 2; i < 10000; i++) {
		snprintf(out, n, "%.*s (%d)%s", (int)base, name, i, name + base);
		snprintf(path, sizeof path, "%s/%s", dir, out);
		if (lstat(path, &st)) return;
	}
}

const char *ac_charsets(int i)
{
	static const char *const cs[] = { "UTF-8", "BIG5", "GBK", "GB18030", "SHIFT_JIS", "EUC-JP", "EUC-KR", "CP437", "CP1252", NULL };
	return i >= 0 && i < 9 ? cs[i] : NULL;
}

/* ---- 外部程式 ---- */

static const char *find_in_path(const char *const *names)
{
	static char found[8][PATH_MAX];
	static int nf;
	const char *path = getenv("PATH");
	if (!path) path = "/usr/local/bin:/usr/bin:/bin";
	for (int i = 0; names[i]; i++) {
		char *dup = strdup(path), *save = NULL;
		for (char *d = strtok_r(dup, ":", &save); d; d = strtok_r(NULL, ":", &save)) {
			char full[PATH_MAX];
			snprintf(full, sizeof full, "%s/%s", d, names[i]);
			if (!access(full, X_OK)) {
				free(dup);
				snprintf(found[nf % 8], PATH_MAX, "%s", full);
				return found[nf++ % 8];
			}
		}
		free(dup);
	}
	return NULL;
}

const char *ac_7z(void)
{
	static const char *r; static int done;
	if (!done) { const char *const n[] = { "7zz", "7z", "7za", NULL }; r = getenv("POMELO_ARCHIVE_7Z") ? getenv("POMELO_ARCHIVE_7Z") : find_in_path(n); done = 1; if (r && !*r) r = NULL; }
	return r;
}

int ac_have_gpg(void)
{
	static int r = -1;
	if (r < 0) { const char *const n[] = { "gpg", "gpg2", NULL }; r = getenv("POMELO_ARCHIVE_NOGPG") ? 0 : find_in_path(n) != NULL; }
	return r;
}

const char *ac_gpg(void)
{
	static const char *r;
	if (!r) { const char *const n[] = { "gpg", "gpg2", NULL }; r = find_in_path(n); }
	return r;
}

/* 執行外部程式。stdin_data 寫進它的標準輸入（密碼用）；標準輸出與錯誤合併後一路交給 line_cb（NULL = 收進 *out）。
 * line_cb 回傳非 0 = 殺掉它。回傳結束碼；被取消回傳 -2；沒辦法執行回傳 -1。 */
int ac_run(char *const argv[], const char *stdin_data, size_t stdin_len, const char *cwd,
           int (*cb)(void *, const char *, size_t), void *cbu, char **out)
{
	int in[2], ot[2];
	static int sigset;
	if (!sigset) { signal(SIGPIPE, SIG_IGN); sigset = 1; }
	if (pipe2(in, O_CLOEXEC) || pipe2(ot, O_CLOEXEC)) return -1;
	pid_t pid = fork();
	if (pid < 0) { close(in[0]); close(in[1]); close(ot[0]); close(ot[1]); return -1; }
	if (!pid) {
		if (cwd && chdir(cwd)) _exit(126);
		if (dup2(in[0], 0) < 0 || dup2(ot[1], 1) < 0 || dup2(ot[1], 2) < 0) _exit(126);
		setenv("LC_ALL", "C.UTF-8", 1);
		execvp(argv[0], argv);
		_exit(127);
	}
	close(in[0]); close(ot[1]);
	if (stdin_data && stdin_len) { if (write(in[1], stdin_data, stdin_len) < 0) { /* 子程式先結束了 */ } }
	close(in[1]);
	size_t cap = 4096, n = 0;
	char *buf = out ? malloc(cap) : NULL;
	int cancelled = 0;
	for (;;) {
		char tmp[4096];
		ssize_t k = read(ot[0], tmp, sizeof tmp);
		if (k <= 0) { if (k < 0 && errno == EINTR) continue; break; }
		if (buf) {
			if (n + (size_t)k + 1 > cap) { cap = (n + (size_t)k) * 2; buf = realloc(buf, cap); }
			memcpy(buf + n, tmp, (size_t)k);
			n += (size_t)k;
		}
		if (cb && cb(cbu, tmp, (size_t)k)) { cancelled = 1; kill(pid, SIGKILL); break; }
	}
	close(ot[0]);
	int st = 0;
	waitpid(pid, &st, 0);
	if (buf) { buf[n] = 0; *out = buf; }
	if (cancelled) return -2;
	if (WIFEXITED(st)) return WEXITSTATUS(st);
	return -1;
}

/* 密碼送進外部程式的標準輸入：每行一個，重複 3 次（7z 打開、建立、驗證各會問一次） */
char *ac_pw_lines(const char *pw, size_t *len)
{
	size_t l = strlen(pw);
	char *s = malloc(3 * (l + 1) + 1);
	char *p = s;
	for (int i = 0; i < 3; i++) { memcpy(p, pw, l); p += l; *p++ = '\n'; }
	*p = 0;
	*len = (size_t)(p - s);
	return s;
}

void ac_pw_free(char *s, size_t len)
{
	if (s) { ac_wipe(s, len); free(s); }
}

/* ---- 清單 ---- */

AcEntry *ac_list_add(AcList *l)
{
	if (l->n == l->cap) {
		l->cap = l->cap ? l->cap * 2 : 256;
		l->e = realloc(l->e, l->cap * sizeof *l->e);
	}
	AcEntry *e = &l->e[l->n++];
	memset(e, 0, sizeof *e);
	e->size = e->csize = -1;
	return e;
}

void ac_close(AcList *l)
{
	for (size_t i = 0; i < l->n; i++) { free(l->e[i].path); free(l->e[i].link); }
	free(l->e);
	free(l->file);
	if (l->plain) { unlink(l->plain); free(l->plain); }
	memset(l, 0, sizeof *l);
}

int ac_ext_for(const char *f, char *out, size_t n)
{
	static const struct { const char *id, *ext; } t[] = {
		{ "zip", ".zip" }, { "zip-zipcrypto", ".zip" }, { "7z", ".7z" }, { "tar", ".tar" }, { "tar.gz", ".tar.gz" }, { "tar.bz2", ".tar.bz2" },
		{ "tar.xz", ".tar.xz" }, { "tar.zst", ".tar.zst" }, { "tar.lz", ".tar.lz" }, { "tar.lz4", ".tar.lz4" }, { "cpio", ".cpio" }, { "iso", ".iso" },
		{ "gz", ".gz" }, { "bz2", ".bz2" }, { "xz", ".xz" }, { "zst", ".zst" }, { "lz", ".lz" }, { "squashfs", ".squashfs" } };
	for (unsigned i = 0; i < sizeof t / sizeof *t; i++) if (!strcmp(f, t[i].id)) { snprintf(out, n, "%s", t[i].ext); return 1; }
	return 0;
}
