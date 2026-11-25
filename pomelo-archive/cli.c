/* SPDX-License-Identifier: GPL-3.0-or-later */
/* cli.c — 無介面的命令列模式（--list --test --extract --create --add --formats）：給腳本、測試與小幫手用 */
#define _GNU_SOURCE
#include "ac-int.h"
#include "cli.h"
#include <getopt.h>
#include <locale.h>

static void read_pw_stdin(char *buf, size_t n)
{
	if (!fgets(buf, (int)n, stdin)) { buf[0] = 0; return; }
	buf[strcspn(buf, "\r\n")] = 0;
}

static int exit_code(int rc)
{
	static const int m[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };
	return rc >= 0 && rc < 10 ? m[rc] : 1;
}

static int cli_prog(void *u, long long done, long long total, const char *cur)
{
	(void)u;
	if (total > 0) fprintf(stderr, "\r%3lld%%  %.60s\033[K", done * 100 / total, cur ? cur : "");
	return 0;
}

static void fmt_time(time_t t, char *out, size_t n)
{
	if (!t) { snprintf(out, n, "-"); return; }
	struct tm tm;
	localtime_r(&t, &tm);
	strftime(out, n, "%Y-%m-%d %H:%M:%S", &tm);
}

static const char *rc_msg(int rc)
{
	switch (rc) {
	case AC_NEED_PASSWORD: return "需要密碼（用 --password-stdin 從標準輸入給）";
	case AC_BAD_PASSWORD: return "密碼不對";
	case AC_NO_BACKEND: return "缺少外部程式";
	case AC_CANCEL: return "已取消";
	case AC_UNSAFE: return "內容不安全";
	case AC_NOSPACE: return "磁碟空間不夠";
	case AC_BOMB: return "解開後大小不正常（可能是壓縮炸彈）；確定要解的話加 --force";
	case AC_CONFLICT: return "目的地已有同名項目：加 --overwrite=yes|no|rename 決定怎麼處理";
	default: return "失敗";
	}
}

