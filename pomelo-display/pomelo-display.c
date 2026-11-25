/* SPDX-License-Identifier: GPL-3.0-or-later */
/* pomelo-display — Pomelo 顯示器設定
 *
 * 每個螢幕：解析度、更新頻率、旋轉、啟用、主要顯示器；多螢幕時設定排列方式（左右上下或鏡像）。
 * 套用後 15 秒內沒按「保留」就自動還原，避免選到不支援的模式而看不到畫面。
 * 保留的設定存在 settings.conf（display.<輸出名稱>），登入時由 pomelo-session 執行
 * `pomelo-display --apply` 套用。
 * 登入中插上螢幕：視窗管理員執行 `pomelo-display --hotplug 新接上的輸出…`，照記住的設定打開，
 * 沒設定過的接在主要顯示器旁邊；拔掉的螢幕關掉（不然畫面還留著一塊看不到的地方）。
 * Wayland 工作階段（pomelo-compositor）：XWayland 的 RandR 改不到真正的螢幕，讀取與套用改用 wlr-output-management
 * （libpomelo 的 pmwl_heads），畫面、保存格式、15 秒還原都一樣。Wayland 沒有「主要顯示器」：位置在最左上的當主要。
 */
#define _GNU_SOURCE
#include <pomelo-wl.h>
#include <pomelo.h>
#include <sys/wait.h>

typedef struct { int w, h, nrates; double rates[16]; RRMode ids[16]; int preferred; } Res;   /* ids：每個頻率的模式編號 */
typedef struct {
	char name[32];
	int on, primary, builtin;
	Res res[64];
	int nres, cur_res, cur_rate, rot, pos, x, y;
} Out;

static Out outs[8], saved[8];
static int nouts;
static char gone[8][32];      /* 已經拔掉、卻還開著的輸出（要關掉） */
static int ngone;
static long revert_at;        /* 等待確認中：到這個時間就還原 */
static char status[200];
static int wl_mode;           /* Wayland 工作階段：wlr-output-management */
static double wl_scale[8];    /* 原生：每個螢幕的縮放（介面沒有改縮放，照原本的） */

static const char *rot_names[] = { N_("標準"), N_("90°"), N_("180°"), N_("270°") };
static const char *rot_args[] = { "normal", "left", "inverted", "right" };
static const char *pos_names[] = { N_("在主要顯示器右邊"), N_("在主要顯示器左邊"), N_("在主要顯示器上方"),
                                   N_("在主要顯示器下方"), N_("與主要顯示器相同（鏡像）") };
static const char *pos_args[] = { "--right-of", "--left-of", "--above", "--below", "--same-as" };

static double mode_rate(const XRRModeInfo *m)
{
	double v = (double)m->hTotal * m->vTotal;
	if (m->modeFlags & RR_DoubleScan) v *= 2;
	if (m->modeFlags & RR_Interlace) v /= 2;
	return v > 0 ? (double)m->dotClock / v : 0;
}

/* 推算其他螢幕相對於主要顯示器的位置（兩種後端共用） */
static void guess_positions(void)
{
	Out *p = NULL;
	for (int i = 0; i < nouts; i++) if (outs[i].primary) p = &outs[i];
	if (!p && nouts) { p = &outs[0]; p->primary = 1; }
	for (int i = 0; i < nouts; i++) {
		Out *o = &outs[i];
		if (o == p) continue;
		o->pos = o->x == p->x && o->y == p->y ? 4 : o->x >= p->x + 1 && o->y == p->y ? 0 :
		         o->x < p->x ? 1 : o->y < p->y ? 2 : o->y > p->y ? 3 : 0;
	}
}

