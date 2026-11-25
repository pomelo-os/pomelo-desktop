/* SPDX-License-Identifier: GPL-3.0-or-later */
/* eng-rime.c — 拼音（簡體）、倉頡、速成、粵拼（librime，dlopen 載入）
 *
 * 方案資料（Rime 官方的 yaml 與字典）由「語言與輸入法」下載到 $XDG_DATA_HOME/pomelo/rime-data
 * （固定版本、檢查 SHA256；Fedora 的 librime 沒附方案資料，所以各發行版一律自己下載）。
 * 使用者資料（編好的字典、學到的詞）在 $XDG_DATA_HOME/pomelo/rime，跟 fcitx、ibus 的 Rime 分開。
 * pomelo-input 一啟動就初始化；第一次用、或方案（剛下載）、每頁候選字數改了時，librime 在背景「部署」
 * （把字典編好，要十幾秒），這段時間按鍵照常交給程式，選字列顯示「正在準備…」。部署完重開工作階段。
 */
#define _GNU_SOURCE
#include "engine.h"
#include "third-party/rime_api.h"
#include <dlfcn.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static RimeApi *api;
static int started;                       /* 已經 initialize */
static int gen;                           /* 第幾次部署：工作階段是之前開的就重開 */
static char shared_dir[PATH_MAX], user_dir[PATH_MAX];

typedef struct { const char *schema; RimeSessionId sid; int gen; char out[1024]; } Rime;

/* 這個引擎要的方案（schema 檔）都下載好了沒 */
static int have_schema(const char *schema)
{
	char p[PATH_MAX + 64];
	snprintf(p, sizeof p, "%s/%s.schema.yaml", shared_dir, schema);
	return !access(p, R_OK);
}

static void dirs(void)
{
	if (shared_dir[0]) return;
	const char *x = getenv("XDG_DATA_HOME"), *h = getenv("HOME");
	char base[PATH_MAX];
	if (x && *x) snprintf(base, sizeof base, "%s/pomelo", x);
	else snprintf(base, sizeof base, "%s/.local/share/pomelo", h ? h : "");
	snprintf(shared_dir, sizeof shared_dir, "%s/rime-data", base);
	snprintf(user_dir, sizeof user_dir, "%s/rime", base);
}

static int load_lib(void)
{
	static int state;   /* 0 還沒試、1 可以、2 找不到 */
	if (state) return state == 1;
	state = 2;
	dirs();
	void *h = dlopen("librime.so.1", RTLD_NOW | RTLD_LOCAL);
	if (!h) return 0;
	RimeApi *(*get)(void) = (RimeApi *(*)(void))dlsym(h, "rime_get_api");
	api = get ? get() : NULL;
	if (!api || !api->setup || !api->initialize || !api->process_key || !api->get_context || !api->create_session) return 0;
	state = 1;
	return 1;
}

/* 方案清單與每頁候選字數寫進 default.custom.yaml；要重新部署時回傳 1 */
static int write_custom(void)
{
	static const char *schemas[] = { "luna_pinyin_simp", "cangjie5", "quick5", "jyut6ping3" };
	char want[1024];
	int n = snprintf(want, sizeof want, "# 由 pomelo-input 產生（改了會被蓋掉）\npatch:\n  menu/page_size: %d\n  schema_list:\n", conf_cand_n);
	for (unsigned i = 0; i < sizeof schemas / sizeof schemas[0]; i++)
		if (have_schema(schemas[i])) n += snprintf(want + n, sizeof want - (size_t)n, "    - schema: %s\n", schemas[i]);
	char p[PATH_MAX + 32], cur[1024] = "";
	snprintf(p, sizeof p, "%s/default.custom.yaml", user_dir);
	FILE *f = fopen(p, "r");
	if (f) { size_t k = fread(cur, 1, sizeof cur - 1, f); cur[k] = 0; fclose(f); }
	if (!strcmp(cur, want)) return 0;
	f = fopen(p, "w");
	if (f) { fputs(want, f); fclose(f); }
	return 1;
}

