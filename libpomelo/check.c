/* SPDX-License-Identifier: GPL-3.0-or-later */
/* check.c — 電腦健康檢查的共用部分（見 pomelo-check.h）：報告、背景執行、輸出、修正動作、畫面。
 * 各類的檢查在 check-net.c、check-sec.c、check-sys.c、check-perf.c、check-hw.c、check-bat.c。 */
#define _GNU_SOURCE
#include "internal.h"
#include "pomelo-check.h"
#include "check-int.h"
#include <pthread.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <stdarg.h>
#include <sys/time.h>

static const char *cat_ids[PM_CHK_NCAT] = { "network", "security", "updates", "stability", "services", "performance", "hardware", "battery" };
static const char *cat_names[PM_CHK_NCAT] = { N_("網路"), N_("安全性"), N_("系統與軟體更新"), N_("開機與系統穩定"), N_("系統服務"), N_("電腦變慢"),
                                              N_("硬體與驅動"), N_("電池") };
static const char *level_names[] = { N_("沒有問題"), N_("提醒"), N_("注意"), N_("有問題"), N_("不適用") };

const char *pm_check_cat_name(int cat) { return cat >= 0 && cat < PM_CHK_NCAT ? _(cat_names[cat]) : ""; }
const char *pm_check_cat_id(int cat) { return cat >= 0 && cat < PM_CHK_NCAT ? cat_ids[cat] : ""; }
const char *pm_check_level_name(int level) { return level >= 0 && level <= PM_CHK_SKIP ? _(level_names[level]) : ""; }

int pm_check_cat_from_id(const char *id)
{
	for (int i = 0; i < PM_CHK_NCAT; i++) if (id && !strcmp(id, cat_ids[i])) return i;
	return -1;
}

/* ---- 給各類檢查用的小工具（check-int.h） ---- */

PmCheck *chk_add(PmReport *r, const char *id, int level, const char *fmt, ...)
{
	if (r->n == r->cap) { r->cap = r->cap ? r->cap * 2 : 16; r->c = realloc(r->c, (size_t)r->cap * sizeof *r->c); }
	PmCheck *c = &r->c[r->n++];
	memset(c, 0, sizeof *c);
	snprintf(c->id, sizeof c->id, "%s", id);
	c->level = level;
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(c->title, sizeof c->title, fmt, ap);
	va_end(ap);
	return c;
}

void chk_detail(PmCheck *c, const char *fmt, ...)
{
	size_t l = strlen(c->detail);
	if (l && l < sizeof c->detail - 1) c->detail[l++] = '\n';
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(c->detail + l, sizeof c->detail - l, fmt, ap);
	va_end(ap);
}

void chk_advice(PmCheck *c, const char *fmt, ...)
{
	size_t l = strlen(c->advice);
	if (l && l < sizeof c->advice - 1) c->advice[l++] = '\n';
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(c->advice + l, sizeof c->advice - l, fmt, ap);
	va_end(ap);
}

void chk_fix(PmCheck *c, const char *fix, const char *arg, const char *label)
{
	snprintf(c->fix, sizeof c->fix, "%s", fix);
	snprintf(c->fix_arg, sizeof c->fix_arg, "%s", arg ? arg : "");
	snprintf(c->fix_label, sizeof c->fix_label, "%s", label);
}

/* 測試時從假的根目錄讀（POMELO_CHECK_ROOT）；每個執行緒輪流用 8 個緩衝區 */
const char *chk_path(const char *path)
{
	static __thread char buf[8][512];
	static __thread int k;
	const char *root = getenv("POMELO_CHECK_ROOT");
	if (!root || !*root) return path;
	char *b = buf[k++ % 8];
	snprintf(b, sizeof buf[0], "%s%s", root, path);
	return b;
}

