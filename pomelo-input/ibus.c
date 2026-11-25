/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ibus.c — 用 IBus 的 D-Bus 協定服務 Qt 程式
 *
 * Qt 沒有 XIM，但 Qt5、Qt6 都內建 IBus 的輸入法外掛（libibusplatforminputcontextplugin，跟著 Qt 一起裝好）。
 * 那個外掛讀 ~/.config/ibus/bus/<machine-id>-<主機>-<顯示器編號> 裡的 IBUS_ADDRESS，連上去當作一條 D-Bus 匯流排，
 * 叫 org.freedesktop.IBus 的 CreateInputContext，之後每個按鍵送 ProcessKeyEvent、打好的字等 CommitText 訊號。
 * 這裡用 libdbus 開一個只有自己一個服務的小「匯流排」：回答 Hello、GetNameOwner 這些匯流排的方法，
 * 自己的唯一名稱是 :1.0（訊號都標成從 :1.0 送出，Qt 才認得）。只接受同一個使用者的連線（libdbus 預設）。
 * pomelo-session 在用 Pomelo 輸入法時設 QT_IM_MODULE=ibus。選字列與 XIM 的程式共用（游標位置用 SetCursorLocation）。
 *
 * GTK4 沒有 XIM，它的 IBus 模組（ibus-gtk4，例如小柚瀏覽器）走同一套協定，但另外會在一般的 session D-Bus 上
 * 監看 org.freedesktop.IBus 這個名稱：沒人擁有就當作 IBus 沒在跑，按鍵不送過來（Qt 的外掛不看這個）。
 * 所以 session bus 上也登記這個名稱（只是佔著名稱，真的 ibus-daemon 已經在跑就不搶）。
 */
#define _GNU_SOURCE
#include "ic.h"
#include <dbus/dbus.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <unistd.h>

#define BUS_NAME "org.freedesktop.IBus"
#define OWNER ":1.0"                       /* 我們自己（IBus 服務）的唯一名稱 */
#define IC_IFACE "org.freedesktop.IBus.InputContext"
#define IC_PREFIX "/org/freedesktop/IBus/InputContext_"
#define RELEASE_MASK (1u << 30)            /* IBUS_RELEASE_MASK */

typedef struct Client {
	DBusConnection *c;
	char name[24];                         /* 給它的唯一名稱（Hello 的回答） */
	IC *ics;
	struct Client *next;
} Client;

static DBusServer *server;
static DBusConnection *session;          /* 一般的 session bus：只用來佔 org.freedesktop.IBus 這個名稱 */
static Client *clients;
static DBusWatch *watches[128];
static int nwatches, client_serial = 1, ic_serial = 1;
static char addr_file[PATH_MAX], addr_file_wl[PATH_MAX];   /* X 顯示器的、Wayland 的（原生 Wayland 的 Qt 照 WAYLAND_DISPLAY 找） */

/* ---- libdbus 的事件：自己用 select 等 ---- */

static dbus_bool_t add_watch(DBusWatch *w, void *d) { (void)d; if (nwatches < 128) watches[nwatches++] = w; return TRUE; }
static void remove_watch(DBusWatch *w, void *d)
{
	(void)d;
	for (int i = 0; i < nwatches; i++) if (watches[i] == w) { watches[i] = watches[--nwatches]; break; }
}
static void toggle_watch(DBusWatch *w, void *d) { (void)w; (void)d; }
/* 逾時只用在認證與等回覆（我們不發出呼叫），不處理 */
static dbus_bool_t add_timeout(DBusTimeout *t, void *d) { (void)t; (void)d; return TRUE; }
static void remove_timeout(DBusTimeout *t, void *d) { (void)t; (void)d; }

