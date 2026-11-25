/* SPDX-License-Identifier: GPL-3.0-or-later */
/* backup.c — 容器的備份與還原（被 pomelo-docker.c #include，在背景的子行程裡執行）
 *
 * 一個備份是一個資料夾：<備份位置>/<容器名稱>-<日期時間>/
 *   backup.json     這份備份的說明（名稱、時間、映像、掛載點與資料檔的對應、備份時有沒有在執行）
 *   container.json  docker inspect 的結果（還原時照它重建容器）
 *   image.tar       容器現在的樣子（docker commit 後再 docker save；可以在別台電腦還原）。只備份資料時沒有
 *   mounts/N.tar    每個掛載點（資料卷、本機資料夾）裡的檔案（docker cp 的格式）
 *   README.txt      不用 Pomelo 也能手動還原的說明
 * 還原：載入映像 → 建立資料卷 → 照原本的設定建立容器（名稱被用掉就加 -restored）→ 把資料放回去 → 原本在執行就啟動。
 */

static void (*bk_progress)(const char *fmt, ...);
static void (*bk_warn)(const char *msg);   /* 還原好了但有小問題（例如沒辦法啟動） */

static void js_del(Js *obj, const char *key)
{
	if (!obj || obj->type != JS_OBJ) return;
	for (Js **p = &obj->kids; *p; p = &(*p)->next)
		if ((*p)->key && !strcmp((*p)->key, key)) {
			Js *k = *p;
			*p = k->next;
			k->next = NULL;
			obj->n--;
			js_free(k);
			return;
		}
}

static int write_file(const char *path, const char *data, size_t n)
{
	FILE *f = fopen(path, "w");
	if (!f) return -1;
	size_t w = fwrite(data, 1, n, f);
	return fclose(f) || w != n ? -1 : 0;
}

static char *read_file(const char *path)
{
	FILE *f = fopen(path, "r");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *s = malloc((size_t)MAX(n, 0) + 1);
	size_t r = fread(s, 1, (size_t)MAX(n, 0), f);
	s[r] = 0;
	fclose(f);
	return s;
}

static int mkdirs(const char *path, mode_t mode)
{
	char p[PATH_MAX];
	snprintf(p, sizeof p, "%s", path);
	for (char *s = p + 1; *s; s++)
		if (*s == '/') { *s = 0; mkdir(p, mode); *s = '/'; }
	return mkdir(p, mode) == 0 || errno == EEXIST ? 0 : -1;
}

static int rm_entry(const char *path, const struct stat *st, int flag, struct FTW *ftw) { return remove(path); }
static int rm_rf(const char *path) { return nftw(path, rm_entry, 16, FTW_DEPTH | FTW_PHYS); }

/* 映像名稱只能小寫英數 */
static void repo_name(char *out, size_t n, const char *name)
{
	size_t k = 0;
	for (const char *p = name; *p && k + 1 < n; p++) {
		char c = (char)tolower((unsigned char)*p);
		out[k++] = isalnum((unsigned char)c) || c == '.' || c == '-' || c == '_' ? c : '-';
	}
	out[k] = 0;
	if (!k) snprintf(out, n, "container");
}

static void mb_progress(long long bytes) { bk_progress(_("已經寫了 %.0f MB…"), bytes / 1048576.0); }

