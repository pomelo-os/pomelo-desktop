/* SPDX-License-Identifier: GPL-3.0-or-later */
/* check-net.c — 網路檢查：從近到遠一段一段查（網路服務 → Wi-Fi 開關 → 網路卡 → IP → 路由器 → DNS → 網際網路），
 * 找到第一個斷掉的地方就說清楚；另外看 Wi-Fi 的加密、訊號、頻道擁擠、VPN、Proxy、IPv6。
 * 會連到外面的只有：一個 DNS 查詢、系統本來就會做的連線檢查（NetworkManager 的 connectivity check）、
 * 連不上時試一下 1.1.1.1:443 能不能接通（分辨「整個斷了」還是「只有 DNS 壞了」）。 */
#define _GNU_SOURCE
#include "internal.h"
#include "check-int.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <arpa/inet.h>
#include <sys/socket.h>

typedef struct { char dev[64], type[32], state[64], conn[256]; } NetDev;

/* TCP 能不能接通（3 秒）；測試時用 POMELO_CHECK_FAKE_TCP=ok／fail 代替 */
static int tcp_ok(const char *ip, int port)
{
	const char *fake = getenv("POMELO_CHECK_FAKE_TCP");
	if (fake) return !strcmp(fake, "ok");
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port) };
	if (inet_pton(AF_INET, ip, &a.sin_addr) != 1) return 0;
	int s = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (s < 0) return 0;
	int ok = 0;
	if (connect(s, (struct sockaddr *)&a, sizeof a) == 0) ok = 1;
	else if (errno == EINPROGRESS) {
		struct pollfd p = { .fd = s, .events = POLLOUT };
		int err = 0;
		socklen_t el = sizeof err;
		if (poll(&p, 1, 3000) == 1 && !getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el) && !err) ok = 1;
	}
	close(s);
	return ok;
}

/* NetworkManager 做連線檢查用的網址的主機名稱（各發行版不同）；沒設定就用 example.com（IANA 保留、一定存在） */
static void check_host(char *out, size_t n)
{
	snprintf(out, n, "example.com");
	const char *dirs[] = { "/etc/NetworkManager/conf.d", "/usr/lib/NetworkManager/conf.d", "/run/NetworkManager/conf.d" };
	char files[48][320];
	int nf = 0;
	snprintf(files[nf++], sizeof files[0], "/etc/NetworkManager/NetworkManager.conf");
	for (size_t i = 0; i < LEN(dirs); i++) {
		DIR *d = opendir(chk_path(dirs[i]));
		struct dirent *e;
		while (d && (e = readdir(d)) && nf < 48)
			if (strstr(e->d_name, ".conf")) snprintf(files[nf++], sizeof files[0], "%s/%s", dirs[i], e->d_name);
		if (d) closedir(d);
	}
	for (int i = 0; i < nf; i++) {
		FILE *f = fopen(chk_path(files[i]), "r");
		char line[512];
		int in_conn = 0;
		while (f && fgets(line, sizeof line, f)) {
			if (line[0] == '[') in_conn = !strncmp(line, "[connectivity]", 14);
			char *u = in_conn ? strstr(line, "uri=") : NULL;
			if (u && u == line) {
				char *h = strstr(u, "://");
				if (!h) continue;
				h += 3;
				size_t l = strcspn(h, "/:\n\r ");
				if (l && l < n) { memcpy(out, h, l); out[l] = 0; }
			}
		}
		if (f) fclose(f);
	}
}

static int is_wifi_type(const char *t) { return !strcmp(t, "wifi") || !strcmp(t, "802-11-wireless"); }
static int is_eth_type(const char *t) { return !strcmp(t, "ethernet") || !strcmp(t, "802-3-ethernet"); }
static int dev_connected(const NetDev *d) { return !strncmp(d->state, "connected", 9) && strstr(d->state, "connecting") == NULL; }

/* 家用網路常見的搜尋網域：這些不算「公司網路」 */
static int home_domain(const char *d)
{
	static const char *h[] = { "lan", "home", "local", "localdomain", "domain", "router", "Home", "station", "" };
	for (size_t i = 0; i < LEN(h); i++) if (!strcmp(d, h[i])) return 1;
	return 0;
}