/* 第一次用：設定 librime、必要時在背景部署；之後再呼叫：方案或每頁字數變了就重新部署 */
static void start(void)
{
	if (started) {
		if (write_custom() && api->start_maintenance) { gen++; api->start_maintenance(True); }
		return;
	}
	started = 1;
	mkdir(user_dir, 0700);
	/* 簡體拼音用 OpenCC 的繁轉簡設定：librime 在方案資料夾的 opencc 裡找 */
	char oc[PATH_MAX + 16];
	snprintf(oc, sizeof oc, "%s/opencc", shared_dir);
	if (access(oc, F_OK) && !access("/usr/share/opencc/t2s.json", R_OK) && symlink("/usr/share/opencc", oc)) { /* 失敗就算了：簡體拼音會出繁體字 */ }
	RIME_STRUCT(RimeTraits, t);
	t.shared_data_dir = shared_dir;
	t.user_data_dir = user_dir;
	t.distribution_name = "Pomelo";
	t.distribution_code_name = "pomelo";
	t.distribution_version = "1";
	t.app_name = "rime.pomelo";
	t.min_log_level = 2;      /* 只記錯誤 */
	t.log_dir = user_dir;
	api->setup(&t);
	api->initialize(&t);
	int changed = write_custom();
	if (api->start_maintenance) api->start_maintenance(changed ? True : False);
}

static int busy(void) { return api->is_maintenance_mode && api->is_maintenance_mode(); }

/* 每個方案一個引擎；載入得了就先在背景部署，等使用者真的打字時通常已經準備好了 */
static int load_schema(const char *schema)
{
	if (!load_lib() || !have_schema(schema)) return 0;
	start();
	return 1;
}
static int load_pinyin(void) { return load_schema("luna_pinyin_simp"); }
static int load_cangjie(void) { return load_schema("cangjie5"); }
static int load_quick(void) { return load_schema("quick5"); }
static int load_jyutping(void) { return load_schema("jyut6ping3"); }

static void *create_schema(const char *schema)
{
	start();
	Rime *r = calloc(1, sizeof *r);
	if (r) r->schema = schema;
	return r;
}
static void *create_pinyin(void) { return create_schema("luna_pinyin_simp"); }
static void *create_cangjie(void) { return create_schema("cangjie5"); }
static void *create_quick(void) { return create_schema("quick5"); }
static void *create_jyutping(void) { return create_schema("jyut6ping3"); }

/* 部署好了才開工作階段 */
static int session(Rime *r)
{
	if (busy()) return 0;
	if (r->sid && r->gen != gen) { api->destroy_session(r->sid); r->sid = 0; }   /* 重新部署過 */
	if (r->sid) return 1;
	if (api->join_maintenance_thread) api->join_maintenance_thread();
	r->sid = api->create_session();
	if (!r->sid) return 0;
	r->gen = gen;
	api->select_schema(r->sid, r->schema);
	api->set_option(r->sid, "ascii_mode", False);   /* 中英切換由 pomelo-input 管（Shift 輕點） */
	return 1;
}

static void destroy(void *c)
{
	Rime *r = c;
	if (!r) return;
	if (r->sid) api->destroy_session(r->sid);
	free(r);
}

static void conf(void *c)
{
	(void)c;
	if (started) start();   /* 每頁候選字數改了：重新部署 */
}

static void take(Rime *r)
{
	RIME_STRUCT(RimeCommit, cm);
	if (!api->get_commit(r->sid, &cm)) return;
	if (cm.text) {
		size_t o = strlen(r->out);
		snprintf(r->out + o, sizeof r->out - o, "%s", cm.text);
	}
	api->free_commit(&cm);
}

