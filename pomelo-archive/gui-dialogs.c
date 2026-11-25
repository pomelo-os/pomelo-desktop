/* SPDX-License-Identifier: GPL-3.0-or-later */
/* gui-dialogs.c — 以 pm_ui_run 為主體的視窗：密碼、解壓縮（進度）、建立壓縮檔、加入檔案
 * 長時間的工作都在背景執行緒做，視窗每 100 毫秒看一次進度；關掉視窗等於按取消（工作會收拾乾淨再結束）。 */
#define _GNU_SOURCE
#include "gui.h"
#include <pthread.h>
#include <sys/stat.h>

#define PROG_COLOR 0x0a64d6

static void read_pw_stdin(char *buf, size_t n)
{
	if (!fgets(buf, (int)n, stdin)) { buf[0] = 0; return; }
	buf[strcspn(buf, "\r\n")] = 0;
}

static const char *base_of(const char *p)
{
	const char *b = strrchr(p, '/');
	return b ? b + 1 : p;
}

static void dir_of(const char *p, char *out, size_t n)
{
	snprintf(out, n, "%s", p);
	char *s = strrchr(out, '/');
	if (s == out) out[1] = 0; else if (s) *s = 0; else snprintf(out, n, ".");
}

/* ======================= 密碼視窗 ======================= */

static char pw_file[PATH_MAX];
static char pw_buf[256];
static int pw_wrong;
static unsigned long pw_parent;

static void pw_done(int ok)
{
	if (ok && pw_buf[0]) { fputs(pw_buf, stdout); fflush(stdout); ac_wipe(pw_buf, sizeof pw_buf); exit(0); }
	exit(1);
}

static void pw_build(PmUI *u)
{
	pa_center(u, pw_parent);
	pm_ui_title(u, _("需要密碼"), base_of(pw_file));
	char msg[400];
	snprintf(msg, sizeof msg, "%s", pw_wrong ? _("密碼不對，請再試一次。") : _("這個壓縮檔有加密，請輸入密碼才能打開或解壓縮。"));
	pm_ui_note(u, msg);
	pm_ui_group(u, NULL);
	u->focus = u->id + 1;   /* 一開就能直接打密碼（下一個元件就是輸入框） */
	if (pm_ui_entry(u, _("密碼"), pw_buf, sizeof pw_buf, 1)) pw_done(1);
	pm_ui_end(u);
	pm_ui_space(u, 6);
	const char *b[] = { _("取消"), _("確定") };
	int r = pm_ui_buttons(u, b, 2, 1);
	if (r == 0) pw_done(0);
	if (r == 1) pw_done(1);
}

int gui_password(int argc, char **argv)
{
	snprintf(pw_file, sizeof pw_file, "%s", argc > 2 ? argv[2] : "");
	for (int i = 3; i < argc; i++) { if (!strcmp(argv[i], "--wrong")) pw_wrong = 1; else if (!strcmp(argv[i], "--parent") && i + 1 < argc) pw_parent = strtoul(argv[++i], NULL, 0); }
	pm_ui_native = 1;
	pm_ui_run(_("需要密碼"), "pomelo-archive", 460, 230, pw_build, NULL, 0, pa_icon);
	return 1;
}

/* ======================= 解壓縮 ======================= */

enum { ST_WORK, ST_PW, ST_CONFLICT, ST_BOMB, ST_DONE, ST_ERR };

typedef struct {
	char *files[64];
	int nfiles, cur;
	char dest[PATH_MAX];
	int here;
	char *only[256];
	int nonly;
	char pw[256];
	int state;
	/* 執行緒與進度 */
	pthread_t th;
	pthread_mutex_t mu;
	int running, joined;
	volatile int cancel;
	long long done, total;
	char curname[256];
	char phase[64];
	/* 結果 */
	int rc;
	char err[512], conflicts[1024];
	int nconflicts, ow, force;
	char outdir[PATH_MAX], first[PATH_MAX];
	int tot_files, skipped_unsafe, skipped_special;
	long long tot_bytes;
	int all_ok;
	char pwbuf[256];
	int pw_tries;
	char charset[16];
} Ex;

static Ex ex;

static int ex_progress(void *u, long long done, long long total, const char *cur)
{
	(void)u;
	pthread_mutex_lock(&ex.mu);
	ex.done = done;
	ex.total = total;
	if (cur) snprintf(ex.curname, sizeof ex.curname, "%s", cur);
	pthread_mutex_unlock(&ex.mu);
	return ex.cancel;
}

