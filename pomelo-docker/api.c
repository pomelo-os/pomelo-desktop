/* SPDX-License-Identifier: GPL-3.0-or-later */
/* api.c — Docker Engine API 用戶端（被 pomelo-docker.c #include）
 *
 * 直接對 Docker 的 UNIX socket 講 HTTP/1.1（每個請求一條連線，Connection: close），回應可能是 chunked。
 * 路徑不帶版本號：Docker 29 起不接受 1.44 以前的版本，不帶版本號就用伺服器自己的版本，新舊 Docker 都通。
 * 大的內容（映像 tar、容器裡的檔案）直接串流到檔案，不放記憶體。
 */

static char dk_sock[PATH_MAX];
enum { DK_OK, DK_NOTINSTALLED, DK_DENIED, DK_DOWN };

static void dk_find_socket(void)
{
	const char *h = getenv("DOCKER_HOST"), *rt = getenv("XDG_RUNTIME_DIR");
	if (h && !strncmp(h, "unix://", 7)) { snprintf(dk_sock, sizeof dk_sock, "%s", h + 7); return; }
	char rootless[PATH_MAX] = "";
	if (rt && *rt) snprintf(rootless, sizeof rootless, "%s/docker.sock", rt);
	const char *c[] = { "/var/run/docker.sock", "/run/docker.sock", rootless };
	for (size_t i = 0; i < LEN(c); i++)
		if (c[i][0] && !access(c[i], F_OK)) { snprintf(dk_sock, sizeof dk_sock, "%s", c[i]); return; }
	snprintf(dk_sock, sizeof dk_sock, "/var/run/docker.sock");
}

static int dk_connect(void)
{
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	if (!dk_sock[0]) dk_find_socket();
	snprintf(a.sun_path, sizeof a.sun_path, "%s", dk_sock);
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) return -1;
	if (connect(fd, (struct sockaddr *)&a, sizeof a)) { int e = errno; close(fd); errno = e; return -1; }
	return fd;
}

/* Docker 能不能用：沒裝、沒權限（不在 docker 群組）、裝了但沒在執行 */
static int dk_status(void)
{
	dk_find_socket();
	int fd = dk_connect();
	if (fd >= 0) { close(fd); return DK_OK; }
	if (errno == EACCES || errno == EPERM) return DK_DENIED;
	if (pm_have("dockerd") || pm_have("docker") || !access("/usr/bin/dockerd", X_OK)) return DK_DOWN;
	return DK_NOTINSTALLED;
}

static int write_all(int fd, const void *p, size_t n)
{
	const char *c = p;
	while (n) {
		ssize_t r = write(fd, c, n);
		if (r < 0 && errno == EINTR) continue;
		if (r <= 0) return -1;
		c += r;
		n -= (size_t)r;
	}
	return 0;
}

/* 一個回應的讀取狀態 */
typedef struct {
	int fd, status, chunked, done;
	long long clen;          /* Content-Length（-1 = 沒給，讀到連線結束） */
	long long left;          /* chunked：這一塊還剩幾個位元組 */
	size_t have, pos;
	int timeout;             /* 毫秒，0 = 一直等 */
	char buf[65536];         /* 放最後：dk_open 只清前面的欄位 */
} Rd;

static int rd_fill(Rd *r)
{
	if (r->pos < r->have) return 1;
	if (r->timeout) {
		struct pollfd pf = { r->fd, POLLIN, 0 };
		if (poll(&pf, 1, r->timeout) <= 0) return -1;
	}
	ssize_t n;
	do n = read(r->fd, r->buf, sizeof r->buf); while (n < 0 && errno == EINTR);
	if (n <= 0) return 0;
	r->have = (size_t)n;
	r->pos = 0;
	return 1;
}

static int rd_byte(Rd *r)
{
	if (rd_fill(r) <= 0) return -1;
	return (unsigned char)r->buf[r->pos++];
}