int ibus_fds(void *rfds, void *wfds, int maxfd)
{
	for (int i = 0; i < nwatches; i++) {
		if (!dbus_watch_get_enabled(watches[i])) continue;
		int fd = dbus_watch_get_unix_fd(watches[i]);
		unsigned f = dbus_watch_get_flags(watches[i]);
		if (fd < 0) continue;
		if (f & DBUS_WATCH_READABLE) FD_SET(fd, (fd_set *)rfds);
		if (f & DBUS_WATCH_WRITABLE) FD_SET(fd, (fd_set *)wfds);
		if (fd > maxfd) maxfd = fd;
	}
	return maxfd;
}

static void dispatch_all(void)
{
	if (session)   /* 有人呼叫我們在 session bus 上的名稱：沒處理的方法 libdbus 會自動回答「不認得」 */
		while (dbus_connection_dispatch(session) == DBUS_DISPATCH_DATA_REMAINS) ;
	for (Client *c = clients, *n; c; c = n) {
		n = c->next;
		dbus_connection_ref(c->c);
		DBusConnection *conn = c->c;
		while (dbus_connection_dispatch(conn) == DBUS_DISPATCH_DATA_REMAINS) ;
		dbus_connection_unref(conn);
	}
}

void ibus_handle(void *rfds, void *wfds)
{
	/* 處理的時候 watches 可能變動：先抄一份 */
	DBusWatch *ws[128];
	int n = nwatches;
	memcpy(ws, watches, sizeof(DBusWatch *) * (size_t)n);
	for (int i = 0; i < n; i++) {
		int still = 0;
		for (int k = 0; k < nwatches; k++) still |= watches[k] == ws[i];
		if (!still || !dbus_watch_get_enabled(ws[i])) continue;
		int fd = dbus_watch_get_unix_fd(ws[i]);
		unsigned f = 0;
		if (fd >= 0 && FD_ISSET(fd, (fd_set *)rfds)) f |= DBUS_WATCH_READABLE;
		if (fd >= 0 && FD_ISSET(fd, (fd_set *)wfds)) f |= DBUS_WATCH_WRITABLE;
		if (f) dbus_watch_handle(ws[i], f);
	}
	dispatch_all();
}

/* ---- 訊息 ---- */

static void send_reply(DBusConnection *c, DBusMessage *r, const char *sender)
{
	if (!r) return;
	dbus_message_set_sender(r, sender);
	dbus_connection_send(c, r, NULL);
	dbus_message_unref(r);
}

static void reply_empty(DBusConnection *c, DBusMessage *m) { send_reply(c, dbus_message_new_method_return(m), OWNER); }

static void reply_error(DBusConnection *c, DBusMessage *m, const char *name, const char *text)
{
	send_reply(c, dbus_message_new_error(m, name, text), OWNER);
}

static IC *find_ic(Client *cl, const char *path)
{
	for (IC *ic = cl->ics; ic; ic = ic->next) if (path && !strcmp(ic->path, path)) return ic;
	return NULL;
}

static void client_gone(Client *cl)
{
	for (IC *ic = cl->ics, *n; ic; ic = n) { n = ic->next; ic_destroy(ic); }
	cl->ics = NULL;
	for (Client **p = &clients; *p; p = &(*p)->next)
		if (*p == cl) { *p = cl->next; break; }
	dbus_connection_unref(cl->c);
	free(cl);
}