static void *ex_worker(void *arg)
{
	(void)arg;
	const char *file = ex.files[ex.cur];
	char err[512] = "";
	AcList l;
	pthread_mutex_lock(&ex.mu);
	snprintf(ex.phase, sizeof ex.phase, "%s", _("正在讀取"));
	ex.done = ex.total = 0;
	ex.curname[0] = 0;
	pthread_mutex_unlock(&ex.mu);
	int rc = ac_open(&l, file, ex.pw, ex.charset[0] ? ex.charset : NULL, err, sizeof err);
	if (rc == AC_OK && l.enc_data && l.backend != AB_GPG) {
		int pr = ac_check_password(&l, ex.pw);
		if (pr != AC_OK) { rc = pr; if (pr == AC_NO_BACKEND) snprintf(err, sizeof err, "%s", l.note); }
	}
	AcExtract o;
	memset(&o, 0, sizeof o);
	if (rc == AC_OK) {
		char dest[PATH_MAX];
		if (ex.here) dir_of(file, dest, sizeof dest); else snprintf(dest, sizeof dest, "%s", ex.dest);
		o.dest = dest;
		o.password = ex.pw;
		o.only = ex.nonly ? (const char *const *)ex.only : NULL;
		o.nonly = ex.nonly;
		o.overwrite = ex.ow;
		o.into_folder = ex.here;
		o.force = ex.force;
		o.progress = ex_progress;
		pthread_mutex_lock(&ex.mu);
		snprintf(ex.phase, sizeof ex.phase, "%s", _("正在解壓縮"));
		pthread_mutex_unlock(&ex.mu);
		rc = ac_extract(&l, &o);
		snprintf(err, sizeof err, "%s", o.err);
	}
	pthread_mutex_lock(&ex.mu);
	ex.rc = rc;
	snprintf(ex.err, sizeof ex.err, "%s", err);
	snprintf(ex.conflicts, sizeof ex.conflicts, "%s", o.conflicts);
	ex.nconflicts = o.nconflicts;
	if (rc == AC_OK) {
		snprintf(ex.outdir, sizeof ex.outdir, "%s", o.outdir);
		snprintf(ex.first, sizeof ex.first, "%s", o.first);
		ex.tot_files += o.nfiles;
		ex.tot_bytes += o.nbytes;
		ex.skipped_unsafe += o.skipped_unsafe;
		ex.skipped_special += o.skipped_special;
	}
	ex.running = 0;
	pthread_mutex_unlock(&ex.mu);
	if (l.file) ac_close(&l);
	return NULL;
}

static void ex_start(void)
{
	pthread_mutex_lock(&ex.mu);
	ex.running = 1;
	ex.joined = 0;
	ex.cancel = 0;
	ex.state = ST_WORK;
	pthread_mutex_unlock(&ex.mu);
	pthread_create(&ex.th, NULL, ex_worker, NULL);
}

static void ex_finish(void)
{
	/* 一份做完：換下一份，或結束 */
	ex.cur++;
	ex.ow = AC_OW_ASK;
	ex.force = 0;
	if (ex.cur < ex.nfiles) { ex.pw[0] = 0; ex.pw_tries = 0; ex_start(); }
	else ex.state = ST_DONE;
}

static void ex_tick(PmUI *u)
{
	pthread_mutex_lock(&ex.mu);
	int running = ex.running;
	pthread_mutex_unlock(&ex.mu);
	if (ex.state == ST_WORK && !running) {
		pthread_join(ex.th, NULL);
		ex.joined = 1;
		switch (ex.rc) {
		case AC_OK: ex_finish(); break;
		case AC_NEED_PASSWORD: case AC_BAD_PASSWORD:
			ex.state = ST_PW;
			ex.pw_tries = ex.rc == AC_BAD_PASSWORD ? ex.pw_tries + 1 : 0;
			ex.pwbuf[0] = 0;
			break;
		case AC_CONFLICT: ex.state = ST_CONFLICT; break;
		case AC_BOMB: ex.state = ST_BOMB; break;
		case AC_CANCEL: exit(5);
		default: ex.state = ST_ERR; break;
		}
	}
	pm_ui_redraw(u);
}

static const char *rc_text(int rc)
{
	switch (rc) {
	case AC_NO_BACKEND: return _("缺少需要的外部程式。");
	case AC_UNSAFE: return _("這個壓縮檔的內容不安全，已經擋下。");
	case AC_NOSPACE: return _("磁碟空間不夠。");
	default: return _("沒辦法完成。");
	}
}