/* Wayland：從 compositor 讀（模式整理成「解析度 → 多個更新頻率」，跟 X 版一樣） */
static void read_outputs_wl(void)
{
	PmwHead h[8];
	int n = pmwl_heads(h, 8);
	nouts = 0;
	ngone = 0;
	int prim = -1, best = 1 << 30;
	for (int i = 0; i < n && nouts < (int)LEN(outs); i++) {
		Out *o = &outs[nouts];
		memset(o, 0, sizeof *o);
		snprintf(o->name, sizeof o->name, "%s", h[i].name);
		o->builtin = !strncmp(o->name, "eDP", 3) || !strncmp(o->name, "LVDS", 4) || !strncmp(o->name, "DSI", 3);
		o->on = h[i].enabled;
		o->x = h[i].x; o->y = h[i].y;
		o->rot = h[i].transform & 3;
		wl_scale[nouts] = h[i].scale > 0 ? h[i].scale : 1;
		for (int m = 0; m < h[i].nmodes; m++) {
			PmwMode *mo = &h[i].modes[m];
			int r;
			for (r = 0; r < o->nres; r++) if (o->res[r].w == mo->w && o->res[r].h == mo->h) break;
			if (r == o->nres) {
				if (o->nres >= (int)LEN(o->res)) continue;
				o->res[r] = (Res){ mo->w, mo->h, 0, { 0 }, { 0 }, 0 };
				o->nres++;
			}
			Res *rs = &o->res[r];
			if (mo->preferred) rs->preferred = 1;
			double rate = mo->refresh / 1000.0;
			int dup = 0;
			for (int q = 0; q < rs->nrates; q++) if (fabs(rs->rates[q] - rate) < 0.05) dup = 1;
			if (!dup && rs->nrates < (int)LEN(rs->rates)) rs->rates[rs->nrates++] = rate;
		}
		for (int r = 0; r < o->nres; r++)
			if (o->res[r].w == h[i].w && o->res[r].h == h[i].h) {
				o->cur_res = r;
				for (int q = 0; q < o->res[r].nrates; q++) if (fabs(o->res[r].rates[q] - h[i].refresh / 1000.0) < 0.05) o->cur_rate = q;
			}
		if (o->on && o->x + o->y < best) { best = o->x + o->y; prim = nouts; }
		nouts++;
	}
	if (prim >= 0) outs[prim].primary = 1;
	guess_positions();
}

/* Wayland：排列方式換成實際座標（主要顯示器在 0,0；最後整體移到不是負的地方），交給 compositor */
static void apply_wl(const Out *v, int n)
{
	PmwHead h[8];
	int k = 0, pw = 0, ph = 0;
	const Out *p = NULL;
	for (int i = 0; i < n; i++) if (v[i].primary && v[i].on) p = &v[i];
	if (p) {
		const Res *r = &p->res[p->cur_res];
		pw = p->rot & 1 ? r->h : r->w;   /* 轉 90°／270° 寬高對調 */
		ph = p->rot & 1 ? r->w : r->h;
	}
	int minx = 0, miny = 0;
	for (int i = 0; i < n && k < 8; i++) {
		const Out *o = &v[i];
		const Res *r = &o->res[o->cur_res];
		PmwHead *x = &h[k++];
		memset(x, 0, sizeof *x);
		snprintf(x->name, sizeof x->name, "%s", o->name);
		x->enabled = o->on;
		x->w = r->w; x->h = r->h;
		x->refresh = r->nrates ? (int)(r->rates[o->cur_rate] * 1000 + 0.5) : 0;
		x->transform = o->rot;
		x->scale = wl_scale[i] > 0 ? wl_scale[i] : 1;
		int ow = o->rot & 1 ? r->h : r->w, oh = o->rot & 1 ? r->w : r->h;
		if (o == p || !p) { x->x = 0; x->y = 0; }
		else switch (o->pos) {
			case 0: x->x = pw; x->y = 0; break;
			case 1: x->x = -ow; x->y = 0; break;
			case 2: x->x = 0; x->y = -oh; break;
			case 3: x->x = 0; x->y = ph; break;
			default: x->x = 0; x->y = 0; break;   /* 鏡像：Wayland 不能真的鏡像，疊在同一個位置 */
		}
		if (x->enabled) { minx = MIN(minx, x->x); miny = MIN(miny, x->y); }
	}
	for (int i = 0; i < k; i++) { h[i].x -= minx; h[i].y -= miny; }
	if (!pmwl_heads_apply(h, k, 0)) snprintf(status, sizeof status, "%s", _("這個設定螢幕不支援，已經改回來"));
}