int chk_read(const char *path, char *out, size_t n)
{
	out[0] = 0;
	FILE *f = fopen(chk_path(path), "r");
	if (!f) return 0;
	size_t k = fread(out, 1, n - 1, f);
	fclose(f);
	out[k] = 0;
	while (k && (out[k - 1] == '\n' || out[k - 1] == ' ')) out[--k] = 0;
	return 1;
}

long chk_read_long(const char *path, long def)
{
	char b[64];
	if (!chk_read(path, b, sizeof b) || !b[0]) return def;
	return strtol(b, NULL, 10);
}

char *chk_run(const char *cmd) { return pm_run(cmd); }

long chk_now_ms(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return tv.tv_sec * 1000L + tv.tv_usec / 1000;
}

/* 一行一行讀指令輸出：for (char *l = out, *nx; l && *l; l = nx) { nx = chk_line(l); … } */
char *chk_line(char *l)
{
	char *e = strchr(l, '\n');
	if (!e) return l + strlen(l);
	*e = 0;
	return e + 1;
}

/* nmcli -t 的欄位：冒號分隔，值裡的冒號寫成 \: */
int chk_split_colon(char *line, char **f, int max)
{
	int n = 0;
	char *w = line;
	f[n++] = line;
	for (char *p = line; *p; p++) {
		if (*p == '\\' && p[1]) { *w++ = *++p; continue; }
		if (*p == ':' && n < max) { *w++ = 0; f[n++] = w; continue; }
		*w++ = *p;
	}
	*w = 0;
	return n;
}

/* ---- 執行、報告 ---- */

void pm_check_run(int cat, PmReport *r, int flags)
{
	memset(r, 0, sizeof *r);
	r->cat = cat;
	r->admin = (flags & PM_CHK_ADMIN) != 0;
	long t0 = chk_now_ms();
	switch (cat) {
	case PM_CHK_NETWORK: chk_network(r, flags); break;
	case PM_CHK_SECURITY: chk_security(r, flags); break;
	case PM_CHK_UPDATES: chk_updates(r, flags); break;
	case PM_CHK_STABILITY: chk_stability(r, flags); break;
	case PM_CHK_SERVICES: chk_services(r, flags); break;
	case PM_CHK_PERFORMANCE: chk_performance(r, flags); break;
	case PM_CHK_HARDWARE: chk_hardware(r, flags); break;
	case PM_CHK_BATTERY: chk_battery(r, flags); break;
	}
	r->ms = chk_now_ms() - t0;
}

void pm_report_free(PmReport *r)
{
	free(r->c);
	r->c = NULL;
	r->n = r->cap = 0;
}

int pm_report_worst(const PmReport *r)
{
	int w = PM_CHK_OK;
	for (int i = 0; i < r->n; i++) if (r->c[i].level != PM_CHK_SKIP && r->c[i].level > w) w = r->c[i].level;
	return w;
}

int pm_report_count(const PmReport *r, int level)
{
	int n = 0;
	for (int i = 0; i < r->n; i++) n += r->c[i].level == level;
	return n;
}

static void json_str(FILE *f, const char *s)
{
	fputc('"', f);
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;
		if (c == '"' || c == '\\') fprintf(f, "\\%c", c);
		else if (c == '\n') fputs("\\n", f);
		else if (c == '\t') fputs("\\t", f);
		else if (c < 0x20) fprintf(f, "\\u%04x", c);
		else fputc(c, f);
	}
	fputc('"', f);
}

static const char *level_ids[] = { "ok", "info", "warn", "bad", "skip" };

void pm_report_json(const PmReport *r, FILE *f)
{
	fprintf(f, "{\"category\":\"%s\",\"name\":", pm_check_cat_id(r->cat));
	json_str(f, pm_check_cat_name(r->cat));
	fprintf(f, ",\"worst\":\"%s\",\"ms\":%ld,\"admin\":%s,\"items\":[", level_ids[pm_report_worst(r)], r->ms, r->admin ? "true" : "false");
	for (int i = 0; i < r->n; i++) {
		const PmCheck *c = &r->c[i];
		fprintf(f, "%s{\"id\":\"%s\",\"level\":\"%s\",\"title\":", i ? "," : "", c->id, level_ids[c->level]);
		json_str(f, c->title);
		fputs(",\"detail\":", f); json_str(f, c->detail);
		fputs(",\"advice\":", f); json_str(f, c->advice);
		if (c->fix[0]) { fputs(",\"fix\":", f); json_str(f, c->fix_label); }
		fputc('}', f);
	}
	fputs("]}\n", f);
}