static void ex_build(PmUI *u)
{
	pa_center(u, 0);
	char title[300], sub[200] = "";
	const char *file = ex.cur < ex.nfiles ? ex.files[ex.cur] : ex.files[ex.nfiles - 1];
	snprintf(title, sizeof title, "%s", base_of(file));
	if (ex.nfiles > 1) snprintf(sub, sizeof sub, _("第 %d／%d 個"), (ex.cur < ex.nfiles ? ex.cur : ex.nfiles - 1) + 1, ex.nfiles);
	pm_ui_title(u, title, sub[0] ? sub : NULL);
	switch (ex.state) {
	case ST_WORK: {
		pthread_mutex_lock(&ex.mu);
		long long done = ex.done, total = ex.total;
		char phase[64], cur[256];
		snprintf(phase, sizeof phase, "%s", ex.phase);
		snprintf(cur, sizeof cur, "%s", ex.curname);
		pthread_mutex_unlock(&ex.mu);
		pm_ui_group(u, NULL);
		char val[64];
		double frac = total > 0 ? (double)done / (double)total : 0;
		if (frac > 1) frac = 1;
		if (total > 0) { char a[24], b[24]; ac_human(a, sizeof a, (double)done); ac_human(b, sizeof b, (double)total); snprintf(val, sizeof val, "%s／%s", a, b); }
		else snprintf(val, sizeof val, "%s", "…");
		pm_ui_meter(u, phase, val, frac, PROG_COLOR);
		pm_ui_end(u);
		if (cur[0]) pm_ui_note(u, cur);
		pm_ui_space(u, 6);
		const char *b[] = { _("取消") };
		if (pm_ui_buttons(u, b, 1, 0) == 0) { ex.cancel = 1; }
		break;
	}
	case ST_PW: {
		pm_ui_note(u, ex.pw_tries ? _("密碼不對，請再試一次。") : _("這個壓縮檔有加密，請輸入密碼。"));
		pm_ui_group(u, NULL);
		u->focus = u->id + 1;
		int enter = pm_ui_entry(u, _("密碼"), ex.pwbuf, sizeof ex.pwbuf, 1);
		pm_ui_end(u);
		pm_ui_space(u, 6);
		const char *b[] = { _("取消"), _("確定") };
		int r = pm_ui_buttons(u, b, 2, 1);
		if (r == 0) exit(2);
		if ((r == 1 || enter) && ex.pwbuf[0]) { snprintf(ex.pw, sizeof ex.pw, "%s", ex.pwbuf); ac_wipe(ex.pwbuf, sizeof ex.pwbuf); ex_start(); }
		break;
	}
	case ST_CONFLICT: {
		char msg[1400];
		snprintf(msg, sizeof msg, _("目的地已經有同名的項目：%s%s"), ex.conflicts, ex.nconflicts > 8 ? _(" 等") : "");
		pm_ui_note(u, msg);
		pm_ui_note(u, _("要怎麼處理？"));
		pm_ui_space(u, 4);
		const char *b[] = { _("取消"), _("兩個都留著"), _("略過這些"), _("取代") };
		int r = pm_ui_buttons(u, b, 4, 1);
		if (r == 0) exit(5);
		if (r > 0) { ex.ow = r == 1 ? AC_OW_RENAME : r == 2 ? AC_OW_NO : AC_OW_YES; ex_start(); }
		break;
	}
	case ST_BOMB: {
		pm_ui_note(u, ex.err);
		pm_ui_note(u, _("要繼續解壓縮嗎？"));
		pm_ui_space(u, 4);
		const char *b[] = { _("取消"), _("還是要解壓縮") };
		int r = pm_ui_buttons(u, b, 2, 0);
		if (r == 0) exit(5);
		if (r == 1) { ex.force = 1; ex_start(); }
		break;
	}
	case ST_ERR: {
		pm_ui_note(u, ex.err[0] ? ex.err : rc_text(ex.rc));
		pm_ui_space(u, 6);
		const char *b[] = { _("關閉") };
		if (pm_ui_buttons(u, b, 1, 0) == 0) exit(1);
		break;
	}
	case ST_DONE: {
		char msg[600], sz[24];
		ac_human(sz, sizeof sz, (double)ex.tot_bytes);
		snprintf(msg, sizeof msg, _("完成：解開 %d 個檔案，共 %s。"), ex.tot_files, sz);
		pm_ui_note(u, msg);
		if (ex.skipped_unsafe || ex.skipped_special) {
			snprintf(msg, sizeof msg, _("為了安全，略過了 %d 個項目（指到外面的連結、不安全的路徑、裝置檔等）。"), ex.skipped_unsafe + ex.skipped_special);
			pm_ui_note(u, msg);
		}
		pm_ui_space(u, 6);
		const char *b[] = { _("關閉"), _("打開資料夾") };
		int r = pm_ui_buttons(u, b, 2, 1);
		if (r == 1) { pm_open(ex.outdir); exit(0); }
		if (r == 0) exit(0);
		break;
	}
	}
}