/* 用 XRandR 讀出目前的狀態 */
static void read_outputs(void)
{
	if (wl_mode) { read_outputs_wl(); return; }
	nouts = 0;
	ngone = 0;
	XRRScreenResources *res = XRRGetScreenResourcesCurrent(dpy, pm_root);
	if (!res) return;
	RROutput prim = XRRGetOutputPrimary(dpy, pm_root);
	for (int i = 0; i < res->noutput && nouts < (int)LEN(outs); i++) {
		XRROutputInfo *oi = XRRGetOutputInfo(dpy, res, res->outputs[i]);
		if (!oi) continue;
		if (oi->connection != RR_Connected) {
			if (oi->crtc && ngone < (int)LEN(gone)) snprintf(gone[ngone++], sizeof gone[0], "%s", oi->name);
			XRRFreeOutputInfo(oi);
			continue;
		}
		Out *o = &outs[nouts++];
		memset(o, 0, sizeof *o);
		snprintf(o->name, sizeof o->name, "%s", oi->name);
		o->builtin = !strncmp(o->name, "eDP", 3) || !strncmp(o->name, "LVDS", 4) || !strncmp(o->name, "DSI", 3);
		o->primary = res->outputs[i] == prim;
		RRMode cur = None;
		if (oi->crtc) {
			XRRCrtcInfo *ci = XRRGetCrtcInfo(dpy, res, oi->crtc);
			if (ci) {
				o->on = ci->mode != None;
				cur = ci->mode;
				o->x = ci->x;
				o->y = ci->y;
				o->rot = ci->rotation & RR_Rotate_90 ? 1 : ci->rotation & RR_Rotate_180 ? 2 : ci->rotation & RR_Rotate_270 ? 3 : 0;
				XRRFreeCrtcInfo(ci);
			}
		}
		/* 把模式整理成「解析度 → 多個更新頻率」 */
		for (int m = 0; m < oi->nmode; m++)
			for (int k = 0; k < res->nmode; k++) {
				XRRModeInfo *mi = &res->modes[k];
				if (mi->id != oi->modes[m]) continue;
				int r;
				for (r = 0; r < o->nres; r++)
					if (o->res[r].w == (int)mi->width && o->res[r].h == (int)mi->height) break;
				if (r == o->nres) {
					if (o->nres >= (int)LEN(o->res)) break;
					o->res[r] = (Res){ (int)mi->width, (int)mi->height, 0, { 0 }, { 0 }, 0 };
					o->nres++;
				}
				Res *rs = &o->res[r];
				if (m < oi->npreferred) rs->preferred = 1;
				double rate = mode_rate(mi);
				int dup = 0;
				for (int q = 0; q < rs->nrates; q++) if (fabs(rs->rates[q] - rate) < 0.05) dup = 1;
				if (!dup && rs->nrates < (int)LEN(rs->rates)) {
					if (mi->id == cur) { o->cur_res = r; o->cur_rate = rs->nrates; }
					rs->ids[rs->nrates] = mi->id;
					rs->rates[rs->nrates++] = rate;
				} else if (mi->id == cur) {
					o->cur_res = r;
				}
			}
		XRRFreeOutputInfo(oi);
	}
	XRRFreeScreenResources(res);
	guess_positions();
}

/* 組出 xrandr 參數並執行（不經過 shell） */
static void apply(const Out *v, int n)
{
	if (wl_mode) { apply_wl(v, n); return; }
	char *argv[200];
	char buf[64][48];
	int k = 0, b = 0;
	argv[k++] = "xrandr";
	const Out *p = NULL;
	for (int i = 0; i < n; i++) if (v[i].primary && v[i].on) p = &v[i];
	for (int i = 0; i < n && k < 180; i++) {
		const Out *o = &v[i];
		argv[k++] = "--output";
		argv[k++] = (char *)o->name;
		if (!o->on) { argv[k++] = "--off"; continue; }
		const Res *r = &o->res[o->cur_res];
		/* 用模式編號指定：模式名稱不一定是「寬x高」（例如 1920x1080i 或自訂模式） */
		if (r->nrates) snprintf(buf[b], 48, "0x%lx", (unsigned long)r->ids[o->cur_rate]);
		else snprintf(buf[b], 48, "%dx%d", r->w, r->h);
		argv[k++] = "--mode"; argv[k++] = buf[b++];
		argv[k++] = "--rotate"; argv[k++] = (char *)rot_args[o->rot];
		if (o == p) {
			argv[k++] = "--primary";
			argv[k++] = "--pos"; argv[k++] = "0x0";
		} else if (p) {
			argv[k++] = (char *)pos_args[o->pos];
			argv[k++] = (char *)p->name;
		}
	}
	for (int i = 0; i < ngone && k < 196; i++) {   /* 拔掉的螢幕：關掉，釋放它佔的畫面 */
		argv[k++] = "--output";
		argv[k++] = gone[i];
		argv[k++] = "--off";
	}
	argv[k] = NULL;
	pid_t pid = fork();
	if (pid == 0) {
		if (dpy) close(ConnectionNumber(dpy));
		execvp("xrandr", argv);
		_exit(127);
	}
	if (pid > 0) waitpid(pid, NULL, 0);
}