/* 匯流排本身的方法（org.freedesktop.DBus）：只有我們一個服務 */
static void bus_method(Client *cl, DBusMessage *m, const char *member)
{
	DBusConnection *c = cl->c;
	DBusMessage *r = NULL;
	const char *arg = NULL;
	dbus_message_get_args(m, NULL, DBUS_TYPE_STRING, &arg, DBUS_TYPE_INVALID);
	if (!strcmp(member, "Hello")) {
		const char *n = cl->name;
		r = dbus_message_new_method_return(m);
		dbus_message_append_args(r, DBUS_TYPE_STRING, &n, DBUS_TYPE_INVALID);
	} else if (!strcmp(member, "GetNameOwner")) {
		const char *owner = arg && (!strcmp(arg, BUS_NAME) || !strcmp(arg, OWNER)) ? OWNER : arg && !strcmp(arg, cl->name) ? cl->name : NULL;
		if (!owner) r = dbus_message_new_error(m, "org.freedesktop.DBus.Error.NameHasNoOwner", "no such name");
		else { r = dbus_message_new_method_return(m); dbus_message_append_args(r, DBUS_TYPE_STRING, &owner, DBUS_TYPE_INVALID); }
	} else if (!strcmp(member, "NameHasOwner")) {
		dbus_bool_t has = arg && (!strcmp(arg, BUS_NAME) || !strcmp(arg, OWNER) || !strcmp(arg, cl->name));
		r = dbus_message_new_method_return(m);
		dbus_message_append_args(r, DBUS_TYPE_BOOLEAN, &has, DBUS_TYPE_INVALID);
	} else if (!strcmp(member, "RequestName") || !strcmp(member, "ReleaseName") || !strcmp(member, "StartServiceByName")) {
		dbus_uint32_t v = !strcmp(member, "StartServiceByName") ? 2 : 1;
		r = dbus_message_new_method_return(m);
		dbus_message_append_args(r, DBUS_TYPE_UINT32, &v, DBUS_TYPE_INVALID);
	} else if (!strcmp(member, "ListNames") || !strcmp(member, "ListActivatableNames")) {
		const char *names[] = { "org.freedesktop.DBus", BUS_NAME, OWNER };
		const char **pn = names;
		r = dbus_message_new_method_return(m);
		dbus_message_append_args(r, DBUS_TYPE_ARRAY, DBUS_TYPE_STRING, &pn, 3, DBUS_TYPE_INVALID);
	} else if (!strcmp(member, "GetId")) {
		const char *id = "pomeloinput00000000000000000000";
		r = dbus_message_new_method_return(m);
		dbus_message_append_args(r, DBUS_TYPE_STRING, &id, DBUS_TYPE_INVALID);
	} else if (!strcmp(member, "AddMatch") || !strcmp(member, "RemoveMatch")) {
		r = dbus_message_new_method_return(m);   /* 只有一個服務，訊號直接送給要的人，不用過濾 */
	} else {
		r = dbus_message_new_error(m, "org.freedesktop.DBus.Error.UnknownMethod", member);
	}
	send_reply(c, r, "org.freedesktop.DBus");
}

/* 輸入框的屬性（org.freedesktop.DBus.Properties）。新版 IBus（1.5.28 起）的 GTK 模組用同步模式時，每個按鍵處理完
 * 會讀 PostProcessKeyEvent，取回這次按鍵產生的事件（打好的字、要轉送的鍵）：格式是 (a(yv))。
 * 我們打好的字已經用 CommitText 訊號送出了，這裡回答空的清單就好。回答的格式不對（或回錯誤），
 * ibus 函式庫會直接 g_assert 讓程式結束，所以一定要回答正確的型別 */
static void ic_properties(Client *cl, DBusMessage *m, const char *member)
{
	DBusConnection *c = cl->c;
	const char *iface = NULL, *prop = NULL;
	if (!strcmp(member, "Set")) { reply_empty(c, m); return; }
	DBusMessage *r = dbus_message_new_method_return(m);
	if (!r) return;
	DBusMessageIter it, var, st, arr;
	dbus_message_iter_init_append(r, &it);
	if (!strcmp(member, "GetAll")) {
		dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &arr);
		dbus_message_iter_close_container(&it, &arr);
	} else if (!strcmp(member, "Get") &&
	           dbus_message_get_args(m, NULL, DBUS_TYPE_STRING, &iface, DBUS_TYPE_STRING, &prop, DBUS_TYPE_INVALID) &&
	           !strcmp(prop, "PostProcessKeyEvent")) {
		dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT, "(a(yv))", &var);
		dbus_message_iter_open_container(&var, DBUS_TYPE_STRUCT, NULL, &st);
		dbus_message_iter_open_container(&st, DBUS_TYPE_ARRAY, "(yv)", &arr);
		dbus_message_iter_close_container(&st, &arr);
		dbus_message_iter_close_container(&var, &st);
		dbus_message_iter_close_container(&it, &var);
	} else {
		dbus_message_unref(r);
		reply_error(c, m, "org.freedesktop.DBus.Error.UnknownProperty", prop ? prop : member);
		return;
	}
	send_reply(c, r, OWNER);
}