int gui_extract(int argc, char **argv)
{
	memset(&ex, 0, sizeof ex);
	pthread_mutex_init(&ex.mu, NULL);
	int pwstdin = 0;
	const char *mode = argv[1];
	ex.here = !strcmp(mode, "--extract-here");
	for (int i = 2; i < argc; i++) {
		if (!strcmp(argv[i], "--password-stdin")) pwstdin = 1;
		else if (!strcmp(argv[i], "--only") && i + 1 < argc && ex.nonly < 256) ex.only[ex.nonly++] = argv[++i];
		else if (!strcmp(argv[i], "--charset") && i + 1 < argc) snprintf(ex.charset, sizeof ex.charset, "%s", argv[++i]);
		else if (!ex.here && ex.nfiles == 1 && !ex.dest[0]) snprintf(ex.dest, sizeof ex.dest, "%s", argv[i]);
		else if (ex.nfiles < 64) ex.files[ex.nfiles++] = argv[i];
	}
	if (!ex.nfiles || (!ex.here && !ex.dest[0])) { fprintf(stderr, "用法：pomelo-archive --extract-here 檔案…  或  --extract-to 檔案 資料夾 [--only 路徑]…\n"); return 1; }
	if (pwstdin) read_pw_stdin(ex.pw, sizeof ex.pw);
	pm_ui_native = 1;
	ex_start();
	pm_ui_run(_("解壓縮"), "pomelo-archive", 520, 270, ex_build, ex_tick, 100, pa_icon);
	ex.cancel = 1;
	if (ex.running) pthread_join(ex.th, NULL);
	return 5;
}

/* ======================= 建立壓縮檔 ======================= */

enum { CS_FORM, CS_SCAN_WAIT, CS_WORK, CS_OVERWRITE, CS_DONE, CS_ERR };

typedef struct {
	char *inputs[512];
	int ninputs;
	char name[256], dir[PATH_MAX];
	const AcFormat *wf[24];
	const char *labels[24];
	int nwf, fmt, lvl;
	int enc, encnames;
	char pw1[128], pw2[128];
	int state;
	const char *msg;
	char errmsg[600];
	/* 掃描與執行緒 */
	pthread_t th, scan_th;
	pthread_mutex_t mu;
	int running, scanning, scan_started, have_scan;
	volatile int cancel;
	long long done, total, scan_bytes;
	long scan_files;
	char curname[256];
	int rc;
	AcCreate c;
	char out[PATH_MAX];
	int single_file;
	long long outsize;
	int nskipped; char skipped_first[PATH_MAX];
} Cs;

static Cs cs;

static int cs_progress(void *u, long long done, long long total, const char *cur)
{
	(void)u;
	pthread_mutex_lock(&cs.mu);
	cs.done = done;
	if (total > 0) cs.total = total;
	if (cur) snprintf(cs.curname, sizeof cs.curname, "%s", cur);
	pthread_mutex_unlock(&cs.mu);
	return cs.cancel;
}

static int scan_cancel(void *u) { (void)u; return cs.cancel; }

static void *scan_worker(void *arg)
{
	(void)arg;
	long long b = 0;
	long f = 0;
	ac_scan((const char *const *)cs.inputs, cs.ninputs, &b, &f, scan_cancel, NULL);
	pthread_mutex_lock(&cs.mu);
	cs.scan_bytes = b;
	cs.scan_files = f;
	cs.have_scan = 1;
	cs.scanning = 0;
	pthread_mutex_unlock(&cs.mu);
	return NULL;
}

static void cs_choose_formats(void)
{
	int n;
	const AcFormat *all = ac_write_formats(&n);
	cs.nwf = 0;
	for (int i = 0; i < n; i++) {
		if (!all[i].can_write) continue;
		int single = !strcmp(all[i].id, "gz") || !strcmp(all[i].id, "bz2") || !strcmp(all[i].id, "xz") || !strcmp(all[i].id, "zst") || !strcmp(all[i].id, "lz");
		if (single && !cs.single_file) continue;
		if (!strcmp(all[i].id, "zip-zipcrypto") && !cs.enc) continue;   /* 舊式加密只在要加密時才列出來 */
		cs.wf[cs.nwf] = &all[i];
		cs.labels[cs.nwf] = all[i].label;
		cs.nwf++;
	}
}

static const AcFormat *cs_fmt(void) { return cs.fmt >= 0 && cs.fmt < cs.nwf ? cs.wf[cs.fmt] : NULL; }

static void cs_outpath(char *out, size_t n)
{
	const AcFormat *f = cs_fmt();
	char nm[256];
	snprintf(nm, sizeof nm, "%s", cs.name);
	if (f) {
		size_t l = strlen(nm), e = strlen(f->ext);
		if (l > e && !strcasecmp(nm + l - e, f->ext)) nm[l - e] = 0;
	}
	snprintf(out, n, "%s/%s%s", cs.dir, nm, f ? f->ext : "");
}