/* 保留：寫進設定，下次登入自動套用 */
static void save(void)
{
	for (int i = 0; i < nouts; i++) {
		Out *o = &outs[i];
		const Res *r = &o->res[o->cur_res];
		char key[64], val[96];
		snprintf(key, sizeof key, "display.%s", o->name);
		snprintf(val, sizeof val, "%d,%dx%d,%.2f,%d,%d,%d", o->on, r->w, r->h, r->nrates ? r->rates[o->cur_rate] : 0.0,
		         o->rot, o->pos, o->primary);
		pm_conf_set(key, val);
	}
}

/* 照記住的設定（display.<輸出名稱>）設定這個螢幕；沒記過、或記的解析度現在沒有，回傳 0 */
static int load_saved(Out *o)
{
	char key[64];
	snprintf(key, sizeof key, "display.%s", o->name);
	const char *v = pm_conf_get(key, NULL);
	int on, w, h, rot, pos, prim;
	double rate;
	if (!v || sscanf(v, "%d,%dx%d,%lf,%d,%d,%d", &on, &w, &h, &rate, &rot, &pos, &prim) != 7) return 0;
	for (int r = 0; r < o->nres; r++) {
		if (o->res[r].w != w || o->res[r].h != h) continue;
		o->cur_res = r;
		o->cur_rate = 0;
		for (int q = 0; q < o->res[r].nrates; q++) if (fabs(o->res[r].rates[q] - rate) < 0.05) o->cur_rate = q;
		o->on = on;
		o->rot = CLAMP(rot, 0, 3);
		o->pos = CLAMP(pos, 0, 4);
		o->primary = prim;
		return 1;
	}
	return 0;
}

/* 插上螢幕之後該怎麼排（names：新接上的輸出）。記住過的照設定；沒設定過的用建議的解析度，
 * 放在主要顯示器旁邊還沒被佔的一邊（右、左、上、下）。主要顯示器沒開著（例如被拔掉了）就改用
 * 內建螢幕或第一個開著的。回傳 1 = 要套用 */
static int plan_hotplug(Out *o, int n, char *const *names, int nn)
{
	int changed = 0;
	for (int i = 0; i < n; i++) {
		int fresh = 0;
		for (int k = 0; k < nn; k++) if (!strcmp(names[k], o[i].name)) fresh = 1;
		if (!fresh || o[i].on) continue;
		if (load_saved(&o[i])) {
			if (o[i].on) {
				changed = 1;
				if (o[i].primary) for (int k = 0; k < n; k++) if (k != i) o[k].primary = 0;
			}
			continue;
		}
		int r = 0;
		for (int k = 0; k < o[i].nres; k++) if (o[i].res[k].preferred) { r = k; break; }
		o[i].cur_res = r;
		o[i].cur_rate = 0;
		o[i].rot = 0;
		o[i].on = 1;
		o[i].primary = 0;
		int used[4] = { 0 };
		for (int k = 0; k < n; k++) if (k != i && o[k].on && !o[k].primary && o[k].pos < 4) used[o[k].pos] = 1;
		o[i].pos = 0;
		for (int k = 0; k < 4; k++) if (!used[k]) { o[i].pos = k; break; }
		changed = 1;
	}
	int p = -1;
	for (int i = 0; i < n; i++) if (o[i].on && o[i].primary && p < 0) p = i;
	if (p < 0) {
		for (int i = 0; i < n && p < 0; i++) if (o[i].on && o[i].builtin) p = i;
		for (int i = 0; i < n && p < 0; i++) if (o[i].on) p = i;
		for (int i = 0; i < n; i++) o[i].primary = i == p;
		if (p >= 0) changed = 1;
	}
	return changed;
}

/* --hotplug 輸出…：視窗管理員在接上或拔掉螢幕後呼叫 */
static int hotplug(char *const *names, int nn)
{
	read_outputs();
	int changed = plan_hotplug(outs, nouts, names, nn) || ngone > 0;
	int lit = 0;
	for (int i = 0; i < nouts; i++) lit += outs[i].on;
	if (changed && lit) apply(outs, nouts);
	return 0;
}