void pm_report_text(const PmReport *r, FILE *f)
{
	static const char *mark[] = { "✓", "i", "!", "✗", "-" };
	fprintf(f, "== %s\n", pm_check_cat_name(r->cat));
	for (int lv = PM_CHK_BAD; lv >= PM_CHK_OK; lv--)
		for (int i = 0; i < r->n; i++) {
			const PmCheck *c = &r->c[i];
			if (c->level != lv) continue;
			fprintf(f, "[%s] %s\n", mark[c->level], c->title);
			for (const char *p = c->detail; *p; ) { size_t l = strcspn(p, "\n"); fprintf(f, "    %.*s\n", (int)l, p); p += l; if (*p) p++; }
			for (const char *p = c->advice; *p; ) { size_t l = strcspn(p, "\n"); fprintf(f, "    → %.*s\n", (int)l, p); p += l; if (*p) p++; }
		}
	for (int i = 0; i < r->n; i++) if (r->c[i].level == PM_CHK_SKIP) fprintf(f, "[-] %s\n", r->c[i].title);
}

/* ---- 背景執行 ---- */

struct PmCheckJob {
	pthread_t th;
	int cat, flags;
	volatile int done;
	PmReport rep;
};

static void *job_main(void *arg)
{
	PmCheckJob *j = arg;
	pm_check_run(j->cat, &j->rep, j->flags);
	__atomic_store_n(&j->done, 1, __ATOMIC_RELEASE);
	return NULL;
}

PmCheckJob *pm_check_start(int cat, int flags)
{
	PmCheckJob *j = calloc(1, sizeof *j);
	j->cat = cat;
	j->flags = flags;
	if (pthread_create(&j->th, NULL, job_main, j)) { pm_check_run(cat, &j->rep, flags); j->done = 1; j->th = 0; }
	return j;
}

int pm_check_done(PmCheckJob *j) { return j && __atomic_load_n(&j->done, __ATOMIC_ACQUIRE); }

PmReport *pm_check_take(PmCheckJob *j)
{
	if (!j) return NULL;
	if (j->th) pthread_join(j->th, NULL);
	PmReport *r = malloc(sizeof *r);
	*r = j->rep;
	free(j);
	return r;
}

/* ---- 修正動作 ---- */

int pm_check_fix_argv(const PmCheck *c, char **argv, int max, int *admin, char *msg, size_t n)
{
	*admin = 0;
	msg[0] = 0;
	if (!c->fix[0] || max < 8) return 0;
	return chk_fix_argv(c, argv, max, admin, msg, n);
}

int chk_fix_argv(const PmCheck *c, char **argv, int max, int *admin, char *msg, size_t n)
{
	static const struct { const char *pre; int (*fn)(const PmCheck *, char **, int, int *, char *, size_t); } t[] = {
		{ "net.", chk_net_fix }, { "sec.", chk_sec_fix }, { "upd.", chk_upd_fix }, { "sys.", chk_sys_fix }, { "svc.", chk_svc_fix },
		{ "perf.", chk_perf_fix }, { "hw.", chk_hw_fix }, { "bat.", chk_bat_fix },
	};
	for (size_t i = 0; i < LEN(t); i++) if (!strncmp(c->fix, t[i].pre, strlen(t[i].pre))) return t[i].fn(c, argv, max, admin, msg, n);
	return 0;
}

