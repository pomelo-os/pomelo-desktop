/* SPDX-License-Identifier: GPL-3.0-or-later */
/* pomelo-bluetooth — Pomelo 藍牙設定
 *
 * 透過 bluetoothctl（BlueZ）：開關藍牙、搜尋、配對、連線 / 中斷、移除裝置、顯示電量。
 * 配對時把 bluetoothctl 當成背景對話程式：要確認配對碼或輸入 PIN 時在畫面上詢問，
 * 再把答案送回去。所有指令都在背景執行，不會卡住畫面。
 *
 * 搜尋時附近大部分的低功耗裝置（手機的隨機位址、追蹤器、電視…）不廣播名稱，BlueZ 就拿位址當名稱
 * （AA-BB-CC-DD-EE-FF）。這些先收起來（「另外 N 個沒有名稱的裝置」，點了才列出），有名稱的裝置
 * 查一次 bluetoothctl info 拿類型（耳機、滑鼠…）的圖示，結果記住，不會每秒重查。
 */
#define _GNU_SOURCE
#include <pomelo.h>
#include <fcntl.h>
#include <sys/wait.h>

typedef struct { char mac[18], name[128], icon[48]; int paired, connected, battery, noname; } Dev;

static Dev devs[128];
static int ndevs, have_bt = 1, adapter, powered, scanning, show_noname;

/* 沒配對的裝置查過的類型圖示（以位址記住，重新整理時不用再查） */
typedef struct { char mac[18], icon[48]; } IconCache;
static IconCache icache[128];
static int nicache;
static long scan_until;
static char status[256];

/* 配對中的 bluetoothctl */
enum { P_NONE, P_WAIT, P_CONFIRM, P_PIN, P_SHOW };
static int pstate;
static pid_t ppid;
static int pin_fd = -1, pout_fd = -1;
static char pbuf[8192], pmac[18], pname[128], passkey[32], pin[32];
static int plen, paired_ok;   /* paired_ok：已送出 trust / connect */
static long pdeadline;

/* ------------------------------------------------------------------ */
/* 讀取狀態                                                            */
/* ------------------------------------------------------------------ */

static int valid_mac(const char *m)
{
	if (strlen(m) != 17) return 0;
	for (int i = 0; i < 17; i++)
		if (i % 3 == 2 ? m[i] != ':' : !isxdigit((unsigned char)m[i])) return 0;
	return 1;
}

static Dev *find(const char *mac)
{
	for (int i = 0; i < ndevs; i++) if (!strcmp(devs[i].mac, mac)) return &devs[i];
	return NULL;
}

static void parse_devices(const char *cmd, int paired)
{
	char *t = pm_run(cmd);
	char *save = NULL;
	for (char *l = t ? strtok_r(t, "\n", &save) : NULL; l; l = strtok_r(NULL, "\n", &save)) {
		/* Device AA:BB:CC:DD:EE:FF 名稱 */
		if (strncmp(l, "Device ", 7) || strlen(l) < 24) continue;
		char mac[18];
		snprintf(mac, sizeof mac, "%.17s", l + 7);
		if (!valid_mac(mac)) continue;
		Dev *d = find(mac);
		if (!d && ndevs < (int)LEN(devs)) {
			d = &devs[ndevs++];
			memset(d, 0, sizeof *d);
			snprintf(d->mac, sizeof d->mac, "%s", mac);
			snprintf(d->name, sizeof d->name, "%s", l[24] ? l + 25 : mac);
			d->battery = -1;
			/* 不知道名稱時 BlueZ 的別名就是位址（冒號換成 -） */
			char dashed[18];
			snprintf(dashed, sizeof dashed, "%s", mac);
			for (char *c = dashed; *c; c++) if (*c == ':') *c = '-';
			d->noname = !strcasecmp(d->name, dashed) || !strcasecmp(d->name, mac) || !d->name[0];
		}
		if (d && paired) d->paired = 1;
	}
	free(t);
}