/* 備份一個容器到 root 底下的新資料夾；with_image = 連映像一起（可以在別台電腦還原）。成功時 out 是資料夾路徑 */
static int do_backup(const char *id, const char *root, int with_image, char *out, size_t on, char *err, size_t en)
{
	char path[PATH_MAX + 64], q[512];
	int st;
	url_esc(q, sizeof q, id);
	snprintf(path, sizeof path, "/containers/%s/json", q);
	char *raw = dk_call("GET", path, NULL, &st, 10000);
	if (st != 200) { dk_errmsg(raw, st, err, en); free(raw); return -1; }
	Js *ins = js_parse(raw);
	const char *name = js_str(js_get(ins, "Name"), "container");
	if (*name == '/') name++;
	char stamp[32], dir[PATH_MAX], repo[160], tag[220];
	time_t now = time(NULL);
	strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", localtime(&now));
	snprintf(dir, sizeof dir, "%s/%s-%s", root, name, stamp);
	snprintf(out, on, "%s", dir);
	repo_name(repo, sizeof repo, name);
	snprintf(tag, sizeof tag, "pomelo-backup/%s:%s", repo, stamp);
	int rc = -1;
	char sub[PATH_MAX + 32];
	snprintf(sub, sizeof sub, "%s/mounts", dir);
	if (mkdirs(sub, 0700)) { snprintf(err, en, "%s：%s", _("無法建立備份資料夾"), strerror(errno)); goto out; }
	snprintf(sub, sizeof sub, "%s/container.json", dir);
	if (write_file(sub, raw, strlen(raw))) { snprintf(err, en, "%s", strerror(errno)); goto out; }

	if (with_image) {
		bk_progress("%s", _("正在把容器現在的樣子存成映像…"));
		snprintf(path, sizeof path, "/commit?container=%s&repo=pomelo-backup%%2F%s&tag=%s&pause=true", q, repo, stamp);
		if (dk_do("POST", path, "{}", err, en, 0)) goto out;
		bk_progress("%s", _("正在匯出映像…"));
		snprintf(sub, sizeof sub, "%s/image.tar", dir);
		int fd = open(sub, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
		if (fd < 0) { snprintf(err, en, "%s", strerror(errno)); goto out; }
		char tq[300];
		url_esc(tq, sizeof tq, tag);
		snprintf(path, sizeof path, "/images/get?names=%s", tq);
		int r = dk_to_fd("GET", path, fd, err, en, mb_progress);
		if (close(fd) && !r) { snprintf(err, en, "%s", strerror(errno)); r = -1; }
		snprintf(path, sizeof path, "/images/%s", tq);
		char e2[256];
		dk_do("DELETE", path, NULL, e2, sizeof e2, 30000);   /* 暫時的標籤拿掉（圖層跟原本的映像共用，不佔空間） */
		if (r) goto out;
	}

	/* 每個掛載點的檔案 */
	Buf mj = { 0 };
	buf_str(&mj, "[");
	int i = 0;
	for (Js *m = js_get(ins, "Mounts") ? js_get(ins, "Mounts")->kids : NULL; m; m = m->next) {
		const char *dest = js_str(js_get(m, "Destination"), NULL), *type = js_str(js_get(m, "Type"), "");
		if (!dest || (strcmp(type, "volume") && strcmp(type, "bind"))) continue;
		bk_progress(_("正在備份 %s…"), dest);
		char dq[1024];
		url_esc(dq, sizeof dq, dest);
		snprintf(path, sizeof path, "/containers/%s/archive?path=%s", q, dq);
		snprintf(sub, sizeof sub, "%s/mounts/%d.tar", dir, i);
		int fd = open(sub, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
		if (fd < 0) { snprintf(err, en, "%s", strerror(errno)); free(mj.s); goto out; }
		int r = dk_to_fd("GET", path, fd, err, en, mb_progress);
		close(fd);
		if (r) { free(mj.s); goto out; }
		if (i) buf_str(&mj, ",");
		buf_printf(&mj, "{\"file\":\"mounts/%d.tar\",\"type\":", i);
		buf_quote(&mj, type);
		buf_str(&mj, ",\"name\":");
		buf_quote(&mj, js_str(js_get(m, "Name"), ""));
		buf_str(&mj, ",\"source\":");
		buf_quote(&mj, js_str(js_get(m, "Source"), ""));
		buf_str(&mj, ",\"destination\":");
		buf_quote(&mj, dest);
		buf_printf(&mj, ",\"rw\":%s}", js_num(js_get(m, "RW"), 1) ? "true" : "false");
		i++;
	}
	buf_str(&mj, "]");

	Buf b = { 0 };
	const char *orig = js_str(js_get(js_get(ins, "Config"), "Image"), "");
	buf_str(&b, "{\"version\":1,\"name\":");
	buf_quote(&b, name);
	buf_printf(&b, ",\"created\":%lld,\"image\":", (long long)now);
	buf_quote(&b, with_image ? tag : orig);
	buf_str(&b, ",\"original_image\":");
	buf_quote(&b, orig);
	buf_printf(&b, ",\"with_image\":%s,\"running\":%s,\"mounts\":%s}\n", with_image ? "true" : "false",
	           js_num(js_get(js_get(ins, "State"), "Running"), 0) ? "true" : "false", mj.s);
	free(mj.s);
	snprintf(sub, sizeof sub, "%s/backup.json", dir);
	if (write_file(sub, b.s, b.n)) { free(b.s); snprintf(err, en, "%s", strerror(errno)); goto out; }
	free(b.s);

	snprintf(sub, sizeof sub, "%s/README.txt", dir);
	char readme[2048];
	int rl = snprintf(readme, sizeof readme,
	                  "Pomelo container backup: %s (%s)\n\n"
	                  "Restore with Pomelo: open \"Containers\" -> Backups -> Restore, or run: pomelo-docker --restore <this folder>\n\n"
	                  "Manual restore:\n"
	                  "  docker load -i image.tar          # image: %s\n"
	                  "  docker create ... (see container.json for the original settings)\n"
	                  "  docker cp - <container>:<parent of mount point> < mounts/N.tar   # see backup.json\n",
	                  name, stamp, with_image ? tag : orig);
	write_file(sub, readme, (size_t)rl);
	rc = 0;
out:
	if (rc) rm_rf(dir);
	js_free(ins);
	free(raw);
	return rc;
}

/* 名稱有沒有被用掉 */
static int dk_exists(const char *what, const char *name)
{
	char q[512], path[600];
	int st;
	url_esc(q, sizeof q, name);
	snprintf(path, sizeof path, "/%s/%s/json", what, q);
	if (!strcmp(what, "volumes")) snprintf(path, sizeof path, "/volumes/%s", q);
	if (!strcmp(what, "networks")) snprintf(path, sizeof path, "/networks/%s", q);
	free(dk_call("GET", path, NULL, &st, 10000));
	return st == 200;
}

static int is_hex64(const char *s)
{
	size_t n = strlen(s);
	if (n != 64) return 0;
	for (; *s; s++) if (!isxdigit((unsigned char)*s)) return 0;
	return 1;
}

static void load_line(const char *s) { if (*s) bk_progress("%s", s); }
static void pull_line(Js *j)
{
	const char *s = js_str(js_get(j, "status"), NULL), *id = js_str(js_get(j, "id"), "");
	Js *pd = js_get(j, "progressDetail");
	double cur = js_num(js_get(pd, "current"), 0), tot = js_num(js_get(pd, "total"), 0);
	if (s && tot > 0) bk_progress("%s %s %.0f%%", id, s, cur * 100 / tot);
	else if (s) bk_progress("%s %s", id, s);
}

/* 下載映像（名稱:標籤），已經有就不下載 */
static int ensure_image(const char *image, char *err, size_t en)
{
	if (dk_exists("images", image)) return 0;
	char from[512], tag[128] = "", path[1024], fq[600], tq[200];
	snprintf(from, sizeof from, "%s", image);
	char *at = strchr(from, '@'), *slash = strrchr(from, '/'), *colon = strrchr(from, ':');
	if (!at && colon && (!slash || colon > slash)) { *colon = 0; snprintf(tag, sizeof tag, "%s", colon + 1); }
	if (!at && !tag[0]) snprintf(tag, sizeof tag, "latest");
	url_esc(fq, sizeof fq, from);
	url_esc(tq, sizeof tq, tag);
	snprintf(path, sizeof path, "/images/create?fromImage=%s%s%s", fq, tag[0] ? "&tag=" : "", tq);
	bk_progress(_("正在下載映像 %s…"), image);
	err[0] = 0;
	return dk_stream("POST", path, err, en, pull_line);
}

/* 從備份資料夾還原，成功時 out 是新容器的名稱 */
static int do_restore(const char *dir, char *out, size_t on, char *err, size_t en)
{
	char p[PATH_MAX + 64];
	snprintf(p, sizeof p, "%s/backup.json", dir);
	char *braw = read_file(p);
	snprintf(p, sizeof p, "%s/container.json", dir);
	char *craw = read_file(p);
	Js *bj = js_parse(braw), *cj = js_parse(craw);
	int rc = -1;
	Buf b = { 0 };
	if (!bj || !cj) { snprintf(err, en, "%s", _("這不是容器的備份資料夾（缺少 backup.json 或 container.json）")); goto out; }
	const char *image = js_str(js_get(bj, "image"), "");
	/* 映像 */
	snprintf(p, sizeof p, "%s/image.tar", dir);
	if (!access(p, R_OK)) {
		bk_progress("%s", _("正在載入映像…"));
		int fd = open(p, O_RDONLY | O_CLOEXEC);
		struct stat st;
		if (fd < 0 || fstat(fd, &st)) { snprintf(err, en, "%s", strerror(errno)); if (fd >= 0) close(fd); goto out; }
		int r = dk_from_fd("POST", "/images/load?quiet=1", "application/x-tar", fd, (long long)st.st_size, err, en, load_line);
		close(fd);
		if (r) goto out;
	} else if (ensure_image(image, err, en)) goto out;

	/* 名稱：原本的被用掉就加 -restored */
	const char *orig = js_str(js_get(bj, "name"), "restored");
	char name[200];
	snprintf(name, sizeof name, "%s", orig);
	for (int i = 1; dk_exists("containers", name) && i < 100; i++)
		snprintf(name, sizeof name, i == 1 ? "%s-restored" : "%s-restored-%d", orig, i);
	snprintf(out, on, "%s", name);

	/* 掛載點：具名資料卷照原名（被用掉就換名字），匿名資料卷與找不到的本機資料夾改成新的資料卷 */
	Buf binds = { 0 };
	buf_str(&binds, "[");
	int nb = 0;
	char vols[32][200];
	int nmount = 0;
	for (Js *m = js_get(bj, "mounts") ? js_get(bj, "mounts")->kids : NULL; m && nmount < 32; m = m->next, nmount++) {
		const char *type = js_str(js_get(m, "type"), ""), *vname = js_str(js_get(m, "name"), ""), *src = js_str(js_get(m, "source"), "");
		const char *dest = js_str(js_get(m, "destination"), "/data");
		int rw = (int)js_num(js_get(m, "rw"), 1);
		char target[PATH_MAX];
		if (!strcmp(type, "bind") && src[0] && !access(src, F_OK)) snprintf(target, sizeof target, "%s", src);
		else {
			if (!strcmp(type, "volume") && vname[0] && !is_hex64(vname)) snprintf(target, sizeof target, "%s", vname);
			else snprintf(target, sizeof target, "%s-%d", name, nmount);
			for (int k = 1; dk_exists("volumes", target) && k < 100; k++)
				snprintf(target, sizeof target, k == 1 ? "%s-restored" : "%s-restored-%d",
				         !strcmp(type, "volume") && vname[0] && !is_hex64(vname) ? vname : name, k);
			Buf vb = { 0 };
			buf_str(&vb, "{\"Name\":");
			buf_quote(&vb, target);
			buf_str(&vb, "}");
			int r = dk_do("POST", "/volumes/create", vb.s, err, en, 30000);
			free(vb.s);
			if (r) { free(binds.s); goto out; }
		}
		snprintf(vols[nmount], sizeof vols[0], "%s", target);
		char spec[PATH_MAX * 2];
		snprintf(spec, sizeof spec, "%s:%s%s", target, dest, rw ? "" : ":ro");
		if (nb++) buf_str(&binds, ",");
		buf_quote(&binds, spec);
	}
	buf_str(&binds, "]");

	/* 照原本的設定建立：Config 換映像、拿掉舊的主機名稱與 compose 的標籤；HostConfig 的掛載改成上面的 */
	Js *cfg = js_get(cj, "Config"), *hc = js_get(cj, "HostConfig");
	js_del(cfg, "Hostname");
	js_del(cfg, "Image");
	Js *labels = js_get(cfg, "Labels");
	for (int again = 1; again && labels && labels->type == JS_OBJ;) {
		again = 0;
		for (Js *l = labels->kids; l; l = l->next)
			if (l->key && !strncmp(l->key, "com.docker.compose.", 19)) { js_del(labels, l->key); again = 1; break; }
	}
	js_del(hc, "Binds");
	js_del(hc, "Mounts");
	const char *nm = js_str(js_get(hc, "NetworkMode"), "default");
	char netmode[200];
	snprintf(netmode, sizeof netmode, "%s", nm);
	if (strcmp(netmode, "default") && strcmp(netmode, "bridge") && strcmp(netmode, "host") && strcmp(netmode, "none") &&
	    strncmp(netmode, "container:", 10) && !dk_exists("networks", netmode))
		snprintf(netmode, sizeof netmode, "bridge");   /* 原本的網路（例如 compose 建的）這裡沒有 */
	js_del(hc, "NetworkMode");
	buf_str(&b, "{\"Image\":");
	buf_quote(&b, image);
	for (Js *k = cfg ? cfg->kids : NULL; k; k = k->next) {
		buf_str(&b, ",");
		buf_quote(&b, k->key);
		buf_str(&b, ":");
		js_dump(&b, k);
	}
	buf_str(&b, ",\"HostConfig\":{\"Binds\":");
	buf_str(&b, binds.s);
	buf_str(&b, ",\"NetworkMode\":");
	buf_quote(&b, netmode);
	for (Js *k = hc ? hc->kids : NULL; k; k = k->next) {
		buf_str(&b, ",");
		buf_quote(&b, k->key);
		buf_str(&b, ":");
		js_dump(&b, k);
	}
	buf_str(&b, "}}");
	free(binds.s);

	bk_progress(_("正在建立容器 %s…"), name);
	char q[400], path[PATH_MAX + 64];
	url_esc(q, sizeof q, name);
	snprintf(path, sizeof path, "/containers/create?name=%s", q);
	int st;
	char *resp = dk_call("POST", path, b.s, &st, 30000);
	if (st != 201) { dk_errmsg(resp, st, err, en); free(resp); goto out; }
	Js *rj = js_parse(resp);
	char cid[80];
	snprintf(cid, sizeof cid, "%s", js_str(js_get(rj, "Id"), name));
	js_free(rj);
	free(resp);

	/* 資料放回去：tar 裡最上層是掛載點的資料夾名稱，所以解到它的上一層 */
	int i = 0;
	for (Js *m = js_get(bj, "mounts") ? js_get(bj, "mounts")->kids : NULL; m && i < nmount; m = m->next, i++) {
		const char *dest = js_str(js_get(m, "destination"), NULL), *file = js_str(js_get(m, "file"), NULL);
		const char *type = js_str(js_get(m, "type"), "");
		if (!dest || !file) continue;
		if (!strcmp(type, "bind") && !strcmp(vols[i], js_str(js_get(m, "source"), ""))) continue;   /* 原本的本機資料夾還在：不覆蓋 */
		char parent[PATH_MAX], dq[PATH_MAX * 3], tar[PATH_MAX + 64];
		snprintf(parent, sizeof parent, "%s", dest);
		char *sl = strrchr(parent, '/');
		if (sl == parent) parent[1] = 0; else if (sl) *sl = 0;
		snprintf(tar, sizeof tar, "%s/%s", dir, file);
		int fd = open(tar, O_RDONLY | O_CLOEXEC);
		struct stat stt;
		if (fd < 0 || fstat(fd, &stt)) { if (fd >= 0) close(fd); continue; }
		bk_progress(_("正在把資料放回 %s…"), dest);
		url_esc(dq, sizeof dq, parent);
		snprintf(path, sizeof path, "/containers/%s/archive?path=%s", cid, dq);
		int r = dk_from_fd("PUT", path, "application/x-tar", fd, (long long)stt.st_size, err, en, NULL);
		close(fd);
		if (r) goto out;
	}
	if (js_num(js_get(bj, "running"), 0)) {
		bk_progress("%s", _("正在啟動容器…"));
		snprintf(path, sizeof path, "/containers/%s/start", cid);
		if (dk_do("POST", path, NULL, err, en, 60000)) {   /* 容器已經還原好了，只是沒辦法啟動（常見：連接埠被原本的容器佔著） */
			if (bk_warn) bk_warn(err);
			err[0] = 0;
		}
	}
	rc = 0;
out:
	free(b.s);
	js_free(bj);
	js_free(cj);
	free(braw);
	free(craw);
	return rc;
}