static void *cs_worker(void *arg)
{
	(void)arg;
	const AcFormat *f = cs_fmt();
	char out[PATH_MAX];
	cs_outpath(out, sizeof out);
	cs.c = (AcCreate){ .out = out, .format = f->id, .level = cs.lvl == 0 ? 1 : cs.lvl == 1 ? 5 : 9, .password = cs.enc && cs.pw1[0] ? cs.pw1 : NULL,
	                   .encrypt_names = cs.encnames, .inputs = (const char *const *)cs.inputs, .ninputs = cs.ninputs, .progress = cs_progress };
	int rc = ac_create(&cs.c);
	pthread_mutex_lock(&cs.mu);
	cs.rc = rc;
	snprintf(cs.errmsg, sizeof cs.errmsg, "%s", cs.c.err);
	snprintf(cs.out, sizeof cs.out, "%s", cs.c.outpath);
	cs.nskipped = cs.c.nskipped;
	snprintf(cs.skipped_first, sizeof cs.skipped_first, "%s", cs.c.skipped_first);
	struct stat st;
	cs.outsize = rc == AC_OK && !stat(cs.c.outpath, &st) ? st.st_size : 0;
	cs.running = 0;
	pthread_mutex_unlock(&cs.mu);
	return NULL;
}

static void cs_start(void)
{
	pthread_mutex_lock(&cs.mu);
	cs.running = 1;
	cs.cancel = 0;
	cs.state = CS_WORK;
	cs.done = 0;
	cs.total = cs.scan_bytes;
	cs.curname[0] = 0;
	pthread_mutex_unlock(&cs.mu);
	pthread_create(&cs.th, NULL, cs_worker, NULL);
}

static void cs_tick(PmUI *u)
{
	pthread_mutex_lock(&cs.mu);
	int running = cs.running;
	pthread_mutex_unlock(&cs.mu);
	if (cs.state == CS_WORK && !running) {
		pthread_join(cs.th, NULL);
		if (cs.rc == AC_OK) cs.state = CS_DONE;
		else if (cs.rc == AC_CANCEL) exit(5);
		else cs.state = CS_ERR;
	}
	pm_ui_redraw(u);
}

static void cs_pick_location(void)
{
	const AcFormat *f = cs_fmt();
	char suggested[300], out[PATH_MAX];
	snprintf(suggested, sizeof suggested, "%s%s", cs.name, f ? f->ext : "");
	PmFileType types[] = { { f ? f->label : "ZIP", f ? f->ext + 1 : "zip" } };
	int t = 0;
	if (pm_file_dialog(_("選擇存放位置"), "pomelo-archive", pa_icon, 1, cs.dir, suggested, types, 1, &t, out, sizeof out)) {
		dir_of(out, cs.dir, sizeof cs.dir);
		char nm[256];
		snprintf(nm, sizeof nm, "%s", base_of(out));
		if (f) { size_t l = strlen(nm), e = strlen(f->ext); if (l > e && !strcasecmp(nm + l - e, f->ext)) nm[l - e] = 0; }
		snprintf(cs.name, sizeof cs.name, "%s", nm);
	}
}

static void cs_try_create(void)
{
	const AcFormat *f = cs_fmt();
	cs.msg = NULL;
	if (!f) { cs.msg = _("沒有可用的格式。"); return; }
	if (!cs.name[0]) { cs.msg = _("請輸入檔名。"); return; }
	if (strchr(cs.name, '/')) { cs.msg = _("檔名裡不能有「/」。"); return; }
	if (cs.enc) {
		if (!cs.pw1[0]) { cs.msg = _("請輸入密碼，或把「加密」關掉。"); return; }
		if (strcmp(cs.pw1, cs.pw2)) { cs.msg = _("兩次輸入的密碼不一樣。"); return; }
		if (!f->can_encrypt) { cs.msg = _("這個格式不能加密。"); return; }
	}
	struct stat st;
	char out[PATH_MAX];
	cs_outpath(out, sizeof out);
	if (!stat(cs.dir, &st) && !S_ISDIR(st.st_mode)) { cs.msg = _("存放位置不是資料夾。"); return; }
	if (access(cs.dir, W_OK)) { cs.msg = _("存放位置不能寫入。"); return; }
	if (!lstat(out, &st)) { cs.state = CS_OVERWRITE; return; }
	pm_conf_set_int("archive.level", cs.lvl);
	pm_conf_set("archive.format", f->id);
	cs_start();
}