static void refresh(void)
{
	if (!pm_have("bluetoothctl")) { have_bt = 0; return; }
	char *t = pm_run("bluetoothctl show");
	adapter = t && strstr(t, "Controller ");
	powered = t && strstr(t, "Powered: yes");
	scanning = t && strstr(t, "Discovering: yes");
	free(t);
	ndevs = 0;
	if (!adapter) return;
	parse_devices("bluetoothctl devices Paired", 1);
	parse_devices("bluetoothctl devices", 0);
	/* 已配對的才查詳細資訊（連線狀態、圖示、電量） */
	for (int i = 0; i < ndevs; i++) {
		Dev *d = &devs[i];
		if (!d->paired) continue;
		char cmd[64];
		snprintf(cmd, sizeof cmd, "bluetoothctl info %s", d->mac);
		char *info = pm_run(cmd);
		if (!info) continue;
		d->connected = strstr(info, "Connected: yes") != NULL;
		char *p = strstr(info, "Icon: ");
		if (p) { snprintf(d->icon, sizeof d->icon, "%.*s", (int)strcspn(p + 6, "\n"), p + 6); }
		p = strstr(info, "Battery Percentage:");
		if (p && (p = strchr(p, '('))) d->battery = atoi(p + 1);
		free(info);
	}
	/* 沒配對、有名稱的：查一次類型（耳機、滑鼠…），記住 */
	for (int i = 0; i < ndevs; i++) {
		Dev *d = &devs[i];
		if (d->paired || d->noname) continue;
		int k;
		for (k = 0; k < nicache && strcmp(icache[k].mac, d->mac); k++) ;
		if (k == nicache) {
			if (nicache == (int)LEN(icache)) nicache = 0;   /* 滿了從頭蓋 */
			k = nicache++;
			snprintf(icache[k].mac, sizeof icache[k].mac, "%s", d->mac);
			icache[k].icon[0] = 0;
			char cmd[64];
			snprintf(cmd, sizeof cmd, "bluetoothctl info %s", d->mac);
			char *info = pm_run(cmd);
			char *p = info ? strstr(info, "Icon: ") : NULL;
			if (p) snprintf(icache[k].icon, sizeof icache[k].icon, "%.*s", (int)strcspn(p + 6, "\n"), p + 6);
			free(info);
		}
		snprintf(d->icon, sizeof d->icon, "%s", icache[k].icon);
	}
}

/* ------------------------------------------------------------------ */
/* 動作                                                                */
/* ------------------------------------------------------------------ */

/* 不需要互動的 bluetoothctl 指令，在背景執行 */
static void bt(const char *a1, const char *a2)
{
	if (fork() == 0) {
		if (dpy) close(ConnectionNumber(dpy));   /* 原生 Wayland（pm_ui_native）時沒有 X 連線 */
		int null = open("/dev/null", O_RDWR);
		dup2(null, 0); dup2(null, 1); dup2(null, 2);
		execlp("bluetoothctl", "bluetoothctl", a1, a2, (char *)NULL);
		_exit(127);
	}
}

static void pair_write(const char *s)
{
	if (pin_fd >= 0 && write(pin_fd, s, strlen(s)) < 0) { /* 對方已結束 */ }
}

static void pair_end(const char *msg)
{
	if (pin_fd >= 0) { pair_write("quit\n"); close(pin_fd); pin_fd = -1; }
	if (pout_fd >= 0) { close(pout_fd); pout_fd = -1; }
	if (ppid > 0) { kill(ppid, SIGTERM); waitpid(ppid, NULL, 0); ppid = 0; }
	pstate = P_NONE;
	if (msg) snprintf(status, sizeof status, "%s", msg);
}

