/* SPDX-License-Identifier: GPL-3.0-or-later */
/* pomelo-docker — Pomelo 容器（Docker 管理）
 *
 * 安裝 Docker、管理容器（開始／停止／重新啟動／暫停／刪除、從映像建立）、映像（下載、刪除、清理）、資料卷、
 * 進到容器裡（終端機 docker exec）、看紀錄、備份與還原容器、每個容器的 CPU／記憶體／網路／磁碟使用量。
 * 直接對 Docker 的 UNIX socket 講 Engine API（見 api.c），備份還原見 backup.c。
 *
 * 用法：pomelo-docker                         管理視窗
 *       pomelo-docker --list                  列出容器
 *       pomelo-docker --backup 容器 [資料夾] [--no-image]   備份（可以放在排程裡）
 *       pomelo-docker --restore 備份資料夾                    還原
 */
#define _GNU_SOURCE
#include <pomelo.h>
#include <pomelo-widgets.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <poll.h>
#include <stdarg.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>

static cairo_surface_t *icon(int size);
static void job_progress(const char *fmt, ...);

#include "json.c"
#include "api.c"
#include "backup.c"

/* ------------------------------------------------------------------ */
/* 圖示                                                                 */
/* ------------------------------------------------------------------ */

/* 一個貨櫃（側面有直條紋） */
static void draw_box(cairo_t *cr, double x, double y, double w, double h, double r, double g, double b)
{
	pm_rrect(cr, x, y, w, h, 2.5, 2.5);
	cairo_set_source_rgb(cr, r, g, b);
	cairo_fill(cr);
	cairo_set_source_rgba(cr, 0, 0, 0, 0.18);
	cairo_set_line_width(cr, w / 22);
	for (int i = 1; i < 6; i++) {
		double lx = x + w * i / 6;
		cairo_move_to(cr, lx, y + h * 0.18);
		cairo_line_to(cr, lx, y + h * 0.82);
	}
	cairo_stroke(cr);
}

static cairo_surface_t *icon(int size)
{
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
	cairo_t *cr = cairo_create(s);
	cairo_scale(cr, size / 128.0, size / 128.0);
	cairo_pattern_t *g = cairo_pattern_create_linear(0, 0, 0, 128);
	cairo_pattern_add_color_stop_rgb(g, 0, 0.10, 0.72, 0.80);
	cairo_pattern_add_color_stop_rgb(g, 1, 0.03, 0.45, 0.62);
	pm_rrect(cr, 8, 8, 112, 112, 28, 28);
	cairo_set_source(cr, g);
	cairo_fill(cr);
	cairo_pattern_destroy(g);
	/* 三個疊起來的貨櫃 */
	draw_box(cr, 22, 70, 40, 24, 1.0, 0.62, 0.20);
	draw_box(cr, 66, 70, 40, 24, 0.98, 0.98, 1.0);
	draw_box(cr, 44, 42, 40, 24, 0.40, 0.85, 0.55);
	/* 底下的海浪 */
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_set_line_width(cr, 4);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_move_to(cr, 22, 104);
	for (int i = 0; i < 4; i++) cairo_rel_curve_to(cr, 5, -5, 16, -5, 21, 0);
	cairo_stroke(cr);
	cairo_destroy(cr);
	return s;
}

/* 清單用的小圖示：執行中（綠）、暫停（橘）、停止（灰）、映像、資料卷、備份 */
static cairo_surface_t *small_icon(unsigned col, int kind)
{
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 56, 56);
	cairo_t *cr = cairo_create(s);
	pm_rrect(cr, 0, 0, 56, 56, 12, 12);
	pm_rgb(cr, col);
	cairo_fill(cr);
	cairo_set_source_rgb(cr, 1, 1, 1);
	if (kind == 0) draw_box(cr, 10, 17, 36, 22, 1, 1, 1);
	else if (kind == 1) {   /* 映像：三層 */
		for (int i = 0; i < 3; i++) { pm_rrect(cr, 12, 14 + i * 10, 32, 7, 2, 2); cairo_fill(cr); }
	} else if (kind == 2) { /* 資料卷：圓柱 */
		cairo_save(cr);
		cairo_translate(cr, 28, 18);
		cairo_scale(cr, 1, 0.35);
		cairo_arc(cr, 0, 0, 14, 0, 2 * M_PI);
		cairo_restore(cr);
		cairo_fill(cr);
		cairo_rectangle(cr, 14, 18, 28, 20);
		cairo_fill(cr);
		cairo_save(cr);
		cairo_translate(cr, 28, 38);
		cairo_scale(cr, 1, 0.35);
		cairo_arc(cr, 0, 0, 14, 0, 2 * M_PI);
		cairo_restore(cr);
		cairo_fill(cr);
	} else {                /* 備份：箱子＋箭頭 */
		pm_rrect(cr, 12, 24, 32, 20, 3, 3);
		cairo_fill(cr);
		pm_rgb(cr, col);
		cairo_set_line_width(cr, 3);
		cairo_move_to(cr, 28, 28); cairo_line_to(cr, 28, 40); cairo_stroke(cr);
		cairo_move_to(cr, 23, 35); cairo_line_to(cr, 28, 40); cairo_line_to(cr, 33, 35); cairo_stroke(cr);
		cairo_set_source_rgb(cr, 1, 1, 1);
		pm_rrect(cr, 10, 14, 36, 8, 2, 2);
		cairo_fill(cr);
	}
	cairo_destroy(cr);
	return s;
}

/* ------------------------------------------------------------------ */
/* 資料：容器、映像、資料卷                                                */
/* ------------------------------------------------------------------ */

#define MAX_ITEMS 512
typedef struct {
	char id[72], name[160], image[256], imgid[80], state[16], status[128], project[96], ports[200];
	long long created;
} Ctr;
typedef struct { char id[80], tag[256]; long long size, created; int used; } Img;
typedef struct { char name[200], mount[PATH_MAX], used_by[160]; long long size; } Vol;

static Ctr ctrs[MAX_ITEMS];
static int nctrs;
static Img imgs[MAX_ITEMS];
static int nimgs;
static Vol vols[MAX_ITEMS];
static int nvols;
static int dk_state = -1;
static char dk_version[64];
/* 資料卷給誰用（從容器清單的 Mounts 來） */
static struct { char vol[200], ctr[160]; } vuse[MAX_ITEMS];
static int nvuse;

static int ctr_running(const Ctr *c) { return !strcmp(c->state, "running") || !strcmp(c->state, "restarting"); }
static int ctr_paused(const Ctr *c) { return !strcmp(c->state, "paused"); }

static int ctr_cmp(const void *a, const void *b)
{
	const Ctr *x = a, *y = b;
	int rx = ctr_running(x) || ctr_paused(x), ry = ctr_running(y) || ctr_paused(y);
	if (rx != ry) return ry - rx;
	return strcasecmp(x->name, y->name);
}

static void load_containers(void)
{
	Js *j = dk_json("GET", "/containers/json?all=1", NULL, NULL);
	nctrs = 0;
	nvuse = 0;
	for (Js *c = j && j->type == JS_ARR ? j->kids : NULL; c && nctrs < MAX_ITEMS; c = c->next) {
		Ctr *t = &ctrs[nctrs++];
		memset(t, 0, sizeof *t);
		snprintf(t->id, sizeof t->id, "%s", js_str(js_get(c, "Id"), ""));
		Js *names = js_get(c, "Names");
		const char *n = names && names->kids ? js_str(names->kids, "") : "";
		snprintf(t->name, sizeof t->name, "%s", *n == '/' ? n + 1 : n);
		const char *im = js_str(js_get(c, "Image"), "");
		if (!strncmp(im, "sha256:", 7)) snprintf(t->image, sizeof t->image, "%.12s", im + 7);   /* 沒有名稱的映像 */
		else snprintf(t->image, sizeof t->image, "%s", im);
		snprintf(t->imgid, sizeof t->imgid, "%s", js_str(js_get(c, "ImageID"), ""));
		snprintf(t->state, sizeof t->state, "%s", js_str(js_get(c, "State"), ""));
		snprintf(t->status, sizeof t->status, "%s", js_str(js_get(c, "Status"), ""));
		snprintf(t->project, sizeof t->project, "%s", js_str(js_get(js_get(c, "Labels"), "com.docker.compose.project"), ""));
		t->created = (long long)js_num(js_get(c, "Created"), 0);
		/* 連接埠：本機:容器（IPv4、IPv6 各一筆，只留一個） */
		size_t k = 0;
		for (Js *p = js_get(c, "Ports") ? js_get(c, "Ports")->kids : NULL; p; p = p->next) {
			int pub = (int)js_num(js_get(p, "PublicPort"), 0), priv = (int)js_num(js_get(p, "PrivatePort"), 0);
			if (!pub) continue;
			char one[32];
			snprintf(one, sizeof one, "%d→%d", pub, priv);
			if (strstr(t->ports, one)) continue;
			k += (size_t)snprintf(t->ports + k, sizeof t->ports - k, "%s%s", k ? " " : "", one);
			if (k >= sizeof t->ports) break;
		}
		for (Js *m = js_get(c, "Mounts") ? js_get(c, "Mounts")->kids : NULL; m && nvuse < MAX_ITEMS; m = m->next)
			if (!strcmp(js_str(js_get(m, "Type"), ""), "volume")) {
				snprintf(vuse[nvuse].vol, sizeof vuse[0].vol, "%s", js_str(js_get(m, "Name"), ""));
				snprintf(vuse[nvuse].ctr, sizeof vuse[0].ctr, "%s", t->name);
				nvuse++;
			}
	}
	js_free(j);
	qsort(ctrs, (size_t)nctrs, sizeof ctrs[0], ctr_cmp);
}

static int img_cmp(const void *a, const void *b) { return strcasecmp(((const Img *)a)->tag, ((const Img *)b)->tag); }

static void load_images(void)
{
	Js *j = dk_json("GET", "/images/json", NULL, NULL);
	nimgs = 0;
	for (Js *i = j && j->type == JS_ARR ? j->kids : NULL; i && nimgs < MAX_ITEMS; i = i->next) {
		Img *t = &imgs[nimgs++];
		memset(t, 0, sizeof *t);
		snprintf(t->id, sizeof t->id, "%s", js_str(js_get(i, "Id"), ""));
		Js *tags = js_get(i, "RepoTags");
		const char *tag = tags && tags->kids ? js_str(tags->kids, "") : "";
		snprintf(t->tag, sizeof t->tag, "%s", tag[0] && strcmp(tag, "<none>:<none>") ? tag : _("（沒有名稱）"));
		t->size = (long long)js_num(js_get(i, "Size"), 0);
		t->created = (long long)js_num(js_get(i, "Created"), 0);
		for (int c = 0; c < nctrs; c++) if (!strcmp(ctrs[c].imgid, t->id)) t->used = 1;
	}
	js_free(j);
	qsort(imgs, (size_t)nimgs, sizeof imgs[0], img_cmp);
}