static void ic_method(Client *cl, IC *ic, DBusMessage *m, const char *member)
{
	DBusConnection *c = cl->c;
	const char *iface = dbus_message_get_interface(m);
	if (iface && !strcmp(iface, "org.freedesktop.DBus.Properties")) { ic_properties(cl, m, member); return; }
	if (!strcmp(member, "ProcessKeyEvent")) {
		dbus_uint32_t keyval = 0, keycode = 0, state = 0;
		dbus_message_get_args(m, NULL, DBUS_TYPE_UINT32, &keyval, DBUS_TYPE_UINT32, &keycode, DBUS_TYPE_UINT32, &state, DBUS_TYPE_INVALID);
		/* IBus 的修飾鍵：低 8 位元跟 X 一樣；Super（1<<26）、Meta（1<<28）當成 X 的 Mod4 */
		unsigned xs = state & 0xff;
		if (state & ((1u << 26) | (1u << 28))) xs |= 64;
		/* 打好的字（CommitText 訊號）在 ic_key 裡就送出，比這個回覆早到：程式先收到字，再處理沒吃掉的鍵 */
		dbus_bool_t ate = ic_key(ic, keyval, xs, !(state & RELEASE_MASK)) != 0;
		DBusMessage *r = dbus_message_new_method_return(m);
		if (r) dbus_message_append_args(r, DBUS_TYPE_BOOLEAN, &ate, DBUS_TYPE_INVALID);
		send_reply(c, r, OWNER);
		return;
	}
	if (!strcmp(member, "SetCursorLocation") || !strcmp(member, "SetCursorLocationRelative")) {
		dbus_int32_t x = 0, y = 0, w = 0, h = 0;
		dbus_message_get_args(m, NULL, DBUS_TYPE_INT32, &x, DBUS_TYPE_INT32, &y, DBUS_TYPE_INT32, &w, DBUS_TYPE_INT32, &h, DBUS_TYPE_INVALID);
		ic->cx = x; ic->cy = y; ic->cw = w; ic->ch = h;
		ic->rel_cursor = strcmp(member, "SetCursorLocation") != 0;
		ic->has_cursor = 1;   /* 相對座標：焦點在原生 Wayland 視窗時加上它的位置（見 pomelo-input.c 的 place_win） */
		if (dbus_message_get_no_reply(m) == FALSE) reply_empty(c, m);
		ic_cursor_moved(ic);
		return;
	}
	if (!strcmp(member, "FocusIn")) ic_focus(ic, 1);
	else if (!strcmp(member, "FocusOut")) ic_focus(ic, 0);
	else if (!strcmp(member, "Reset")) ic_reset(ic);
	else if (!strcmp(member, "Destroy")) {
		for (IC **p = &cl->ics; *p; p = &(*p)->next) if (*p == ic) { *p = ic->next; break; }
		reply_empty(c, m);
		ic_destroy(ic);
		return;
	}
	/* SetCapabilities、SetSurroundingText、SetContentType、PropertyActivate…：記下來也用不到，回答成功就好 */
	reply_empty(c, m);
}