/* 背景執行：中間多一層子程序等指令做完，把結束碼寫進管道（主程式的 SIGCHLD 常常是忽略的，不能 waitpid） */
int pm_check_fix_exec(const PmCheck *c)
{
	char *argv[32], msg[1024];
	int admin = 0;
	int n = pm_check_fix_argv(c, argv, 30, &admin, msg, sizeof msg);
	if (n <= 0) return -1;
	if (!strcmp(argv[0], "@open")) {
		if (strstr(argv[1], "://")) pm_open(argv[1]);
		else pm_spawn(argv[1]);
		return -2;
	}
	int fd[2];
	if (pipe2(fd, O_CLOEXEC)) return -1;
	pid_t p = fork();
	if (p < 0) { close(fd[0]); close(fd[1]); return -1; }
	if (p == 0) {
		if (dpy) close(ConnectionNumber(dpy));
		close(fd[0]);
		setsid();
		signal(SIGCHLD, SIG_DFL);
		int nul = open("/dev/null", O_RDWR);
		if (nul >= 0) { dup2(nul, 0); dup2(nul, 1); dup2(nul, 2); }
		pid_t q2 = fork();
		if (q2 == 0) {
			char *full[36];
			int k = 0;
			if (admin) full[k++] = "pkexec";
			for (int i = 0; i < n && k < 35; i++) full[k++] = argv[i];
			full[k] = NULL;
			execvp(full[0], full);
			_exit(127);
		}
		int st = 0, code = 127;
		if (q2 > 0 && waitpid(q2, &st, 0) == q2) code = WIFEXITED(st) ? WEXITSTATUS(st) : 128;
		if (write(fd[1], &code, sizeof code) < 0) {}
		_exit(0);
	}
	close(fd[1]);
	return fd[0];
}

int pm_check_fix_poll(int fd)
{
	if (fd < 0) return 0;
	struct pollfd p = { .fd = fd, .events = POLLIN };
	if (poll(&p, 1, 0) <= 0) return -1;
	int code = 127;
	if (read(fd, &code, sizeof code) != sizeof code) code = 127;
	close(fd);
	return code;
}

/* ---- 畫面 ---- */

/* 各類的圖示：圓角方塊漸層＋白色圖案（跟系統設定的圖示同一種樣式） */
static cairo_t *icon_tile(cairo_surface_t **out, unsigned top, unsigned bottom)
{
	*out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 56, 56);
	cairo_t *cr = cairo_create(*out);
	pm_rrect(cr, 2, 2, 52, 52, 12, 12);
	cairo_pattern_t *g = cairo_pattern_create_linear(0, 2, 0, 54);
	cairo_pattern_add_color_stop_rgb(g, 0, (top >> 16 & 255) / 255.0, (top >> 8 & 255) / 255.0, (top & 255) / 255.0);
	cairo_pattern_add_color_stop_rgb(g, 1, (bottom >> 16 & 255) / 255.0, (bottom >> 8 & 255) / 255.0, (bottom & 255) / 255.0);
	cairo_set_source(cr, g);
	cairo_fill(cr);
	cairo_pattern_destroy(g);
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_set_line_width(cr, 3.2);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
	return cr;
}