static int vol_cmp(const void *a, const void *b)
{
	const Vol *x = a, *y = b;
	if (!!x->used_by[0] != !!y->used_by[0]) return !!y->used_by[0] - !!x->used_by[0];
	return strcmp(x->name, y->name);
}

static void load_volumes(void)
{
	Js *j = dk_json("GET", "/volumes", NULL, NULL);
	/* 每個資料卷用了多少空間：只問資料卷那一類（問全部會連映像、容器都算一遍，很慢） */
	Js *df = dk_json("GET", "/system/df?type=volume", NULL, NULL);
	nvols = 0;
	for (Js *v = js_get(j, "Volumes") ? js_get(j, "Volumes")->kids : NULL; v && nvols < MAX_ITEMS; v = v->next) {
		Vol *t = &vols[nvols++];
		memset(t, 0, sizeof *t);
		snprintf(t->name, sizeof t->name, "%s", js_str(js_get(v, "Name"), ""));
		snprintf(t->mount, sizeof t->mount, "%s", js_str(js_get(v, "Mountpoint"), ""));
		t->size = (long long)js_num(js_get(js_get(v, "UsageData"), "Size"), -1);
		for (Js *d = js_get(df, "Volumes") ? js_get(df, "Volumes")->kids : NULL; d && t->size < 0; d = d->next)
			if (!strcmp(js_str(js_get(d, "Name"), ""), t->name)) t->size = (long long)js_num(js_get(js_get(d, "UsageData"), "Size"), -1);
		for (int u = 0; u < nvuse; u++)
			if (!strcmp(vuse[u].vol, t->name)) {
				size_t l = strlen(t->used_by);
				snprintf(t->used_by + l, sizeof t->used_by - l, "%s%s", l ? "、" : "", vuse[u].ctr);
			}
	}
	js_free(j);
	js_free(df);
	qsort(vols, (size_t)nvols, sizeof vols[0], vol_cmp);
}

static Ctr *ctr_find(const char *key)
{
	for (int i = 0; i < nctrs; i++) if (!strcmp(ctrs[i].id, key) || !strcmp(ctrs[i].name, key)) return &ctrs[i];
	for (int i = 0; i < nctrs; i++) if (strlen(key) >= 4 && !strncmp(ctrs[i].id, key, strlen(key))) return &ctrs[i];
	return NULL;
}

static void load_version(void)
{
	Js *j = dk_json("GET", "/version", NULL, NULL);
	snprintf(dk_version, sizeof dk_version, "%s", js_str(js_get(j, "Version"), ""));
	js_free(j);
}

/* ------------------------------------------------------------------ */
/* 使用量（stats，一次一筆；CPU 跟 docker stats 一樣：100% = 一個核心）      */
/* ------------------------------------------------------------------ */

#define HIST 60
typedef struct {
	char id[72];
	unsigned long long cpu, sys, rx, tx, rd, wr;
	long last_ms;
	int n, pids;
	long long mem, limit;
	double cpu_h[HIST], rx_h[HIST], tx_h[HIST], rd_h[HIST], wr_h[HIST];
} Stats;

static Stats stats[MAX_ITEMS];
static int nstats;

static Stats *stats_for(const char *id)
{
	for (int i = 0; i < nstats; i++) if (!strcmp(stats[i].id, id)) return &stats[i];
	if (nstats >= MAX_ITEMS) return NULL;
	Stats *s = &stats[nstats++];
	memset(s, 0, sizeof *s);
	snprintf(s->id, sizeof s->id, "%s", id);
	return s;
}

static void push(double *h, double v)
{
	memmove(h, h + 1, (HIST - 1) * sizeof *h);
	h[HIST - 1] = v;
}

static void stats_sample(const Ctr *c)
{
	Stats *s = stats_for(c->id);
	if (!s) return;
	char path[200];
	snprintf(path, sizeof path, "/containers/%s/stats?stream=false&one-shot=true", c->id);
	Js *j = dk_json("GET", path, NULL, NULL);
	if (!j) return;
	Js *cs = js_get(j, "cpu_stats"), *ms = js_get(j, "memory_stats"), *mst = js_get(ms, "stats");
	unsigned long long cpu = (unsigned long long)js_num(js_get(js_get(cs, "cpu_usage"), "total_usage"), 0);
	unsigned long long sys = (unsigned long long)js_num(js_get(cs, "system_cpu_usage"), 0);
	int ncpu = (int)js_num(js_get(cs, "online_cpus"), 1);
	/* 記憶體：跟 docker stats 一樣扣掉可以丟掉的檔案快取 */
	double cache = js_num(js_get(mst, "inactive_file"), js_num(js_get(mst, "total_inactive_file"), 0));
	s->mem = (long long)MAX(0, js_num(js_get(ms, "usage"), 0) - cache);
	s->limit = (long long)js_num(js_get(ms, "limit"), 0);
	s->pids = (int)js_num(js_get(js_get(j, "pids_stats"), "current"), 0);
	unsigned long long rx = 0, tx = 0, rd = 0, wr = 0;
	for (Js *n = js_get(j, "networks") ? js_get(j, "networks")->kids : NULL; n; n = n->next) {
		rx += (unsigned long long)js_num(js_get(n, "rx_bytes"), 0);
		tx += (unsigned long long)js_num(js_get(n, "tx_bytes"), 0);
	}
	for (Js *b = js_get(js_get(j, "blkio_stats"), "io_service_bytes_recursive") ?
	             js_get(js_get(j, "blkio_stats"), "io_service_bytes_recursive")->kids : NULL; b; b = b->next) {
		const char *op = js_str(js_get(b, "op"), "");
		if (!strcasecmp(op, "read")) rd += (unsigned long long)js_num(js_get(b, "value"), 0);
		else if (!strcasecmp(op, "write")) wr += (unsigned long long)js_num(js_get(b, "value"), 0);
	}
	js_free(j);
	long now = pm_now_ms();
	if (s->last_ms && now - s->last_ms > 200) {
		double dt = (now - s->last_ms) / 1000.0;
		double cpu_pct = sys > s->sys && cpu >= s->cpu ? (double)(cpu - s->cpu) / (double)(sys - s->sys) * ncpu * 100 : 0;
		push(s->cpu_h, cpu_pct);
		push(s->rx_h, rx >= s->rx ? (rx - s->rx) / dt : 0);
		push(s->tx_h, tx >= s->tx ? (tx - s->tx) / dt : 0);
		push(s->rd_h, rd >= s->rd ? (rd - s->rd) / dt : 0);
		push(s->wr_h, wr >= s->wr ? (wr - s->wr) / dt : 0);
		if (s->n < HIST) s->n++;
	}
	s->last_ms = now;
	s->cpu = cpu; s->sys = sys; s->rx = rx; s->tx = tx; s->rd = rd; s->wr = wr;
}

static void human_bytes(char *out, size_t n, double b)
{
	if (b >= 1e9) snprintf(out, n, "%.1f GB", b / 1e9);
	else if (b >= 1e6) snprintf(out, n, "%.0f MB", b / 1e6);
	else if (b >= 1e3) snprintf(out, n, "%.0f KB", b / 1e3);
	else snprintf(out, n, "%.0f B", b);
}

static void fmt_rate(char *out, size_t n, double bps)
{
	human_bytes(out, n, bps);
	size_t l = strlen(out);
	snprintf(out + l, n - l, "/s");
}

static double peak(const double *h)
{
	double m = 0;
	for (int i = 0; i < HIST; i++) m = MAX(m, h[i]);
	return m;
}

/* ------------------------------------------------------------------ */
/* 背景工作（停止容器、下載映像、備份…會花時間的都在子行程做）               */
/* ------------------------------------------------------------------ */
/* 子行程一行一行回報：「P 進度」，最後一行「O 結果」或「E 錯誤訊息」 */

enum { JOB_ACTION, JOB_PULL, JOB_CREATE, JOB_BACKUP, JOB_RESTORE, JOB_PRUNE, JOB_REMOVE };
enum { PG_HOME, PG_CTR, PG_NEW };
static int page;
static char cur_id[72];
static void go(int pg);
typedef struct {
	int fd, kind;
	char key[256], label[256], progress[300], result[PATH_MAX], err[1024], warn[1024];
	char buf[4096];
	size_t n;
} Job;

static Job jobs[16];
static int njobs;
static int job_fd = -1;           /* 子行程裡：寫回報的管道 */
static char status[1200];
static int status_err;
static int need_reload;

/* Docker 常見的英文錯誤訊息，換成看得懂的話（原文放在括號裡） */
static const char *friendly(const char *m, char *buf, size_t n)
{
	static const struct { const char *key, *msg; } map[] = {
		{ "port is already allocated", N_("連接埠已經被別的程式或容器用了") },
		{ "address already in use", N_("連接埠已經被別的程式或容器用了") },
		{ "pull access denied", N_("找不到這個映像（名稱打錯，或是要先登入倉庫）") },
		{ "repository does not exist", N_("找不到這個映像（名稱打錯，或是要先登入倉庫）") },
		{ "manifest unknown", N_("這個映像沒有這個標籤（版本）") },
		{ "is already in use by container", N_("這個名稱已經有容器在用了") },
		{ "cannot remove a running container", N_("容器還在執行，要先停止") },
		{ "image is being used", N_("還有容器在用這個映像") },
		{ "image is referenced in multiple repositories", N_("這個映像有好幾個名稱，請一個一個刪") },
		{ "volume is in use", N_("還有容器在用這個資料卷") },
		{ "no space left on device", N_("磁碟空間不夠") },
		{ "permission denied", N_("沒有權限") },
		{ "dial tcp", N_("連不上映像倉庫（請檢查網路）") },
		{ "TLS handshake timeout", N_("連不上映像倉庫（請檢查網路）") },
	};
	for (size_t i = 0; i < LEN(map); i++)
		if (strcasestr(m, map[i].key)) { snprintf(buf, n, "%s（%s）", _(map[i].msg), m); return buf; }
	return m;
}

static void say(int err, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(status, sizeof status, fmt, ap);
	va_end(ap);
	status_err = err;
}

static void job_progress(const char *fmt, ...)
{
	char line[400];
	va_list ap;
	va_start(ap, fmt);
	int l = vsnprintf(line + 2, sizeof line - 3, fmt, ap);
	va_end(ap);
	line[0] = 'P'; line[1] = ' ';
	l = MIN(l + 2, (int)sizeof line - 2);
	for (int i = 2; i < l; i++) if (line[i] == '\n') line[i] = ' ';
	line[l] = '\n';
	if (job_fd >= 0) { if (write(job_fd, line, (size_t)l + 1) < 0) { /* 主程式已經不在：繼續做完 */ } }
	else if (isatty(2)) { line[l] = 0; fprintf(stderr, "\r\033[K%s", line + 2); }
}