void chk_network(PmReport *r, int flags)
{
	(void)flags;
	PmCheck *c;
	int have_nm = pm_have("nmcli");
	NetDev devs[24];
	int nd = 0;

	/* 1. 網路服務 */
	if (have_nm) {
		char *o = chk_run("nmcli -t -f RUNNING general");
		int running = o && !strncmp(o, "running", 7);
		free(o);
		if (!running) {
			c = chk_add(r, "net.service", PM_CHK_BAD, "%s", _("網路服務（NetworkManager）沒有在執行"));
			chk_detail(c, "%s", _("沒有它就不能連線、也不能切換 Wi-Fi。"));
			chk_fix(c, "net.nm_restart", NULL, _("啟動網路服務"));
			return;
		}
		chk_add(r, "net.service", PM_CHK_OK, "%s", _("網路服務正常執行中"));
		o = chk_run("nmcli -t -f DEVICE,TYPE,STATE,CONNECTION device");
		for (char *l = o, *nx; l && *l && nd < (int)LEN(devs); l = nx) {
			nx = chk_line(l);
			char *f[4];
			if (chk_split_colon(l, f, 4) < 4) continue;
			if (!is_wifi_type(f[1]) && !is_eth_type(f[1])) continue;
			snprintf(devs[nd].dev, sizeof devs[0].dev, "%s", f[0]);
			snprintf(devs[nd].type, sizeof devs[0].type, "%s", is_wifi_type(f[1]) ? "wifi" : "ethernet");
			snprintf(devs[nd].state, sizeof devs[0].state, "%s", f[2]);
			snprintf(devs[nd].conn, sizeof devs[0].conn, "%s", f[3]);
			nd++;
		}
		free(o);
	} else {
		chk_add(r, "net.service", PM_CHK_SKIP, "%s", _("這台電腦沒有用 NetworkManager，只檢查連線"));
	}

	/* 2. Wi-Fi 開關（飛航模式、硬體開關） */
	int wifi_soft = 0, wifi_hard = 0, have_wifi_dev = 0;
	DIR *d = opendir(chk_path("/sys/class/rfkill"));
	struct dirent *e;
	while (d && (e = readdir(d))) {
		if (e->d_name[0] == '.') continue;
		char p[320], t[32];
		snprintf(p, sizeof p, "/sys/class/rfkill/%s/type", e->d_name);
		if (!chk_read(p, t, sizeof t) || strcmp(t, "wlan")) continue;
		snprintf(p, sizeof p, "/sys/class/rfkill/%s/soft", e->d_name);
		wifi_soft |= chk_read_long(p, 0) == 1;
		snprintf(p, sizeof p, "/sys/class/rfkill/%s/hard", e->d_name);
		wifi_hard |= chk_read_long(p, 0) == 1;
	}
	if (d) closedir(d);
	for (int i = 0; i < nd; i++) have_wifi_dev |= !strcmp(devs[i].type, "wifi");

	/* 3. 連線狀態 */
	int any = 0, wired_conn = 0;
	NetDev *wifi = NULL, *eth = NULL;
	for (int i = 0; i < nd; i++) {
		if (!dev_connected(&devs[i])) continue;
		any = 1;
		if (!strcmp(devs[i].type, "wifi") && !wifi) wifi = &devs[i];
		if (!strcmp(devs[i].type, "ethernet") && !eth) { eth = &devs[i]; wired_conn = 1; }
	}
	if (wifi_hard) {
		c = chk_add(r, "net.wifi_off", wired_conn ? PM_CHK_INFO : PM_CHK_BAD, "%s", _("Wi-Fi 被硬體開關關掉了"));
		chk_advice(c, "%s", _("打開筆電上的無線網路開關，或按鍵盤上有天線圖示的功能鍵（通常要配合 Fn）。"));
	} else if (wifi_soft) {
		c = chk_add(r, "net.wifi_off", wired_conn ? PM_CHK_INFO : PM_CHK_BAD, "%s", _("Wi-Fi 關著（可能是飛航模式）"));
		chk_fix(c, "net.wifi_on", NULL, _("打開 Wi-Fi"));
	}
	/* 網路線：有網路卡但沒接線 */
	for (int i = 0; i < nd; i++) {
		if (strcmp(devs[i].type, "ethernet")) continue;
		char p[160];
		snprintf(p, sizeof p, "/sys/class/net/%s/carrier", devs[i].dev);
		long carrier = chk_read_long(p, -1);
		if (carrier == 0 && !any) {
			c = chk_add(r, "net.cable", have_wifi_dev ? PM_CHK_INFO : PM_CHK_BAD, _("網路線沒有接上（%s）"), devs[i].dev);
			chk_advice(c, "%s", _("檢查網路線兩端有沒有插好；換一個路由器上的插孔或換一條線試試。"));
		}
	}
	if (have_nm && !any) {
		c = chk_add(r, "net.link", PM_CHK_BAD, "%s", _("沒有連上任何網路"));
		chk_advice(c, "%s", have_wifi_dev ? _("在網路設定選一個 Wi-Fi，或插上網路線。") : _("插上網路線；這台電腦沒有 Wi-Fi 網路卡。"));
		chk_fix(c, "net.open_settings", NULL, _("開啟網路設定"));
		chk_add(r, "net.internet", PM_CHK_SKIP, "%s", _("沒有連線，略過 IP、路由器、DNS、網際網路的檢查"));
		return;
	}
	if (have_nm) {
		c = chk_add(r, "net.link", PM_CHK_OK, "%s", wifi ? _("已連上 Wi-Fi") : _("已連上有線網路"));
		if (wifi) chk_detail(c, "%s", wifi->conn);
		if (eth) chk_detail(c, _("有線網路：%s"), eth->conn);
	}

	/* 4. Wi-Fi：加密、訊號、頻道 */
	if (wifi) {
		char *o = chk_run("nmcli -t -f IN-USE,SSID,SIGNAL,SECURITY,CHAN device wifi list --rescan no");
		int cur_sig = -1, cur_chan = 0, same = 0;
		char cur_sec[128] = "", cur_ssid[256] = "";
		/* 先找目前連著的那一個，再數同頻道的 */
		for (int pass = 0; pass < 2; pass++) {
			char *dup = o ? strdup(o) : NULL;
			for (char *l = dup, *nx; l && *l; l = nx) {
				nx = chk_line(l);
				char *f[5];
				if (chk_split_colon(l, f, 5) < 5) continue;
				int ch = atoi(f[4]);
				if (pass == 0 && !strcmp(f[0], "*")) {
					cur_sig = atoi(f[2]); cur_chan = ch;
					snprintf(cur_sec, sizeof cur_sec, "%s", f[3]);
					snprintf(cur_ssid, sizeof cur_ssid, "%s", f[1]);
				} else if (pass == 1 && strcmp(f[0], "*") && ch && ch == cur_chan) same++;
			}
			free(dup);
		}
		free(o);
		if (cur_sig >= 0) {
			if (cur_sig >= 60) chk_add(r, "net.signal", PM_CHK_OK, _("Wi-Fi 訊號良好（%d%%）"), cur_sig);
			else if (cur_sig >= 40) {
				c = chk_add(r, "net.signal", PM_CHK_INFO, _("Wi-Fi 訊號普通（%d%%）"), cur_sig);
				chk_advice(c, "%s", _("網路慢的時候，可以靠近路由器一點。"));
			} else {
				c = chk_add(r, "net.signal", PM_CHK_WARN, _("Wi-Fi 訊號弱（%d%%）"), cur_sig);
				chk_detail(c, "%s", _("訊號弱會讓網路變慢、斷斷續續。"));
				chk_advice(c, "%s", _("靠近路由器、避開牆壁與金屬，或改用 5 GHz 的網路、有線網路。"));
			}
			const char *sec = cur_sec;
			if (!*sec || !strcmp(sec, "--")) {
				c = chk_add(r, "net.wifi_sec", PM_CHK_WARN, _("「%s」沒有加密"), cur_ssid);
				chk_detail(c, "%s", _("同一個 Wi-Fi 上的人可能看得到你沒有加密的連線內容。"));
				chk_advice(c, "%s", _("公共場所盡量只開 https 的網站，或使用 VPN；自己家裡的路由器請設定 WPA2 或 WPA3 密碼。"));
			} else if (strstr(sec, "WEP")) {
				c = chk_add(r, "net.wifi_sec", PM_CHK_BAD, _("「%s」用的是 WEP 加密，幾分鐘就能被破解"), cur_ssid);
				chk_advice(c, "%s", _("把路由器改成 WPA2 或 WPA3。"));
			} else if (!strstr(sec, "WPA2") && !strstr(sec, "WPA3") && strstr(sec, "WPA")) {
				c = chk_add(r, "net.wifi_sec", PM_CHK_WARN, _("「%s」用的是舊的 WPA 加密"), cur_ssid);
				chk_advice(c, "%s", _("把路由器改成 WPA2 或 WPA3。"));
			} else chk_add(r, "net.wifi_sec", PM_CHK_OK, _("Wi-Fi 有加密（%s）"), sec);
			if (cur_chan && cur_chan <= 14 && same >= 6) {
				c = chk_add(r, "net.channel", PM_CHK_INFO, _("附近有 %d 個 Wi-Fi 跟你用同一個頻道（%d）"), same, cur_chan);
				chk_advice(c, "%s", _("2.4 GHz 很擁擠；路由器有 5 GHz 的話改連 5 GHz，或在路由器把頻道改成自動。"));
			}
		}
	}

	/* 5. IP 與路由器 */
	char gw[64] = "", gwdev[64] = "";
	{
		char *o = chk_run("ip -4 route show default");
		char *p = o ? strstr(o, "via ") : NULL;
		if (p) sscanf(p + 4, "%63s", gw);
		p = o ? strstr(o, " dev ") : NULL;
		if (p) sscanf(p + 5, "%63s", gwdev);
		free(o);
	}
	NetDev *main_dev = NULL;
	for (int i = 0; i < nd; i++) if (!strcmp(devs[i].dev, gwdev)) main_dev = &devs[i];
	if (!main_dev) main_dev = wifi ? wifi : eth;
	char ip[64] = "";
	int linklocal = 0;
	if (main_dev || gwdev[0]) {
		char cmd[160];
		snprintf(cmd, sizeof cmd, "ip -4 -o addr show dev %s", gwdev[0] ? gwdev : main_dev->dev);
		char *o = chk_run(cmd);
		for (char *l = o, *nx; l && *l; l = nx) {
			nx = chk_line(l);
			char *p = strstr(l, " inet ");
			if (!p) continue;
			char a[64];
			if (sscanf(p + 6, "%63[^/ ]", a) != 1) continue;
			if (!strncmp(a, "169.254.", 8)) { linklocal = 1; continue; }
			snprintf(ip, sizeof ip, "%s", a);
			break;
		}
		free(o);
	}
	const char *dname = main_dev ? main_dev->dev : gwdev;
	if (!ip[0]) {
		c = chk_add(r, "net.ip", PM_CHK_BAD, "%s", _("沒有拿到 IP 位址"));
		chk_detail(c, "%s", linklocal ? _("只有 169.254 開頭的位址：路由器沒有回應自動分配（DHCP）的要求。")
		                               : _("網路卡連上了，但還沒有 IP 位址。"));
		chk_advice(c, "%s", _("先試著重新連線；還是不行就重新啟動路由器。"));
		if (dname[0]) chk_fix(c, "net.reconnect", dname, _("重新連線"));
		chk_add(r, "net.internet", PM_CHK_SKIP, "%s", _("沒有 IP 位址，略過路由器、DNS、網際網路的檢查"));
		return;
	}
	c = chk_add(r, "net.ip", PM_CHK_OK, _("IP 位址：%s"), ip);
	if (dname[0]) chk_detail(c, _("網路卡：%s"), dname);

	int gw_ok = 1;
	if (!gw[0]) {
		c = chk_add(r, "net.gateway", PM_CHK_BAD, "%s", _("沒有預設路由器（閘道）"));
		chk_detail(c, "%s", _("有 IP 位址，但不知道要從哪裡出去上網；通常是手動設定少填了路由器。"));
		chk_fix(c, "net.open_settings", NULL, _("開啟網路設定"));
		gw_ok = 0;
	} else {
		char cmd[160];
		snprintf(cmd, sizeof cmd, "ping -n -c 4 -i 0.25 -W 1 %s", gw);
		char *o = chk_run(cmd);
		int loss = 100;
		double avg = -1;
		char *p = o ? strstr(o, "% packet loss") : NULL;
		if (p) { while (p > o && (isdigit((unsigned char)p[-1]) || p[-1] == '.')) p--; loss = atoi(p); }
		p = o ? strstr(o, "rtt min/avg/max") : NULL;
		if (!p && o) p = strstr(o, "round-trip min/avg/max");
		if (p && (p = strchr(p, '='))) { double mn; sscanf(p + 1, "%lf/%lf", &mn, &avg); }
		free(o);
		if (loss >= 100) {
			c = chk_add(r, "net.gateway", PM_CHK_INFO, _("路由器（%s）沒有回應 ping"), gw);
			chk_detail(c, "%s", _("有些路由器會關掉 ping，不一定是問題；請看下面能不能上網。"));
		} else if (loss > 0) {
			c = chk_add(r, "net.gateway", PM_CHK_WARN, _("連到路由器時掉了 %d%% 的封包"), loss);
			chk_detail(c, _("路由器 %s，平均 %.0f 毫秒。"), gw, avg);
			chk_advice(c, "%s", wifi ? _("Wi-Fi 受到干擾或訊號不好；靠近路由器，或改用有線網路。") : _("檢查網路線與路由器的插孔。"));
		} else if (avg > 30) {
			c = chk_add(r, "net.gateway", PM_CHK_WARN, _("連到路由器很慢（平均 %.0f 毫秒）"), avg);
			chk_advice(c, "%s", _("正常應該在 10 毫秒以內；可能是 Wi-Fi 擁擠或路由器太忙，試著重新啟動路由器。"));
		} else chk_add(r, "net.gateway", PM_CHK_OK, _("路由器回應正常（%s，%.0f 毫秒）"), gw, avg < 1 ? 1.0 : avg);
	}
	(void)gw_ok;

	/* 6. DNS */
	char host[256], dns[256] = "", domain[256] = "", conn[256] = "";
	check_host(host, sizeof host);
	if (have_nm && main_dev) {
		snprintf(conn, sizeof conn, "%s", main_dev->conn);
		char cmd[256];
		snprintf(cmd, sizeof cmd, "nmcli -t -f IP4.DNS,IP4.DOMAIN device show %s", main_dev->dev);
		char *o = chk_run(cmd);
		for (char *l = o, *nx; l && *l; l = nx) {
			nx = chk_line(l);
			char *v = strchr(l, ':');
			if (!v) continue;
			*v++ = 0;
			if (!strncmp(l, "IP4.DNS", 7) && strlen(dns) + strlen(v) + 2 < sizeof dns) { if (dns[0]) strcat(dns, " "); strcat(dns, v); }
			if (!strncmp(l, "IP4.DOMAIN", 10) && !home_domain(v) && !domain[0]) snprintf(domain, sizeof domain, "%s", v);
		}
		free(o);
	}
	int dns_ok = 0;
	long dns_ms = 0;
	{
		char cmd[400];
		snprintf(cmd, sizeof cmd, "timeout 5 getent ahostsv4 %s", host);
		long t0 = chk_now_ms();
		char *o = chk_run(cmd);
		dns_ms = chk_now_ms() - t0;
		dns_ok = o && *o;
		free(o);
	}
	int raw_ip = tcp_ok("1.1.1.1", 443);   /* 不用 DNS、直接用 IP 能不能出去 */
	int public_dns = strstr(dns, "1.1.1.1") && strstr(dns, "8.8.8.8");
	if (!dns_ok) {
		c = chk_add(r, "net.dns", PM_CHK_BAD, "%s", raw_ip ? _("DNS 查不到網址（網路本身是通的）") : _("DNS 查不到網址"));
		chk_detail(c, _("查詢 %s 失敗。"), host);
		if (dns[0]) chk_detail(c, _("目前的 DNS 伺服器：%s"), dns);
		if (domain[0]) {
			chk_advice(c, _("這看起來是公司或學校的網路（網域 %s），請聯絡網管人員；不要自己改 DNS，不然內部網站會連不到。"), domain);
		} else if (raw_ip && have_nm && conn[0]) {
			chk_advice(c, "%s", _("可以改用公共 DNS（Cloudflare 1.1.1.1 與 Google 8.8.8.8），之後隨時可以改回自動。"));
			chk_fix(c, "net.dns_public", conn, _("改用公共 DNS"));
		} else chk_advice(c, "%s", _("重新啟動路由器；還是不行請聯絡網路業者。"));
	} else if (dns_ms > 1500) {
		c = chk_add(r, "net.dns", PM_CHK_WARN, _("DNS 回應很慢（%.1f 秒）"), dns_ms / 1000.0);
		if (dns[0]) chk_detail(c, _("目前的 DNS 伺服器：%s"), dns);
		chk_detail(c, "%s", _("每打開一個新網站都要先等 DNS，網頁會感覺「卡一下才開始載入」。"));
		if (!domain[0] && have_nm && conn[0] && !public_dns) chk_fix(c, "net.dns_public", conn, _("改用公共 DNS"));
	} else {
		c = chk_add(r, "net.dns", PM_CHK_OK, _("DNS 正常（%ld 毫秒）"), dns_ms < 1 ? 1 : dns_ms);
		if (dns[0]) chk_detail(c, _("DNS 伺服器：%s"), dns);
	}
	if (public_dns && have_nm && conn[0]) {
		c = chk_add(r, "net.dns_override", PM_CHK_INFO, "%s", _("這個網路目前用公共 DNS（1.1.1.1、8.8.8.8）"));
		chk_detail(c, "%s", _("如果是之前在這裡設定的，網路正常後可以改回路由器給的 DNS。"));
		chk_fix(c, "net.dns_auto", conn, _("改回自動"));
	}

	/* 7. 網際網路（NetworkManager 的連線檢查：分得出「要先登入的 Wi-Fi」） */
	char state[32] = "";
	if (have_nm) {
		char *o = chk_run("timeout 12 nmcli networking connectivity check");
		if (o) sscanf(o, "%31s", state);
		free(o);
	}
	int online = 0;
	if (!strcmp(state, "full")) online = 1;
	else if (!strcmp(state, "unknown") || !state[0]) online = dns_ok && raw_ip;   /* 連線檢查被關掉了：自己判斷 */
	if (!strcmp(state, "portal")) {
		c = chk_add(r, "net.internet", PM_CHK_WARN, "%s", _("這個網路要先在網頁上登入"));
		chk_detail(c, "%s", _("飯店、咖啡店、機場的 Wi-Fi 常常要先同意條款或輸入房號才能上網。"));
		chk_fix(c, "net.portal", NULL, _("開啟登入頁"));
	} else if (online) {
		chk_add(r, "net.internet", PM_CHK_OK, "%s", _("可以連上網際網路"));
	} else if (raw_ip && !dns_ok) {
		c = chk_add(r, "net.internet", PM_CHK_WARN, "%s", _("網路是通的，但因為 DNS 壞了，打不開網站"));
		chk_advice(c, "%s", _("見上面 DNS 那一項。"));
	} else {
		c = chk_add(r, "net.internet", PM_CHK_BAD, "%s", _("連得到路由器，但連不到網際網路"));
		chk_detail(c, "%s", _("問題在路由器之外：路由器沒有連上網路業者，或業者那邊斷線。"));
		chk_advice(c, "%s", _("把路由器（和數據機）拔掉電源 30 秒再插回去；還是不行請聯絡網路業者，看看是不是有斷線公告。"));
	}

	/* 8. VPN、Proxy、IPv6 */
	if (have_nm) {
		char *o = chk_run("nmcli -t -f TYPE,NAME connection show --active");
		for (char *l = o, *nx; l && *l; l = nx) {
			nx = chk_line(l);
			char *f[2];
			if (chk_split_colon(l, f, 2) < 2) continue;
			if (!strcmp(f[0], "vpn") || !strcmp(f[0], "wireguard") || !strcmp(f[0], "tun")) {
				c = chk_add(r, "net.vpn", PM_CHK_INFO, _("VPN「%s」連線中"), f[1]);
				chk_detail(c, "%s", _("網路流量會先經過 VPN；網路慢或有些網站打不開時，可以先中斷 VPN 比較看看。"));
			}
		}
		free(o);
	}
	const char *px = getenv("https_proxy");
	if (!px || !*px) px = getenv("HTTPS_PROXY");
	if (!px || !*px) px = getenv("http_proxy");
	if (!px || !*px) px = getenv("all_proxy");
	if (px && *px) {
		c = chk_add(r, "net.proxy", PM_CHK_INFO, "%s", _("有設定 Proxy（代理伺服器）"));
		/* 不顯示帳號密碼（http://帳號:密碼@主機） */
		const char *at = strrchr(px, '@'), *sch = strstr(px, "://");
		chk_detail(c, "%s", at ? at + 1 : sch ? sch + 3 : px);
		chk_detail(c, "%s", _("程式的連線會經過它；Proxy 停擺時網頁會打不開。"));
	}
	{
		char *o = chk_run("ip -6 -o addr show scope global");
		int v6 = o && strstr(o, "inet6");
		free(o);
		if (v6) chk_add(r, "net.ipv6", PM_CHK_OK, "%s", _("有 IPv6"));
		else chk_add(r, "net.ipv6", PM_CHK_OK, "%s", _("沒有 IPv6（大部分網站都不影響）"));
	}
}