static void pair_start(Dev *d)
{
	int in[2], out[2];
	if (ppid || pipe(in) || pipe(out)) return;
	signal(SIGCHLD, SIG_DFL);
	ppid = fork();
	if (ppid == 0) {
		dup2(in[0], 0);
		dup2(out[1], 1);
		dup2(out[1], 2);
		close(in[1]); close(out[0]);
		if (dpy) close(ConnectionNumber(dpy));   /* 原生 Wayland（pm_ui_native）時沒有 X 連線 */
		setenv("LC_ALL", "C.UTF-8", 1);
		execlp("bluetoothctl", "bluetoothctl", (char *)NULL);
		_exit(127);
	}
	close(in[0]);
	close(out[1]);
	pin_fd = in[1];
	pout_fd = out[0];
	fcntl(pout_fd, F_SETFL, O_NONBLOCK);
	plen = 0;
	snprintf(pmac, sizeof pmac, "%s", d->mac);
	snprintf(pname, sizeof pname, "%s", d->name);
	passkey[0] = pin[0] = 0;
	paired_ok = 0;
	pstate = P_WAIT;
	pdeadline = pm_now_ms() + 60000;
	char cmd[64];
	snprintf(cmd, sizeof cmd, "pair %s\n", d->mac);
	pair_write(cmd);
	snprintf(status, sizeof status, _("正在與「%s」配對…"), d->name);
}

/* 讀 bluetoothctl 的輸出，去掉顏色碼後找關鍵字 */
static void pair_poll(void)
{
	ssize_t k;
	while (pout_fd >= 0 && (k = read(pout_fd, pbuf + plen, sizeof pbuf - plen - 1)) > 0) {
		plen += (int)k;
		if (plen > (int)sizeof pbuf - 1024) { memmove(pbuf, pbuf + plen - 2048, 2048); plen = 2048; }
	}
	pbuf[plen] = 0;
	/* 去掉 ANSI 顏色碼與 \r */
	char clean[8192];
	int c = 0;
	for (int i = 0; i < plen && c < (int)sizeof clean - 1; i++) {
		if (pbuf[i] == 0x1b) { while (i < plen && !isalpha((unsigned char)pbuf[i])) i++; continue; }
		if (pbuf[i] == '\r' || pbuf[i] == 1 || pbuf[i] == 2) continue;
		clean[c++] = pbuf[i];
	}
	clean[c] = 0;
	char *p;
	if (pstate == P_WAIT && (p = strstr(clean, "Confirm passkey "))) {
		snprintf(passkey, sizeof passkey, "%.*s", (int)strspn(p + 16, "0123456789"), p + 16);
		pstate = P_CONFIRM;
	} else if (pstate == P_WAIT && (strstr(clean, "Enter PIN code") || strstr(clean, "Enter passkey"))) {
		pstate = P_PIN;
	} else if (pstate == P_WAIT && (p = strstr(clean, "Passkey: "))) {
		snprintf(passkey, sizeof passkey, "%.*s", (int)strspn(p + 9, "0123456789"), p + 9);
		pstate = P_SHOW;   /* 請使用者在藍牙鍵盤上打這組數字 */
	}
	if (!paired_ok && strstr(clean, "Pairing successful")) {
		/* 配對成功：設為信任（以後自動連線）並連線 */
		char cmd[96];
		snprintf(cmd, sizeof cmd, "trust %s\nconnect %s\n", pmac, pmac);
		pair_write(cmd);
		pstate = P_WAIT;
		paired_ok = 1;
	}
	if (strstr(clean, "Connection successful")) {
		char msg[200];
		snprintf(msg, sizeof msg, _("已連線到「%s」"), pname);
		pair_end(msg);
		refresh();
	} else if (strstr(clean, "Failed to pair") || strstr(clean, "Failed to connect") ||
	           strstr(clean, "AuthenticationFailed") || strstr(clean, "AuthenticationCanceled")) {
		pair_end(_("配對失敗，請確認裝置在配對模式後再試一次"));
		refresh();
	} else if (pm_now_ms() > pdeadline) {
		pair_end(_("配對逾時"));
	}
}

static Dev *menu_dev;
static void m_connect(const Arg *a) { (void)a; bt("connect", menu_dev->mac); snprintf(status, sizeof status, "%s", _("正在連線…")); }
static void m_disconnect(const Arg *a) { (void)a; bt("disconnect", menu_dev->mac); }
static void m_remove(const Arg *a) { (void)a; bt("remove", menu_dev->mac); }