static void cs_build(PmUI *u)
{
	pa_center(u, 0);
	char sub[160] = "";
	pthread_mutex_lock(&cs.mu);
	int have = cs.have_scan;
	long long sb = cs.scan_bytes;
	long sf = cs.scan_files;
	pthread_mutex_unlock(&cs.mu);
	if (have) { char sz[24]; ac_human(sz, sizeof sz, (double)sb); snprintf(sub, sizeof sub, _("%d 個項目、%ld 個檔案，約 %s"), cs.ninputs, sf, sz); }
	else snprintf(sub, sizeof sub, _("%d 個項目（計算大小中…）"), cs.ninputs);
	pm_ui_title(u, _("建立壓縮檔"), sub);
	switch (cs.state) {
	case CS_FORM: {
		pm_ui_group(u, NULL);
		pm_ui_entry(u, _("檔名"), cs.name, sizeof cs.name, 0);
		char out[PATH_MAX];
		cs_outpath(out, sizeof out);
		if (pm_ui_button(u, cs.dir, _("更改位置…"))) cs_pick_location();
		pm_ui_end(u);
		pm_ui_group(u, NULL);
		int old = cs.fmt;
		pm_ui_choice(u, _("格式"), cs.labels, cs.nwf, &cs.fmt);
		const AcFormat *f = cs_fmt();
		if (old != cs.fmt && f && cs.enc && !f->can_encrypt) cs.enc = 0;
		static const char *lv[3];
		lv[0] = _("速度優先"); lv[1] = _("標準"); lv[2] = _("體積優先");
		pm_ui_segment(u, _("壓縮程度"), lv, 3, &cs.lvl);
		pm_ui_end(u);
		if (f && f->warn) pm_ui_note(u, f->warn);
		if (f && !strcmp(f->id, "zip")) pm_ui_note(u, _("ZIP 最通用：Windows、手機和大多數系統都能直接開。"));
		else if (f && !strcmp(f->id, "7z")) pm_ui_note(u, _("7z 壓縮率高，對方需要 7-Zip 之類的軟體才能開。"));
		else if (f && !strncmp(f->id, "tar", 3)) pm_ui_note(u, _("tar 系列會保留 Linux 的權限與符號連結，適合備份與在 Linux 之間傳送。"));
		pm_ui_group(u, NULL);
		if (f && f->can_encrypt) {
			int was = cs.enc;
			pm_ui_switch(u, _("用密碼加密"), &cs.enc);
			if (cs.enc != was) cs_choose_formats();
			if (cs.enc) {
				pm_ui_entry(u, _("密碼"), cs.pw1, sizeof cs.pw1, 1);
				pm_ui_entry(u, _("再輸入一次"), cs.pw2, sizeof cs.pw2, 1);
				if (f->enc_names) pm_ui_switch(u, _("連檔名一起加密"), &cs.encnames);
			}
		} else {
			pm_ui_info(u, _("加密"), _("這個格式不支援"));
		}
		pm_ui_end(u);
		if (cs.enc && f && !strcmp(f->id, "zip")) pm_ui_note(u, _("使用 AES-256 加密；檔名還是看得到，內容需要密碼。"));
		else if (cs.enc && f && !strcmp(f->id, "7z")) pm_ui_note(u, cs.encnames ? _("AES-256 加密，連檔名都看不到。") : _("AES-256 加密；檔名還是看得到。打開「連檔名一起加密」可以連檔名一起藏起來。"));
		else if (cs.enc && f && f->can_encrypt) pm_ui_note(u, _("整包用 OpenPGP（AES-256）加密，會存成 .gpg；用 gpg 或這個程式都能解開。"));
		else if (f && !f->can_encrypt && f->why_not) pm_ui_note(u, f->why_not);
		if (cs.msg) pm_ui_note(u, cs.msg);
		pm_ui_space(u, 6);
		const char *b[] = { _("取消"), _("建立") };
		int r = pm_ui_buttons(u, b, 2, 1);
		if (r == 0) exit(1);
		if (r == 1) cs_try_create();
		break;
	}
	case CS_OVERWRITE: {
		char out[PATH_MAX], msg[PATH_MAX + 80];
		cs_outpath(out, sizeof out);
		snprintf(msg, sizeof msg, _("「%s」已經存在，要取代它嗎？"), base_of(out));
		pm_ui_note(u, msg);
		pm_ui_space(u, 6);
		const char *b[] = { _("返回"), _("取代") };
		int r = pm_ui_buttons(u, b, 2, 0);
		if (r == 0) cs.state = CS_FORM;
		if (r == 1) { const AcFormat *f = cs_fmt(); pm_conf_set_int("archive.level", cs.lvl); if (f) pm_conf_set("archive.format", f->id); cs_start(); }
		break;
	}
	case CS_WORK: {
		pthread_mutex_lock(&cs.mu);
		long long done = cs.done, total = cs.total;
		char cur[256];
		snprintf(cur, sizeof cur, "%s", cs.curname);
		pthread_mutex_unlock(&cs.mu);
		pm_ui_group(u, NULL);
		char val[64];
		double frac = total > 0 ? (double)done / (double)total : 0;
		if (frac > 1) frac = 1;
		if (total > 0) { char a[24], b[24]; ac_human(a, sizeof a, (double)done); ac_human(b, sizeof b, (double)total); snprintf(val, sizeof val, "%s／%s", a, b); } else snprintf(val, sizeof val, "…");
		pm_ui_meter(u, _("正在壓縮"), val, frac, PROG_COLOR);
		pm_ui_end(u);
		if (cur[0]) pm_ui_note(u, cur);
		pm_ui_space(u, 6);
		const char *b[] = { _("取消") };
		if (pm_ui_buttons(u, b, 1, 0) == 0) cs.cancel = 1;
		break;
	}
	case CS_DONE: {
		char msg[PATH_MAX + 200], sz[24], in[24];
		ac_human(sz, sizeof sz, (double)cs.outsize);
		ac_human(in, sizeof in, (double)cs.scan_bytes);
		snprintf(msg, sizeof msg, _("已建立「%s」：%s（原本 %s）。"), base_of(cs.out), sz, in);
		pm_ui_note(u, msg);
		if (cs.nskipped) {
			snprintf(msg, sizeof msg, _("有 %d 個項目讀不了而略過（例如 %s）。"), cs.nskipped, cs.skipped_first);
			pm_ui_note(u, msg);
		}
		pm_ui_space(u, 6);
		const char *b[] = { _("關閉"), _("打開所在的資料夾") };
		int r = pm_ui_buttons(u, b, 2, 1);
		if (r == 1) { pm_open(cs.dir); exit(0); }
		if (r == 0) exit(0);
		break;
	}
	case CS_ERR: {
		pm_ui_note(u, cs.errmsg[0] ? cs.errmsg : rc_text(cs.rc));
		pm_ui_space(u, 6);
		const char *b[] = { _("關閉"), _("返回") };
		int r = pm_ui_buttons(u, b, 2, 1);
		if (r == 0) exit(1);
		if (r == 1) cs.state = CS_FORM;
		break;
	}
	}
}