static int rd_line(Rd *r, char *out, size_t n)
{
	size_t k = 0;
	for (;;) {
		int c = rd_byte(r);
		if (c < 0) return -1;
		if (c == '\n') break;
		if (c != '\r' && k + 1 < n) out[k++] = (char)c;
	}
	out[k] = 0;
	return 0;
}

/* 送出請求；body 是小的內容（JSON），大的用 send_len 告訴長度、之後自己寫 */
static int dk_open(Rd *r, const char *method, const char *path, const char *ctype, const void *body, size_t blen,
                   long long send_len, int timeout)
{
	memset(r, 0, offsetof(Rd, buf));
	r->timeout = timeout;
	r->fd = dk_connect();
	if (r->fd < 0) return -1;
	char head[4096];
	long long len = body ? (long long)blen : send_len;
	int l = snprintf(head, sizeof head, "%s %s HTTP/1.1\r\nHost: docker\r\nUser-Agent: pomelo-docker\r\nConnection: close\r\n", method, path);
	if (ctype) l += snprintf(head + l, sizeof head - (size_t)l, "Content-Type: %s\r\n", ctype);
	if (len >= 0) l += snprintf(head + l, sizeof head - (size_t)l, "Content-Length: %lld\r\n", len);
	l += snprintf(head + l, sizeof head - (size_t)l, "\r\n");
	if (write_all(r->fd, head, (size_t)l) || (body && blen && write_all(r->fd, body, blen))) { close(r->fd); r->fd = -1; return -1; }
	return 0;
}

/* 讀狀態列與標頭 */
static int dk_head(Rd *r)
{
	char line[4096];
	if (rd_line(r, line, sizeof line) || strncmp(line, "HTTP/1.", 7)) return -1;
	r->status = atoi(line + 9);
	r->clen = -1;
	for (;;) {
		if (rd_line(r, line, sizeof line)) return -1;
		if (!line[0]) break;
		if (!strncasecmp(line, "Transfer-Encoding:", 18) && strcasestr(line, "chunked")) r->chunked = 1;
		else if (!strncasecmp(line, "Content-Length:", 15)) r->clen = atoll(line + 15);
	}
	if (r->clen == 0 && !r->chunked) r->done = 1;
	return 0;
}

/* 讀回應內容（chunked 自動拆開），回傳讀到的位元組數，0 = 結束，-1 = 錯誤 */
static ssize_t dk_read(Rd *r, void *out, size_t n)
{
	if (r->done) return 0;
	if (r->chunked && r->left == 0) {
		char line[64];
		if (rd_line(r, line, sizeof line)) return -1;
		if (!line[0] && rd_line(r, line, sizeof line)) return -1;   /* 上一塊後面的 CRLF */
		r->left = strtoll(line, NULL, 16);
		if (r->left <= 0) { r->done = 1; return 0; }
	}
	long long want = (long long)n;
	if (r->chunked) want = MIN(want, r->left);
	else if (r->clen >= 0) want = MIN(want, r->clen);
	if (want <= 0) { r->done = 1; return 0; }
	int f = rd_fill(r);
	if (f < 0) return -1;
	if (f == 0) { r->done = 1; return 0; }
	size_t k = MIN((size_t)want, r->have - r->pos);
	memcpy(out, r->buf + r->pos, k);
	r->pos += k;
	if (r->chunked) r->left -= (long long)k;
	else if (r->clen >= 0 && (r->clen -= (long long)k) == 0) r->done = 1;
	return (ssize_t)k;
}

static void dk_close(Rd *r)
{
	if (r->fd >= 0) close(r->fd);
	r->fd = -1;
}