static DBusHandlerResult filter(DBusConnection *c, DBusMessage *m, void *data)
{
	Client *cl = data;
	if (dbus_message_is_signal(m, DBUS_INTERFACE_LOCAL, "Disconnected")) { client_gone(cl); return DBUS_HANDLER_RESULT_HANDLED; }
	if (dbus_message_get_type(m) != DBUS_MESSAGE_TYPE_METHOD_CALL) return DBUS_HANDLER_RESULT_HANDLED;
	const char *iface = dbus_message_get_interface(m), *member = dbus_message_get_member(m), *path = dbus_message_get_path(m);
	if (getenv("POMELO_INPUT_DEBUG")) fprintf(stderr, "ibus %s: %s %s.%s\n", cl->name, path ? path : "-", iface ? iface : "-", member ? member : "-");
	if (!member) return DBUS_HANDLER_RESULT_HANDLED;
	if (iface && !strcmp(iface, "org.freedesktop.DBus")) bus_method(cl, m, member);
	else if (iface && !strcmp(iface, "org.freedesktop.DBus.Peer")) {
		if (!strcmp(member, "GetMachineId")) {
			char *id = dbus_get_local_machine_id();
			DBusMessage *r = dbus_message_new_method_return(m);
			const char *s = id ? id : "";
			dbus_message_append_args(r, DBUS_TYPE_STRING, &s, DBUS_TYPE_INVALID);
			send_reply(c, r, OWNER);
			dbus_free(id);
		} else reply_empty(c, m);
	} else if (iface && !strcmp(iface, "org.freedesktop.DBus.Introspectable")) {
		const char *xml = "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\" "
		                  "\"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\"><node/>";
		DBusMessage *r = dbus_message_new_method_return(m);
		dbus_message_append_args(r, DBUS_TYPE_STRING, &xml, DBUS_TYPE_INVALID);
		send_reply(c, r, OWNER);
	} else if (path && !strcmp(path, "/org/freedesktop/IBus") && !strcmp(member, "CreateInputContext")) {
		IC *ic = ic_new();
		if (!ic) { reply_error(c, m, "org.freedesktop.DBus.Error.NoMemory", "out of memory"); return DBUS_HANDLER_RESULT_HANDLED; }
		ic->ibus = 1;
		ic->conn = c;
		snprintf(ic->path, sizeof ic->path, IC_PREFIX "%d", ic_serial++);
		ic->next = cl->ics;
		cl->ics = ic;
		const char *p = ic->path;
		DBusMessage *r = dbus_message_new_method_return(m);
		dbus_message_append_args(r, DBUS_TYPE_OBJECT_PATH, &p, DBUS_TYPE_INVALID);
		send_reply(c, r, OWNER);
	} else if (path && !strncmp(path, IC_PREFIX, strlen(IC_PREFIX))) {
		IC *ic = find_ic(cl, path);
		if (ic) ic_method(cl, ic, m, member);
		else reply_error(c, m, "org.freedesktop.DBus.Error.UnknownObject", path);
	} else if (iface && !strcmp(iface, "org.freedesktop.DBus.Properties") && !strcmp(member, "Set")) {
		reply_empty(c, m);
	} else {
		reply_error(c, m, "org.freedesktop.DBus.Error.UnknownMethod", member);
	}
	dbus_connection_flush(c);
	return DBUS_HANDLER_RESULT_HANDLED;
}

static void new_connection(DBusServer *s, DBusConnection *c, void *data)
{
	(void)s; (void)data;
	Client *cl = calloc(1, sizeof *cl);
	if (!cl) return;
	cl->c = dbus_connection_ref(c);
	snprintf(cl->name, sizeof cl->name, ":1.%d", client_serial++);
	dbus_connection_set_watch_functions(c, add_watch, remove_watch, toggle_watch, NULL, NULL);
	dbus_connection_set_timeout_functions(c, add_timeout, remove_timeout, NULL, NULL, NULL);
	dbus_connection_add_filter(c, filter, cl, NULL);
	cl->next = clients;
	clients = cl;
}