void pm_check_icons(void)
{
	static int done;
	if (done) return;
	done = 1;
	cairo_surface_t *s;
	cairo_t *cr;
	/* 網路：地球 */
	cr = icon_tile(&s, 0x4aa3ff, 0x0a6fe0);
	cairo_arc(cr, 28, 28, 14, 0, 2 * M_PI); cairo_stroke(cr);
	cairo_save(cr); cairo_translate(cr, 28, 28); cairo_scale(cr, 0.45, 1); cairo_arc(cr, 0, 0, 14, 0, 2 * M_PI); cairo_restore(cr); cairo_stroke(cr);
	cairo_move_to(cr, 14, 28); cairo_line_to(cr, 42, 28); cairo_stroke(cr);
	cairo_destroy(cr); pm_ui_add_icon("@chk-network", s);
	/* 安全性：盾牌＋勾 */
	cr = icon_tile(&s, 0x3ccf7a, 0x1e9e55);
	cairo_move_to(cr, 28, 12); cairo_line_to(cr, 41, 17); cairo_curve_to(cr, 41, 32, 36, 40, 28, 45); cairo_curve_to(cr, 20, 40, 15, 32, 15, 17); cairo_close_path(cr);
	cairo_fill(cr);
	cairo_set_source_rgb(cr, 0.12, 0.62, 0.33);
	cairo_move_to(cr, 22, 28); cairo_line_to(cr, 26.5, 32.5); cairo_line_to(cr, 34, 24); cairo_stroke(cr);
	cairo_destroy(cr); pm_ui_add_icon("@chk-security", s);
	/* 更新：兩個繞圈的箭頭 */
	cr = icon_tile(&s, 0x6b7bff, 0x4148d6);
	cairo_arc(cr, 28, 28, 13, -M_PI * 0.15, M_PI * 0.75); cairo_stroke(cr);
	cairo_arc(cr, 28, 28, 13, M_PI * 0.85, M_PI * 1.75); cairo_stroke(cr);
	cairo_move_to(cr, 39, 18); cairo_line_to(cr, 40.5, 24.5); cairo_line_to(cr, 34, 24); cairo_stroke(cr);
	cairo_move_to(cr, 17, 38); cairo_line_to(cr, 15.5, 31.5); cairo_line_to(cr, 22, 32); cairo_stroke(cr);
	cairo_destroy(cr); pm_ui_add_icon("@chk-updates", s);
	/* 開機與系統穩定：心電圖 */
	cr = icon_tile(&s, 0xff6b81, 0xe03157);
	cairo_move_to(cr, 11, 29); cairo_line_to(cr, 20, 29); cairo_line_to(cr, 24, 19); cairo_line_to(cr, 30, 39); cairo_line_to(cr, 34, 29); cairo_line_to(cr, 45, 29);
	cairo_stroke(cr);
	cairo_destroy(cr); pm_ui_add_icon("@chk-stability", s);
	/* 系統服務：伺服器機櫃 */
	cr = icon_tile(&s, 0x8e9aaf, 0x5c677d);
	for (int i = 0; i < 3; i++) {
		pm_rrect(cr, 15, 13 + i * 11, 26, 8, 2, 2); cairo_fill(cr);
		cairo_set_source_rgb(cr, 0.36, 0.40, 0.49);
		cairo_arc(cr, 35, 17 + i * 11, 1.6, 0, 2 * M_PI); cairo_fill(cr);
		cairo_set_source_rgb(cr, 1, 1, 1);
	}
	cairo_destroy(cr); pm_ui_add_icon("@chk-services", s);
	/* 電腦變慢：速度表 */
	cr = icon_tile(&s, 0xffb340, 0xf07c00);
	cairo_arc(cr, 28, 33, 14, M_PI, 2 * M_PI); cairo_stroke(cr);
	cairo_move_to(cr, 28, 33); cairo_line_to(cr, 36, 24); cairo_stroke(cr);
	cairo_arc(cr, 28, 33, 3, 0, 2 * M_PI); cairo_fill(cr);
	cairo_destroy(cr); pm_ui_add_icon("@chk-performance", s);
	/* 硬體與驅動：晶片 */
	cr = icon_tile(&s, 0x2ec4b6, 0x168a80);
	pm_rrect(cr, 18, 18, 20, 20, 3, 3); cairo_stroke(cr);
	cairo_set_line_width(cr, 2.4);
	for (int i = 0; i < 3; i++) {
		double p = 22 + i * 6;
		cairo_move_to(cr, p, 12); cairo_line_to(cr, p, 17); cairo_move_to(cr, p, 39); cairo_line_to(cr, p, 44);
		cairo_move_to(cr, 12, p); cairo_line_to(cr, 17, p); cairo_move_to(cr, 39, p); cairo_line_to(cr, 44, p);
	}
	cairo_stroke(cr);
	cairo_destroy(cr); pm_ui_add_icon("@chk-hardware", s);
	/* 電池 */
	cr = icon_tile(&s, 0x5fd068, 0x2f9e3b);
	pm_rrect(cr, 12, 20, 29, 16, 3, 3); cairo_stroke(cr);
	cairo_rectangle(cr, 42, 25, 3, 6); cairo_fill(cr);
	cairo_rectangle(cr, 16, 24, 15, 8); cairo_fill(cr);
	cairo_destroy(cr); pm_ui_add_icon("@chk-battery", s);
	/* 磁碟（連到「儲存空間」） */
	cr = icon_tile(&s, 0xb0b5c0, 0x7c8391);
	pm_rrect(cr, 13, 17, 30, 22, 4, 4); cairo_stroke(cr);
	cairo_arc(cr, 36, 33, 2, 0, 2 * M_PI); cairo_fill(cr);
	cairo_move_to(cr, 17, 26); cairo_line_to(cr, 39, 26); cairo_stroke(cr);
	cairo_destroy(cr); pm_ui_add_icon("@chk-disk", s);
	/* 套件清單：立體的箱子 */
	cr = icon_tile(&s, 0xb08968, 0x8a6442);
	cairo_move_to(cr, 15, 20); cairo_line_to(cr, 28, 13); cairo_line_to(cr, 41, 20); cairo_line_to(cr, 41, 36); cairo_line_to(cr, 28, 43); cairo_line_to(cr, 15, 36); cairo_close_path(cr); cairo_stroke(cr);
	cairo_move_to(cr, 15, 20); cairo_line_to(cr, 28, 27); cairo_line_to(cr, 41, 20); cairo_stroke(cr);
	cairo_move_to(cr, 28, 27); cairo_line_to(cr, 28, 43); cairo_stroke(cr);
	cairo_destroy(cr); pm_ui_add_icon("@chk-sbom", s);
	/* 「N 項正常」那一列的綠色勾 */
	s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 56, 56);
	cr = cairo_create(s);
	cairo_arc(cr, 28, 28, 18, 0, 2 * M_PI);
	cairo_set_source_rgb(cr, 0.20, 0.78, 0.35);
	cairo_fill(cr);
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_set_line_width(cr, 4);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_move_to(cr, 20, 28); cairo_line_to(cr, 26, 34); cairo_line_to(cr, 36, 22); cairo_stroke(cr);
	cairo_destroy(cr); pm_ui_add_icon("@chk-ok", s);
}