static void job_warn(const char *msg)
{
	char line[1100];
	int l = snprintf(line, sizeof line, "W %s", msg);
	l = MIN(l, (int)sizeof line - 2);
	for (int i = 2; i < l; i++) if (line[i] == '\n') line[i] = ' ';
	line[l] = '\n';
	if (job_fd >= 0 && write(job_fd, line, (size_t)l + 1) < 0) { /* 主程式已經不在 */ }
}

static void cli_warn(const char *msg)
{
	char b[1400];
	fprintf(stderr, "\r\033[K%s：%s\n", _("沒辦法啟動"), friendly(msg, b, sizeof b));
}

static void cli_progress(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	if (isatty(2)) { fprintf(stderr, "\r\033[K"); vfprintf(stderr, fmt, ap); }
	va_end(ap);
}

static Job *job_for(const char *key)
{
	for (int i = 0; i < njobs; i++) if (!strcmp(jobs[i].key, key)) return &jobs[i];
	return NULL;
}

typedef int (*JobFn)(const char *a, const char *b, int flag, char *res, size_t rn, char *err, size_t en);

static void run_job(int kind, const char *key, const char *label, JobFn fn, const char *a, const char *b, int flag)
{
	if (job_for(key) || njobs >= (int)LEN(jobs)) return;
	int p[2];
	if (pipe(p)) return;
	pid_t pid = fork();
	if (pid == 0) {
		close(p[0]);
		if (dpy) close(ConnectionNumber(dpy));   /* 原生 Wayland（pm_ui_native）時沒有 X 連線 */
		job_fd = p[1];
		bk_progress = job_progress;
		bk_warn = job_warn;
		char res[PATH_MAX] = "", err[1024] = "";
		int rc = fn(a, b, flag, res, sizeof res, err, sizeof err);
		char line[PATH_MAX + 1100];
		const char *msg = rc ? (err[0] ? err : "error") : res;
		int l = snprintf(line, sizeof line, "%c %s\n", rc ? 'E' : 'O', msg);
		for (int i = 2; i < l - 1; i++) if (line[i] == '\n') line[i] = ' ';
		if (write(p[1], line, (size_t)MIN(l, (int)sizeof line - 1)) < 0) _exit(1);
		_exit(0);
	}
	close(p[1]);
	if (pid < 0) { close(p[0]); return; }
	fcntl(p[0], F_SETFL, O_NONBLOCK);
	Job *j = &jobs[njobs++];
	memset(j, 0, sizeof *j);
	j->fd = p[0];
	j->kind = kind;
	snprintf(j->key, sizeof j->key, "%s", key);
	snprintf(j->label, sizeof j->label, "%s", label);
	status[0] = 0;
}

static void job_done(Job *j, int ok)
{
	need_reload = 1;
	char fb[1400];
	if (!ok) {
		const char *fr = friendly(j->err, fb, sizeof fb);
		if (fr != j->err) snprintf(j->err, sizeof j->err, "%s", fr);   /* 沒有對應的說明時 friendly 回傳原字串：自己複製自己會變成空字串（FORTIFY） */
	}
	switch (j->kind) {
	case JOB_ACTION: if (!ok) say(1, "%s：%s", j->label, j->err); break;
	case JOB_REMOVE:
		if (ok && page == PG_CTR && !strcmp(j->key, cur_id)) go(PG_HOME);   /* 刪掉的就是這頁的容器：回到清單 */
		if (ok) say(0, _("已經刪除「%s」"), j->label); else say(1, "%s：%s", _("無法刪除"), j->err);
		break;
	case JOB_PULL: if (ok) say(0, _("映像 %s 下載好了"), j->label); else say(1, "%s：%s", _("無法下載映像"), j->err); break;
	case JOB_CREATE: if (ok) say(0, _("容器「%s」建立好了"), j->result); else say(1, "%s：%s", _("無法建立容器"), j->err); break;
	case JOB_BACKUP: if (ok) say(0, _("備份好了：%s"), j->result); else say(1, "%s：%s", _("無法備份"), j->err); break;
	case JOB_RESTORE:
		if (ok && j->warn[0]) say(1, _("容器「%s」還原好了，但沒辦法啟動：%s"), j->result, friendly(j->warn, fb, sizeof fb));
		else if (ok) say(0, _("還原好了，容器名稱是「%s」"), j->result);
		else say(1, "%s：%s", _("無法還原"), j->err);
		break;
	case JOB_PRUNE: if (ok) say(0, "%s", j->result); else say(1, "%s：%s", _("無法清理"), j->err); break;
	}
}

static int poll_jobs(void)
{
	int changed = 0;
	for (int i = 0; i < njobs; i++) {
		Job *j = &jobs[i];
		int eof = 0;
		for (;;) {
			ssize_t r = read(j->fd, j->buf + j->n, sizeof j->buf - 1 - j->n);
			if (r > 0) { j->n += (size_t)r; continue; }
			if (r < 0 && (errno == EAGAIN || errno == EINTR)) break;
			eof = 1;
			break;
		}
		char *nl;
		int ok = -1;
		while ((nl = memchr(j->buf, '\n', j->n))) {
			char line[sizeof j->buf];
			size_t used = (size_t)(nl - j->buf) + 1;
			memcpy(line, j->buf, used - 1);
			line[used - 1] = 0;
			const char *msg = used > 2 ? line + 2 : "";
			if (line[0] == 'P') snprintf(j->progress, sizeof j->progress, "%s", msg);
			else if (line[0] == 'O') { ok = 1; snprintf(j->result, sizeof j->result, "%s", msg); }
			else if (line[0] == 'E') { ok = 0; snprintf(j->err, sizeof j->err, "%s", msg); }
			else if (line[0] == 'W') snprintf(j->warn, sizeof j->warn, "%s", msg);
			memmove(j->buf, nl + 1, j->n - used);
			j->n -= used;
			changed = 1;
		}
		if (j->n >= sizeof j->buf - 1) j->n = 0;
		if (ok >= 0 || eof) {
			close(j->fd);
			Job done = *j;
			memmove(j, j + 1, (size_t)(njobs - i - 1) * sizeof *j);
			njobs--;
			i--;
			if (ok < 0) { ok = 0; if (!done.err[0]) snprintf(done.err, sizeof done.err, "%s", _("背景工作意外結束")); }
			job_done(&done, ok);
			changed = 1;
		}
	}
	return changed;
}

/* 各種工作 */
static int fn_action(const char *id, const char *action, int flag, char *res, size_t rn, char *err, size_t en)
{
	char path[256];
	snprintf(path, sizeof path, "/containers/%s/%s", id, action);
	return dk_do("POST", path, NULL, err, en, 120000);
}

static int fn_remove_ctr(const char *id, const char *b, int force, char *res, size_t rn, char *err, size_t en)
{
	char path[256];
	snprintf(path, sizeof path, "/containers/%s?v=1%s", id, force ? "&force=1" : "");   /* 匿名資料卷一起刪 */
	int r = dk_do("DELETE", path, NULL, err, en, 120000);
	/* 同時刪映像：b 是映像 ID。映像刪不掉（別的容器或標籤還在用）就算了，不讓整件事失敗 */
	if (r == 0 && b && b[0]) {
		char q[200], ie[256];
		url_esc(q, sizeof q, b);
		snprintf(path, sizeof path, "/images/%s", q);
		dk_do("DELETE", path, NULL, ie, sizeof ie, 120000);
	}
	return r;
}

static int fn_remove_img(const char *id, const char *b, int flag, char *res, size_t rn, char *err, size_t en)
{
	char path[256], q[200];
	url_esc(q, sizeof q, id);
	snprintf(path, sizeof path, "/images/%s", q);
	return dk_do("DELETE", path, NULL, err, en, 120000);
}

static int fn_remove_vol(const char *name, const char *b, int flag, char *res, size_t rn, char *err, size_t en)
{
	char path[512], q[400];
	url_esc(q, sizeof q, name);
	snprintf(path, sizeof path, "/volumes/%s", q);
	return dk_do("DELETE", path, NULL, err, en, 120000);
}

static int fn_pull(const char *image, const char *b, int flag, char *res, size_t rn, char *err, size_t en)
{
	snprintf(res, rn, "%s", image);
	char from[512], tag[128] = "", path[1024], fq[600], tq[200];
	snprintf(from, sizeof from, "%s", image);
	char *at = strchr(from, '@'), *slash = strrchr(from, '/'), *colon = strrchr(from, ':');
	if (!at && colon && (!slash || colon > slash)) { *colon = 0; snprintf(tag, sizeof tag, "%s", colon + 1); }
	if (!at && !tag[0]) snprintf(tag, sizeof tag, "latest");
	url_esc(fq, sizeof fq, from);
	url_esc(tq, sizeof tq, tag);
	snprintf(path, sizeof path, "/images/create?fromImage=%s%s%s", fq, tag[0] ? "&tag=" : "", tq);
	err[0] = 0;
	return dk_stream("POST", path, err, en, pull_line);
}

static int fn_prune(const char *what, const char *b, int flag, char *res, size_t rn, char *err, size_t en)
{
	char path[256];
	if (!strcmp(what, "volumes")) snprintf(path, sizeof path, "/volumes/prune?filters=%%7B%%22all%%22%%3A%%5B%%22true%%22%%5D%%7D");
	else snprintf(path, sizeof path, "/%s/prune", what);
	int st;
	char *r = dk_call("POST", path, NULL, &st, 0);
	if (st != 200) { dk_errmsg(r, st, err, en); free(r); return -1; }
	Js *j = js_parse(r);
	Js *list = NULL;
	const char *keys[] = { "ContainersDeleted", "ImagesDeleted", "VolumesDeleted", "CachesDeleted" };
	for (size_t i = 0; i < LEN(keys) && !list; i++) list = js_get(j, keys[i]);
	char space[32];
	human_bytes(space, sizeof space, js_num(js_get(j, "SpaceReclaimed"), 0));
	snprintf(res, rn, _("清掉 %d 個，空出 %s"), list && list->type == JS_ARR ? list->n : 0, space);
	js_free(j);
	free(r);
	return 0;
}

static int fn_backup(const char *id, const char *root, int with_image, char *res, size_t rn, char *err, size_t en)
{
	return do_backup(id, root, with_image, res, rn, err, en);
}

static int fn_restore(const char *dir, const char *b, int flag, char *res, size_t rn, char *err, size_t en)
{
	return do_restore(dir, res, rn, err, en);
}