/* --apply：依設定檔套用（登入時呼叫）；設定的模式不存在就略過那個螢幕 */
static int apply_saved(void)
{
	read_outputs();
	int any = 0;
	for (int i = 0; i < nouts; i++) any |= load_saved(&outs[i]);
	/* 至少保留一個開著的螢幕 */
	int lit = 0;
	for (int i = 0; i < nouts; i++) lit += outs[i].on;
	if (any && lit) apply(outs, nouts);
	return 0;
}

/* ------------------------------------------------------------------ */
/* 畫面                                                                */
/* ------------------------------------------------------------------ */

static void start_confirm(void)
{
	apply(outs, nouts);
	revert_at = pm_now_ms() + 15000;
	status[0] = 0;
}

static void build(PmUI *u)
{
	pm_ui_title(u, _("顯示器"), NULL);
	if (!nouts) {
		pm_ui_group(u, NULL);
		pm_ui_info(u, _("找不到顯示器資訊"), NULL);
		pm_ui_end(u);
		return;
	}
	if (revert_at) {
		char msg[128];
		snprintf(msg, sizeof msg, _("%ld 秒後還原"), MAX(0, (revert_at - pm_now_ms() + 999) / 1000));
		pm_ui_group(u, _("要保留這些顯示器設定嗎？"));
		pm_ui_info(u, msg, NULL);
		if (pm_ui_button(u, NULL, _("保留"))) { revert_at = 0; save(); snprintf(status, sizeof status, "%s", _("已儲存，下次登入也會套用")); }
		if (pm_ui_button(u, NULL, _("還原"))) {
			revert_at = 0;
			memcpy(outs, saved, sizeof outs);
			apply(outs, nouts);
			snprintf(status, sizeof status, "%s", _("已還原"));
		}
		pm_ui_end(u);
		return;
	}
	int lit = 0;
	for (int i = 0; i < nouts; i++) lit += outs[i].on;
	for (int i = 0; i < nouts; i++) {
		Out *o = &outs[i];
		char title[96];
		snprintf(title, sizeof title, "%s%s", o->builtin ? _("內建顯示器 ") : "", o->name);
		pm_ui_group(u, title);
		int changed = 0;
		if (nouts > 1) {
			int on = o->on;
			if (pm_ui_switch(u, _("使用這個顯示器"), &on) && (on || lit > 1)) { o->on = on; changed = 1; }
		}
		if (o->on) {
			static char labels[8][64][48];
			static const char *opts[8][64];
			for (int r = 0; r < o->nres; r++) {
				snprintf(labels[i][r], 48, "%d × %d%s", o->res[r].w, o->res[r].h, o->res[r].preferred ? _("（建議）") : "");
				opts[i][r] = labels[i][r];
			}
			int cr = o->cur_res;
			if (pm_ui_choice(u, _("解析度"), opts[i], o->nres, &cr)) { o->cur_res = cr; o->cur_rate = 0; changed = 1; }
			Res *rs = &o->res[o->cur_res];
			if (rs->nrates > 1) {
				static char rl[8][16][24];
				static const char *ro[8][16];
				for (int q = 0; q < rs->nrates; q++) { snprintf(rl[i][q], 24, "%.0f Hz", rs->rates[q]); ro[i][q] = rl[i][q]; }
				if (pm_ui_choice(u, _("更新頻率"), ro[i], rs->nrates, &o->cur_rate)) changed = 1;
			}
			if (pm_ui_choice(u, _("旋轉"), rot_names, LEN(rot_names), &o->rot)) changed = 1;
			if (nouts > 1) {
				int prim = o->primary;
				if (pm_ui_switch(u, _("主要顯示器（選單列與 Dock 在這裡）"), &prim) && prim) {
					for (int k = 0; k < nouts; k++) outs[k].primary = k == i;
					changed = 1;
				}
				if (!o->primary && pm_ui_choice(u, _("位置"), pos_names, LEN(pos_names), &o->pos)) changed = 1;
			}
		}
		pm_ui_end(u);
		if (changed) {
			start_confirm();
			pm_ui_redraw(u);
			return;
		}
	}
	if (status[0]) pm_ui_note(u, status);
	pm_ui_note(u, _("變更後 15 秒內要按「保留」，否則會自動還原。"));
}