/* 分區：需要處理、建議注意、提醒；正常與不適用的收在最後一列，點了才展開 */
int pm_ui_report(PmUI *u, const PmReport *r)
{
	pm_check_icons();
	static int expanded[PM_CHK_NCAT];
	static const char *caps[] = { NULL, N_("提醒"), N_("建議注意"), N_("需要處理") };
	int hit = -1, nok = 0;
	for (int lv = PM_CHK_BAD; lv >= PM_CHK_INFO; lv--) {
		int any = 0;
		for (int i = 0; i < r->n; i++) any |= r->c[i].level == lv;
		if (!any) continue;
		pm_ui_group(u, _(caps[lv]));
		for (int i = 0; i < r->n; i++) {
			const PmCheck *c = &r->c[i];
			if (c->level != lv) continue;
			if (pm_ui_status(u, c->level, c->title, c->detail, c->advice, c->fix[0] ? c->fix_label : NULL)) hit = i;
		}
		pm_ui_end(u);
	}
	for (int i = 0; i < r->n; i++) nok += r->c[i].level == PM_CHK_OK || r->c[i].level == PM_CHK_SKIP;
	if (!nok) return hit;
	int cat = r->cat >= 0 && r->cat < PM_CHK_NCAT ? r->cat : 0;
	pm_ui_group(u, _("沒有問題"));
	if (!expanded[cat]) {
		char t[128];
		snprintf(t, sizeof t, _("%d 項正常"), nok);
		if (pm_ui_item(u, "@chk-ok", t, NULL, _("顯示"), 0)) { expanded[cat] = 1; pm_ui_redraw(u); }
		pm_ui_end(u);
		return hit;
	}
	for (int lv = PM_CHK_OK; lv <= PM_CHK_SKIP; lv += PM_CHK_SKIP - PM_CHK_OK)
		for (int i = 0; i < r->n; i++) {
			const PmCheck *c = &r->c[i];
			if (c->level != lv) continue;
			if (pm_ui_status(u, c->level, c->title, c->detail, c->advice, c->fix[0] ? c->fix_label : NULL)) hit = i;
		}
	if (pm_ui_item(u, NULL, _("收起正常的項目"), NULL, NULL, 0)) { expanded[cat] = 0; pm_ui_redraw(u); }
	pm_ui_end(u);
	return hit;
}