/* 建立容器：a 是設定（一行一個「鍵=值」），需要時先下載映像，建好就啟動 */
typedef struct { char image[256], name[160], ports[400], mounts[1200], env[2000], cmd[600]; int restart; } NewCtr;
static NewCtr nc;
static const char *restart_names[] = { "no", "unless-stopped", "always", "on-failure" };

/* 用逗號（環境變數用分號）分開，去掉前後空白 */
static int split(const char *s, char sep, char out[][600], int max)
{
	int n = 0;
	const char *p = s;
	while (*p && n < max) {
		while (*p == ' ' || *p == sep) p++;
		if (!*p) break;
		const char *e = strchr(p, sep);
		size_t l = e ? (size_t)(e - p) : strlen(p);
		while (l && p[l - 1] == ' ') l--;
		snprintf(out[n++], 600, "%.*s", (int)l, p);
		p += e ? (size_t)(e - p) : strlen(p);
	}
	return n;
}

static int fn_create(const char *a, const char *b, int flag, char *res, size_t rn, char *err, size_t en)
{
	const NewCtr *c = &nc;   /* fork 時複製過來的 */
	if (ensure_image(c->image, err, en)) return -1;
	char items[64][600];
	Buf body = { 0 };
	buf_str(&body, "{\"Image\":");
	buf_quote(&body, c->image);
	/* 環境變數 */
	int n = split(c->env, ';', items, 64);
	if (n) {
		buf_str(&body, ",\"Env\":[");
		for (int i = 0; i < n; i++) { if (i) buf_str(&body, ","); buf_quote(&body, items[i]); }
		buf_str(&body, "]");
	}
	/* 指令（用空白分開，雙引號裡的不分） */
	if (c->cmd[0]) {
		buf_str(&body, ",\"Cmd\":[");
		const char *p = c->cmd;
		int first = 1;
		while (*p) {
			while (*p == ' ') p++;
			if (!*p) break;
			char word[600];
			size_t k = 0;
			int q = 0;
			while (*p && (q || *p != ' ') && k + 1 < sizeof word) {
				if (*p == '"') { q = !q; p++; continue; }
				word[k++] = *p++;
			}
			word[k] = 0;
			if (!first) buf_str(&body, ",");
			buf_quote(&body, word);
			first = 0;
		}
		buf_str(&body, "]");
	}
	/* 連接埠：本機:容器（可以加 /udp） */
	n = split(c->ports, ',', items, 64);
	Buf exp = { 0 }, bind = { 0 };
	buf_str(&exp, "{");
	buf_str(&bind, "{");
	for (int i = 0; i < n; i++) {
		char proto[8] = "tcp";
		char *sl = strchr(items[i], '/');
		if (sl) { snprintf(proto, sizeof proto, "%s", sl + 1); *sl = 0; }
		char *col = strrchr(items[i], ':');
		const char *host = col ? items[i] : "", *ctr = col ? col + 1 : items[i];
		if (col) *col = 0;
		char key[64];
		snprintf(key, sizeof key, "%s/%s", ctr, proto);
		if (i) { buf_str(&exp, ","); buf_str(&bind, ","); }
		buf_quote(&exp, key);
		buf_str(&exp, ":{}");
		buf_quote(&bind, key);
		buf_str(&bind, ":[{\"HostPort\":");
		buf_quote(&bind, host[0] ? host : ctr);
		buf_str(&bind, "}]");
	}
	buf_str(&exp, "}");
	buf_str(&bind, "}");
	if (n) { buf_str(&body, ",\"ExposedPorts\":"); buf_str(&body, exp.s); }
	buf_str(&body, ",\"HostConfig\":{\"PortBindings\":");
	buf_str(&body, bind.s);
	free(exp.s);
	free(bind.s);
	/* 資料夾：本機路徑或資料卷名稱:容器裡的路徑 */
	n = split(c->mounts, ',', items, 64);
	buf_str(&body, ",\"Binds\":[");
	for (int i = 0; i < n; i++) {
		if (items[i][0] == '~') {   /* ~ 換成家目錄 */
			char t[600];
			snprintf(t, sizeof t, "%s%s", getenv("HOME") ? getenv("HOME") : "", items[i] + 1);
			snprintf(items[i], 600, "%s", t);
		}
		if (i) buf_str(&body, ",");
		buf_quote(&body, items[i]);
	}
	buf_str(&body, "],\"RestartPolicy\":{\"Name\":");
	buf_quote(&body, restart_names[CLAMP(c->restart, 0, 3)]);
	buf_str(&body, "}}}");
	char path[512], q[400];
	url_esc(q, sizeof q, c->name);
	snprintf(path, sizeof path, "/containers/create%s%s", c->name[0] ? "?name=" : "", q);
	bk_progress("%s", _("正在建立容器…"));
	int st;
	char *r = dk_call("POST", path, body.s, &st, 30000);
	free(body.s);
	if (st != 201) { dk_errmsg(r, st, err, en); free(r); return -1; }
	Js *j = js_parse(r);
	char id[80];
	snprintf(id, sizeof id, "%s", js_str(js_get(j, "Id"), ""));
	js_free(j);
	free(r);
	snprintf(res, rn, "%s", c->name[0] ? c->name : id);
	bk_progress("%s", _("正在啟動容器…"));
	snprintf(path, sizeof path, "/containers/%s/start", id);
	return dk_do("POST", path, NULL, err, en, 60000);
}

/* ------------------------------------------------------------------ */
/* 終端機（進到容器、看紀錄、安裝）                                         */
/* ------------------------------------------------------------------ */

static const char *sh_safe(const char *s)
{
	static char buf[2][600];
	static int k;
	char *o = buf[k ^= 1];
	size_t n = 0;
	for (; *s && n + 4 < sizeof buf[0]; s++) {
		if (*s == '\'') { memcpy(o + n, "’", 3); n += 3; }
		else o[n++] = *s;
	}
	o[n] = 0;
	return o;
}

static void terminal(const char *title, const char *cmd, int wait)
{
	char full[2048];
	if (wait) snprintf(full, sizeof full, "%s; echo; echo '%s'; read x", cmd, sh_safe(_("完成，按 Enter 關閉這個視窗")));
	else snprintf(full, sizeof full, "%s", cmd);
	if (fork() == 0) {
		if (fork() == 0) {
			setsid();
			if (dpy) close(ConnectionNumber(dpy));   /* 原生 Wayland（pm_ui_native）時沒有 X 連線 */
			execlp("pomelo-terminal", "pomelo-terminal", "-t", title, "-e", "sh", "-c", full, (char *)NULL);
			execlp("x-terminal-emulator", "x-terminal-emulator", "-e", "sh", "-c", full, (char *)NULL);
			_exit(127);
		}
		_exit(0);
	}
}

static void exec_shell(const Ctr *c)
{
	char cmd[400], title[300];
	snprintf(cmd, sizeof cmd, "docker exec -it %.12s sh -c 'if command -v bash >/dev/null 2>&1; then exec bash; else exec sh; fi'", c->id);
	snprintf(title, sizeof title, "%s — %s", c->name, _("容器"));
	terminal(title, cmd, 0);
}

static void show_logs(const Ctr *c)
{
	char cmd[300], title[300];
	snprintf(cmd, sizeof cmd, "docker logs -f --tail 300 %.12s", c->id);
	snprintf(title, sizeof title, "%s — %s", c->name, _("紀錄"));
	terminal(title, cmd, 1);
}

/* 安裝：發行版自己的 Docker（Ubuntu／Debian 的 docker.io、Fedora 的 moby-engine）；Rocky／RHEL 沒有，用 Docker 官方的套件庫 */
static void install_docker(void)
{
	const char *tail = " && sudo systemctl enable --now docker && sudo usermod -aG docker \"$USER\"";
	char cmd[1200];
	if (pm_pkg_kind() == PM_PKG_DNF) {
		if (pm_os_like("rhel") || pm_os_like("centos"))
			snprintf(cmd, sizeof cmd, "sudo dnf install -y dnf-plugins-core && sudo dnf config-manager --add-repo "
			         "https://download.docker.com/linux/rhel/docker-ce.repo && sudo dnf install -y docker-ce docker-ce-cli containerd.io%s", tail);
		else snprintf(cmd, sizeof cmd, "sudo dnf install -y --setopt=install_weak_deps=False moby-engine%s", tail);
	} else snprintf(cmd, sizeof cmd, "sudo apt-get install -y --no-install-recommends docker.io ca-certificates%s", tail);
	terminal(_("安裝 Docker"), cmd, 1);
}

/* ------------------------------------------------------------------ */
/* 設定（備份位置）：~/.config/pomelo/docker.conf                          */
/* ------------------------------------------------------------------ */

static char backup_dir[PATH_MAX];

static void conf_path(char *out, size_t n)
{
	const char *c = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
	if (c && *c) snprintf(out, n, "%s/pomelo/docker.conf", c);
	else snprintf(out, n, "%s/.config/pomelo/docker.conf", home ? home : "/tmp");
}

static void conf_load(void)
{
	const char *home = getenv("HOME");
	snprintf(backup_dir, sizeof backup_dir, "%s/docker-backups", home ? home : "/tmp");
	char p[PATH_MAX], line[PATH_MAX + 32];
	conf_path(p, sizeof p);
	FILE *f = fopen(p, "r");
	while (f && fgets(line, sizeof line, f)) {
		line[strcspn(line, "\n")] = 0;
		if (!strncmp(line, "backup_dir = ", 13) && line[13]) snprintf(backup_dir, sizeof backup_dir, "%s", line + 13);
	}
	if (f) fclose(f);
}

static void conf_save(void)
{
	char p[PATH_MAX];
	conf_path(p, sizeof p);
	char *sl = strrchr(p, '/');
	if (sl) { *sl = 0; mkdirs(p, 0700); *sl = '/'; }
	FILE *f = fopen(p, "w");
	if (!f) return;
	fprintf(f, "# Pomelo 容器（Docker）的設定\nbackup_dir = %s\n", backup_dir);
	fclose(f);
}

/* 備份清單 */
typedef struct { char dir[PATH_MAX], name[160]; long long created, size; int with_image; } Bk;
static Bk bks[256];
static int nbks;

static int sum_size(const char *path, const struct stat *st, int flag, struct FTW *ftw);
static long long size_acc;
static int sum_size(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
	if (flag == FTW_F) size_acc += st->st_size;
	return 0;
}

static int bk_cmp(const void *a, const void *b)
{
	const Bk *x = a, *y = b;
	return x->created < y->created ? 1 : x->created > y->created ? -1 : 0;
}