static int key(void *c, unsigned ks, unsigned state)
{
	Rime *r = c;
	if (!session(r)) return 0;   /* 還在部署：按鍵照常交給程式 */
	int handled = api->process_key(r->sid, (int)ks, (int)(state & 0xff));
	take(r);
	return handled;
}

static void view(void *c, View *v)
{
	Rime *r = c;
	if (!session(r)) {
		if (busy()) snprintf(v->aux, sizeof v->aux, "%s", tr(N_("正在準備輸入法…（第一次要十幾秒）")));
		return;
	}
	RIME_STRUCT(RimeContext, ctx);
	if (!api->get_context(r->sid, &ctx)) return;
	const char *pe = ctx.composition.preedit;
	if (pe && *pe) {
		/* 選到的那一段（正在轉換的）用強調色，前後照常 */
		int len = (int)strlen(pe), a = ctx.composition.sel_start, b = ctx.composition.sel_end;
		if (a < 0 || a > len) a = 0;
		if (b < a || b > len) b = len;
		snprintf(v->pre, sizeof v->pre, "%.*s", a, pe);
		snprintf(v->comp, sizeof v->comp, "%.*s", b - a, pe + a);
		snprintf(v->post, sizeof v->post, "%s", pe + b);
	}
	RimeMenu *m = &ctx.menu;
	for (int i = 0; i < m->num_candidates && v->ncand < CAND_MAX; i++) {
		const char *t = m->candidates[i].text ? m->candidates[i].text : "";
		const char *cm = m->candidates[i].comment;
		snprintf(v->cand[v->ncand], sizeof v->cand[0], "%s", t);
		if (cm && *cm) snprintf(v->note[v->ncand], sizeof v->note[0], "%s", cm);   /* 例如粵拼的讀音、倉頡的補碼提示 */
		if (ctx.select_labels && ctx.select_labels[i]) snprintf(v->label[v->ncand], sizeof v->label[0], "%s", ctx.select_labels[i]);
		else if (m->select_keys && (int)strlen(m->select_keys) > i) snprintf(v->label[v->ncand], sizeof v->label[0], "%c", m->select_keys[i]);
		v->ncand++;
	}
	if (v->ncand) {
		v->sel = m->highlighted_candidate_index;
		v->page = m->page_no;
		v->npage = 0;            /* librime 不告訴我們總共幾頁 */
		v->more = !m->is_last_page;
	}
	api->free_context(&ctx);
}

static int commit(void *c, char *out, size_t n)
{
	Rime *r = c;
	if (!r->out[0]) return 0;
	snprintf(out, n, "%s", r->out);
	r->out[0] = 0;
	return 1;
}

static void flush(void *c)
{
	Rime *r = c;
	if (!r->sid || busy()) return;
	api->commit_composition(r->sid);
	take(r);
}

static void reset(void *c)
{
	Rime *r = c;
	if (r->sid && !busy()) api->clear_composition(r->sid);
}

static void choose(void *c, int i)
{
	Rime *r = c;
	if (!session(r)) return;
	if (RIME_API_AVAILABLE(api, select_candidate_on_current_page)) api->select_candidate_on_current_page(r->sid, (size_t)i);
	else api->process_key(r->sid, '1' + i, 0);
	take(r);
}

const Engine eng_rime_pinyin = { "rime-pinyin", N_("拼音（簡體）"), "拼", load_pinyin, create_pinyin, destroy, conf, key, view, commit, flush, reset, choose };
const Engine eng_rime_cangjie = { "rime-cangjie", N_("倉頡"), "倉", load_cangjie, create_cangjie, destroy, conf, key, view, commit, flush, reset, choose };
const Engine eng_rime_quick = { "rime-quick", N_("速成"), "速", load_quick, create_quick, destroy, conf, key, view, commit, flush, reset, choose };
const Engine eng_rime_jyutping = { "rime-jyutping", N_("粵拼"), "粵", load_jyutping, create_jyutping, destroy, conf, key, view, commit, flush, reset, choose };