/* 送出打好的字：CommitText(v)，v 是 IBusText＝(「IBusText」, {}, 字, (「IBusAttrList」, {}, [])) */
void ibus_commit(IC *ic, const char *utf8)
{
	if (!ic || !ic->conn || !utf8 || !*utf8) return;
	if (getenv("POMELO_INPUT_DEBUG")) fprintf(stderr, "ibus commit %s: %s\n", ic->path, utf8);
	DBusMessage *m = dbus_message_new_signal(ic->path, IC_IFACE, "CommitText");
	if (!m) return;
	DBusMessageIter it, var, st, dict, var2, st2, dict2, arr;
	const char *tname = "IBusText", *aname = "IBusAttrList";
	dbus_message_iter_init_append(m, &it);
	dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT, "(sa{sv}sv)", &var);
	dbus_message_iter_open_container(&var, DBUS_TYPE_STRUCT, NULL, &st);
	dbus_message_iter_append_basic(&st, DBUS_TYPE_STRING, &tname);
	dbus_message_iter_open_container(&st, DBUS_TYPE_ARRAY, "{sv}", &dict);
	dbus_message_iter_close_container(&st, &dict);
	dbus_message_iter_append_basic(&st, DBUS_TYPE_STRING, &utf8);
	dbus_message_iter_open_container(&st, DBUS_TYPE_VARIANT, "(sa{sv}av)", &var2);
	dbus_message_iter_open_container(&var2, DBUS_TYPE_STRUCT, NULL, &st2);
	dbus_message_iter_append_basic(&st2, DBUS_TYPE_STRING, &aname);
	dbus_message_iter_open_container(&st2, DBUS_TYPE_ARRAY, "{sv}", &dict2);
	dbus_message_iter_close_container(&st2, &dict2);
	dbus_message_iter_open_container(&st2, DBUS_TYPE_ARRAY, "v", &arr);
	dbus_message_iter_close_container(&st2, &arr);
	dbus_message_iter_close_container(&var2, &st2);
	dbus_message_iter_close_container(&st, &var2);
	dbus_message_iter_close_container(&var, &st);
	dbus_message_iter_close_container(&it, &var);
	dbus_message_set_sender(m, OWNER);
	dbus_connection_send(ic->conn, m, NULL);
	dbus_connection_flush(ic->conn);
	dbus_message_unref(m);
}

/* ~/.config/ibus/bus/<machine-id>-<主機>-<顯示器編號>（Qt 的 IBus 外掛照這個規則找） */
static int address_path(const char *display, char *out, size_t n)
{
	char *id = dbus_get_local_machine_id();
	if (!id) return -1;
	char host[64] = "unix", num[16] = "0";
	const char *colon = display ? strchr(display, ':') : NULL;
	if (colon) {
		if (colon > display) snprintf(host, sizeof host, "%.*s", (int)(colon - display), display);
		snprintf(num, sizeof num, "%.*s", (int)strcspn(colon + 1, "."), colon + 1);
	}
	const char *cfg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
	char base[PATH_MAX];
	if (cfg && *cfg) snprintf(base, sizeof base, "%s", cfg);
	else snprintf(base, sizeof base, "%s/.config", home ? home : "/tmp");
	char d[PATH_MAX + 16];
	mkdir(base, 0700);   /* 新帳號第一次登入時 ~/.config 可能還不存在（例如光碟試用的帳號）*/
	snprintf(d, sizeof d, "%s/ibus", base); mkdir(d, 0700);
	snprintf(d, sizeof d, "%s/ibus/bus", base); mkdir(d, 0700);
	snprintf(out, n, "%s/ibus/bus/%s-%s-%s", base, id, host, num);
	dbus_free(id);
	return 0;
}