int pm_ui_check(PmUI *u, const PmReport *r, PmCheckUI *st)
{
	int again = 0;
	if (st->fd >= 0) {
		int code = pm_check_fix_poll(st->fd);
		if (code >= 0) { st->fd = -1; st->last = code; again = 1; }
	}
	if (st->fd >= 0) {
		char t[160];
		snprintf(t, sizeof t, _("正在執行「%s」…"), st->label);
		pm_ui_group(u, NULL);
		pm_ui_status(u, PM_CHK_INFO, t, st->admin ? _("如果跳出密碼視窗，請輸入你的密碼。") : NULL, NULL, NULL);
		pm_ui_end(u);
	} else if (st->pending >= 0 && st->pending < r->n) {
		/* 按了修正按鈕：先在這裡問 */
		const PmCheck *c = &r->c[st->pending];
		char *argv[32], msg[1024], q[200];
		int admin = 0;
		pm_check_fix_argv(c, argv, 30, &admin, msg, sizeof msg);
		snprintf(q, sizeof q, _("要%s嗎？"), c->fix_label);
		pm_ui_group(u, NULL);
		pm_ui_status(u, PM_CHK_INFO, q, msg, admin ? _("需要輸入管理員密碼。") : NULL, NULL);
		pm_ui_end(u);
		const char *btn[2] = { _("取消"), c->fix_label };
		int b = pm_ui_buttons(u, btn, 2, 1);
		if (b < 0 && u->key == XK_Escape) b = 0;
		pm_ui_space(u, 12);
		if (b == 0) st->pending = -1;
		else if (b == 1) {
			snprintf(st->label, sizeof st->label, "%s", c->fix_label);
			st->admin = admin;
			st->last = -1;
			st->fd = pm_check_fix_exec(c);
			st->pending = -1;
			if (st->fd == -2) st->fd = -1;
			pm_ui_redraw(u);
		}
	} else if (st->last >= 0) {
		pm_ui_group(u, NULL);
		if (st->last == 0) pm_ui_status(u, PM_CHK_OK, _("已完成，下面是重新檢查的結果"), NULL, NULL, NULL);
		else if (st->last == 126 || st->last == 127) pm_ui_status(u, PM_CHK_INFO, _("已取消（沒有取得管理員權限）"), NULL, NULL, NULL);
		else pm_ui_status(u, PM_CHK_WARN, _("沒有成功完成，請看下面的檢查結果"), NULL, NULL, NULL);
		pm_ui_end(u);
	}
	int hit = pm_ui_report(u, r);
	if (hit >= 0 && st->fd < 0) {
		char *argv[32], msg[1024];
		int admin = 0;
		int n = pm_check_fix_argv(&r->c[hit], argv, 30, &admin, msg, sizeof msg);
		if (n > 0 && !strcmp(argv[0], "@open")) pm_check_fix_exec(&r->c[hit]);   /* 只是開程式或網頁：不用問 */
		else if (n > 0) { st->pending = hit; st->last = -1; u->scroll = 0; pm_ui_redraw(u); }
	}
	return again;
}