/* 一般的請求：整個回應讀進記憶體（呼叫者 free），*status 是 HTTP 狀態碼（-1 = 連不上） */
static char *dk_call(const char *method, const char *path, const char *json, int *status, int timeout)
{
	Rd *r = malloc(sizeof *r);
	char *out = NULL;
	size_t n = 0, cap = 0;
	*status = -1;
	if (dk_open(r, method, path, json ? "application/json" : NULL, json, json ? strlen(json) : 0, json ? -1 : (strcmp(method, "GET") ? 0 : -1), timeout) ||
	    dk_head(r)) { if (r->fd >= 0) dk_close(r); free(r); return NULL; }
	*status = r->status;
	char tmp[16384];
	ssize_t k;
	while ((k = dk_read(r, tmp, sizeof tmp)) > 0) {
		if (n + (size_t)k + 1 > cap) { cap = (n + (size_t)k + 1) * 2; out = realloc(out, cap); }
		memcpy(out + n, tmp, (size_t)k);
		n += (size_t)k;
	}
	if (!out) out = calloc(1, 1);
	else out[n] = 0;
	dk_close(r);
	free(r);
	return out;
}

static Js *dk_json(const char *method, const char *path, const char *body, int *status)
{
	int st;
	char *r = dk_call(method, path, body, &st, 10000);
	if (status) *status = st;
	Js *j = r ? js_parse(r) : NULL;
	free(r);
	return j;
}

/* 錯誤回應裡的訊息（{"message": "..."}） */
static void dk_errmsg(const char *body, int status, char *err, size_t en)
{
	Js *j = body ? js_parse(body) : NULL;
	const char *m = js_str(js_get(j, "message"), NULL);
	if (m) snprintf(err, en, "%s", m);
	else if (status < 0) snprintf(err, en, "%s", _("連不上 Docker"));
	else snprintf(err, en, "HTTP %d", status);
	js_free(j);
}

/* 送一個動作（POST／DELETE），成功回傳 0，失敗把訊息寫到 err */
static int dk_do(const char *method, const char *path, const char *json, char *err, size_t en, int timeout)
{
	int st;
	char *r = dk_call(method, path, json, &st, timeout);
	int ok = st >= 200 && st < 300;
	if (!ok) dk_errmsg(r, st, err, en);
	free(r);
	return ok ? 0 : -1;
}

/* 把回應內容寫到 fd（映像 tar、容器裡的檔案）。progress 每 1 MB 呼叫一次（可為 NULL） */
static int dk_to_fd(const char *method, const char *path, int out, char *err, size_t en, void (*progress)(long long))
{
	Rd *r = malloc(sizeof *r);
	int rc = -1;
	if (dk_open(r, method, path, NULL, NULL, 0, strcmp(method, "GET") ? 0 : -1, 0) || dk_head(r)) {
		snprintf(err, en, "%s", _("連不上 Docker"));
		goto out;
	}
	if (r->status < 200 || r->status >= 300) {
		char body[4096];
		ssize_t k = dk_read(r, body, sizeof body - 1);
		body[k > 0 ? k : 0] = 0;
		dk_errmsg(body, r->status, err, en);
		goto out;
	}
	char tmp[65536];
	ssize_t k;
	long long total = 0, next = 1 << 20;
	while ((k = dk_read(r, tmp, sizeof tmp)) > 0) {
		if (write_all(out, tmp, (size_t)k)) { snprintf(err, en, "%s", strerror(errno)); goto out; }
		total += k;
		if (progress && total >= next) { progress(total); next = total + (1 << 20); }
	}
	rc = k < 0 ? -1 : 0;
	if (rc) snprintf(err, en, "%s", _("跟 Docker 的連線中斷了"));
out:
	if (r->fd >= 0) dk_close(r);
	free(r);
	return rc;
}