static void tick(PmUI *u)
{
	if (!revert_at) {
		/* 接上或拔掉螢幕時更新清單 */
		static int n;
		if (++n % 3 == 0) {
			int old = nouts;
			char first[32];
			snprintf(first, sizeof first, "%s", nouts ? outs[0].name : "");
			read_outputs();
			memcpy(saved, outs, sizeof saved);
			if (old != nouts || strcmp(first, nouts ? outs[0].name : "")) pm_ui_redraw(u);
		}
		return;
	}
	if (pm_now_ms() >= revert_at) {   /* 時間到：還原 */
		revert_at = 0;
		memcpy(outs, saved, sizeof outs);
		apply(outs, nouts);
		snprintf(status, sizeof status, "%s", _("沒有確認，已自動還原"));
	}
	pm_ui_redraw(u);
}

static cairo_surface_t *icon(int size)
{
	/* 程式圖示：螢幕 */
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
	cairo_t *cr = cairo_create(s);
	cairo_scale(cr, size / 128.0, size / 128.0);
	pm_rrect(cr, 10, 10, 108, 108, 26, 26);
	cairo_pattern_t *g = cairo_pattern_create_linear(0, 10, 0, 118);
	cairo_pattern_add_color_stop_rgb(g, 0, 0.35, 0.55, 0.98);
	cairo_pattern_add_color_stop_rgb(g, 1, 0.18, 0.33, 0.85);
	cairo_set_source(cr, g);
	cairo_fill(cr);
	cairo_pattern_destroy(g);
	pm_rrect(cr, 26, 32, 76, 50, 6, 6);
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_fill(cr);
	pm_rrect(cr, 31, 37, 66, 40, 3, 3);
	cairo_pattern_t *sc = cairo_pattern_create_linear(31, 37, 97, 77);
	cairo_pattern_add_color_stop_rgb(sc, 0, 0.35, 0.70, 1.0);
	cairo_pattern_add_color_stop_rgb(sc, 1, 0.75, 0.50, 0.95);
	cairo_set_source(cr, sc);
	cairo_fill(cr);
	cairo_pattern_destroy(sc);
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_rectangle(cr, 58, 82, 12, 10);
	cairo_fill(cr);
	pm_rrect(cr, 44, 92, 40, 6, 3, 3);
	cairo_fill(cr);
	cairo_destroy(cr);
	return s;
}

int main(int argc, char *argv[])
{
	if (argc > 2 && !strcmp(argv[1], "--icon")) return pm_ui_write_icon(icon, argv[2]);
	setlocale(LC_ALL, "");
	pm_conf_load();
	{   /* Wayland 工作階段：compositor 有 wlr-output-management 就用它（XWayland 的 RandR 改不到真正的螢幕） */
		PmwHead probe[1];
		wl_mode = getenv("WAYLAND_DISPLAY") && pmwl_heads(probe, 1) >= 0;
	}
	if (wl_mode && argc > 1 && !strcmp(argv[1], "--apply")) return apply_saved();
	if (wl_mode && argc > 1 && !strcmp(argv[1], "--hotplug")) return hotplug(argv + 2, argc - 2);
	if (wl_mode) {
		read_outputs();
		memcpy(saved, outs, sizeof saved);
		pm_ui_native = 1;   /* 介面沒有直接呼叫 X：原生視窗 */
		return pm_ui_run(_("顯示器"), "pomelo-display", 640, 600, build, tick, 1000, icon);
	}
	if (argc > 1 && !strcmp(argv[1], "--apply")) {
		if (!(dpy = XOpenDisplay(NULL))) return 1;
		pm_root = DefaultRootWindow(dpy);
		return apply_saved();
	}
	if (argc > 1 && !strcmp(argv[1], "--hotplug")) {
		if (!(dpy = XOpenDisplay(NULL))) return 1;
		pm_root = DefaultRootWindow(dpy);
		return hotplug(argv + 2, argc - 2);
	}
	if (!(dpy = XOpenDisplay(NULL))) { fputs(_("pomelo-display: 無法開啟 X display\n"), stderr); return 1; }
	pm_root = DefaultRootWindow(dpy);
	read_outputs();
	memcpy(saved, outs, sizeof saved);
	return pm_ui_run(_("顯示器"), "pomelo-display", 640, 600, build, tick, 1000, icon);
}