/* ---- 修正動作 ---- */

int chk_net_fix(const PmCheck *c, char **argv, int max, int *admin, char *msg, size_t n)
{
	(void)max;
	int k = 0;
	if (!strcmp(c->fix, "net.nm_restart")) {
		*admin = 1;
		snprintf(msg, n, "%s", _("重新啟動網路服務（NetworkManager），網路會中斷幾秒。"));
		argv[k++] = "systemctl"; argv[k++] = "restart"; argv[k++] = "NetworkManager";
	} else if (!strcmp(c->fix, "net.wifi_on")) {
		snprintf(msg, n, "%s", _("打開 Wi-Fi。"));
		argv[k++] = "nmcli"; argv[k++] = "radio"; argv[k++] = "wifi"; argv[k++] = "on";
	} else if (!strcmp(c->fix, "net.reconnect")) {
		snprintf(msg, n, _("重新連線「%s」，網路會中斷幾秒。"), c->fix_arg);
		argv[k++] = "nmcli"; argv[k++] = "device"; argv[k++] = "connect"; argv[k++] = (char *)c->fix_arg;
	} else if (!strcmp(c->fix, "net.dns_public") || !strcmp(c->fix, "net.dns_auto")) {
		int pub = !strcmp(c->fix, "net.dns_public");
		snprintf(msg, n, pub ? _("讓「%s」改用公共 DNS（1.1.1.1、8.8.8.8），並重新連線一次。之後可以在這裡或網路設定改回自動。")
		                     : _("讓「%s」改回使用路由器給的 DNS，並重新連線一次。"), c->fix_arg);
		/* 連線名稱當成參數傳給 sh（$1），不會被當成指令的一部分 */
		argv[k++] = "sh"; argv[k++] = "-c";
		argv[k++] = pub ? "nmcli connection modify \"$1\" ipv4.dns '1.1.1.1 8.8.8.8' ipv4.ignore-auto-dns yes && nmcli connection up \"$1\""
		                : "nmcli connection modify \"$1\" ipv4.dns '' ipv4.ignore-auto-dns no && nmcli connection up \"$1\"";
		argv[k++] = "sh"; argv[k++] = (char *)c->fix_arg;
	} else if (!strcmp(c->fix, "net.portal")) {
		snprintf(msg, n, "%s", _("用瀏覽器打開一個網頁，Wi-Fi 的登入頁會自己跳出來。"));
		argv[k++] = "@open"; argv[k++] = "http://example.com/";
	} else if (!strcmp(c->fix, "net.open_settings")) {
		argv[k++] = "@open"; argv[k++] = "pomelo-network";
	}
	argv[k] = NULL;
	return k;
}