/* 把檔案的內容送上去（還原映像、把檔案放回容器）；resp 收回應（呼叫者 free，可為 NULL） */
static int dk_from_fd(const char *method, const char *path, const char *ctype, int in, long long size, char *err, size_t en,
                      void (*line_cb)(const char *))
{
	Rd *r = malloc(sizeof *r);
	int rc = -1;
	if (dk_open(r, method, path, ctype, NULL, 0, size, 0)) { snprintf(err, en, "%s", _("連不上 Docker")); goto out; }
	char tmp[65536];
	ssize_t k;
	while ((k = read(in, tmp, sizeof tmp)) > 0)
		if (write_all(r->fd, tmp, (size_t)k)) { snprintf(err, en, "%s", _("跟 Docker 的連線中斷了")); goto out; }
	if (dk_head(r)) { snprintf(err, en, "%s", _("跟 Docker 的連線中斷了")); goto out; }
	/* 回應：錯誤訊息，或一行一個 JSON 的進度（映像載入：200 但內容可能是 {"error": "..."}） */
	char line[8192], body[8192];
	size_t ln = 0, bn = 0;
	int failed = 0;
	while ((k = dk_read(r, tmp, sizeof tmp)) > 0)
		for (ssize_t i = 0; i < k; i++) {
			if (bn + 1 < sizeof body) body[bn++] = tmp[i];
			if (tmp[i] != '\n' && ln + 1 < sizeof line) { line[ln++] = tmp[i]; continue; }
			line[ln] = 0;
			ln = 0;
			Js *j = js_parse(line);
			const char *e = js_str(js_get(j, "error"), NULL);
			if (e) { snprintf(err, en, "%s", e); failed = 1; }
			else if (line_cb && j) line_cb(js_str(js_get(j, "stream"), ""));
			js_free(j);
		}
	if (ln) {   /* 最後一行沒有換行 */
		line[ln] = 0;
		Js *j = js_parse(line);
		const char *e = js_str(js_get(j, "error"), NULL);
		if (e) { snprintf(err, en, "%s", e); failed = 1; }
		js_free(j);
	}
	body[bn] = 0;
	if (r->status < 200 || r->status >= 300) { dk_errmsg(body, r->status, err, en); goto out; }
	if (failed) goto out;
	rc = 0;
out:
	if (r->fd >= 0) dk_close(r);
	free(r);
	return rc;
}

/* 一行一個 JSON 的串流（下載映像的進度）：每一行呼叫 cb；有 "error" 就失敗 */
static int dk_stream(const char *method, const char *path, char *err, size_t en, void (*cb)(Js *))
{
	Rd *r = malloc(sizeof *r);
	int rc = -1;
	if (dk_open(r, method, path, NULL, NULL, 0, 0, 0) || dk_head(r)) { snprintf(err, en, "%s", _("連不上 Docker")); goto out; }
	char tmp[16384], line[16384];
	size_t ln = 0;
	ssize_t k;
	int failed = r->status < 200 || r->status >= 300;
	while ((k = dk_read(r, tmp, sizeof tmp)) > 0)
		for (ssize_t i = 0; i < k; i++) {
			if (tmp[i] != '\n' && ln + 1 < sizeof line) { line[ln++] = tmp[i]; continue; }
			line[ln] = 0;
			ln = 0;
			Js *j = js_parse(line);
			if (!j) continue;
			const char *e = js_str(js_get(j, "error"), js_str(js_get(j, "message"), NULL));
			if (e && (failed || js_get(j, "error"))) { snprintf(err, en, "%s", e); failed = 1; }
			else if (cb) cb(j);
			js_free(j);
		}
	if (failed && !err[0]) snprintf(err, en, "HTTP %d", r->status);
	rc = failed ? -1 : 0;
out:
	if (r->fd >= 0) dk_close(r);
	free(r);
	return rc;
}

/* 網址裡的參數要編碼 */
static void url_esc(char *out, size_t n, const char *s)
{
	size_t k = 0;
	for (; *s && k + 4 < n; s++) {
		unsigned char c = (unsigned char)*s;
		if (isalnum(c) || strchr("-_.~", c)) out[k++] = (char)c;
		else k += (size_t)snprintf(out + k, n - k, "%%%02X", c);
	}
	out[k] = 0;
}