static void load_backups(void)
{
	nbks = 0;
	DIR *d = opendir(backup_dir);
	struct dirent *e;
	while (d && (e = readdir(d)) && nbks < (int)LEN(bks)) {
		if (e->d_name[0] == '.') continue;
		Bk *b = &bks[nbks];
		snprintf(b->dir, sizeof b->dir, "%s/%s", backup_dir, e->d_name);
		char p[PATH_MAX + 32];
		snprintf(p, sizeof p, "%s/backup.json", b->dir);
		char *raw = read_file(p);
		Js *j = js_parse(raw);
		free(raw);
		if (!j) continue;
		snprintf(b->name, sizeof b->name, "%s", js_str(js_get(j, "name"), e->d_name));
		b->created = (long long)js_num(js_get(j, "created"), 0);
		b->with_image = (int)js_num(js_get(j, "with_image"), 0);
		js_free(j);
		size_acc = 0;
		nftw(b->dir, sum_size, 8, FTW_PHYS);
		b->size = size_acc;
		nbks++;
	}
	if (d) closedir(d);
	qsort(bks, (size_t)nbks, sizeof bks[0], bk_cmp);
}

/* ------------------------------------------------------------------ */
/* 介面                                                                 */
/* ------------------------------------------------------------------ */

enum { TAB_CTR, TAB_IMG, TAB_VOL, TAB_BAK };
static int tab;
static Js *inspect;               /* PG_CTR：docker inspect 的結果 */
static char sel_img[80], sel_vol[200], sel_bak[PATH_MAX];
static char pull_name[256], rename_buf[160];
static int confirm_remove, confirm_prune, with_image = 1, confirm_bak_del, del_image;
static PmUI *ui;
static long next_list, next_stats, next_state, next_inspect;

static void go(int pg)
{
	page = pg;
	confirm_remove = confirm_prune = confirm_bak_del = 0;
	if (!njobs) status[0] = 0;   /* 舊的訊息不要留到別頁（還有工作在跑時留著，做完才看得到結果） */
	if (ui) { ui->again = 1; ui->scroll = 0; }
}

static void load_inspect(void)
{
	char path[200];
	snprintf(path, sizeof path, "/containers/%s/json", cur_id);
	js_free(inspect);
	inspect = dk_json("GET", path, NULL, NULL);
}

static void open_ctr(const Ctr *c)
{
	snprintf(cur_id, sizeof cur_id, "%s", c->id);
	snprintf(rename_buf, sizeof rename_buf, "%s", c->name);
	load_inspect();
	go(PG_CTR);
}

static void status_note(PmUI *u)
{
	if (!status[0]) return;
	char t[1300];
	snprintf(t, sizeof t, "%s%s", status_err ? "⚠ " : "", status);
	pm_ui_note(u, t);
}

/* Docker 不能用的時候：裝、啟動、加入群組 */
static int env_notes(PmUI *u)
{
	if (dk_state == DK_OK) return 1;
	pm_ui_group(u, NULL);
	if (dk_state == DK_NOTINSTALLED) {
		if (pm_ui_button(u, _("這台電腦還沒有安裝 Docker"), _("安裝"))) install_docker();
		pm_ui_end(u);
		pm_ui_note(u, pm_pkg_kind() == PM_PKG_DNF && (pm_os_like("rhel") || pm_os_like("centos"))
		              ? _("會開一個終端機視窗安裝（要輸入你的密碼）：加上 Docker 官方的套件庫，安裝 Docker 並設定開機自動執行，再把你加進 docker 群組。裝好後要登出再登入。")
		              : _("會開一個終端機視窗安裝（要輸入你的密碼）：安裝系統內建的 Docker 並設定開機自動執行，再把你加進 docker 群組。裝好後要登出再登入。"));
	} else if (dk_state == DK_DOWN) {
		if (pm_ui_button(u, _("Docker 沒有在執行"), _("啟動"))) terminal(_("啟動 Docker"), "sudo systemctl enable --now docker", 1);
		pm_ui_end(u);
		pm_ui_note(u, _("會開一個終端機視窗啟動 Docker，並設定以後開機自動執行。"));
	} else {
		if (pm_ui_button(u, _("你的帳號還不能管理 Docker"), _("加入 docker 群組"))) terminal(_("加入 docker 群組"), "sudo usermod -aG docker \"$USER\"", 1);
		pm_ui_end(u);
		pm_ui_note(u, _("加入 docker 群組後要登出再登入才會生效。注意：docker 群組的成員可以透過容器取得整台電腦的管理員權限，只加信任的帳號。"));
	}
	return 0;
}

static const char *state_label(const char *st)
{
	if (!strcmp(st, "running")) return _("執行中");
	if (!strcmp(st, "paused")) return _("已暫停");
	if (!strcmp(st, "restarting")) return _("重新啟動中");
	if (!strcmp(st, "created")) return _("已建立");
	if (!strcmp(st, "dead")) return _("無法使用");
	return _("已停止");
}

/* ---- 搜尋、篩選、排序（容器、映像、資料卷三個清單共用） ---- */
typedef struct { char q[120]; int show, sort; } Flt;
static Flt flt_ctr, flt_img, flt_vol;
static int flt_mode;

static int has_ci(const char *hay, const char *needle)
{
	if (!needle[0]) return 1;
	return strcasestr(hay, needle) != NULL;
}

/* 清單上方的一列：搜尋框、顯示哪些、排序方式；回傳 1 代表有改變 */
static int filter_bar(PmUI *u, Flt *f, const char **show, int nshow, const char **sort, int nsort)
{
	int ch = 0;
	pm_ui_group(u, NULL);
	pm_ui_entry(u, _("搜尋"), f->q, sizeof f->q, 0);
	ch |= pm_ui_segment(u, _("顯示"), show, nshow, &f->show);
	ch |= pm_ui_segment(u, _("排序"), sort, nsort, &f->sort);
	pm_ui_end(u);
	return ch;
}

static int ord[MAX_ITEMS];

static int ctr_ord_cmp(const void *a, const void *b)
{
	const Ctr *x = &ctrs[*(const int *)a], *y = &ctrs[*(const int *)b];
	if (flt_mode == 1) return strcasecmp(x->name, y->name);
	if (flt_mode == 2) return x->created < y->created ? 1 : x->created > y->created ? -1 : 0;
	return ctr_cmp(x, y);
}

static int img_ord_cmp(const void *a, const void *b)
{
	const Img *x = &imgs[*(const int *)a], *y = &imgs[*(const int *)b];
	if (flt_mode == 1) return x->size < y->size ? 1 : x->size > y->size ? -1 : strcasecmp(x->tag, y->tag);
	if (flt_mode == 2) return x->created < y->created ? 1 : x->created > y->created ? -1 : 0;
	return strcasecmp(x->tag, y->tag);
}

static int vol_ord_cmp(const void *a, const void *b)
{
	const Vol *x = &vols[*(const int *)a], *y = &vols[*(const int *)b];
	if (flt_mode == 1) return x->size < y->size ? 1 : x->size > y->size ? -1 : strcmp(x->name, y->name);
	if (flt_mode == 2) return vol_cmp(x, y);
	return strcmp(x->name, y->name);
}

static void build_containers(PmUI *u)
{
	int shown_head = -1;
	const char *fshow[] = { _("全部"), _("執行中"), _("已停止") };
	const char *fsort[] = { _("預設"), _("名稱"), _("最新建立") };
	if (nctrs) filter_bar(u, &flt_ctr, fshow, 3, fsort, 3);
	int nord = 0;
	for (int i = 0; i < nctrs; i++) {
		Ctr *c = &ctrs[i];
		int run = ctr_running(c) || ctr_paused(c);
		if ((flt_ctr.show == 1 && !run) || (flt_ctr.show == 2 && run)) continue;
		if (!(has_ci(c->name, flt_ctr.q) || has_ci(c->image, flt_ctr.q) || has_ci(c->project, flt_ctr.q) || has_ci(c->ports, flt_ctr.q))) continue;
		ord[nord++] = i;
	}
	flt_mode = flt_ctr.sort;
	qsort(ord, (size_t)nord, sizeof ord[0], ctr_ord_cmp);
	if (nctrs && !nord) pm_ui_note(u, _("沒有符合的容器。"));
	for (int oi = 0; oi < nord; oi++) {
		Ctr *c = &ctrs[ord[oi]];
		int run = ctr_running(c) || ctr_paused(c);
		int grp = flt_ctr.sort == 0 ? run : 2;   /* 預設排序才分「執行中」「已停止」兩組 */
		if (grp != shown_head) {
			if (shown_head >= 0) pm_ui_end(u);
			pm_ui_group(u, grp == 2 ? _("符合的容器") : run ? _("執行中") : _("已停止"));
			shown_head = grp;
		}
		char sub[400], right[200];
		Stats *s = stats_for(c->id);
		Job *j = job_for(c->id);
		if (j) snprintf(sub, sizeof sub, "%s", j->progress[0] ? j->progress : _("處理中…"));
		else if (ctr_running(c) && s && s->n) {
			char mem[24], net[24], disk[24];
			human_bytes(mem, sizeof mem, (double)s->mem);
			fmt_rate(net, sizeof net, s->rx_h[HIST - 1] + s->tx_h[HIST - 1]);
			fmt_rate(disk, sizeof disk, s->rd_h[HIST - 1] + s->wr_h[HIST - 1]);
			snprintf(sub, sizeof sub, _("CPU %.0f%% · 記憶體 %s · 網路 %s · 磁碟 %s"), s->cpu_h[HIST - 1], mem, net, disk);
		} else if (!strcmp(c->state, "exited")) {   /* Docker 的狀態文字是英文：自己說 */
			int code = 0;
			const char *par = strchr(c->status, '(');
			if (par) code = atoi(par + 1);
			char ex[80];
			if (code) snprintf(ex, sizeof ex, _("結束代碼 %d"), code);
			else snprintf(ex, sizeof ex, "%s", _("正常結束"));
			snprintf(sub, sizeof sub, "%s · %s", c->image, ex);
		} else snprintf(sub, sizeof sub, "%s · %s", c->image, state_label(c->state));
		snprintf(right, sizeof right, "%s", c->ports[0] ? c->ports : state_label(c->state));
		char title[300];
		if (c->project[0]) snprintf(title, sizeof title, "%s  (%s)", c->name, c->project);
		else snprintf(title, sizeof title, "%s", c->name);
		if (pm_ui_item(u, ctr_running(c) ? "@run" : ctr_paused(c) ? "@pause" : "@stop", title, sub, right, 0)) open_ctr(c);
	}
	if (shown_head >= 0) pm_ui_end(u);
	pm_ui_group(u, NULL);
	if (pm_ui_button(u, nctrs ? _("從映像建立一個新的容器") : _("還沒有容器：從映像建立第一個"), _("新增容器"))) {
		memset(&nc, 0, sizeof nc);
		nc.restart = 1;
		go(PG_NEW);
	}
	int stopped = 0;
	for (int i = 0; i < nctrs; i++) if (!ctr_running(&ctrs[i]) && !ctr_paused(&ctrs[i])) stopped++;
	if (stopped && !job_for("prune-containers")) {
		char msg[200];
		snprintf(msg, sizeof msg, _("刪除全部 %d 個已停止的容器"), stopped);
		if (!confirm_prune) { if (pm_ui_button(u, msg, _("清理"))) { confirm_prune = 1; u->again = 1; } }
		else if (pm_ui_button(u, _("確定嗎？已停止的容器裡沒存到資料卷的檔案會不見"), _("確定清理"))) {
			run_job(JOB_PRUNE, "prune-containers", "", fn_prune, "containers", NULL, 0);
			confirm_prune = 0;
		}
	}
	pm_ui_end(u);
	pm_ui_note(u, _("點容器可以開始、停止、進到容器裡、看紀錄、備份。CPU 的 100% 等於一個處理器核心。"));
}