int cli_main(int argc, char **argv)
{
	const char *mode = argc > 1 ? argv[1] : "";
	ac_init_locale();
	if (!strcmp(mode, "--formats")) { ac_print_formats(stdout); return 0; }
	int is_cli = !strcmp(mode, "--list") || !strcmp(mode, "--test") || !strcmp(mode, "--extract") || !strcmp(mode, "--create") || !strcmp(mode, "--add") || !strcmp(mode, "--check-password");
	if (!is_cli) return -1;
	char pw[512] = "", charset[32] = "", err[512] = "", dir[PATH_MAX] = ".", format[32] = "";
	int use_pw = 0, level = 0, enc_names = 0, force = 0, ow = AC_OW_ASK, into = 0;
	const char *only[256];
	int nonly = 0;
	const char *files[1024];
	int nf = 0;
	const char *target = NULL;
	for (int i = 2; i < argc; i++) {
		const char *a = argv[i];
		if (!strcmp(a, "--password-stdin")) use_pw = 1;
		else if (!strcmp(a, "-C") && i + 1 < argc) snprintf(dir, sizeof dir, "%s", argv[++i]);
		else if (!strcmp(a, "--charset") && i + 1 < argc) snprintf(charset, sizeof charset, "%s", argv[++i]);
		else if (!strncmp(a, "--format=", 9)) snprintf(format, sizeof format, "%s", a + 9);
		else if (!strncmp(a, "--level=", 8)) level = atoi(a + 8);
		else if (!strcmp(a, "--encrypt-names")) enc_names = 1;
		else if (!strcmp(a, "--force")) force = 1;
		else if (!strcmp(a, "--into-folder")) into = 1;
		else if (!strncmp(a, "--overwrite=", 12)) ow = !strcmp(a + 12, "yes") ? AC_OW_YES : !strcmp(a + 12, "no") ? AC_OW_NO : !strcmp(a + 12, "rename") ? AC_OW_RENAME : AC_OW_ASK;
		else if (!strcmp(a, "--only") && i + 1 < argc && nonly < 256) only[nonly++] = argv[++i];
		else if (!target && strcmp(mode, "--create") && strcmp(mode, "--add")) target = a;
		else if (!target) target = a;
		else if (nf < 1024) files[nf++] = a;
	}
	if (use_pw) read_pw_stdin(pw, sizeof pw);
	const char *p = use_pw ? pw : NULL;
	int rc;
	if (!strcmp(mode, "--create")) {
		if (!target || !nf) { fprintf(stderr, "用法：pomelo-archive --create 輸出檔 [--format=zip|7z|tar.gz…] [--level=1-9] [--password-stdin] [--encrypt-names] 檔案…\n"); return 1; }
		if (!format[0]) {
			/* 沒指定就看輸出檔的副檔名 */
			static const char *const try[] = { "tar.gz", "tar.bz2", "tar.xz", "tar.zst", "tar.lz4", "tar.lz", "tar", "zip", "7z", "cpio", "iso", "squashfs", "gz", "bz2", "xz", "zst", "lz", NULL };
			for (int i = 0; try[i]; i++) { char ext[24]; ac_ext_for(try[i], ext, sizeof ext); size_t l = strlen(target), e = strlen(ext); if (!strcmp(try[i], "squashfs")) snprintf(ext, sizeof ext, ".squashfs"), e = strlen(ext); if (l > e && !strcasecmp(target + l - e, ext)) { snprintf(format, sizeof format, "%s", try[i]); break; } }
			if (!format[0]) snprintf(format, sizeof format, "zip");
		}
		AcCreate c = { .out = target, .format = format, .level = level, .password = p && *p ? p : NULL, .encrypt_names = enc_names,
		               .inputs = files, .ninputs = nf, .progress = isatty(2) ? cli_prog : NULL };
		rc = ac_create(&c);
		if (isatty(2)) fprintf(stderr, "\r\033[K");
		if (rc) { fprintf(stderr, "pomelo-archive：%s\n", c.err[0] ? c.err : rc_msg(rc)); ac_wipe(pw, sizeof pw); return exit_code(rc); }
		printf("%s\t%d\t%lld\n", c.outpath, c.nfiles, c.nbytes);
		if (c.nskipped) fprintf(stderr, "略過 %d 個讀不了的項目（例如 %s）\n", c.nskipped, c.skipped_first);
		ac_wipe(pw, sizeof pw);
		return 0;
	}
	if (!strcmp(mode, "--add")) {
		if (!target || !nf) { fprintf(stderr, "用法：pomelo-archive --add 封存檔 檔案… [--password-stdin]\n"); return 1; }
		rc = ac_add(target, p, files, nf, NULL, NULL, err, sizeof err);
		if (rc) fprintf(stderr, "pomelo-archive：%s\n", err[0] ? err : rc_msg(rc));
		ac_wipe(pw, sizeof pw);
		return exit_code(rc);
	}
	if (!target) { fprintf(stderr, "用法：pomelo-archive %s 封存檔\n", mode); return 1; }
	AcList l;
	rc = ac_open(&l, target, p, charset[0] ? charset : NULL, err, sizeof err);
	if (rc) { fprintf(stderr, "pomelo-archive：%s\n", err[0] ? err : rc_msg(rc)); ac_wipe(pw, sizeof pw); return exit_code(rc); }
	if (!strcmp(mode, "--check-password")) {
		rc = ac_check_password(&l, p);
		fprintf(stderr, "%s\n", rc == AC_OK ? "密碼正確" : rc_msg(rc));
		ac_close(&l); ac_wipe(pw, sizeof pw);
		return exit_code(rc);
	}
	if (!strcmp(mode, "--list")) {
		fprintf(stderr, "格式：%s%s%s  項目：%zu  解開後：%lld 位元組  加密：%s%s  編碼：%s%s  後端：%s\n", l.fmt, l.codec[0] ? "+" : "", l.codec, l.n, l.total,
		        l.enc_data ? "有" : "無", l.enc_names ? "（含檔名）" : "", l.charset[0] ? l.charset : "UTF-8", l.charset_guessed ? "（自動偵測）" : "",
		        l.backend == AB_LA ? "libarchive" : l.backend == AB_7Z ? "7-Zip" : "gpg+libarchive");
		for (size_t i = 0; i < l.n; i++) {
			char t[40];
			fmt_time(l.e[i].mtime, t, sizeof t);
			printf("%c\t%lld\t%s\t%s%s%s\n", l.e[i].type == AE_DIR ? 'D' : l.e[i].type == AE_LINK ? 'L' : l.e[i].type == AE_OTHER ? 'O' : 'F', l.e[i].size, t, l.e[i].path,
			       l.e[i].link ? " -> " : "", l.e[i].link ? l.e[i].link : "");
		}
		ac_close(&l); ac_wipe(pw, sizeof pw);
		return 0;
	}
	if (!strcmp(mode, "--test")) {
		rc = ac_test(&l, p, isatty(2) ? cli_prog : NULL, NULL, err, sizeof err);
		if (isatty(2)) fprintf(stderr, "\r\033[K");
		if (rc) fprintf(stderr, "pomelo-archive：%s\n", err[0] ? err : rc_msg(rc)); else printf("OK\t%zu\n", l.n);
		ac_close(&l); ac_wipe(pw, sizeof pw);
		return exit_code(rc);
	}
	/* --extract */
	AcExtract o = { .dest = dir, .password = p, .only = nonly ? only : NULL, .nonly = nonly, .overwrite = ow, .force = force, .into_folder = into,
	                .progress = isatty(2) ? cli_prog : NULL };
	rc = ac_extract(&l, &o);
	if (isatty(2)) fprintf(stderr, "\r\033[K");
	if (rc) {
		fprintf(stderr, "pomelo-archive：%s%s%s\n", o.err[0] ? o.err : rc_msg(rc), o.conflicts[0] ? "：" : "", o.conflicts);
	} else {
		printf("%s\t%d\t%lld\n", o.outdir, o.nfiles, o.nbytes);
		if (o.skipped_unsafe || o.skipped_special) fprintf(stderr, "略過 %d 個不安全的項目、%d 個特殊檔案\n", o.skipped_unsafe, o.skipped_special);
	}
	ac_close(&l);
	ac_wipe(pw, sizeof pw);
	return exit_code(rc);
}