int gui_compress(int argc, char **argv)
{
	memset(&cs, 0, sizeof cs);
	pthread_mutex_init(&cs.mu, NULL);
	for (int i = 2; i < argc && cs.ninputs < 512; i++) {
		if (!strncmp(argv[i], "--", 2)) continue;
		cs.inputs[cs.ninputs++] = argv[i];
	}
	if (!cs.ninputs) { fprintf(stderr, "用法：pomelo-archive --compress 檔案或資料夾…\n"); return 1; }
	/* 預設名稱與位置：單一項目用它的名字，多個用「封存檔」；放在第一個項目旁邊 */
	char first[PATH_MAX];
	if (!realpath(cs.inputs[0], first)) snprintf(first, sizeof first, "%s", cs.inputs[0]);
	dir_of(first, cs.dir, sizeof cs.dir);
	if (cs.ninputs == 1) { ac_base_noext(base_of(first), cs.name, sizeof cs.name); struct stat st; if (!stat(first, &st) && S_ISDIR(st.st_mode)) snprintf(cs.name, sizeof cs.name, "%s", base_of(first)); }
	else snprintf(cs.name, sizeof cs.name, "%s", _("封存檔"));
	if (!cs.name[0]) snprintf(cs.name, sizeof cs.name, "%s", _("封存檔"));
	struct stat st;
	cs.single_file = cs.ninputs == 1 && !lstat(cs.inputs[0], &st) && S_ISREG(st.st_mode);
	cs_choose_formats();
	const char *last = pm_conf_get("archive.format", "zip");
	cs.fmt = 0;
	for (int i = 0; i < cs.nwf; i++) if (!strcmp(cs.wf[i]->id, last)) cs.fmt = i;
	cs.lvl = pm_conf_int("archive.level", 1);
	if (cs.lvl < 0 || cs.lvl > 2) cs.lvl = 1;
	cs.scanning = 1;
	pthread_create(&cs.scan_th, NULL, scan_worker, NULL);
	pm_ui_native = 1;
	pm_ui_run(_("建立壓縮檔"), "pomelo-archive", 560, 600, cs_build, cs_tick, 100, pa_icon);
	cs.cancel = 1;
	pthread_join(cs.scan_th, NULL);
	if (cs.running) pthread_join(cs.th, NULL);
	return 1;
}

/* ======================= 加入檔案（有進度） ======================= */

static struct {
	char *archive;
	char *files[512];
	int nfiles, state, rc, running, need_pw;
	char pw[256], pwbuf[256], err[512];
	pthread_t th;
	pthread_mutex_t mu;
	volatile int cancel;
	long long done, total;
	char cur[256];
} ad;