static void build_images(PmUI *u)
{
	pm_ui_group(u, NULL);
	if (pm_ui_entry(u, _("下載映像"), pull_name, sizeof pull_name, 0) && pull_name[0]) {
		char key[300];
		snprintf(key, sizeof key, "pull:%s", pull_name);
		run_job(JOB_PULL, key, pull_name, fn_pull, pull_name, NULL, 0);
		pull_name[0] = 0;
	}
	for (int i = 0; i < njobs; i++)
		if (jobs[i].kind == JOB_PULL) pm_ui_info(u, jobs[i].label, jobs[i].progress[0] ? jobs[i].progress : _("處理中…"));
	pm_ui_end(u);
	pm_ui_note(u, _("輸入名稱後按 Enter，例如 nginx、mariadb:11、ghcr.io/某人/某個映像:標籤（從 Docker Hub 或其他倉庫下載）。"));

	if (nimgs) {
		const char *fshow[] = { _("全部"), _("有容器在用"), _("沒有容器在用") };
		const char *fsort[] = { _("名稱"), _("大小"), _("建立日期") };
		filter_bar(u, &flt_img, fshow, 3, fsort, 3);
		int nord = 0;
		long long total = 0, shown_total = 0;
		for (int i = 0; i < nimgs; i++) {
			total += imgs[i].size;
			if ((flt_img.show == 1 && !imgs[i].used) || (flt_img.show == 2 && imgs[i].used)) continue;
			if (!has_ci(imgs[i].tag, flt_img.q)) continue;
			ord[nord++] = i;
			shown_total += imgs[i].size;
		}
		flt_mode = flt_img.sort;
		qsort(ord, (size_t)nord, sizeof ord[0], img_ord_cmp);
		char cap[160], ts[24], ss[24];
		human_bytes(ts, sizeof ts, (double)total);
		human_bytes(ss, sizeof ss, (double)shown_total);
		if (nord == nimgs) snprintf(cap, sizeof cap, _("%d 個映像，共 %s"), nimgs, ts);
		else snprintf(cap, sizeof cap, _("符合的 %d 個映像（共 %d 個），%s"), nord, nimgs, ss);
		pm_ui_group(u, cap);
		if (!nord) pm_ui_info(u, _("沒有符合的映像"), NULL);
		for (int oi = 0; oi < nord; oi++) {
			Img *m = &imgs[ord[oi]];
			char sub[200], size[24], when[32];
			human_bytes(size, sizeof size, (double)m->size);
			time_t t = (time_t)m->created;
			strftime(when, sizeof when, "%Y-%m-%d", localtime(&t));
			snprintf(sub, sizeof sub, "%s · %s · %s", size, when, m->used ? _("有容器在用") : _("沒有容器在用"));
			int open = !strcmp(sel_img, m->id);
			if (pm_ui_item(u, "@image", m->tag, sub, open ? NULL : "›", open)) {
				snprintf(sel_img, sizeof sel_img, "%s", open ? "" : m->id);
				confirm_remove = 0;
				u->again = 1;
			}
			if (!open) continue;
			if (strcmp(m->tag, _("（沒有名稱）")) && pm_ui_button(u, _("用這個映像建立容器"), _("建立容器"))) {
				memset(&nc, 0, sizeof nc);
				nc.restart = 1;
				snprintf(nc.image, sizeof nc.image, "%s", m->tag);
				go(PG_NEW);
			}
			if (job_for(m->id)) pm_ui_info(u, _("處理中…"), NULL);
			else if (m->used) pm_ui_info(u, _("有容器在用，不能刪除"), NULL);
			else if (!confirm_remove) { if (pm_ui_button(u, _("刪除這個映像"), _("刪除"))) { confirm_remove = 1; u->again = 1; } }
			else if (pm_ui_button(u, _("確定要刪除嗎？之後要用還得重新下載"), _("確定刪除"))) {
				run_job(JOB_REMOVE, m->id, m->tag, fn_remove_img, m->id, NULL, 0);
				confirm_remove = 0;
			}
		}
		pm_ui_end(u);
	}
	pm_ui_group(u, NULL);
	if (job_for("prune-images")) pm_ui_info(u, _("正在清理…"), NULL);
	else if (pm_ui_button(u, _("刪除沒有名稱、也沒有容器在用的映像（通常是更新後留下的舊版）"), _("清理")))
		run_job(JOB_PRUNE, "prune-images", "", fn_prune, "images", NULL, 0);
	if (job_for("prune-build")) pm_ui_info(u, _("正在清理…"), NULL);
	else if (pm_ui_button(u, _("刪除建置映像時留下的快取"), _("清理")))
		run_job(JOB_PRUNE, "prune-build", "", fn_prune, "build", NULL, 0);
	pm_ui_end(u);
}

static void build_volumes(PmUI *u)
{
	if (!nvols) { pm_ui_note(u, _("還沒有資料卷。容器要長期保存的資料（資料庫、上傳的檔案）放在資料卷裡，刪掉容器也不會不見。")); return; }
	const char *fshow[] = { _("全部"), _("使用中"), _("未使用") };
	const char *fsort[] = { _("名稱"), _("大小"), _("使用中優先") };
	filter_bar(u, &flt_vol, fshow, 3, fsort, 3);
	int unused = 0, nord = 0;
	long long vtotal = 0;
	for (int i = 0; i < nvols; i++) {
		Vol *v = &vols[i];
		if (!v->used_by[0]) unused++;
		if (v->size > 0) vtotal += v->size;
		if ((flt_vol.show == 1 && !v->used_by[0]) || (flt_vol.show == 2 && v->used_by[0])) continue;
		if (!(has_ci(v->name, flt_vol.q) || has_ci(v->used_by, flt_vol.q))) continue;
		ord[nord++] = i;
	}
	flt_mode = flt_vol.sort;
	qsort(ord, (size_t)nord, sizeof ord[0], vol_ord_cmp);
	char vcap[160], vts[24];
	human_bytes(vts, sizeof vts, (double)vtotal);
	if (nord == nvols) snprintf(vcap, sizeof vcap, _("%d 個資料卷，共 %s"), nvols, vts);
	else snprintf(vcap, sizeof vcap, _("符合的 %d 個資料卷（共 %d 個）"), nord, nvols);
	pm_ui_group(u, vcap);
	if (!nord) pm_ui_info(u, _("沒有符合的資料卷"), NULL);
	for (int oi = 0; oi < nord; oi++) {
		Vol *v = &vols[ord[oi]];
		char title[220], sub[340], sz[24];
		if (is_hex64(v->name)) snprintf(title, sizeof title, _("匿名資料卷 %.12s"), v->name);
		else snprintf(title, sizeof title, "%s", v->name);
		if (v->size >= 0) human_bytes(sz, sizeof sz, (double)v->size); else snprintf(sz, sizeof sz, "%s", _("大小不明"));
		if (v->used_by[0]) snprintf(sub, sizeof sub, _("%s · 給 %s 用"), sz, v->used_by);
		else snprintf(sub, sizeof sub, _("%s · 沒有容器在用"), sz);
		int open = !strcmp(sel_vol, v->name);
		if (pm_ui_item(u, "@volume", title, sub, open ? NULL : "›", open)) {
			snprintf(sel_vol, sizeof sel_vol, "%s", open ? "" : v->name);
			confirm_remove = 0;
			u->again = 1;
		}
		if (!open) continue;
		pm_ui_info(u, _("位置"), v->mount);
		if (job_for(v->name)) pm_ui_info(u, _("處理中…"), NULL);
		else if (v->used_by[0]) pm_ui_info(u, _("有容器在用，不能刪除"), NULL);
		else if (!confirm_remove) { if (pm_ui_button(u, _("刪除這個資料卷"), _("刪除"))) { confirm_remove = 1; u->again = 1; } }
		else if (pm_ui_button(u, _("確定要刪除嗎？裡面的資料救不回來"), _("確定刪除"))) {
			run_job(JOB_REMOVE, v->name, title, fn_remove_vol, v->name, NULL, 0);
			confirm_remove = 0;
		}
	}
	pm_ui_end(u);
	pm_ui_note(u, _("資料卷的檔案在 Docker 的資料夾裡（要管理員權限才看得到），要拿出來請用容器的「備份」。"));
	if (unused) {
		pm_ui_group(u, NULL);
		char msg[200];
		snprintf(msg, sizeof msg, _("刪除全部 %d 個沒有容器在用的資料卷"), unused);
		if (job_for("prune-volumes")) pm_ui_info(u, _("正在清理…"), NULL);
		else if (!confirm_prune) { if (pm_ui_button(u, msg, _("清理"))) { confirm_prune = 1; u->again = 1; } }
		else if (pm_ui_button(u, _("確定嗎？裡面的資料救不回來"), _("確定清理"))) {
			run_job(JOB_PRUNE, "prune-volumes", "", fn_prune, "volumes", NULL, 0);
			confirm_prune = 0;
		}
		pm_ui_end(u);
	}
}