static void device_menu(PmUI *u, Dev *d)
{
	menu_dev = d;
	MenuItem m[] = {
		d->connected ? (MenuItem){ _("中斷連線"), m_disconnect, {0} } : (MenuItem){ _("連線"), m_connect, {0} },
		{ NULL, NULL, {0} },
		{ _("移除這個裝置"), m_remove, {0} },
	};
	pm_ui_menu(u, m, LEN(m));
}

/* ------------------------------------------------------------------ */
/* 畫面                                                                */
/* ------------------------------------------------------------------ */

static const char *dev_icon(const Dev *d)
{
	if (!d->icon[0]) return "bluetooth";
	if (strstr(d->icon, "audio") || strstr(d->icon, "headset") || strstr(d->icon, "headphone")) return "audio-headphones";
	if (strstr(d->icon, "mouse")) return "input-mouse";
	if (strstr(d->icon, "keyboard")) return "input-keyboard";
	if (strstr(d->icon, "phone")) return "phone";
	if (strstr(d->icon, "gaming")) return "input-gaming";
	return d->icon;
}

static void build(PmUI *u)
{
	pm_ui_title(u, _("藍牙"), NULL);
	if (!have_bt || !adapter) {
		pm_ui_group(u, NULL);
		pm_ui_info(u, !have_bt ? _("需要安裝 bluez") : _("找不到藍牙硬體"), NULL);
		pm_ui_end(u);
		return;
	}
	pm_ui_group(u, NULL);
	int on = powered;
	if (pm_ui_switch(u, _("藍牙"), &on)) { bt("power", on ? "on" : "off"); powered = on; }
	pm_ui_end(u);
	if (!powered) { pm_ui_note(u, _("藍牙已關閉。")); return; }

	/* 配對中的對話 */
	if (pstate != P_NONE) {
		char title[200];
		snprintf(title, sizeof title, _("與「%s」配對"), pname);
		pm_ui_group(u, title);
		if (pstate == P_WAIT) {
			pm_ui_info(u, _("等待裝置回應…"), NULL);
		} else if (pstate == P_CONFIRM) {
			pm_ui_info(u, _("確認裝置上顯示的配對碼"), passkey);
			if (pm_ui_button(u, NULL, _("配對"))) { pair_write("yes\n"); pstate = P_WAIT; }
		} else if (pstate == P_SHOW) {
			pm_ui_info(u, _("請在裝置上輸入這組數字，然後按 Enter"), passkey);
		} else if (pstate == P_PIN) {
			if (pm_ui_entry(u, _("PIN 碼（通常是 0000 或 1234）"), pin, sizeof pin, 0) && pin[0]) {
				char line[40];
				snprintf(line, sizeof line, "%s\n", pin);
				pair_write(line);
				pstate = P_WAIT;
			}
		}
		if (pm_ui_button(u, NULL, _("取消"))) pair_end(_("已取消配對"));
		pm_ui_end(u);
	}

	pm_ui_group(u, _("我的裝置"));
	int np = 0;
	for (int i = 0; i < ndevs; i++) {
		Dev *d = &devs[i];
		if (!d->paired) continue;
		np++;
		char sub[64];
		if (d->battery >= 0) snprintf(sub, sizeof sub, _("%s・電量 %d%%"), d->connected ? _("已連線") : _("未連線"), d->battery);
		else snprintf(sub, sizeof sub, "%s", d->connected ? _("已連線") : _("未連線"));
		if (pm_ui_item(u, dev_icon(d), d->name, sub, NULL, d->connected) && pstate == P_NONE) device_menu(u, d);
	}
	if (!np) pm_ui_info(u, _("還沒有配對的裝置"), NULL);
	pm_ui_end(u);

	pm_ui_group(u, _("其他裝置"));
	int no = 0, nn = 0;
	for (int i = 0; i < ndevs; i++) {
		Dev *d = &devs[i];
		if (d->paired) continue;
		if (d->noname) { nn++; continue; }
		no++;
		if (pm_ui_item(u, dev_icon(d), d->name, d->mac, _("配對"), 0) && pstate == P_NONE) pair_start(d);
	}
	int searching = scanning || pm_now_ms() < scan_until;
	if (!no) pm_ui_info(u, searching ? _("正在搜尋附近的裝置…") : _("按「搜尋」尋找附近的裝置"), NULL);
	/* 沒有名稱的：通常是附近的手機、追蹤器、電視的廣播，不能（也不需要）配對，先收起來 */
	if (nn) {
		char t[96];
		snprintf(t, sizeof t, _("另外 %d 個沒有名稱的裝置"), nn);
		if (pm_ui_item(u, NULL, t, show_noname ? NULL : _("通常是附近的手機、手環或電視，不能配對"), show_noname ? _("收起") : _("顯示"), 0))
			show_noname = !show_noname;
		for (int i = 0; show_noname && i < ndevs; i++) {
			Dev *d = &devs[i];
			if (d->paired || !d->noname) continue;
			if (pm_ui_item(u, "bluetooth", _("沒有名稱的裝置"), d->mac, _("配對"), 0) && pstate == P_NONE) pair_start(d);
		}
	}
	if (!searching && pm_ui_button(u, NULL, _("搜尋"))) {
		if (fork() == 0) {
			if (dpy) close(ConnectionNumber(dpy));   /* 原生 Wayland（pm_ui_native）時沒有 X 連線 */
			int null = open("/dev/null", O_RDWR);
			dup2(null, 0); dup2(null, 1); dup2(null, 2);
			execlp("bluetoothctl", "bluetoothctl", "--timeout", "20", "scan", "on", (char *)NULL);
			_exit(127);
		}
		scan_until = pm_now_ms() + 20000;
	}
	pm_ui_end(u);
	pm_ui_note(u, status[0] ? status : _("要配對的裝置請先切換到配對模式（通常是長按電源或藍牙鍵）。"));
}