static int ad_progress(void *u, long long d, long long t, const char *c)
{
	(void)u;
	pthread_mutex_lock(&ad.mu);
	ad.done = d; ad.total = t;
	if (c) snprintf(ad.cur, sizeof ad.cur, "%s", c);
	pthread_mutex_unlock(&ad.mu);
	return ad.cancel;
}

static void *ad_worker(void *arg)
{
	(void)arg;
	char err[512] = "";
	int rc = ac_add(ad.archive, ad.pw[0] ? ad.pw : NULL, (const char *const *)ad.files, ad.nfiles, ad_progress, NULL, err, sizeof err);
	pthread_mutex_lock(&ad.mu);
	ad.rc = rc;
	snprintf(ad.err, sizeof ad.err, "%s", err);
	ad.running = 0;
	pthread_mutex_unlock(&ad.mu);
	return NULL;
}

static void ad_start(void)
{
	ad.running = 1; ad.cancel = 0; ad.state = 0;
	pthread_create(&ad.th, NULL, ad_worker, NULL);
}

static void ad_tick(PmUI *u)
{
	pthread_mutex_lock(&ad.mu);
	int running = ad.running;
	pthread_mutex_unlock(&ad.mu);
	if (ad.state == 0 && !running) {
		pthread_join(ad.th, NULL);
		if (ad.rc == AC_OK) exit(0);
		if (ad.rc == AC_CANCEL) exit(5);
		ad.state = (ad.rc == AC_NEED_PASSWORD || ad.rc == AC_BAD_PASSWORD) ? 1 : 2;
	}
	pm_ui_redraw(u);
}

static void ad_build(PmUI *u)
{
	pa_center(u, 0);
	pm_ui_title(u, _("加入檔案"), base_of(ad.archive));
	if (ad.state == 0) {
		pthread_mutex_lock(&ad.mu);
		long long d = ad.done, t = ad.total;
		char cur[256];
		snprintf(cur, sizeof cur, "%s", ad.cur);
		pthread_mutex_unlock(&ad.mu);
		pm_ui_group(u, NULL);
		char val[40];
		snprintf(val, sizeof val, "%s", t > 0 ? "" : "…");
		pm_ui_meter(u, _("正在加入"), val, t > 0 ? (double)d / (double)t : 0, PROG_COLOR);
		pm_ui_end(u);
		if (cur[0]) pm_ui_note(u, cur);
		pm_ui_space(u, 6);
		const char *b[] = { _("取消") };
		if (pm_ui_buttons(u, b, 1, 0) == 0) ad.cancel = 1;
	} else if (ad.state == 1) {
		pm_ui_note(u, _("這個壓縮檔有加密，要有密碼才能加入檔案（原本的內容要重新寫入）。"));
		pm_ui_group(u, NULL);
		u->focus = u->id + 1;
		int enter = pm_ui_entry(u, _("密碼"), ad.pwbuf, sizeof ad.pwbuf, 1);
		pm_ui_end(u);
		pm_ui_space(u, 6);
		const char *b[] = { _("取消"), _("確定") };
		int r = pm_ui_buttons(u, b, 2, 1);
		if (r == 0) exit(2);
		if ((r == 1 || enter) && ad.pwbuf[0]) { snprintf(ad.pw, sizeof ad.pw, "%s", ad.pwbuf); ac_wipe(ad.pwbuf, sizeof ad.pwbuf); ad_start(); }
	} else {
		pm_ui_note(u, ad.err[0] ? ad.err : _("沒辦法加入。"));
		pm_ui_space(u, 6);
		const char *b[] = { _("關閉") };
		if (pm_ui_buttons(u, b, 1, 0) == 0) exit(1);
	}
}

int gui_add(int argc, char **argv)
{
	memset(&ad, 0, sizeof ad);
	pthread_mutex_init(&ad.mu, NULL);
	int pwstdin = 0;
	for (int i = 2; i < argc; i++) {
		if (!strcmp(argv[i], "--password-stdin")) pwstdin = 1;
		else if (!ad.archive) ad.archive = argv[i];
		else if (ad.nfiles < 512) ad.files[ad.nfiles++] = argv[i];
	}
	if (!ad.archive || !ad.nfiles) { fprintf(stderr, "用法：pomelo-archive --add-gui 封存檔 檔案…\n"); return 1; }
	if (pwstdin) read_pw_stdin(ad.pw, sizeof ad.pw);
	pm_ui_native = 1;
	ad_start();
	pm_ui_run(_("加入檔案"), "pomelo-archive", 480, 240, ad_build, ad_tick, 100, pa_icon);
	ad.cancel = 1;
	if (ad.running) pthread_join(ad.th, NULL);
	return 5;
}