static void build_backups(PmUI *u)
{
	pm_ui_group(u, NULL);
	if (pm_ui_entry(u, _("備份存放位置"), backup_dir, sizeof backup_dir, 0)) { conf_save(); load_backups(); }
	pm_ui_end(u);
	pm_ui_note(u, _("改好按 Enter。可以放在外接硬碟或網路磁碟，換電腦時把整個資料夾複製過去就能還原。"));
	for (int i = 0; i < njobs; i++)
		if (jobs[i].kind == JOB_RESTORE || jobs[i].kind == JOB_BACKUP) {
			pm_ui_group(u, NULL);
			pm_ui_info(u, jobs[i].label, jobs[i].progress[0] ? jobs[i].progress : _("處理中…"));
			pm_ui_end(u);
		}
	if (!nbks) { pm_ui_note(u, _("這裡還沒有備份。到容器的頁面按「備份」。")); return; }
	pm_ui_group(u, _("備份"));
	for (int i = 0; i < nbks; i++) {
		Bk *b = &bks[i];
		char sub[200], when[40], size[24];
		time_t t = (time_t)b->created;
		strftime(when, sizeof when, "%Y-%m-%d %H:%M", localtime(&t));
		human_bytes(size, sizeof size, (double)b->size);
		snprintf(sub, sizeof sub, "%s · %s · %s", when, size, b->with_image ? _("含映像") : _("只有資料"));
		int open = !strcmp(sel_bak, b->dir);
		if (pm_ui_item(u, "@backup", b->name, sub, open ? NULL : "›", open)) {
			snprintf(sel_bak, sizeof sel_bak, "%s", open ? "" : b->dir);
			confirm_bak_del = 0;
			u->again = 1;
		}
		if (!open) continue;
		pm_ui_info(u, _("位置"), b->dir);
		if (job_for(b->dir)) pm_ui_info(u, _("處理中…"), NULL);
		else {
			if (pm_ui_button(u, _("照備份重建容器（名稱被用掉時會加上 -restored）"), _("{備份}還原"))) {
				char label[300];
				snprintf(label, sizeof label, _("還原 %s"), b->name);
				run_job(JOB_RESTORE, b->dir, label, fn_restore, b->dir, NULL, 0);
			}
			if (pm_ui_button(u, _("在檔案管理員打開"), _("打開"))) {
				char cmd[PATH_MAX + 32];
				snprintf(cmd, sizeof cmd, "exec pomelo-open '%s'", sh_safe(b->dir));
				pm_spawn(cmd);
			}
			if (!confirm_bak_del) { if (pm_ui_button(u, _("刪除這份備份"), _("刪除"))) { confirm_bak_del = 1; u->again = 1; } }
			else if (pm_ui_button(u, _("確定要刪除這份備份嗎？"), _("確定刪除"))) {
				rm_rf(b->dir);
				load_backups();
				confirm_bak_del = 0;
			}
		}
	}
	pm_ui_end(u);
}

static void build_home(PmUI *u)
{
	pm_ui_title(u, _("容器"), _("管理 Docker 的容器、映像與備份"));
	if (!env_notes(u)) { status_note(u); return; }
	int run = 0;
	for (int i = 0; i < nctrs; i++) if (ctr_running(&ctrs[i])) run++;
	const char *tabs[] = { _("容器"), _("{分頁}映像"), _("資料卷"), _("{分頁}備份") };
	if (pm_ui_segment(u, NULL, tabs, 4, &tab)) {
		next_list = 0;
		if (!njobs) status[0] = 0;
		confirm_remove = confirm_prune = 0;
		if (tab == TAB_BAK) load_backups();
	}
	pm_ui_end(u);
	u->y -= 8;
	char info[200];
	snprintf(info, sizeof info, _("Docker %s · %d 個容器在執行，共 %d 個"), dk_version, run, nctrs);
	pm_ui_note(u, info);
	status_note(u);
	if (tab == TAB_CTR) build_containers(u);
	else if (tab == TAB_IMG) build_images(u);
	else if (tab == TAB_VOL) build_volumes(u);
	else build_backups(u);
}

static void build_usage(PmUI *u, const Ctr *c)
{
	Stats *s = stats_for(c->id);
	if (!s || !s->n) return;
	static const unsigned blue = 0x0a84ff, orange = 0xff9f0a, green = 0x30d158, purple = 0xbf5af2;
	char label[200], a[24], b[24], pk[24];
	pm_ui_group(u, _("使用量"));
	snprintf(label, sizeof label, "CPU　%.0f%%", s->cpu_h[HIST - 1]);
	const double *cpu[] = { s->cpu_h };
	pm_ui_graph(u, label, NULL, cpu, &blue, 1, HIST, MAX(100, peak(s->cpu_h) * 1.15));
	char val[80];
	human_bytes(a, sizeof a, (double)s->mem);
	if (s->limit > 0 && s->limit < (1LL << 60)) {
		human_bytes(b, sizeof b, (double)s->limit);
		snprintf(val, sizeof val, _("%s／上限 %s"), a, b);
	} else snprintf(val, sizeof val, "%s", a);
	pm_ui_meter(u, _("記憶體"), val, s->limit > 0 ? (double)s->mem / (double)s->limit : 0, green);
	fmt_rate(a, sizeof a, s->rx_h[HIST - 1]);
	fmt_rate(b, sizeof b, s->tx_h[HIST - 1]);
	fmt_rate(pk, sizeof pk, MAX(peak(s->rx_h), peak(s->tx_h)));
	snprintf(label, sizeof label, _("網路　收 %s　送 %s　（最高 %s）"), a, b, pk);
	const char *nleg[] = { _("收"), _("送") };
	const double *net[] = { s->rx_h, s->tx_h };
	unsigned ncol[] = { green, purple };
	pm_ui_graph(u, label, nleg, net, ncol, 2, HIST, 0);
	fmt_rate(a, sizeof a, s->rd_h[HIST - 1]);
	fmt_rate(b, sizeof b, s->wr_h[HIST - 1]);
	fmt_rate(pk, sizeof pk, MAX(peak(s->rd_h), peak(s->wr_h)));
	snprintf(label, sizeof label, _("磁碟　讀 %s　寫 %s　（最高 %s）"), a, b, pk);
	const char *dleg[] = { _("讀取"), _("寫入") };
	const double *disk[] = { s->rd_h, s->wr_h };
	unsigned dcol[] = { blue, orange };
	pm_ui_graph(u, label, dleg, disk, dcol, 2, HIST, 0);
	char pids[16];
	snprintf(pids, sizeof pids, "%d", s->pids);
	pm_ui_info(u, _("行程數"), pids);
	pm_ui_end(u);
}

static void build_ctr(PmUI *u)
{
	Ctr *c = ctr_find(cur_id);
	if (pm_ui_back(u, _("容器"))) { go(PG_HOME); return; }
	if (!c) { pm_ui_title(u, _("容器"), NULL); pm_ui_note(u, _("這個容器已經不在了。")); status_note(u); return; }
	char sub[400];
	snprintf(sub, sizeof sub, "%s · %s", state_label(c->state), c->image);
	pm_ui_title(u, c->name, sub);
	Job *j = job_for(c->id);
	int run = ctr_running(c), paused = ctr_paused(c);

	pm_ui_group(u, NULL);
	if (j) pm_ui_info(u, j->label, j->progress[0] ? j->progress : _("處理中…"));
	else if (run) {
		if (pm_ui_button(u, _("在終端機裡進到容器（bash 或 sh）"), _("進入"))) exec_shell(c);
		if (pm_ui_button(u, _("看容器的輸出紀錄（會一直更新）"), _("紀錄"))) show_logs(c);
		if (pm_ui_button(u, _("停止容器"), _("停止"))) run_job(JOB_ACTION, c->id, _("停止"), fn_action, c->id, "stop", 0);
		if (pm_ui_button(u, _("重新啟動容器"), _("重新啟動"))) run_job(JOB_ACTION, c->id, _("重新啟動"), fn_action, c->id, "restart", 0);
		if (pm_ui_button(u, _("暫停（容器裡的程式先凍結）"), _("暫停"))) run_job(JOB_ACTION, c->id, _("暫停"), fn_action, c->id, "pause", 0);
	} else if (paused) {
		if (pm_ui_button(u, _("容器暫停中"), _("{執行}繼續"))) run_job(JOB_ACTION, c->id, _("{執行}繼續"), fn_action, c->id, "unpause", 0);
	} else {
		if (pm_ui_button(u, _("啟動容器"), _("啟動"))) run_job(JOB_ACTION, c->id, _("啟動"), fn_action, c->id, "start", 0);
		if (pm_ui_button(u, _("看容器的輸出紀錄"), _("紀錄"))) show_logs(c);
	}
	pm_ui_end(u);
	status_note(u);
	if (run) build_usage(u, c);

	/* 資訊 */
	Js *cfg = js_get(inspect, "Config"), *hc = js_get(inspect, "HostConfig"), *stt = js_get(inspect, "State");
	pm_ui_group(u, _("資訊"));
	pm_ui_info(u, _("映像"), c->image);
	char when[64];
	time_t t = (time_t)c->created;
	strftime(when, sizeof when, "%Y-%m-%d %H:%M", localtime(&t));
	pm_ui_info(u, _("建立時間"), when);
	if (run) {
		const char *started = js_str(js_get(stt, "StartedAt"), "");
		struct tm tm = { 0 };
		if (strptime(started, "%Y-%m-%dT%H:%M:%S", &tm)) {
			time_t st0 = timegm(&tm);
			strftime(when, sizeof when, "%Y-%m-%d %H:%M", localtime(&st0));
			pm_ui_info(u, _("啟動時間"), when);
		}
	}
	for (Js *n = js_get(js_get(inspect, "NetworkSettings"), "Networks") ? js_get(js_get(inspect, "NetworkSettings"), "Networks")->kids : NULL; n; n = n->next) {
		const char *ip = js_str(js_get(n, "IPAddress"), "");
		if (!ip[0]) continue;
		char l[200];
		snprintf(l, sizeof l, "%s（%s）", _("IP 位址"), n->key ? n->key : "");
		pm_ui_info(u, l, ip);
	}
	if (c->ports[0]) pm_ui_info(u, _("連接埠（本機→容器）"), c->ports);
	for (Js *m = js_get(inspect, "Mounts") ? js_get(inspect, "Mounts")->kids : NULL; m; m = m->next) {
		const char *type = js_str(js_get(m, "Type"), ""), *name = js_str(js_get(m, "Name"), ""), *src = js_str(js_get(m, "Source"), "");
		char v[600];
		if (!strcmp(type, "volume")) snprintf(v, sizeof v, is_hex64(name) ? _("匿名資料卷 %.12s") : _("資料卷 %s"), name);
		else snprintf(v, sizeof v, "%s", src);
		pm_ui_info(u, js_str(js_get(m, "Destination"), ""), v);
	}
	int nenv = js_get(cfg, "Env") ? js_get(cfg, "Env")->n : 0;
	char ne[16];
	snprintf(ne, sizeof ne, "%d", nenv);
	pm_ui_info(u, _("環境變數"), ne);
	pm_ui_end(u);
	pm_ui_note(u, _("環境變數可能有密碼，這裡只顯示數量；要看內容請進到容器裡執行 env。"));

	/* 設定 */
	pm_ui_group(u, _("{群組}設定"));
	if (pm_ui_entry(u, _("名稱"), rename_buf, sizeof rename_buf, 0) && rename_buf[0] && strcmp(rename_buf, c->name)) {
		char path[400], q[300], err[512];
		url_esc(q, sizeof q, rename_buf);
		snprintf(path, sizeof path, "/containers/%s/rename?name=%s", c->id, q);
		if (dk_do("POST", path, NULL, err, sizeof err, 10000)) say(1, "%s：%s", _("無法改名"), err);
		load_containers();
	}
	const char *rp = js_str(js_get(js_get(hc, "RestartPolicy"), "Name"), "no");
	int r = 0;
	for (int i = 0; i < 4; i++) if (!strcmp(rp, restart_names[i])) r = i;
	const char *ropts[] = { _("不要自動啟動"), _("開機時自動啟動（手動停止的除外）"), _("一直保持執行"), _("出錯停止時重新啟動") };
	if (pm_ui_choice(u, _("自動啟動"), ropts, 4, &r)) {
		char path[200], body[120], err[512];
		snprintf(path, sizeof path, "/containers/%s/update", c->id);
		snprintf(body, sizeof body, "{\"RestartPolicy\":{\"Name\":\"%s\"}}", restart_names[r]);
		if (dk_do("POST", path, body, err, sizeof err, 10000)) say(1, "%s：%s", _("無法修改"), err);
		load_inspect();
	}
	pm_ui_end(u);

	/* 備份與刪除 */
	pm_ui_group(u, _("備份"));
	pm_ui_switch(u, _("連映像一起備份（可以在別台電腦還原）"), &with_image);
	if (!j && pm_ui_button(u, _("把容器與它的資料存到備份資料夾"), _("備份"))) {
		mkdirs(backup_dir, 0700);
		char label[300];
		snprintf(label, sizeof label, _("備份 %s"), c->name);
		run_job(JOB_BACKUP, c->id, label, fn_backup, c->id, backup_dir, with_image);
	}
	pm_ui_end(u);
	pm_ui_note(u, _("會備份容器的設定、資料卷與掛載資料夾裡的檔案；連映像一起的話，容器裡改過的東西也都在。執行中的容器備份時會暫停一下。"));

	pm_ui_group(u, NULL);
	if (run || paused) pm_ui_info(u, _("要刪除請先停止容器"), NULL);
	else if (!j) {
		/* 這個映像還有別的容器在用嗎？沒有的話，刪容器時可以順便刪映像（不然映像會留在磁碟上，要到「映像」頁另外刪） */
		int img_shared = 0;
		for (int k = 0; k < nctrs; k++) if (&ctrs[k] != c && !strcmp(ctrs[k].imgid, c->imgid)) img_shared = 1;
		if (!confirm_remove) { if (pm_ui_button(u, _("刪除這個容器（資料卷會留著）"), _("刪除"))) { confirm_remove = 1; del_image = 0; u->again = 1; } }
		else {
			if (!img_shared && c->imgid[0]) {
				char q[300];
				snprintf(q, sizeof q, _("同時刪除映像 %s（以後要用得重新下載；自己建置的映像刪了就沒了）"), c->image);
				pm_ui_switch(u, q, &del_image);
			} else if (img_shared) pm_ui_info(u, _("映像還有別的容器在用，會保留"), NULL);
			if (pm_ui_button(u, _("確定要刪除嗎？容器裡沒存到資料卷的檔案會不見"), _("確定刪除"))) {
				run_job(JOB_REMOVE, c->id, c->name, fn_remove_ctr, c->id, del_image && !img_shared ? c->imgid : NULL, 0);
				confirm_remove = 0;
			}
		}
	}
	pm_ui_end(u);
}