static void tick(PmUI *u)
{
	static int n;
	if (pstate != P_NONE) { pair_poll(); pm_ui_redraw(u); }
	if (ppid == 0) while (waitpid(-1, NULL, WNOHANG) > 0);
	/* 每 3 秒（搜尋中每秒）重新整理 */
	if (++n % (pm_now_ms() < scan_until ? 1 : 3) == 0 && pstate == P_NONE) { refresh(); pm_ui_redraw(u); }
}

static cairo_surface_t *icon(int size)
{
	/* 程式圖示：藍牙符號 */
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
	cairo_t *cr = cairo_create(s);
	cairo_scale(cr, size / 128.0, size / 128.0);
	pm_rrect(cr, 10, 10, 108, 108, 26, 26);
	cairo_pattern_t *g = cairo_pattern_create_linear(0, 10, 0, 118);
	cairo_pattern_add_color_stop_rgb(g, 0, 0.20, 0.55, 1.0);
	cairo_pattern_add_color_stop_rgb(g, 1, 0.05, 0.35, 0.85);
	cairo_set_source(cr, g);
	cairo_fill(cr);
	cairo_pattern_destroy(g);
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_set_line_width(cr, 8);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
	cairo_move_to(cr, 44, 46);
	cairo_line_to(cr, 82, 80);
	cairo_line_to(cr, 64, 97);
	cairo_line_to(cr, 64, 31);
	cairo_line_to(cr, 82, 48);
	cairo_line_to(cr, 44, 82);
	cairo_stroke(cr);
	cairo_destroy(cr);
	return s;
}

int main(int argc, char *argv[])
{
	if (argc > 2 && !strcmp(argv[1], "--icon")) return pm_ui_write_icon(icon, argv[2]);
	setlocale(LC_ALL, "");
	signal(SIGPIPE, SIG_IGN);
	pm_conf_load();
	refresh();
	pm_ui_native = 1;   /* build() 裡沒有直接呼叫 X：Wayland 工作階段直接當原生程式 */
	return pm_ui_run(_("藍牙"), "pomelo-bluetooth", 620, 620, build, tick, 1000, icon);
}