int ibus_start(const char *display)
{
	const char *rt = getenv("XDG_RUNTIME_DIR");
	char sock[PATH_MAX], addr[PATH_MAX + 32];
	const char *num = display && strchr(display, ':') ? strchr(display, ':') + 1 : "0";
	snprintf(sock, sizeof sock, "%s/pomelo-input-ibus-%.*s", rt && *rt ? rt : "/tmp", (int)strcspn(num, "."), num);
	DBusError err;
	dbus_error_init(&err);
	if (strlen(sock) < 100) {
		unlink(sock);   /* 上次留下的（同一個顯示器只會有一個 pomelo-input：XIM 登記不到就不會走到這裡） */
		snprintf(addr, sizeof addr, "unix:path=%s", sock);
	} else   /* 路徑太長放不進 socket 名稱：用抽象 socket（一樣只有同一個使用者通得過認證） */
		snprintf(addr, sizeof addr, "unix:abstract=pomelo-input-ibus-%d-%.*s", (int)getuid(), (int)strcspn(num, "."), num);
	server = dbus_server_listen(addr, &err);
	if (!server) { fprintf(stderr, "pomelo-input: IBus 服務開不起來：%s\n", err.message ? err.message : ""); dbus_error_free(&err); return -1; }
	const char *mechs[] = { "EXTERNAL", NULL };
	dbus_server_set_auth_mechanisms(server, mechs);
	dbus_server_set_new_connection_function(server, new_connection, NULL, NULL);
	dbus_server_set_watch_functions(server, add_watch, remove_watch, toggle_watch, NULL, NULL);
	dbus_server_set_timeout_functions(server, add_timeout, remove_timeout, NULL, NULL, NULL);
	if (strlen(sock) < 100) chmod(sock, 0600);
	if (address_path(display, addr_file, sizeof addr_file)) return 0;
	/* 原生 Wayland 的 Qt 找的檔名是「機器-unix-wayland-0」（用 WAYLAND_DISPLAY，不是 X 的顯示器編號） */
	const char *wd = getenv("WAYLAND_DISPLAY");
	if (wd && *wd && !strchr(wd, '/')) {
		char tmp[80];
		snprintf(tmp, sizeof tmp, ":%s", wd);
		if (address_path(tmp, addr_file_wl, sizeof addr_file_wl)) addr_file_wl[0] = 0;
	}
	char *a = dbus_server_get_address(server);
	const char *files[] = { addr_file, addr_file_wl };
	for (int i = 0; i < 2; i++) {
		if (!files[i][0]) continue;
		FILE *f = fopen(files[i], "w");
		if (f) {
			fprintf(f, "# 由 pomelo-input 產生：Qt 程式用 IBus 協定接 Pomelo 輸入法\nIBUS_ADDRESS=%s\nIBUS_DAEMON_PID=%d\n", a ? a : addr, (int)getpid());
			fclose(f);
		}
	}
	dbus_free(a);
	/* GTK4 的 IBus 模組要看到 session bus 上有 org.freedesktop.IBus 才會送按鍵（見檔案開頭） */
	DBusError e2;
	dbus_error_init(&e2);
	session = dbus_bus_get_private(DBUS_BUS_SESSION, &e2);
	if (session) {
		dbus_connection_set_exit_on_disconnect(session, FALSE);
		if (dbus_bus_request_name(session, BUS_NAME, DBUS_NAME_FLAG_DO_NOT_QUEUE, &e2) == DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
			dbus_connection_set_watch_functions(session, add_watch, remove_watch, toggle_watch, NULL, NULL);
			dbus_connection_set_timeout_functions(session, add_timeout, remove_timeout, NULL, NULL, NULL);
		} else {   /* 真的 ibus-daemon 在跑 */
			dbus_connection_close(session);
			dbus_connection_unref(session);
			session = NULL;
		}
	}
	dbus_error_free(&e2);
	return 0;
}

static void remove_ours(char *file)
{
	/* 位址檔還是我們寫的才刪（真的 ibus-daemon 之後可能蓋過去） */
	if (!file[0]) return;
	FILE *f = fopen(file, "r");
	char line[256], want[64];
	int ours = 0;
	snprintf(want, sizeof want, "IBUS_DAEMON_PID=%d", (int)getpid());
	while (f && fgets(line, sizeof line, f)) if (!strncmp(line, want, strlen(want))) ours = 1;
	if (f) fclose(f);
	if (ours) unlink(file);
	file[0] = 0;
}

void ibus_stop(void)
{
	remove_ours(addr_file);
	remove_ours(addr_file_wl);
}