static void build_new(PmUI *u)
{
	if (pm_ui_back(u, _("容器"))) { go(PG_HOME); return; }
	pm_ui_title(u, _("新增容器"), NULL);
	pm_ui_group(u, NULL);
	pm_ui_entry(u, _("映像"), nc.image, sizeof nc.image, 0);
	pm_ui_entry(u, _("名稱（可留空）"), nc.name, sizeof nc.name, 0);
	pm_ui_end(u);
	pm_ui_note(u, _("映像例如 nginx、mariadb:11；這台電腦還沒有的會先下載。"));
	pm_ui_group(u, _("連接與資料"));
	pm_ui_entry(u, _("連接埠"), nc.ports, sizeof nc.ports, 0);
	pm_ui_entry(u, _("資料夾"), nc.mounts, sizeof nc.mounts, 0);
	pm_ui_entry(u, _("環境變數"), nc.env, sizeof nc.env, 0);
	pm_ui_entry(u, _("指令（可留空）"), nc.cmd, sizeof nc.cmd, 0);
	const char *ropts[] = { _("不要自動啟動"), _("開機時自動啟動（手動停止的除外）"), _("一直保持執行"), _("出錯停止時重新啟動") };
	pm_ui_choice(u, _("自動啟動"), ropts, 4, &nc.restart);
	pm_ui_end(u);
	pm_ui_note(u, _("連接埠寫「本機:容器」，例如 8080:80（用逗號分開多個）。資料夾寫「本機路徑或資料卷名稱:容器裡的路徑」，例如 ~/網站:/usr/share/nginx/html、db:/var/lib/mysql。"
	                "環境變數用分號分開，例如 TZ=Asia/Taipei; MARIADB_ROOT_PASSWORD=密碼。"));
	Job *j = job_for("create");
	if (j) {
		pm_ui_group(u, NULL);
		pm_ui_info(u, _("正在建立…"), j->progress);
		pm_ui_end(u);
	} else {
		const char *btns[] = { _("取消"), _("建立並啟動") };
		int r = pm_ui_buttons(u, btns, 2, 1);
		if (r == 0) { go(PG_HOME); return; }
		if (r == 1) {
			if (!nc.image[0]) say(1, "%s", _("請輸入映像名稱"));
			else run_job(JOB_CREATE, "create", nc.image, fn_create, NULL, NULL, 0);
		}
		u->y += 16;
	}
	status_note(u);
}

static void build(PmUI *u)
{
	ui = u;
	if (page == PG_CTR) build_ctr(u);
	else if (page == PG_NEW) build_new(u);
	else build_home(u);
}

static void tick(PmUI *u)
{
	int changed = poll_jobs();
	long now = pm_now_ms();
	if (now >= next_state) {   /* Docker 本身的狀態：裝了沒、在跑嗎 */
		next_state = now + (dk_state == DK_OK ? 10000 : 2000);
		int old = dk_state;
		dk_state = dk_status();
		if (dk_state != old) { changed = 1; need_reload = 1; if (dk_state == DK_OK) load_version(); }
	}
	if (dk_state != DK_OK) { if (changed) pm_ui_redraw(u); return; }
	if (need_reload || now >= next_list) {
		next_list = now + 2000;
		load_containers();
		if (page == PG_HOME && tab == TAB_IMG) load_images();
		if (page == PG_HOME && tab == TAB_VOL) load_volumes();
		if (need_reload && page == PG_HOME && tab == TAB_BAK) load_backups();
		if (page == PG_CTR && (need_reload || now >= next_inspect)) { load_inspect(); next_inspect = now + 5000; }
		if (page == PG_NEW && need_reload && !job_for("create") && !status_err && status[0]) go(PG_HOME);   /* 建好了：回到清單 */
		need_reload = 0;
		changed = 1;
	}
	/* 使用量：清單頁每兩秒取全部執行中的，容器頁每秒取那一個 */
	if (now >= next_stats) {
		if (page == PG_CTR) {
			next_stats = now + 1000;
			Ctr *c = ctr_find(cur_id);
			if (c && ctr_running(c)) { stats_sample(c); changed = 1; }
		} else if (page == PG_HOME && tab == TAB_CTR) {
			next_stats = now + 2000;
			for (int i = 0; i < nctrs; i++) if (ctr_running(&ctrs[i])) { stats_sample(&ctrs[i]); changed = 1; }
		}
	}
	if (changed) pm_ui_redraw(u);
}

/* ------------------------------------------------------------------ */
/* 指令列                                                               */
/* ------------------------------------------------------------------ */

static void usage(void)
{
	printf("%s:\n  pomelo-docker\n  pomelo-docker --list\n  pomelo-docker --backup %s [%s] [--no-image]\n  pomelo-docker --restore %s\n",
	       _("用法"), _("容器"), _("資料夾"), _("備份資料夾"));
}

static int cli(int argc, char *argv[])
{
	char err[1024] = "", res[PATH_MAX] = "", fb[1400];
	bk_progress = cli_progress;
	bk_warn = cli_warn;
	int st = dk_status();
	if (st != DK_OK) {
		fprintf(stderr, "pomelo-docker: %s\n", st == DK_DENIED ? _("你的帳號還不能管理 Docker") : st == DK_DOWN ? _("Docker 沒有在執行") : _("這台電腦還沒有安裝 Docker"));
		return 1;
	}
	if (!strcmp(argv[1], "--list")) {
		load_containers();
		for (int i = 0; i < nctrs; i++) printf("%.12s\t%s\t%s\t%s\n", ctrs[i].id, ctrs[i].name, state_label(ctrs[i].state), ctrs[i].image);
		return 0;
	}
	int rc;
	if (!strcmp(argv[1], "--backup") && argc > 2) {
		const char *dir = argc > 3 && argv[3][0] != '-' ? argv[3] : backup_dir;
		int img = !(argc > 3 && !strcmp(argv[argc - 1], "--no-image"));
		mkdirs(dir, 0700);
		rc = do_backup(argv[2], dir, img, res, sizeof res, err, sizeof err);
	} else if (!strcmp(argv[1], "--restore") && argc > 2) rc = do_restore(argv[2], res, sizeof res, err, sizeof err);
	else { usage(); return 1; }
	if (isatty(2)) fprintf(stderr, "\r\033[K");
	if (rc) { fprintf(stderr, "pomelo-docker: %s\n", friendly(err, fb, sizeof fb)); return 1; }
	printf("%s\n", res);
	return 0;
}

int main(int argc, char *argv[])
{
	if (argc > 2 && !strcmp(argv[1], "--icon")) return pm_ui_write_icon(icon, argv[2]);
	if (argc > 1 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) { usage(); return 0; }
	setlocale(LC_ALL, "");
	signal(SIGPIPE, SIG_IGN);
	conf_load();
	if (argc > 1) return cli(argc, argv);
	pm_ui_add_icon("@run", small_icon(0x30a14e, 0));
	pm_ui_add_icon("@pause", small_icon(0xff9f0a, 0));
	pm_ui_add_icon("@stop", small_icon(0x8e8e93, 0));
	pm_ui_add_icon("@image", small_icon(0x0a84ff, 1));
	pm_ui_add_icon("@volume", small_icon(0x5e5ce6, 2));
	pm_ui_add_icon("@backup", small_icon(0x14a3b8, 3));
	pm_ui_native = 1;   /* build() 裡沒有直接呼叫 X：Wayland 工作階段直接當原生程式 */
	return pm_ui_run(_("容器"), "pomelo-docker", 700, 760, build, tick, 500, icon);
}
